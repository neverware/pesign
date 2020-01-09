/*
 * Copyright 2012-2014 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include "fix_coverity.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>
#include <grp.h>

#include "pesign.h"
#include "file_kmod.h"

#include <prerror.h>
#include <nss.h>

static int should_exit = 0;

typedef struct {
	cms_context *cms;
	cms_context *backup_cms;
	uid_t uid;
	gid_t gid;
	pid_t pid;
	int sd;
	int priority;
	char *errstr;
	uint8_t **tokennames;
	int ntokennames;
} context;

static void
steal_from_cms(cms_context *old, cms_context *new)
{
	new->tokenname = old->tokenname;
	new->certname = old->certname;

	new->selected_digest = old->selected_digest;

	new->log = old->log;
	new->log_priv = old->log_priv;
}

static void
hide_stolen_goods_from_cms(cms_context *new,
			   cms_context *old UNUSED)
{
	new->tokenname = NULL;
	new->certname = NULL;
}

static void
send_response(context *ctx, cms_context *cms, struct pollfd *pollfd, int32_t rc)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	int msglen = ctx->errstr ? strlen(ctx->errstr) + 1 : 0;

	iov.iov_len = sizeof(pesignd_msghdr) + sizeof(pesignd_cmd_response)
			+ msglen;

	void *buffer = calloc(1, iov.iov_len);
	if (!buffer) {
		cms->log(cms, ctx->priority|LOG_ERR,
			"could not allocate memory: %m");
		exit(1);
	}

	iov.iov_base = buffer;

	pesignd_msghdr *pm = buffer;
	pesignd_cmd_response *resp = (pesignd_cmd_response *)((uint8_t *)pm +
					offsetof(pesignd_msghdr, size) +
					sizeof (pm->size));

	pm->version = PESIGND_VERSION;
	pm->command = CMD_RESPONSE;
	pm->size = sizeof(resp->rc) + msglen;

	memset(&msg, '\0', sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	resp->rc = rc;
	if (ctx->errstr)
		memcpy(resp->errmsg, ctx->errstr, msglen);

	n = sendmsg(pollfd->fd, &msg, 0);
	if (n < 0)
		cms->log(cms, ctx->priority|LOG_WARNING,
			"could not send response to client: %m");

	free(buffer);
}

static void
handle_kill_daemon(context *ctx UNUSED,
		   struct pollfd *pollfd UNUSED,
		   socklen_t size UNUSED)
{
	should_exit = 1;
}

static int
cmpstringp(const void *p1, const void *p2)
{
	return strcmp(*(char * const *)p1, *(char * const *)p2);
}

static int
add_token_to_authenticated_list(context *ctx, uint8_t *tokenname)
{
	char *tmp;
	uint8_t **newtokennames = realloc(ctx->tokennames,
					sizeof (uint8_t *)
					* (ctx->ntokennames+1));
	if (!newtokennames)
		return -1;
	ctx->tokennames = newtokennames;

	tmp = strdup((char *)tokenname);
	if (!tmp)
		return -1;

	newtokennames[ctx->ntokennames++] = (uint8_t *)tmp;

	qsort(newtokennames, ctx->ntokennames, sizeof (char *), cmpstringp);
	return 0;
}

static void
handle_unlock_token(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	int rc = cms_context_alloc(&ctx->cms);
	if (rc < 0) {
		send_response(ctx, ctx->backup_cms, pollfd, rc);
		return;
	}

	steal_from_cms(ctx->backup_cms, ctx->cms);

	char *buffer = malloc(size);
	if (!buffer) {
oom:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));

	iov.iov_base = buffer;
	iov.iov_len = size;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);

	pesignd_string *tn = (pesignd_string *)buffer;
	if (n < (long long)sizeof(tn->size)) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"possible exploit attempt. closing.");
		close(pollfd->fd);
		return;
	}
	n -= sizeof(tn->size);
	if ((size_t)n < tn->size)
		goto malformed;
	n -= tn->size;

	if (tn->value[tn->size - 1] != '\0')
		goto malformed;

	pesignd_string *tp = pesignd_string_next(tn);
	if ((size_t)n < sizeof(tp->size))
		goto malformed;
	n -= sizeof(tp->size);
	if ((size_t)n < tp->size)
		goto malformed;
	n -= tp->size;

	if (tn->value[tn->size - 1] != '\0')
		goto malformed;

	if (n != 0)
		goto malformed;

	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
		"unlocking token \"%s\"", tn->value);

	/* authenticating with nss frees this ... best API ever. */
	ctx->cms->tokenname = PORT_ArenaStrdup(ctx->cms->arena,
						(char *)tn->value);
	if (!ctx->cms->tokenname)
		goto oom;

	char *pin = (char *)tp->value;
	if (!pin)
		goto oom;

	cms_set_pw_callback(ctx->cms, get_password_passthrough);
	cms_set_pw_data(ctx->cms, pin);

	rc = unlock_nss_token(ctx->cms);

	cms_set_pw_callback(ctx->cms, get_password_fail);
	cms_set_pw_data(ctx->cms, NULL);

	if (rc == -1)
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not find token \"%s\"", tn->value);
	else if (rc == 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
			"authentication succeeded for token \"%s\"",
			tn->value);
		rc = add_token_to_authenticated_list(ctx, tn->value);
		if (rc < 0)
			ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
				"couldn't add token to internal list: %m");
	}

	send_response(ctx, ctx->cms, pollfd, rc);
	free(buffer);

	hide_stolen_goods_from_cms(ctx->cms, ctx->backup_cms);
	cms_context_fini(ctx->cms);
}

static void
handle_is_token_unlocked(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	int rc = cms_context_alloc(&ctx->cms);
	if (rc < 0) {
		send_response(ctx, ctx->backup_cms, pollfd, rc);
		return;
	}

	steal_from_cms(ctx->backup_cms, ctx->cms);

	char *buffer = malloc(size);
	if (!buffer) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));

	iov.iov_base = buffer;
	iov.iov_len = size;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);

	pesignd_string *tn = (pesignd_string *)buffer;
	if (n < (long long)sizeof(tn->size)) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"possible exploit attempt. closing.");
		close(pollfd->fd);
		return;
	}
	n -= sizeof(tn->size);
	if ((size_t)n < tn->size)
		goto malformed;
	n -= tn->size;

	if (tn->value[tn->size - 1] != '\0')
		goto malformed;

	if (n != 0)
		goto malformed;

	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
		"querying token \"%s\"", tn->value);

	char *key = (char *)tn->value;
	char *tokenname;

	tokenname = bsearch(&key, ctx->tokennames, ctx->ntokennames,
				sizeof (char *), cmpstringp);
	send_response(ctx, ctx->cms, pollfd, tokenname == NULL ? 1 : 0);

	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
			"token \"%s\" is %sunlocked", tn->value,
			tokenname == NULL ? "not " : "");

	free(buffer);

	hide_stolen_goods_from_cms(ctx->cms, ctx->backup_cms);
	cms_context_fini(ctx->cms);
}

static void
handle_get_cmd_version(context *ctx, struct pollfd *pollfd, socklen_t size);

static void
socket_get_fd(context *ctx, int sd, int *fd)
{
	struct msghdr msg;
	struct iovec iov;
	char buf[2];

	size_t controllen = CMSG_SPACE(sizeof(int));
	struct cmsghdr *cm = malloc(controllen);
	if (!cm) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));
	iov.iov_base = buf;
	iov.iov_len = 2;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cm;
	msg.msg_controllen = controllen;

	ssize_t n;
	n = recvmsg(sd, &msg, MSG_WAITALL);
	if (n < 0) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"possible exploit attempt. closing.");
		close(sd);
		return;
	}

	struct cmsghdr *cme = CMSG_FIRSTHDR(&msg);

	if (cme == NULL)
		goto malformed;

	if (cme->cmsg_level != SOL_SOCKET)
		goto malformed;

	if (cme->cmsg_type != SCM_RIGHTS)
		goto malformed;

	*fd = *((int *)CMSG_DATA(cme));

	free(cm);
}

static int
set_up_inpe(context *ctx, int fd, Pe **pe)
{
	*pe = pe_begin(fd, PE_C_READ_MMAP, NULL);
	if (!*pe)
		*pe = pe_begin(fd, PE_C_READ, NULL);
	if (!*pe) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not parse PE binary: %s",
			pe_errmsg(pe_errno()));
		return -1;
	}

	int rc = parse_signatures(&ctx->cms->signatures,
				  &ctx->cms->num_signatures, *pe);
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not parse signature list");
		pe_end(*pe);
		*pe = NULL;
		return -1;
	}
	return 0;
}

static int
set_up_outpe(context *ctx, int fd, Pe *inpe, Pe **outpe)
{
	size_t size;
	char *addr;

	addr = pe_rawfile(inpe, &size);

	off_t offset = lseek(fd, 0, SEEK_SET);
	if (offset < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not read output file: %m");
		return -1;
	}

	int rc = ftruncate(fd, size);
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not extend output file: %m");
		return -1;
	}
	rc = write(fd, addr, size);
	if (rc < 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not write to output file: %m");
		return -1;
	}

	*outpe = pe_begin(fd, PE_C_RDWR_MMAP, NULL);
	if (!*outpe)
		*outpe = pe_begin(fd, PE_C_RDWR, NULL);
	if (!*outpe) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not set up output: %s",
			pe_errmsg(pe_errno()));
		return -1;
	}

	pe_clearcert(*outpe);
	return 0;
}

static int
sign_pe(context *ctx, int infd, int outfd, int attached)
{
	Pe *inpe = NULL;

	int rc = set_up_inpe(ctx, infd, &inpe);
	if (rc < 0)
		goto finish;

	if (attached) {
		Pe *outpe = NULL;
		rc = set_up_outpe(ctx, outfd, inpe, &outpe);
		if (rc < 0)
			goto finish;

		rc = generate_digest(ctx->cms, outpe, 1);
		if (rc < 0) {
err_attached:
			pe_end(outpe);
			ftruncate(outfd, 0);
			goto finish;
		}
		ssize_t sigspace = calculate_signature_space(ctx->cms, outpe);
		if (sigspace < 0)
			goto err_attached;
		allocate_signature_space(outpe, sigspace);
		rc = generate_digest(ctx->cms, outpe, 1);
		if (rc < 0)
			goto err_attached;
		rc = generate_signature(ctx->cms);
		if (rc < 0)
			goto err_attached;
		insert_signature(ctx->cms, ctx->cms->num_signatures);
		finalize_signatures(ctx->cms->signatures,
				ctx->cms->num_signatures, outpe);
		pe_end(outpe);
	} else {
		ftruncate(outfd, 0);
		rc = generate_digest(ctx->cms, inpe, 1);
		if (rc < 0) {
err_detached:
			ftruncate(outfd, 0);
			goto finish;
		}
		rc = generate_signature(ctx->cms);
		if (rc < 0)
			goto err_detached;
		rc = export_signature(ctx->cms, outfd, 0);
		if (rc >= 0)
			ftruncate(outfd, rc);
		else if (rc < 0)
			goto err_detached;
	}

finish:
	if (inpe)
		pe_end(inpe);

	return rc;
}

static int
sign_kmod(context *ctx, int infd, int outfd, int attached)
{
	unsigned char *map;
	struct stat statbuf;
	ssize_t sig_len;
	int rc;

	rc = fstat(infd, &statbuf);
	if (rc != 0) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not stat input file: %m");
		return rc;
	}

	map = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, infd, 0);
	if (map == MAP_FAILED) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"could not map input file: %m");
		return -1;

	}

	rc = kmod_generate_digest(ctx->cms, map, statbuf.st_size);
	if (rc < 0)
		goto out;

	if (attached) {
		rc = write_file(outfd, map, statbuf.st_size);
		if (rc) {
			ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
				"could not write module data: %m");
			goto out;
		}
	}

	sig_len = kmod_write_signature(ctx->cms, outfd);
	if (sig_len < 0) {
		rc = sig_len;
		goto out;
	}

	rc = kmod_write_sig_info(ctx->cms, outfd, sig_len);

out:
	munmap(map, statbuf.st_size);
	return rc;
}

static void
handle_signing(context *ctx, struct pollfd *pollfd, socklen_t size,
	int attached)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	char *buffer = malloc(size);
	uint32_t file_format;

	if (!buffer) {
oom:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));

	iov.iov_base = buffer;
	iov.iov_len = size;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);

	file_format = *((uint32_t *) buffer);
	n -= sizeof(uint32_t);

	pesignd_string *tn = (pesignd_string *)(buffer + sizeof(uint32_t));
	if (n < (long long)sizeof(tn->size)) {
malformed:
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"handle_signing: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"possible exploit attempt. closing.");
		close(pollfd->fd);
		return;
	}

	n -= sizeof(tn->size);
	if ((size_t)n < tn->size)
		goto malformed;
	n -= tn->size;

	/* authenticating with nss frees these ... best API ever. */
	ctx->cms->tokenname = PORT_ArenaStrdup(ctx->cms->arena,
						(char *)tn->value);
	if (!ctx->cms->tokenname)
		goto oom;

	if ((size_t)n < sizeof(tn->size))
		goto malformed;
	pesignd_string *cn = pesignd_string_next(tn);
	n -= sizeof(cn->size);
	if ((size_t)n < cn->size)
		goto malformed;

	ctx->cms->certname = PORT_ArenaStrdup(ctx->cms->arena,
						(char *)cn->value);
	if (!ctx->cms->certname)
		goto oom;

	n -= cn->size;
	if (n != 0)
		goto malformed;

	int infd=-1;
	socket_get_fd(ctx, pollfd->fd, &infd);

	int outfd=-1;
	socket_get_fd(ctx, pollfd->fd, &outfd);

	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
		"attempting to sign with key \"%s:%s\"",
		tn->value, cn->value);
	free(buffer);

	int rc = find_certificate(ctx->cms, 1);
	if (rc < 0) {
		goto finish;
	}

	switch (file_format) {
	case FORMAT_PE_BINARY:
		rc = sign_pe(ctx, infd, outfd, attached);
		break;
	case FORMAT_KERNEL_MODULE:
		rc = sign_kmod(ctx, infd, outfd, attached);
		break;
	default:
		rc = -1;
		break;
	}

	if (rc < 0)
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			      "unrecognised format %d", file_format);

finish:
	close(infd);
	close(outfd);

	send_response(ctx, ctx->cms, pollfd, rc);
	teardown_digests(ctx->cms);
}

static void
handle_sign_attached(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	int rc = cms_context_alloc(&ctx->cms);
	if (rc < 0)
		return;

	steal_from_cms(ctx->backup_cms, ctx->cms);

	handle_signing(ctx, pollfd, size, 1);

	hide_stolen_goods_from_cms(ctx->cms, ctx->backup_cms);
	cms_context_fini(ctx->cms);
}

static void
handle_sign_detached(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	int rc = cms_context_alloc(&ctx->cms);
	if (rc < 0)
		return;

	steal_from_cms(ctx->backup_cms, ctx->cms);

	handle_signing(ctx, pollfd, size, 0);

	hide_stolen_goods_from_cms(ctx->cms, ctx->backup_cms);
	cms_context_fini(ctx->cms);
}

static void
#if 0
NORETURN
#endif
handle_invalid_input(pesignd_cmd cmd, context *ctx,
		     struct pollfd *pollfd UNUSED,
		     socklen_t size UNUSED)
{
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"got unexpected command 0x%x", cmd);
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"possible exploit attempt");
}

typedef void (*cmd_handler)(context *ctx, struct pollfd *pollfd,
				socklen_t size);

typedef struct {
	pesignd_cmd cmd;
	cmd_handler func;
	char *name;
	int32_t version;
} cmd_table_t;

cmd_table_t cmd_table[] = {
		{ CMD_KILL_DAEMON, handle_kill_daemon, "kill-daemon", 0 },
		{ CMD_UNLOCK_TOKEN, handle_unlock_token, "unlock-token", 0 },
		{ CMD_SIGN_ATTACHED, handle_sign_attached, "sign-attached", 0 },
		{ CMD_SIGN_DETACHED, handle_sign_detached, "sign-detached", 0 },
		{ CMD_RESPONSE, NULL, "response",  0 },
		{ CMD_IS_TOKEN_UNLOCKED, handle_is_token_unlocked,
			"is-token-unlocked", 0 },
		{ CMD_GET_CMD_VERSION, handle_get_cmd_version,
			"get-cmd-version", 0 },
		{ CMD_LIST_END, NULL, "list-end", 0 }
	};

static void
handle_get_cmd_version(context *ctx, struct pollfd *pollfd, socklen_t size)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	int rc = cms_context_alloc(&ctx->cms);
	if (rc < 0) {
		send_response(ctx, ctx->backup_cms, pollfd, rc);
		return;
	}

	steal_from_cms(ctx->backup_cms, ctx->cms);

	char *buffer = malloc(size);
	if (!buffer) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unable to allocate memory: %m");
		exit(1);
	}

	memset(&msg, '\0', sizeof(msg));

	iov.iov_base = buffer;
	iov.iov_len = size;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);

	int32_t version = -1;
	uint32_t command;

	if (n < (long long)sizeof(command)) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"unlock-token: invalid data");
		ctx->cms->log(ctx->cms, ctx->priority|LOG_ERR,
			"possible exploit attempt. closing.");
		close(pollfd->fd);
		return;
	}

	memcpy(&command, buffer, sizeof (command));
	ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
			"searching for command %d", command);

	for (int i = 0; cmd_table[i].cmd != CMD_LIST_END; i++) {
		if (cmd_table[i].cmd == command) {
			ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
					"cmd-version: found command \"%s\" "
					"version %d",
					cmd_table[i].name,
					cmd_table[i].version);
			version = cmd_table[i].version;
			break;
		}
	}

	if (version == -1) {
		ctx->cms->log(ctx->cms, ctx->priority|LOG_NOTICE,
				"cmd-version: could not find command %d",
				command);
	}
	send_response(ctx, ctx->cms, pollfd, version);

	free(buffer);

	hide_stolen_goods_from_cms(ctx->cms, ctx->backup_cms);
	cms_context_fini(ctx->cms);
}

static int
handle_event(context *ctx, struct pollfd *pollfd)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	pesignd_msghdr pm;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov.iov_base = &pm;
	iov.iov_len = sizeof(pm);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	char control[1024];
	msg.msg_controllen = 1024;
	msg.msg_control = control;

	n = recvmsg(pollfd->fd, &msg, MSG_WAITALL);
	if (n < 0) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_WARNING,
			"recvmsg failed: %m");
		return n;
	}

	/* if recvmsg returned 0, we're not going to get any valid data. */
	/* This *probably* means we were hung up on. */
	if (n == 0)
		return n;

	if (n < (long long)sizeof (pm)) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"got message with invalid size %zu", n);
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"possible exploit attempt.  closing.");
		close(pollfd->fd);
		return -1;
	}

	if (pm.version != PESIGND_VERSION) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"got version %d, expected version %d",
			pm.version, PESIGND_VERSION);
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"possible exploit attempt.  closing.");
		close(pollfd->fd);
		return -1;
	}

	for (int i = 0; cmd_table[i].cmd != CMD_LIST_END; i++) {
		if (cmd_table[i].cmd == pm.command) {
			if (cmd_table[i].func == NULL) {
				handle_invalid_input(pm.command, ctx, pollfd,
							pm.size);
				close(pollfd->fd);
			}
			cmd_table[i].func(ctx, pollfd, pm.size);
			return 0;
		}
	}

	handle_invalid_input(pm.command, ctx, pollfd, pm.size);
	close(pollfd->fd);
	return 0;
}

static void
do_shutdown(context *ctx, int nsockets, struct pollfd *pollfds)
{
	unlink(SOCKPATH);
	unlink(PIDFILE);

	for (int i = 0; i < ctx->ntokennames; i++)
		free(ctx->tokennames[i]);
	if (ctx->tokennames)
		free(ctx->tokennames);
	ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_NOTICE,
			"pesignd exiting (pid %d)", getpid());

	xfree(ctx->errstr);

	for (int i = 0; i < nsockets; i++)
		close(pollfds[i].fd);
	free(pollfds);
}

static int
handle_events(context *ctx)
{
	int rc;
	int nsockets = 1;

	struct pollfd *pollfds = calloc(1, sizeof(struct pollfd));

	if (!pollfds) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"could not allocate memory: %m");
		exit(1);
	}

	pollfds[0].fd = ctx->sd;
	pollfds[0].events = POLLIN|POLLPRI|POLLHUP;

	while (1) {
		if (should_exit != 0) {
shutdown:
			do_shutdown(ctx, nsockets, pollfds);
			return 0;
		}
		rc = ppoll(pollfds, nsockets, NULL, NULL);
		if (should_exit != 0)
			goto shutdown;
		if (rc < 0) {
			ctx->backup_cms->log(ctx->backup_cms,
				ctx->priority|LOG_WARNING,
				"ppoll: %m");
			continue;
		}

		if (pollfds[0].revents & POLLIN) {
			nsockets++;
			struct pollfd *newpollfds = realloc(pollfds,
				nsockets * sizeof(struct pollfd));

			if (!newpollfds) {
				ctx->backup_cms->log(ctx->backup_cms,
					ctx->priority|LOG_ERR,
					"could not allocate memory: %m");
				exit(1);
			}
			pollfds = newpollfds;

			struct sockaddr_un remote;
			socklen_t len = sizeof(remote);
			pollfds[nsockets-1].fd = accept(pollfds[0].fd, &remote,
							&len);
			pollfds[nsockets-1].events = POLLIN|POLLPRI|POLLHUP;
			pollfds[nsockets-1].revents = pollfds[0].revents;
		}
		for (int i = 1; i < nsockets; i++) {
			if (pollfds[i].revents & (POLLHUP|POLLNVAL)) {
				close(pollfds[i].fd);
				if (i == nsockets-1) {
					nsockets--;
					continue;
				}
				for (int j = i; j < nsockets - 1; j++) {
					pollfds[j].fd = pollfds[j+1].fd;
					pollfds[j].events =
						pollfds[j].events;
					pollfds[j].revents =
						pollfds[j].revents;
				}
				nsockets--;
				i--;
				continue;
			}

			if (pollfds[i].revents & (POLLIN|POLLPRI))
				handle_event(ctx, &pollfds[i]);
		}
	}
	return 0;
}

static int
get_uid_and_gid(context *ctx, char **homedir)
{
	struct passwd *passwd;

	passwd = getpwnam("pesign");

	if (!passwd)
		return -1;

	ctx->uid = passwd->pw_uid;
	ctx->gid = passwd->pw_gid;

	if (ctx->uid == 0 || ctx->gid == 0) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"cowardly refusing to start with uid = %d and gid = %d",
			ctx->uid, ctx->gid);
		errno = EINVAL;
		return -1;
	}

	*homedir = passwd->pw_dir;

	return 0;
}

static void
quit_handler(int signal UNUSED)
{
	should_exit = 1;
}

static int
set_up_socket(context *ctx)
{
	int sd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sd < 0) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"unable to create socket: %m");
		exit(1);
	}

	struct sockaddr_un addr_un = {
		.sun_family = AF_UNIX,
		.sun_path = SOCKPATH,
	};

	int rc = bind(sd, &addr_un, sizeof(addr_un));
	if (rc < 0) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"unable to bind to \"%s\": %m",
			addr_un.sun_path);
		exit(1);
	}
	rc = chmod(SOCKPATH, 0660);
	if (rc < 0) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"could not set permissions for \"%s\": %m",
			SOCKPATH);
		exit(1);
	}

	rc = listen(sd, 5);
	if (rc < 0) {
		ctx->backup_cms->log(ctx->backup_cms, ctx->priority|LOG_ERR,
			"unable to listen on socket: %m");
		exit(1);
	}


	ctx->sd = sd;
	return 0;
}

static void
check_socket(context *ctx UNUSED)
{
	errno = 0;
	int rc = access(SOCKPATH, R_OK);
	if (rc == 0) {
		struct sockaddr_un addr_un = {
			.sun_family = AF_UNIX,
			.sun_path = SOCKPATH,
		};

		int sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		if (sd < 0) {
			fprintf(stderr, "unable to create socket: %m");
			exit(1);
		}

		socklen_t len = strlen(addr_un.sun_path) +
				sizeof(addr_un.sun_family);

		rc = connect(sd, (struct sockaddr *)&addr_un, len);
		if (rc < 0) {
			close(sd);
			unlink(SOCKPATH);
			return;
		}

		struct sockaddr_un remote;
		socklen_t size = sizeof(remote);
		rc = getpeername(sd, &remote, &size);
		if (rc < 0) {
			close(sd);
			return;
		} else {
			fprintf(stderr, "already running");
			exit(1);
		}
	} else {
		/* It could be something other than EEXIST, but it really
		 * doesn't matter since the daemon isn't running.  Blindly
		 * remove it. */
		unlink(SOCKPATH);
	}
}

static int PRINTF(3, 4)
daemon_logger(cms_context *cms, int priority, char *fmt, ...)
{
	context *ctx = (context *)cms->log_priv;
	va_list ap;
	int rc = 0;

	if (ctx->errstr)
		xfree(ctx->errstr);

	va_start(ap, fmt);
	if (priority & LOG_ERR) {
		va_list aq;

		va_copy(aq, ap);
		rc = vasprintf(&ctx->errstr, fmt, aq);
		va_end(aq);
	}

	vsyslog(ctx->priority | priority, fmt, ap);
	va_end(ap);
	return rc;
}

static void
write_pid_file(int pid)
{
	int fd = open(PIDFILE, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
err:
		fprintf(stderr, "couldn't open pidfile: %m\n");
		exit(1);
	}
	char *pidstr = NULL;
	int rc = asprintf(&pidstr, "%d\n", pid);
	if (rc < 0)
		goto err;

	rc = write(fd, pidstr, strlen(pidstr)+1);
	if (rc < 0)
		goto err;

	free(pidstr);
	close(fd);
}

int
daemonize(cms_context *cms_ctx, char *certdir, int do_fork)
{
	int rc = 0;
	context ctx = {
		.backup_cms = cms_ctx,
		.priority = do_fork ? LOG_PID
				    : LOG_PID|LOG_PERROR,
	};

	ctx.backup_cms = cms_ctx;
	ctx.backup_cms->log_priv = &ctx;
	ctx.sd = -1;

	if (getuid() != 0) {
		fprintf(stderr, "pesignd must be started as root");
		exit(1);
	}

	check_socket(&ctx);

	openlog("pesignd", LOG_PID, LOG_DAEMON);

	if (do_fork) {
		pid_t pid;

		if ((pid = fork())) {
			sleep(2);
			return 0;
		}
	}
	ctx.pid = getpid();
	write_pid_file(ctx.pid);
	ctx.backup_cms->log(ctx.backup_cms, ctx.priority|LOG_NOTICE,
		"pesignd starting (pid %d)", ctx.pid);
	daemon_logger(ctx.backup_cms, ctx.priority|LOG_NOTICE,
		"pesignd starting (pid %d)", ctx.pid);

	SECStatus status = NSS_Init(certdir);
	int error = errno;
	if (status != SECSuccess) {
		char *globpattern = NULL;
		rc = asprintf(&globpattern, "%s/cert*.db",
			      certdir);
		if (rc > 0) {
			glob_t globbuf;
			memset(&globbuf, 0, sizeof(globbuf));
			rc = glob(globpattern, GLOB_ERR, NULL,
				  &globbuf);
			if (rc != 0) {
				errno = error;
				ctx.backup_cms->log(ctx.backup_cms,
					ctx.priority|LOG_NOTICE,
					"Could not open NSS database (\"%s\"): %m",
					PORT_ErrorToString(PORT_GetError()));
				exit(1);
			}
		}
	}
	if (status != SECSuccess) {
		errno = error;
		ctx.backup_cms->log(ctx.backup_cms, ctx.priority|LOG_NOTICE,
				    "Could not initialize nss.\n"
				    "NSS says \"%s\" errno says \"%m\"\n",
				    PORT_ErrorToString(PORT_GetError()));
		exit(1);
	}

	status = register_oids(ctx.backup_cms);
	if (status != SECSuccess) {
		ctx.backup_cms->log(ctx.backup_cms, ctx.priority|LOG_NOTICE,
			"Could not register OIDs\n");
		exit(1);
	}

	if (do_fork) {
		int fd = open("/dev/zero", O_RDONLY);
		if (fd < 0) {
			ctx.backup_cms->log(ctx.backup_cms,
					ctx.priority|LOG_ERR,
					"could not open /dev/zero: %m");
			exit(1);
		}
		close(STDIN_FILENO);
		rc = dup2(fd, STDIN_FILENO);
		if (rc < 0) {
			ctx.backup_cms->log(ctx.backup_cms,
				ctx.priority|LOG_ERR,
				"could not set up standard input: %m");
			exit(1);
		}
		close(fd);

		fd = open("/dev/null", O_WRONLY);
		if (fd < 0) {
			ctx.backup_cms->log(ctx.backup_cms,
					ctx.priority|LOG_ERR,
					"could not open /dev/null: %m");
			exit(1);
		}
		close(STDOUT_FILENO);
		rc = dup2(fd, STDOUT_FILENO);
		if (rc < 0) {
			ctx.backup_cms->log(ctx.backup_cms,
				ctx.priority|LOG_ERR,
				"could not set up standard output: %m");
			exit(1);
		}

		close(STDERR_FILENO);
		rc = dup2(fd, STDERR_FILENO);
		if (rc < 0) {
			ctx.backup_cms->log(ctx.backup_cms,
				ctx.priority|LOG_ERR,
				"could not set up standard error: %m");
			exit(1);
		}
		close(fd);
	}

	prctl(PR_SET_NAME, "pesignd", 0, 0, 0);

	setsid();

	if (do_fork) {
		struct sigaction sa = {
			.sa_handler = quit_handler,
		};
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}

	char *homedir = NULL;

	rc = get_uid_and_gid(&ctx, &homedir);
	if (rc < 0) {
		ctx.backup_cms->log(ctx.backup_cms, ctx.priority|LOG_ERR,
			"could not get group and user information "
			"for pesign: %m");
		exit(1);
	}

	chdir(homedir ? homedir : "/");

	if (getuid() == 0) {
		/* process is running as root, drop privileges */
		if (setgid(ctx.gid) != 0 || setgroups(0, NULL)) {
			ctx.backup_cms->log(ctx.backup_cms,
				ctx.priority|LOG_ERR,
				"unable to drop group privileges: %m");
			exit(1);
		}
		if (setuid(ctx.uid) != 0) {
			ctx.backup_cms->log(ctx.backup_cms,
				ctx.priority|LOG_ERR,
				"unable to drop user privileges: %m");
			exit(1);
		}
	}

	set_up_socket(&ctx);

	cms_set_pw_callback(ctx.backup_cms, get_password_fail);
	cms_set_pw_data(ctx.backup_cms, NULL);
	if (do_fork)
		ctx.backup_cms->log = daemon_logger;

	rc = handle_events(&ctx);

	status = NSS_Shutdown();
	if (status != SECSuccess) {
		ctx.backup_cms->log(ctx.backup_cms, ctx.priority|LOG_ERR,
			"NSS_Shutdown failed: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		exit(1);
	}
	return rc;
}
