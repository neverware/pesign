/*
 * Copyright 2011-2012 Red Hat, Inc.
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
#ifndef PESIGN_UTIL_H
#define PESIGN_UTIL_H 1

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <libdpe/pe.h>

#include "compiler.h"

#define xfree(x) ({if (x) { free(x); x = NULL; }})

#define save_errno(x)					\
	({						\
		typeof (errno) __saved_errno = errno;	\
		x;					\
		errno = __saved_errno;			\
	})
#define save_pe_errno(x)					\
	({							\
		typeof (errno) __saved_errno = pe_errno();	\
		x;						\
		__libpe_seterrno(__saved_errno);		\
	})

#define nsserr(rv, fmt, args...) ({					\
		errx((rv), "%s:%s:%d: " fmt ": %s",			\
			__FILE__, __func__, __LINE__, ##args,		\
			PORT_ErrorToString(PORT_GetError()));		\
	})
#define nssreterr(rv, fmt, args...) ({					\
		fprintf(stderr, "%s:%s:%d: " fmt ": %s\n",		\
			__FILE__, __func__, __LINE__, ##args,		\
			PORT_ErrorToString(PORT_GetError()));		\
		return rv;						\
	})
#define liberr(rv, fmt, args...) ({					\
		err((rv), "%s:%s:%d: " fmt,				\
			__FILE__, __func__, __LINE__, ##args);		\
	})
#define libreterr(rv, fmt, args...) ({					\
		fprintf(stderr, "%s:%s:%d: " fmt ": %m\n",		\
			__FILE__, __func__, __LINE__, ##args);		\
		return rv;						\
	})
#define peerr(rv, fmt, args...) ({					\
		errx((rv), "%s:%s:%d: " fmt ": %s",			\
			__FILE__, __func__, __LINE__, ##args,		\
			pe_errmsg(pe_errno()));				\
	})
#define pereterr(rv, fmt, args...) ({					\
		fprintf(stderr, "%s:%s:%d: " fmt ": %s\n",		\
			__FILE__, __func__, __LINE__, ##args,		\
			pe_errmsg(pe_errno()));				\
		return rv;						\
	})

static inline int UNUSED
read_file(int fd, char **bufp, size_t *lenptr) {
    int alloced = 0, size = 0, i = 0;
    char * buf = NULL;

    do {
	size += i;
	if ((size + 1024) > alloced) {
	    alloced += 4096;
	    buf = realloc(buf, alloced + 1);
	}
    } while ((i = read(fd, buf + size, 1024)) > 0);

    if (i < 0) {
        free(buf);
	return -1;
    }

    *bufp = buf;
    *lenptr = size;

    return 0;
}

static inline int UNUSED
write_file(int fd, const void *data, size_t len)
{
	int rc;
	size_t written = 0;

	while (written < len) {
		rc = write(fd, ((unsigned char *) data) + written,
			   len - written);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return rc;
		}
		written += rc;
	}

	return 0;
}

static int
compare_shdrs (const void *a, const void *b)
{
	const struct section_header *shdra = (const struct section_header *)a;
	const struct section_header *shdrb = (const struct section_header *)b;
	int rc;

	if (shdra->data_addr > shdrb->data_addr)
		return 1;
	if (shdrb->data_addr > shdra->data_addr)
		return -1;

	if (shdra->virtual_address > shdrb->virtual_address)
		return 1;
	if (shdrb->virtual_address > shdra->virtual_address)
		return -1;

	rc = strcmp(shdra->name, shdrb->name);
	if (rc != 0)
		return rc;

	if (shdra->virtual_size > shdrb->virtual_size)
		return 1;
	if (shdrb->virtual_size > shdra->virtual_size)
		return -1;

	if (shdra->raw_data_size > shdrb->raw_data_size)
		return 1;
	if (shdrb->raw_data_size > shdra->raw_data_size)
		return -1;

	return 0;
}

static void UNUSED
sort_shdrs (struct section_header *shdrs, size_t sections)
{
	qsort(shdrs, sections, sizeof(*shdrs), compare_shdrs);
}

static void UNUSED
free_poison(void  *addrv, ssize_t len)
{
	uint8_t *addr = addrv;
	char poison_pills[] = "\xa5\x5a";
	for (int x = 0; x < len; x++)
		addr[x] = poison_pills[x % 2];
}

static int UNUSED
content_is_empty(uint8_t *data, ssize_t len)
{
	if (len < 1)
		return 1;

	for (int i = 0; i < len; i++)
		if (data[i] != 0)
			return 0;
	return 1;
}

#endif /* PESIGN_UTIL_H */
