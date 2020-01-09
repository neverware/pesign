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
#ifndef DAEMON_H
#define DAEMON_H 1

extern int daemonize(cms_context *ctx, char *certdir, int do_fork);

typedef struct {
	uint32_t version;
	uint32_t command;
	uint32_t size;
} pesignd_msghdr;

typedef struct  {
	int32_t rc;
	uint8_t errmsg[];
} pesignd_cmd_response;

typedef struct {
	uint32_t size;
	uint8_t value[];
} pesignd_string;

typedef enum {
	CMD_KILL_DAEMON,
	CMD_UNLOCK_TOKEN,
	CMD_SIGN_ATTACHED,
	CMD_SIGN_DETACHED,
	CMD_RESPONSE,
	CMD_IS_TOKEN_UNLOCKED,
	CMD_GET_CMD_VERSION,
	CMD_LIST_END
} pesignd_cmd;

#define PESIGND_VERSION 0x2a9edaf0
#define SOCKPATH	"/var/run/pesign/socket"
#define PIDFILE		"/var/run/pesign.pid"

static inline uint32_t UNUSED
pesignd_string_size(char *buffer)
{
	pesignd_string *s;
	return sizeof(s->size) + (buffer ? strlen(buffer) : 0) + 1;
}

static inline void UNUSED
pesignd_string_set(pesignd_string *str, char *value)
{
	str->size = (value ? strlen(value) : 0) + 1;
	if (value)
		strcpy((char *)str->value, value);
	else
		str->value[0] = '\0';
}

static inline pesignd_string * UNUSED
pesignd_string_next(pesignd_string *str)
{
	char *buffer = (char *)str;
	buffer += sizeof(str->size) + str->size;
	return (pesignd_string *)buffer;
}

#endif /* DAEMON_H */
