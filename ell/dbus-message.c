/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2011-2014  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License version 2.1 as published by the Free Software Foundation.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

#include "dbus.h"

#include "util.h"
#include "private.h"
#include "dbus-private.h"

#define DBUS_MESSAGE_LITTLE_ENDIAN	('l')
#define DBUS_MESSAGE_BIG_ENDIAN		('B')

#define DBUS_MESSAGE_PROTOCOL_VERSION	1

#define DBUS_MESSAGE_FLAG_NO_REPLY_EXPECTED	0x01
#define DBUS_MESSAGE_FLAG_NO_AUTO_START		0x02

#define DBUS_MESSAGE_FIELD_PATH		1
#define DBUS_MESSAGE_FIELD_INTERFACE	2
#define DBUS_MESSAGE_FIELD_MEMBER	3
#define DBUS_MESSAGE_FIELD_ERROR_NAME	4
#define DBUS_MESSAGE_FIELD_REPLY_SERIAL	5
#define DBUS_MESSAGE_FIELD_DESTINATION	6
#define DBUS_MESSAGE_FIELD_SENDER	7
#define DBUS_MESSAGE_FIELD_SIGNATURE	8
#define DBUS_MESSAGE_FIELD_UNIX_FDS	9

struct l_dbus_message {
	int refcount;
	void *header;
	size_t header_size;
	const char *signature;
	void *body;
	size_t body_size;
	int fds[16];
	uint32_t num_fds;
};

static inline bool _dbus_message_is_gvariant(struct l_dbus_message *msg)
{
	struct dbus_header *hdr = msg->header;

	return hdr->version == 2;
}

void *_dbus_message_get_header(struct l_dbus_message *msg, size_t *out_size)
{
	if (out_size)
		*out_size = msg->header_size;

	return msg->header;
}

void *_dbus_message_get_body(struct l_dbus_message *msg, size_t *out_size)
{
	if (out_size)
		*out_size = msg->body_size;

	return msg->body;
}

void _dbus_message_set_serial(struct l_dbus_message *msg, uint32_t serial)
{
	struct dbus_header *hdr = msg->header;

	hdr->serial = serial;
}

static unsigned int encode_header(unsigned char field, char type,
			const char *value, uint32_t len, void *data)
{
	unsigned char *ptr = data;

	ptr[0] = field;
	ptr[1] = 0x01;
	ptr[2] = (unsigned char) type;
	ptr[3] = 0x00;
	ptr[4] = (unsigned char) len;

	if (type == 's' || type == 'o') {
		ptr[5] = 0x00;
		ptr[6] = 0x00;
		ptr[7] = 0x00;

		strcpy(data + 8, value);

		return 4 + 4 + len + 1;
	} else {
		strcpy(data + 5, value);

		return 4 + 1 + len + 1;
	}
}

LIB_EXPORT struct l_dbus_message *l_dbus_message_new_method_call(const char *destination,
                const char *path, const char *interface, const char *method)
{
	struct l_dbus_message *message;
	struct dbus_header *hdr;
	unsigned int len;
	uint32_t dlen, plen, ilen, mlen;
	uint32_t size;

	message = l_new(struct l_dbus_message, 1);

	message->refcount = 1;

	dlen = strlen(destination);
	plen = strlen(path);
	ilen = strlen(interface);
	mlen = strlen(method);

	size = DBUS_HEADER_SIZE +
			align_len(dlen + 9, 8) + align_len(plen + 9, 8) +
			align_len(ilen + 9, 8) + align_len(mlen + 9, 8);

	message->header = l_malloc(size);

	memset(message->header, 0, size);

	hdr = message->header;

	hdr->endian = DBUS_MESSAGE_LITTLE_ENDIAN;
	hdr->message_type = DBUS_MESSAGE_TYPE_METHOD_CALL;
	hdr->flags = DBUS_MESSAGE_FLAG_NO_AUTO_START;
	hdr->version = DBUS_MESSAGE_PROTOCOL_VERSION;
	hdr->body_length = 0;
	hdr->serial = 0x00;

	len = DBUS_HEADER_SIZE;

	len += encode_header(6, 's', destination, dlen, message->header + len);
	len = align_len(len, 8);
	len += encode_header(1, 'o', path, plen, message->header + len);
	len = align_len(len, 8);
	len += encode_header(2, 's', interface, ilen, message->header + len);
	len = align_len(len, 8);
	len += encode_header(3, 's', method, mlen, message->header + len);

	message->header_size = size;

	hdr->field_length = len - DBUS_HEADER_SIZE;

	return message;
}

LIB_EXPORT struct l_dbus_message *l_dbus_message_ref(struct l_dbus_message *message)
{
	if (unlikely(!message))
		return NULL;

	__sync_fetch_and_add(&message->refcount, 1);

	return message;
}

LIB_EXPORT void l_dbus_message_unref(struct l_dbus_message *message)
{
	unsigned int i;

	if (unlikely(!message))
		return;

	if (__sync_sub_and_fetch(&message->refcount, 1))
		return;

	for (i = 0; i < message->num_fds; i++)
		close(message->fds[i]);

	l_free(message->header);
	l_free(message->body);
	l_free(message);
}

#define put_u8(ptr,val)		(*((uint8_t *) (ptr)) = (val))
#define put_u16(ptr,val)	(*((uint16_t *) (ptr)) = (val))
#define put_u32(ptr, val)	(*((uint32_t *) (ptr)) = (val))
#define put_u64(ptr, val)	(*((uint64_t *) (ptr)) = (val))
#define put_s16(ptr, val)	(*((int16_t *) (ptr)) = (val))
#define put_s32(ptr, val)	(*((int32_t *) (ptr)) = (val))
#define put_s64(ptr, val)	(*((int64_t *) (ptr)) = (val))

static bool dbus1_message_iter_next_entry_valist(struct dbus1_iter *orig,
							va_list args)
{
	static const char *simple_types = "sogybnqiuxtd";
	struct dbus1_iter *iter = orig;
	const char *signature = iter->sig_start + iter->sig_pos;
	const char *end;
	struct dbus1_iter *sub_iter;
	struct dbus1_iter stack[32];
	unsigned int indent = 0;
	uint32_t uint32_val;
	int fd;
	void *arg;
	size_t pos;

	while (iter->sig_pos < iter->sig_len &&
			signature < iter->sig_start + iter->sig_len) {
		if (strchr(simple_types, *signature)) {
			arg = va_arg(args, void *);
			if (!_dbus1_iter_next_entry_basic(iter,
							*signature, arg))
				return false;

			signature += 1;
			continue;
		}

		switch (*signature) {
		case 'h':
			if (!_dbus1_iter_next_entry_basic(iter, 'h',
								&uint32_val))
				return false;

			if (uint32_val < iter->message->num_fds)
				fd = fcntl(iter->message->fds[uint32_val],
						F_DUPFD_CLOEXEC, 3);
			else
				fd = -1;

			*va_arg(args, int *) = fd;
			signature += 1;
			break;
		case '(':
		case '{':
			signature += 1;
			indent += 1;

			if (!_dbus1_iter_enter_struct(iter, &stack[indent - 1]))
				return false;

			iter = &stack[indent - 1];

			break;
		case ')':
		case '}':
			signature += 1;
			indent -= 1;

			if (indent == 0)
				iter = orig;
			else
				iter = &stack[indent - 1];
			break;
		case 'a':
			sub_iter = va_arg(args, void *);

			if (!_dbus1_iter_enter_array(iter, sub_iter))
				return false;

			end = _dbus_signature_end(signature + 1);
			signature = end + 1;
			break;
		case 'v':
			sub_iter = va_arg(args, void *);

			if (!_dbus1_iter_enter_variant(iter, sub_iter))
				return false;

			signature += 1;
			break;
		default:
			return false;
		}
	}

	return true;
}

static inline bool dbus1_iter_next_entry(struct dbus1_iter *iter, ...)
{
	va_list args;
	bool result;

        va_start(args, iter);
	result = dbus1_message_iter_next_entry_valist(iter, args);
	va_end(args);

	return result;
}

static bool get_header_field_from_iter_valist(struct l_dbus_message *message,
						uint8_t type, va_list args)
{
	struct dbus1_iter header;
	struct dbus1_iter array, iter;
	uint8_t endian, message_type, flags, version, field_type;
	uint32_t body_length, serial;

	_dbus1_iter_init(&header, message, "yyyyuua(yv)", NULL,
				message->header, message->header_size);

	if (!dbus1_iter_next_entry(&header, &endian,
					&message_type, &flags, &version,
					&body_length, &serial, &array))
		return false;

	while (dbus1_iter_next_entry(&array, &field_type, &iter)) {
		if (field_type != type)
			continue;

		return dbus1_message_iter_next_entry_valist(&iter, args);
	}

	return false;
}

static inline bool get_header_field(struct l_dbus_message *message,
                                                uint8_t type, ...)
{
	va_list args;
	bool result;

	va_start(args, type);
	result = get_header_field_from_iter_valist(message, type, args);
	va_end(args);

	return result;
}

struct l_dbus_message *dbus_message_from_blob(const void *data, size_t size)
{
	const struct dbus_header *hdr = data;
	struct l_dbus_message *message;

	if (unlikely(size < DBUS_HEADER_SIZE))
		return NULL;

	message = l_new(struct l_dbus_message, 1);

	message->refcount = 1;

	message->header_size = align_len(DBUS_HEADER_SIZE +
						hdr->field_length, 8);
	message->header = l_malloc(message->header_size);

	message->body_size = hdr->body_length;
	message->body = l_malloc(message->body_size);

	memcpy(message->header, data, message->header_size);
	memcpy(message->body, data + message->header_size, message->body_size);

	get_header_field(message, DBUS_MESSAGE_FIELD_SIGNATURE,
						&message->signature);

	return message;
}

struct l_dbus_message *dbus_message_build(void *header, size_t header_size,
						void *body, size_t body_size,
						int fds[], uint32_t num_fds)
{
	struct l_dbus_message *message;

	message = l_new(struct l_dbus_message, 1);

	message->refcount = 1;
	message->header_size = header_size;
	message->header = header;
	message->body_size = body_size;
	message->body = body;

	message->num_fds = num_fds;
	memcpy(message->fds, fds, num_fds * sizeof(int));

	get_header_field(message, DBUS_MESSAGE_FIELD_SIGNATURE,
						&message->signature);

	return message;
}

bool dbus_message_compare(struct l_dbus_message *message,
					const void *data, size_t size)
{
	struct l_dbus_message *other;

	other = dbus_message_from_blob(data, size);

	if (message->signature) {
		if (!other->signature)
			return false;

		if (strcmp(message->signature, other->signature))
			return false;
	} else {
		if (other->signature)
			return false;
	}

	if (message->body_size != other->body_size)
		return false;

	if (message->header_size != other->header_size)
		return false;

	return !memcmp(message->body, other->body, message->body_size);
}

static inline size_t body_realloc(struct l_dbus_message *message,
					size_t len, unsigned int boundary)
{
	size_t size = align_len(message->body_size, boundary);

	if (size + len > message->body_size) {
		message->body = l_realloc(message->body, size + len);

		if (size - message->body_size > 0)
			memset(message->body + message->body_size, 0,
						size - message->body_size);

		message->body_size = size + len;
	}

	return size;
}

static bool append_arguments(struct l_dbus_message *message,
					const char *signature, va_list args)
{
	struct dbus_header *hdr;
	uint32_t size, slen;
	size_t len, pos;

	slen = strlen(signature);

	size = message->header_size + align_len(slen + 6, 8);

	message->header = l_realloc(message->header, size);

	hdr = message->header;

	pos = DBUS_HEADER_SIZE + align_len(hdr->field_length, 8);
	len = pos + encode_header(8, 'g', signature, slen,
						message->header + pos);

	message->signature = message->header + pos + 5;

	hdr->field_length = len - DBUS_HEADER_SIZE;

	message->header_size = size;

	while (*signature) {
		const char *str;
		uint8_t uint8_val;
		uint16_t uint16_val;
		uint32_t uint32_val;
		uint64_t uint64_val;
		int16_t int16_val;
		int32_t int32_val;
		int64_t int64_val;
		double double_val;

		switch (*signature++) {
		case 'o':
		case 's':
			str = *va_arg(args, const char **);
			len = strlen(str);
			pos = body_realloc(message, len + 5, 4);
			put_u32(message->body + pos, len);
			strcpy(message->body + pos + 4, str);
			break;
		case 'g':
			str = *va_arg(args, const char **);
			len = strlen(str);
			pos = body_realloc(message, len + 2, 1);
			put_u8(message->body + pos, len);
			strcpy(message->body + pos + 1, str);
			break;
		case 'b':
			uint32_val = *va_arg(args, bool *);
			pos = body_realloc(message, 4, 4);
			put_u32(message->body + pos, uint32_val);
			break;
		case 'y':
			uint8_val = *va_arg(args, uint8_t *);
			pos = body_realloc(message, 1, 1);
			put_u8(message->body + pos, uint8_val);
			break;
		case 'n':
			int16_val = *va_arg(args, int16_t *);
			pos = body_realloc(message, 2, 2);
			put_s16(message->body + pos, int16_val);
			break;
		case 'q':
			uint16_val = *va_arg(args, uint16_t *);
			pos = body_realloc(message, 2, 2);
			put_u16(message->body + pos, uint16_val);
			break;
		case 'i':
			int32_val = *va_arg(args, int32_t *);
			pos = body_realloc(message, 4, 4);
			put_s32(message->body + pos, int32_val);
			break;
		case 'u':
			uint32_val = *va_arg(args, uint32_t *);
			pos = body_realloc(message, 4, 4);
			put_u32(message->body + pos, uint32_val);
			break;
		case 'x':
			int64_val = *va_arg(args, int64_t *);
			pos = body_realloc(message, 8, 8);
			put_s64(message->body + pos, int64_val);
			break;
		case 't':
			uint64_val = *va_arg(args, uint64_t *);
			pos = body_realloc(message, 8, 8);
			put_u64(message->body + pos, uint64_val);
			break;
		case 'd':
			double_val = *va_arg(args, double *);
			pos = body_realloc(message, 8, 8);
			*((double *) (message->body + pos)) = double_val;
			break;
		case '(':
		case '{':
			pos = body_realloc(message, 0, 8);
			break;
		case ')':
		case '}':
			break;
		default:
			return false;
		}
	}

	hdr->body_length = message->body_size;

	return true;
}

LIB_EXPORT bool l_dbus_message_get_error(struct l_dbus_message *message,
					const char **name, const char **text)
{
	struct dbus1_iter iter;
	struct dbus_header *hdr;
	const char *str;

	if (unlikely(!message))
		return false;

	hdr = message->header;

	if (hdr->message_type != DBUS_MESSAGE_TYPE_ERROR)
		return false;

	if (!message->signature)
		return false;

	if (strcmp(message->signature, "s"))
		return false;

	_dbus1_iter_init(&iter, message, message->signature, NULL,
				message->body, message->body_size);

	if (!dbus1_iter_next_entry(&iter, &str))
		return false;

	if (name)
		get_header_field(message, DBUS_MESSAGE_FIELD_ERROR_NAME, name);

	if (text)
		*text = str;

	return true;
}

LIB_EXPORT bool l_dbus_message_get_arguments(struct l_dbus_message *message,
						const char *signature, ...)
{
	struct dbus1_iter iter;
	va_list args;
	bool result;

	if (unlikely(!message))
		return false;

	if (!message->signature) {
		/* An empty signature is valid */
		if (!signature || *signature == '\0')
			return true;

		return false;
	}

	if (!signature || strcmp(message->signature, signature))
		return false;

	_dbus1_iter_init(&iter, message, message->signature, NULL,
				message->body, message->body_size);

	va_start(args, signature);
	result = dbus1_message_iter_next_entry_valist(&iter, args);
	va_end(args);

	return result;
}

LIB_EXPORT bool l_dbus_message_set_arguments(struct l_dbus_message *message,
						const char *signature, ...)
{
	va_list args;
	bool result;

	if (unlikely(!message))
		return false;

	if (!signature)
		return true;

	va_start(args, signature);
	result = append_arguments(message, signature, args);
	va_end(args);

	return result;
}

LIB_EXPORT const char *l_dbus_message_get_path(struct l_dbus_message *message)
{
	const char *path;

	if (unlikely(!message))
		return NULL;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_PATH, &path))
		return path;

	return NULL;
}

LIB_EXPORT const char *l_dbus_message_get_interface(struct l_dbus_message *message)
{
	const char *interface;

	if (unlikely(!message))
		return NULL;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_INTERFACE, &interface))
		return interface;

	return NULL;
}

LIB_EXPORT const char *l_dbus_message_get_member(struct l_dbus_message *message)
{
	const char *member;

	if (unlikely(!message))
		return NULL;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_MEMBER, &member))
		return member;

	return NULL;
}

LIB_EXPORT const char *l_dbus_message_get_destination(struct l_dbus_message *message)
{
	const char *destination;

	if (unlikely(!message))
		return NULL;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_DESTINATION,
							&destination))
		return destination;

	return NULL;
}

LIB_EXPORT const char *l_dbus_message_get_sender(struct l_dbus_message *message)
{
	const char *sender;

	if (unlikely(!message))
		return NULL;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_SENDER, &sender))
		return sender;

	return NULL;
}

LIB_EXPORT const char *l_dbus_message_get_signature(
						struct l_dbus_message *message)
{
	const char *signature;

	if (unlikely(!message))
		return NULL;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_SIGNATURE, &signature))
		return signature;

	return NULL;
}

uint32_t _dbus_message_get_reply_serial(struct l_dbus_message *message)
{
	uint32_t serial;

	if (get_header_field(message, DBUS_MESSAGE_FIELD_REPLY_SERIAL, &serial))
		return serial;

	return 0;
}

enum dbus_message_type _dbus_message_get_type(struct l_dbus_message *message)
{
	struct dbus_header *header;

	header = message->header;
	return header->message_type;
}

LIB_EXPORT char l_dbus_message_iter_get_type(struct l_dbus_message_iter *iter)
{
	struct dbus1_iter *real_iter;

	if (unlikely(!iter))
		return '\0';

	real_iter = (struct dbus1_iter *) iter;

	if (!real_iter->sig_start)
		return '\0';

	return real_iter->sig_start[real_iter->sig_pos];
}

LIB_EXPORT bool l_dbus_message_iter_is_valid(struct l_dbus_message_iter *iter)
{
	struct dbus1_iter *real_iter;

	if (unlikely(!iter))
		return false;

	real_iter = (struct dbus1_iter *) iter;

	if (!real_iter->sig_start)
		return false;

	if (real_iter->sig_pos >= real_iter->sig_len)
		return false;

	return true;
}

LIB_EXPORT bool l_dbus_message_iter_next_entry(struct l_dbus_message_iter *iter,
									...)
{
	struct dbus1_iter *real_iter;
	va_list args;
	bool result;

	if (unlikely(!iter))
		return false;

	real_iter = (struct dbus1_iter *) iter;

	va_start(args, iter);
	result = dbus1_message_iter_next_entry_valist(real_iter, args);
	va_end(args);

	return result;
}

LIB_EXPORT bool l_dbus_message_iter_get_variant(struct l_dbus_message_iter *iter,
						const char *signature, ...)
{
	struct dbus1_iter *real_iter;
	va_list args;
	bool result;

	if (unlikely(!iter))
		return false;

	real_iter = (struct dbus1_iter *) iter;

	if (!real_iter->sig_start || strcmp(real_iter->sig_start, signature))
		return false;

	va_start(args, signature);
	result = dbus1_message_iter_next_entry_valist(real_iter, args);
	va_end(args);

	return result;
}
