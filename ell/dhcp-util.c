/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2020  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#include <stdint.h>
#include <linux/types.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <arpa/inet.h>

#include "dhcp-private.h"
#include "private.h"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)

static void dhcp_init_header(struct dhcp_message *message, uint8_t type)
{
	switch (type) {
	case DHCP_MESSAGE_TYPE_OFFER:
	case DHCP_MESSAGE_TYPE_NAK:
	case DHCP_MESSAGE_TYPE_ACK:
		message->op = DHCP_OP_CODE_BOOTREPLY;
		break;
	default:
		message->op = DHCP_OP_CODE_BOOTREQUEST;
		break;
	}

	message->htype = 1;
	message->hlen = 6;
	message->magic = htonl(DHCP_MAGIC);
	message->options[0] = DHCP_OPTION_END;
}

#define LEN_CHECK(builder, next) \
	if ((builder)->pos - (builder)->start + (next) > (builder)->max) \
		return false;

bool _dhcp_message_builder_append(struct dhcp_message_builder *builder,
					uint8_t code, size_t optlen,
					const void *optval)
{
	switch (code) {
	case DHCP_OPTION_PAD:
	case DHCP_OPTION_END:
		LEN_CHECK(builder, 1);

		builder->pos[0] = code;
		builder->pos += 1;
		break;
	default:
		LEN_CHECK(builder, optlen + 2);

		builder->pos[0] = code;
		builder->pos[1] = optlen;
		memcpy(builder->pos + 2, optval, optlen);

		builder->pos += optlen + 2;
		break;
	}

	return true;
}

bool _dhcp_message_builder_append_prl(struct dhcp_message_builder *builder,
					const unsigned long *reqopts)
{
	uint8_t optlen = 0;
	unsigned int i;
	unsigned int j;

	for (i = 0; i < 256 / BITS_PER_LONG; i++)
		optlen += __builtin_popcountl(reqopts[i]);

	/*
	 * This function assumes that there's enough space to put the PRL
	 * into the buffer without resorting to file or sname overloading
	 */
	LEN_CHECK(builder, optlen + 2U);

	i = 0;
	builder->pos[i++] = DHCP_OPTION_PARAMETER_REQUEST_LIST;
	builder->pos[i++] = optlen;

	for (j = 0; j < 256; j++) {
		if (reqopts[j / BITS_PER_LONG] & 1UL << (j % BITS_PER_LONG)) {
			builder->pos[i++] = j;
		}
	}

	builder->pos += optlen + 2;

	return true;
}

bool _dhcp_message_builder_init(struct dhcp_message_builder *builder,
				struct dhcp_message *message,
				size_t len, uint8_t type)
{
	if (!builder || !message || !len)
		return false;

	builder->max = len;
	builder->pos = (uint8_t *) message->options;
	builder->start = (uint8_t *) message;

	dhcp_init_header(message, type);

	return _dhcp_message_builder_append(builder, DHCP_OPTION_MESSAGE_TYPE,
						1, &type);
}

static inline int dhcp_message_optimize(struct dhcp_message *message,
					const uint8_t *end)
{
	/*
	 * Don't bother sending a full sized dhcp_message as it is most likely
	 * mostly zeros.  Instead truncate it at DHCP_OPTION_END and align to
	 * the nearest 4 byte boundary.  Many implementations expect a packet
	 * of a certain size or it is filtered, so we cap the length in
	 * accordance to RFC 1542:
	 * "The IP Total Length and UDP Length must be large enough to contain
	 * the minimal BOOTP header of 300 octets"
	 */
	size_t len = align_len(end - (uint8_t *) message, 4);
	if (len < 300)
		len = 300;

	return len;
}

uint8_t *_dhcp_message_builder_finalize(struct dhcp_message_builder *builder,
					size_t *outlen)
{
	_dhcp_message_builder_append(builder, DHCP_OPTION_END, 0, NULL);

	*outlen = dhcp_message_optimize((struct dhcp_message *)builder->start,
				builder->pos);

	return builder->start;
}
