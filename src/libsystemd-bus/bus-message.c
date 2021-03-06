/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>

#include "util.h"
#include "utf8.h"
#include "strv.h"

#include "sd-bus.h"
#include "bus-message.h"
#include "bus-internal.h"
#include "bus-type.h"
#include "bus-signature.h"

static int message_parse_fields(sd_bus_message *m);
static int message_append_basic(sd_bus_message *m, char type, const void *p, const void **stored);

static void reset_containers(sd_bus_message *m) {
        unsigned i;

        assert(m);

        for (i = 0; i < m->n_containers; i++)
                free(m->containers[i].signature);

        free(m->containers);
        m->containers = NULL;

        m->n_containers = 0;
        m->root_container.index = 0;
}

static void message_free(sd_bus_message *m) {
        unsigned i;

        assert(m);

        if (m->free_header)
                free(m->header);

        if (m->free_fields)
                free(m->fields);

        if (m->free_body)
                free(m->body);

        for (i = 0; i < m->n_fds; i++)
                close_nointr_nofail(m->fds[i]);

        reset_containers(m);
        free(m->root_container.signature);

        free(m->peeked_signature);
        free(m);
}

static void* buffer_extend(void **p, uint32_t *sz, size_t align, size_t extend) {
        size_t start, n;
        void *k;

        assert(p);
        assert(sz);
        assert(align > 0);

        start = ALIGN_TO((size_t) *sz, align);
        n = start + extend;

        if (n == *sz)
                return (uint8_t*) *p + start;

        if (n > (size_t) ((uint32_t) -1))
                return NULL;

        k = realloc(*p, n);
        if (!k)
                return NULL;

        /* Zero out padding */
        if (start > *sz)
                memset((uint8_t*) k + *sz, 0, start - *sz);

        *p = k;
        *sz = n;

        return (uint8_t*) k + start;
}

static void *message_extend_fields(sd_bus_message *m, size_t align, size_t sz) {
        void *p, *o;

        assert(m);

        o = m->fields;
        p = buffer_extend(&m->fields, &m->header->fields_size, align, sz);
        if (!p)
                return NULL;

        if (o != m->fields) {
                /* Adjust quick access pointers */

                if (m->path)
                        m->path = (const char*) m->fields + (m->path - (const char*) o);
                if (m->interface)
                        m->interface = (const char*) m->fields + (m->interface - (const char*) o);
                if (m->member)
                        m->member = (const char*) m->fields + (m->member - (const char*) o);
                if (m->destination)
                        m->destination = (const char*) m->fields + (m->destination - (const char*) o);
                if (m->sender)
                        m->sender = (const char*) m->fields + (m->sender - (const char*) o);
                if (m->error.name)
                        m->error.name = (const char*) m->fields + (m->error.name - (const char*) o);
        }

        m->free_fields = true;

        return p;
}

static int message_append_field_string(
                sd_bus_message *m,
                uint8_t h,
                char type,
                const char *s,
                const char **ret) {

        size_t l;
        uint8_t *p;

        assert(m);

        l = strlen(s);
        if (l > (size_t) (uint32_t) -1)
                return -EINVAL;

        /* field id byte + signature length + signature 's' + NUL + string length + string + NUL */
        p = message_extend_fields(m, 8, 4 + 4 + l + 1);
        if (!p)
                return -ENOMEM;

        p[0] = h;
        p[1] = 1;
        p[2] = type;
        p[3] = 0;

        ((uint32_t*) p)[1] = l;
        memcpy(p + 8, s, l + 1);

        if (ret)
                *ret = (const char*) p + 8;

        return 0;
}

static int message_append_field_signature(
                sd_bus_message *m,
                uint8_t h,
                const char *s,
                const char **ret) {

        size_t l;
        uint8_t *p;

        assert(m);

        l = strlen(s);
        if (l > 255)
                return -EINVAL;

        /* field id byte + signature length + signature 'g' + NUL + string length + string + NUL */
        p = message_extend_fields(m, 8, 4 + 1 + l + 1);
        if (!p)
                return -ENOMEM;

        p[0] = h;
        p[1] = 1;
        p[2] = SD_BUS_TYPE_SIGNATURE;
        p[3] = 0;
        p[4] = l;
        memcpy(p + 5, s, l + 1);

        if (ret)
                *ret = (const char*) p + 5;

        return 0;
}

static int message_append_field_uint32(sd_bus_message *m, uint8_t h, uint32_t x) {
        uint8_t *p;

        assert(m);

        /* field id byte + signature length + signature 'u' + NUL + value */
        p = message_extend_fields(m, 8, 4 + 4);
        if (!p)
                return -ENOMEM;

        p[0] = h;
        p[1] = 1;
        p[2] = SD_BUS_TYPE_UINT32;
        p[3] = 0;

        ((uint32_t*) p)[1] = x;

        return 0;
}

int bus_message_from_malloc(
                void *buffer,
                size_t length,
                struct ucred *ucred,
                const char *label,
                sd_bus_message **ret) {

        sd_bus_message *m;
        struct bus_header *h;
        size_t total, fs, bs, label_sz, a;
        int r;

        assert(buffer || length <= 0);
        assert(ret);

        if (length < sizeof(struct bus_header))
                return -EBADMSG;

        h = buffer;
        if (h->version != 1)
                return -EBADMSG;

        if (h->serial == 0)
                return -EBADMSG;

        if (h->type == _SD_BUS_MESSAGE_TYPE_INVALID)
                return -EBADMSG;

        if (h->endian == SD_BUS_NATIVE_ENDIAN) {
                fs = h->fields_size;
                bs = h->body_size;
        } else if (h->endian == SD_BUS_REVERSE_ENDIAN) {
                fs = bswap_32(h->fields_size);
                bs = bswap_32(h->body_size);
        } else
                return -EBADMSG;

        total = sizeof(struct bus_header) + ALIGN_TO(fs, 8) + bs;
        if (length != total)
                return -EBADMSG;

        if (label) {
                label_sz = strlen(label);
                a = ALIGN(sizeof(sd_bus_message)) + label_sz + 1;
        } else
                a = sizeof(sd_bus_message);

        m = malloc0(a);
        if (!m)
                return -ENOMEM;

        m->n_ref = 1;
        m->header = h;
        m->free_header = true;
        m->fields = (uint8_t*) buffer + sizeof(struct bus_header);
        m->body = (uint8_t*) buffer + sizeof(struct bus_header) + ALIGN_TO(fs, 8);
        m->sealed = true;

        if (ucred) {
                m->uid = ucred->uid;
                m->pid = ucred->pid;
                m->gid = ucred->gid;
                m->uid_valid = m->gid_valid = true;
        }

        if (label) {
                m->label = (char*) m + ALIGN(sizeof(sd_bus_message));
                memcpy(m->label, label, label_sz + 1);
        }

        m->n_iovec = 1;
        m->iovec[0].iov_base = buffer;
        m->iovec[0].iov_len = length;

        r = message_parse_fields(m);
        if (r < 0) {
                message_free(m);
                return r;
        }

        *ret = m;
        return 0;
}

static sd_bus_message *message_new(sd_bus *bus, uint8_t type) {
        sd_bus_message *m;

        m = malloc0(ALIGN(sizeof(sd_bus_message)) + sizeof(struct bus_header));
        if (!m)
                return NULL;

        m->n_ref = 1;
        m->header = (struct bus_header*) ((uint8_t*) m + ALIGN(sizeof(struct sd_bus_message)));
        m->header->endian = SD_BUS_NATIVE_ENDIAN;
        m->header->type = type;
        m->header->version = bus ? bus->message_version : 1;

        return m;
}

int sd_bus_message_new_signal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *member,
                sd_bus_message **m) {

        sd_bus_message *t;
        int r;

        if (!path)
                return -EINVAL;
        if (!interface)
                return -EINVAL;
        if (!member)
                return -EINVAL;
        if (!m)
                return -EINVAL;

        t = message_new(bus, SD_BUS_MESSAGE_TYPE_SIGNAL);
        if (!t)
                return -ENOMEM;

        t->header->flags |= SD_BUS_MESSAGE_NO_REPLY_EXPECTED;

        r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_PATH, SD_BUS_TYPE_OBJECT_PATH, path, &t->path);
        if (r < 0)
                goto fail;
        r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_INTERFACE, SD_BUS_TYPE_STRING, interface, &t->interface);
        if (r < 0)
                goto fail;
        r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_MEMBER, SD_BUS_TYPE_STRING, member, &t->member);
        if (r < 0)
                goto fail;

        *m = t;
        return 0;

fail:
        sd_bus_message_unref(t);
        return r;
}

int sd_bus_message_new_method_call(
                sd_bus *bus,
                const char *destination,
                const char *path,
                const char *interface,
                const char *member,
                sd_bus_message **m) {

        sd_bus_message *t;
        int r;

        if (!path)
                return -EINVAL;
        if (!member)
                return -EINVAL;
        if (!m)
                return -EINVAL;

        t = message_new(bus, SD_BUS_MESSAGE_TYPE_METHOD_CALL);
        if (!t)
                return -ENOMEM;

        r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_PATH, SD_BUS_TYPE_OBJECT_PATH, path, &t->path);
        if (r < 0)
                goto fail;
        r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_MEMBER, SD_BUS_TYPE_STRING, member, &t->member);
        if (r < 0)
                goto fail;

        if (interface) {
                r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_INTERFACE, SD_BUS_TYPE_STRING, interface, &t->interface);
                if (r < 0)
                        goto fail;
        }

        if (destination) {
                r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_DESTINATION, SD_BUS_TYPE_STRING, destination, &t->destination);
                if (r < 0)
                        goto fail;
        }

        *m = t;
        return 0;

fail:
        message_free(t);
        return r;
}

static int message_new_reply(
                sd_bus *bus,
                sd_bus_message *call,
                uint8_t type,
                sd_bus_message **m) {

        sd_bus_message *t;
        int r;

        if (!call)
                return -EINVAL;
        if (!call->sealed)
                return -EPERM;
        if (call->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return -EINVAL;
        if (!m)
                return -EINVAL;

        t = message_new(bus, type);
        if (!t)
                return -ENOMEM;

        t->header->flags |= SD_BUS_MESSAGE_NO_REPLY_EXPECTED;
        t->reply_serial = BUS_MESSAGE_SERIAL(call);

        r = message_append_field_uint32(t, SD_BUS_MESSAGE_HEADER_REPLY_SERIAL, t->reply_serial);
        if (r < 0)
                goto fail;

        if (call->sender) {
                r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_DESTINATION, SD_BUS_TYPE_STRING, call->sender, &t->sender);
                if (r < 0)
                        goto fail;
        }

        t->dont_send = !!(call->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED);

        *m = t;
        return 0;

fail:
        message_free(t);
        return r;
}

int sd_bus_message_new_method_return(
                sd_bus *bus,
                sd_bus_message *call,
                sd_bus_message **m) {

        return message_new_reply(bus, call, SD_BUS_MESSAGE_TYPE_METHOD_RETURN, m);
}

int sd_bus_message_new_method_error(
                sd_bus *bus,
                sd_bus_message *call,
                const sd_bus_error *e,
                sd_bus_message **m) {

        sd_bus_message *t;
        int r;

        if (!sd_bus_error_is_set(e))
                return -EINVAL;
        if (!m)
                return -EINVAL;

        r = message_new_reply(bus, call, SD_BUS_MESSAGE_TYPE_METHOD_ERROR, &t);
        if (r < 0)
                return r;

        r = message_append_field_string(t, SD_BUS_MESSAGE_HEADER_ERROR_NAME, SD_BUS_TYPE_STRING, e->name, &t->error.name);
        if (r < 0)
                goto fail;

        if (e->message) {
                r = message_append_basic(t, SD_BUS_TYPE_STRING, e->message, (const void**) &t->error.message);
                if (r < 0)
                        goto fail;
        }

        *m = t;
        return 0;

fail:
        message_free(t);
        return r;
}

sd_bus_message* sd_bus_message_ref(sd_bus_message *m) {
        if (!m)
                return NULL;

        assert(m->n_ref > 0);
        m->n_ref++;

        return m;
}

sd_bus_message* sd_bus_message_unref(sd_bus_message *m) {
        if (!m)
                return NULL;

        assert(m->n_ref > 0);
        m->n_ref--;

        if (m->n_ref <= 0)
                message_free(m);

        return NULL;
}

int sd_bus_message_get_type(sd_bus_message *m, uint8_t *type) {
        if (!m)
                return -EINVAL;
        if (!type)
                return -EINVAL;

        *type = m->header->type;
        return 0;
}

int sd_bus_message_get_serial(sd_bus_message *m, uint64_t *serial) {
        if (!m)
                return -EINVAL;
        if (!serial)
                return -EINVAL;
        if (m->header->serial == 0)
                return -ENOENT;

        *serial = BUS_MESSAGE_SERIAL(m);
        return 0;
}

int sd_bus_message_get_reply_serial(sd_bus_message *m, uint64_t *serial) {
        if (!m)
                return -EINVAL;
        if (!serial)
                return -EINVAL;
        if (m->reply_serial == 0)
                return -ENOENT;

        *serial = m->reply_serial;
        return 0;
}

int sd_bus_message_get_no_reply(sd_bus_message *m) {
        if (!m)
                return -EINVAL;

        return m->header->type == SD_BUS_MESSAGE_TYPE_METHOD_CALL ? !!(m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED) : 0;
}

const char *sd_bus_message_get_path(sd_bus_message *m) {
        if (!m)
                return NULL;

        return m->path;
}

const char *sd_bus_message_get_interface(sd_bus_message *m) {
        if (!m)
                return NULL;

        return m->interface;
}

const char *sd_bus_message_get_member(sd_bus_message *m) {
        if (!m)
                return NULL;

        return m->member;
}
const char *sd_bus_message_get_destination(sd_bus_message *m) {
        if (!m)
                return NULL;

        return m->destination;
}

const char *sd_bus_message_get_sender(sd_bus_message *m) {
        if (!m)
                return NULL;

        return m->sender;
}

const sd_bus_error *sd_bus_message_get_error(sd_bus_message *m) {
        if (!m)
                return NULL;

        if (!sd_bus_error_is_set(&m->error))
                return NULL;

        return &m->error;
}

int sd_bus_message_get_uid(sd_bus_message *m, uid_t *uid) {
        if (!m)
                return -EINVAL;
        if (!m->uid_valid)
                return -ENOENT;

        *uid = m->uid;
        return 0;
}

int sd_bus_message_get_gid(sd_bus_message *m, gid_t *gid) {
        if (!m)
                return -EINVAL;
        if (!m->gid_valid)
                return -ENOENT;

        *gid = m->gid;
        return 0;
}

int sd_bus_message_get_pid(sd_bus_message *m, pid_t *pid) {
        if (!m)
                return -EINVAL;
        if (m->pid <= 0)
                return -ENOENT;

        *pid = m->pid;
        return 0;
}

int sd_bus_message_get_tid(sd_bus_message *m, pid_t *tid) {
        if (!m)
                return -EINVAL;
        if (m->tid <= 0)
                return -ENOENT;

        *tid = m->tid;
        return 0;
}

const char *sd_bus_message_get_label(sd_bus_message *m) {
        if (!m)
                return NULL;

        return m->label;
}

int sd_bus_message_is_signal(sd_bus_message *m, const char *interface, const char *member) {
        if (!m)
                return -EINVAL;

        if (m->header->type != SD_BUS_MESSAGE_TYPE_SIGNAL)
                return 0;

        if (interface && (!m->interface || !streq(m->interface, interface)))
                return 0;

        if (member &&  (!m->member || !streq(m->member, member)))
                return 0;

        return 1;
}

int sd_bus_message_is_method_call(sd_bus_message *m, const char *interface, const char *member) {
        if (!m)
                return -EINVAL;

        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return 0;

        if (interface && (!m->interface || !streq(m->interface, interface)))
                return 0;

        if (member &&  (!m->member || !streq(m->member, member)))
                return 0;

        return 1;
}

int sd_bus_message_is_method_error(sd_bus_message *m, const char *name) {
        if (!m)
                return -EINVAL;

        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_ERROR)
                return 0;

        if (name && (!m->error.name || !streq(m->error.name, name)))
                return 0;

        return 1;
}

int sd_bus_message_set_no_reply(sd_bus_message *m, int b) {
        if (!m)
                return -EINVAL;
        if (m->sealed)
                return -EPERM;
        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return -EPERM;

        if (b)
                m->header->flags |= SD_BUS_MESSAGE_NO_REPLY_EXPECTED;
        else
                m->header->flags &= ~SD_BUS_MESSAGE_NO_REPLY_EXPECTED;

        return 0;
}

static struct bus_container *message_get_container(sd_bus_message *m) {
        assert(m);

        if (m->n_containers == 0)
                return &m->root_container;

        assert(m->containers);
        return m->containers + m->n_containers - 1;
}

static void *message_extend_body(sd_bus_message *m, size_t align, size_t sz) {
        void *p, *o;
        size_t added;
        struct bus_container *c;

        assert(m);
        assert(align > 0);

        o = m->body;
        added = m->header->body_size;

        p = buffer_extend(&m->body, &m->header->body_size, align, sz);
        if (!p)
                return NULL;

        added = m->header->body_size - added;

        for (c = m->containers; c < m->containers + m->n_containers; c++)
                if (c->array_size) {
                        c->array_size = (uint32_t*) ((uint8_t*) m->body + ((uint8_t*) c->array_size - (uint8_t*) o));
                        *c->array_size += added;
                }

        if (o != m->body) {
                if (m->error.message)
                        m->error.message = (const char*) m->body + (m->error.message - (const char*) o);
        }

        m->free_body = true;

        return p;
}

int message_append_basic(sd_bus_message *m, char type, const void *p, const void **stored) {
        struct bus_container *c;
        size_t sz, align;
        uint32_t k;
        void *a;
        char *e = NULL;

        if (!m)
                return -EINVAL;
        if (m->sealed)
                return -EPERM;
        if (!bus_type_is_basic(type))
                return -EINVAL;

        c = message_get_container(m);

        if (c->signature && c->signature[c->index]) {
                /* Container signature is already set */

                if (c->signature[c->index] != type)
                        return -ENXIO;
        } else {
                /* Maybe we can append to the signature? But only if this is the top-level container*/
                if (c->enclosing != 0)
                        return -ENXIO;

                e = strextend(&c->signature, CHAR_TO_STR(type), NULL);
                if (!e)
                        return -ENOMEM;
        }

        switch (type) {

        case SD_BUS_TYPE_STRING:
        case SD_BUS_TYPE_OBJECT_PATH:

                if (!p) {
                        if (e)
                                c->signature[c->index] = 0;

                        return -EINVAL;
                }

                align = 4;
                sz = 4 + strlen(p) + 1;
                break;

        case SD_BUS_TYPE_SIGNATURE:

                if (!p) {
                        if (e)
                                c->signature[c->index] = 0;

                        return -EINVAL;
                }

                align = 1;
                sz = 1 + strlen(p) + 1;
                break;

        case SD_BUS_TYPE_BOOLEAN:
                align = sz = 4;

                assert_cc(sizeof(int) == sizeof(uint32_t));
                memcpy(&k, p, 4);
                k = !!k;
                p = &k;
                break;

        default:
                align = bus_type_get_alignment(type);
                sz = bus_type_get_size(type);
                break;
        }

        assert(align > 0);
        assert(sz > 0);

        a = message_extend_body(m, align, sz);
        if (!a) {
                /* Truncate extended signature again */
                if (e)
                        c->signature[c->index] = 0;

                return -ENOMEM;
        }

        if (type == SD_BUS_TYPE_STRING || type == SD_BUS_TYPE_OBJECT_PATH) {
                *(uint32_t*) a = sz - 5;
                memcpy((uint8_t*) a + 4, p, sz - 4);

                if (stored)
                        *stored = (const uint8_t*) a + 4;

        } else if (type == SD_BUS_TYPE_SIGNATURE) {
                *(uint8_t*) a = sz - 1;
                memcpy((uint8_t*) a + 1, p, sz - 1);

                if (stored)
                        *stored = (const uint8_t*) a + 1;

        } else {
                memcpy(a, p, sz);

                if (stored)
                        *stored = a;
        }

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index++;

        return 0;
}

int sd_bus_message_append_basic(sd_bus_message *m, char type, const void *p) {
        return message_append_basic(m, type, p, NULL);
}

static int bus_message_open_array(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents,
                uint32_t **array_size) {

        unsigned nindex;
        char *e = NULL;
        void *a, *b;
        int alignment;
        size_t saved;

        assert(m);
        assert(c);
        assert(contents);
        assert(array_size);

        if (!signature_is_single(contents))
                return -EINVAL;

        alignment = bus_type_get_alignment(contents[0]);
        if (alignment < 0)
                return alignment;

        if (c->signature && c->signature[c->index]) {

                /* Verify the existing signature */

                if (c->signature[c->index] != SD_BUS_TYPE_ARRAY)
                        return -ENXIO;

                if (!startswith(c->signature + c->index + 1, contents))
                        return -ENXIO;

                nindex = c->index + 1 + strlen(contents);
        } else {
                if (c->enclosing != 0)
                        return -ENXIO;

                /* Extend the existing signature */

                e = strextend(&c->signature, CHAR_TO_STR(SD_BUS_TYPE_ARRAY), contents, NULL);
                if (!e)
                        return -ENOMEM;

                nindex = e - c->signature;
        }

        saved = m->header->body_size;
        a = message_extend_body(m, 4, 4);
        if (!a) {
                /* Truncate extended signature again */
                if (e)
                        c->signature[c->index] = 0;

                return -ENOMEM;
        }
        b = m->body;

        if (!message_extend_body(m, alignment, 0)) {
                /* Add alignment between size and first element */
                if (e)
                        c->signature[c->index] = 0;

                m->header->body_size = saved;
                return -ENOMEM;
        }

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index = nindex;

        /* m->body might have changed so let's readjust a */
        a = (uint8_t*) m->body + ((uint8_t*) a - (uint8_t*) b);
        *(uint32_t*) a = 0;

        *array_size = a;
        return 0;
}

static int bus_message_open_variant(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents) {

        char *e = NULL;
        size_t l;
        void *a;

        assert(m);
        assert(c);
        assert(contents);

        if (!signature_is_single(contents))
                return -EINVAL;

        if (*contents == SD_BUS_TYPE_DICT_ENTRY_BEGIN)
                return -EINVAL;

        if (c->signature && c->signature[c->index]) {

                if (c->signature[c->index] != SD_BUS_TYPE_VARIANT)
                        return -ENXIO;

        } else {
                if (c->enclosing != 0)
                        return -ENXIO;

                e = strextend(&c->signature, CHAR_TO_STR(SD_BUS_TYPE_VARIANT), NULL);
                if (!e)
                        return -ENOMEM;
        }

        l = strlen(contents);
        a = message_extend_body(m, 1, 1 + l + 1);
        if (!a) {
                /* Truncate extended signature again */
                if (e)
                        c->signature[c->index] = 0;

                return -ENOMEM;
        }

        *(uint8_t*) a = l;
        memcpy((uint8_t*) a + 1, contents, l + 1);

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index++;

        return 0;
}

static int bus_message_open_struct(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents) {

        size_t nindex;
        char *e = NULL;

        assert(m);
        assert(c);
        assert(contents);

        if (!signature_is_valid(contents, false))
                return -EINVAL;

        if (c->signature && c->signature[c->index]) {
                size_t l;

                l = strlen(contents);

                if (c->signature[c->index] != SD_BUS_TYPE_STRUCT_BEGIN ||
                    !startswith(c->signature + c->index + 1, contents) ||
                    c->signature[c->index + 1 + l] != SD_BUS_TYPE_STRUCT_END)
                        return -ENXIO;

                nindex = c->index + 1 + l + 1;
        } else {
                if (c->enclosing != 0)
                        return -ENXIO;

                e = strextend(&c->signature, CHAR_TO_STR(SD_BUS_TYPE_STRUCT_BEGIN), contents, CHAR_TO_STR(SD_BUS_TYPE_STRUCT_END), NULL);
                if (!e)
                        return -ENOMEM;

                nindex = e - c->signature;
        }

        /* Align contents to 8 byte boundary */
        if (!message_extend_body(m, 8, 0)) {
                if (e)
                        c->signature[c->index] = 0;

                return -ENOMEM;
        }

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index = nindex;

        return 0;
}

static int bus_message_open_dict_entry(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents) {

        size_t nindex;

        assert(m);
        assert(c);
        assert(contents);

        if (!signature_is_pair(contents))
                return -EINVAL;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                return -ENXIO;

        if (c->signature && c->signature[c->index]) {
                size_t l;

                l = strlen(contents);

                if (c->signature[c->index] != SD_BUS_TYPE_DICT_ENTRY_BEGIN ||
                    !startswith(c->signature + c->index + 1, contents) ||
                    c->signature[c->index + 1 + l] != SD_BUS_TYPE_DICT_ENTRY_END)
                        return -ENXIO;

                nindex = c->index + 1 + l + 1;
        } else
                return -ENXIO;

        /* Align contents to 8 byte boundary */
        if (!message_extend_body(m, 8, 0))
                return -ENOMEM;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index = nindex;

        return 0;
}

int sd_bus_message_open_container(
                sd_bus_message *m,
                char type,
                const char *contents) {

        struct bus_container *c, *w;
        uint32_t *array_size = NULL;
        char *signature;
        int r;

        if (!m)
                return -EINVAL;
        if (m->sealed)
                return -EPERM;
        if (!contents)
                return -EINVAL;

        /* Make sure we have space for one more container */
        w = realloc(m->containers, sizeof(struct bus_container) * (m->n_containers + 1));
        if (!w)
                return -ENOMEM;
        m->containers = w;

        c = message_get_container(m);

        signature = strdup(contents);
        if (!signature)
                return -ENOMEM;

        if (type == SD_BUS_TYPE_ARRAY)
                r = bus_message_open_array(m, c, contents, &array_size);
        else if (type == SD_BUS_TYPE_VARIANT)
                r = bus_message_open_variant(m, c, contents);
        else if (type == SD_BUS_TYPE_STRUCT)
                r = bus_message_open_struct(m, c, contents);
        else if (type == SD_BUS_TYPE_DICT_ENTRY)
                r = bus_message_open_dict_entry(m, c, contents);
        else
                r = -EINVAL;

        if (r < 0) {
                free(signature);
                return r;
        }

        /* OK, let's fill it in */
        w += m->n_containers++;
        w->enclosing = type;
        w->signature = signature;
        w->index = 0;
        w->array_size = array_size;
        w->begin = 0;

        return 0;
}

int sd_bus_message_close_container(sd_bus_message *m) {
        struct bus_container *c;

        if (!m)
                return -EINVAL;
        if (m->sealed)
                return -EPERM;
        if (m->n_containers <= 0)
                return -EINVAL;

        c = message_get_container(m);
        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                if (c->signature && c->signature[c->index] != 0)
                        return -EINVAL;

        free(c->signature);
        m->n_containers--;

        return 0;
}

static int message_append_ap(
                sd_bus_message *m,
                const char *types,
                va_list ap) {

        const char *t;
        int r;

        assert(m);
        assert(types);

        for (t = types; *t; t++) {
                switch (*t) {

                case SD_BUS_TYPE_BYTE: {
                        uint8_t x;

                        x = (uint8_t) va_arg(ap, int);
                        r = sd_bus_message_append_basic(m, *t, &x);
                        break;
                }

                case SD_BUS_TYPE_BOOLEAN:
                case SD_BUS_TYPE_INT32:
                case SD_BUS_TYPE_UINT32:
                case SD_BUS_TYPE_UNIX_FD: {
                        uint32_t x;

                        /* We assume a boolean is the same as int32_t */
                        assert_cc(sizeof(int32_t) == sizeof(int));

                        x = va_arg(ap, uint32_t);
                        r = sd_bus_message_append_basic(m, *t, &x);
                        break;
                }

                case SD_BUS_TYPE_INT16:
                case SD_BUS_TYPE_UINT16: {
                        uint16_t x;

                        x = (uint16_t) va_arg(ap, int);
                        r = sd_bus_message_append_basic(m, *t, &x);
                        break;
                }

                case SD_BUS_TYPE_INT64:
                case SD_BUS_TYPE_UINT64:
                case SD_BUS_TYPE_DOUBLE: {
                        uint64_t x;

                        x = va_arg(ap, uint64_t);
                        r = sd_bus_message_append_basic(m, *t, &x);
                        break;
                }

                case SD_BUS_TYPE_STRING:
                case SD_BUS_TYPE_OBJECT_PATH:
                case SD_BUS_TYPE_SIGNATURE: {
                        const char *x;

                        x = va_arg(ap, const char*);
                        r = sd_bus_message_append_basic(m, *t, x);
                        break;
                }

                case SD_BUS_TYPE_ARRAY: {
                        size_t k;

                        r = signature_element_length(t + 1, &k);
                        if (r < 0)
                                return r;

                        {
                                unsigned i, n;
                                char s[k + 1];

                                memcpy(s, t + 1, k);
                                s[k] = 0;
                                t += k;

                                r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, s);
                                if (r < 0)
                                        return r;

                                n = va_arg(ap, unsigned);
                                for (i = 0; i < n; i++) {
                                        r = message_append_ap(m, s, ap);
                                        if (r < 0)
                                                return r;
                                }

                                r = sd_bus_message_close_container(m);
                        }

                        break;
                }

                case SD_BUS_TYPE_VARIANT: {
                        const char *s;

                        s = va_arg(ap, const char*);
                        if (!s)
                                return -EINVAL;

                        r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, s);
                        if (r < 0)
                                return r;

                        r = message_append_ap(m, s, ap);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_close_container(m);
                        break;
                }

                case SD_BUS_TYPE_STRUCT_BEGIN:
                case SD_BUS_TYPE_DICT_ENTRY_BEGIN: {
                        size_t k;

                        r = signature_element_length(t, &k);
                        if (r < 0)
                                return r;

                        {
                                char s[k - 1];

                                memcpy(s, t + 1, k - 2);
                                s[k - 2] = 0;

                                r = sd_bus_message_open_container(m, *t == SD_BUS_TYPE_STRUCT_BEGIN ? SD_BUS_TYPE_STRUCT : SD_BUS_TYPE_DICT_ENTRY, s);
                                if (r < 0)
                                        return r;

                                t += k - 1;

                                r = message_append_ap(m, s, ap);
                                if (r < 0)
                                        return r;

                                r = sd_bus_message_close_container(m);
                        }

                        break;
                }

                default:
                        r = -EINVAL;
                }

                if (r < 0)
                        return r;
        }

        return 0;
}

int sd_bus_message_append(sd_bus_message *m, const char *types, ...) {
        va_list ap;
        int r;

        if (!m)
                return -EINVAL;
        if (m->sealed)
                return -EPERM;
        if (!types)
                return -EINVAL;

        va_start(ap, types);
        r = message_append_ap(m, types, ap);
        va_end(ap);

        return r;
}

static int buffer_peek(const void *p, uint32_t sz, size_t *rindex, size_t align, size_t nbytes, void **r) {
        size_t k, start, n;

        assert(rindex);
        assert(align > 0);

        start = ALIGN_TO((size_t) *rindex, align);
        n = start + nbytes;

        if (n > sz)
                return -EBADMSG;

        /* Verify that padding is 0 */
        for (k = *rindex; k < start; k++)
                if (((const uint8_t*) p)[k] != 0)
                        return -EBADMSG;

        if (r)
                *r = (uint8_t*) p + start;

        *rindex = n;

        return 1;
}

static bool message_end_of_array(sd_bus_message *m, size_t index) {
        struct bus_container *c;

        assert(m);

        c = message_get_container(m);
        if (!c->array_size)
                return false;

        return index >= c->begin + BUS_MESSAGE_BSWAP32(m, *c->array_size);
}

static int message_peek_body(sd_bus_message *m, size_t *rindex, size_t align, size_t nbytes, void **ret) {
        assert(m);
        assert(rindex);
        assert(align > 0);

        if (message_end_of_array(m, *rindex))
                return 0;

        return buffer_peek(m->body, BUS_MESSAGE_BODY_SIZE(m), rindex, align, nbytes, ret);
}

static bool validate_nul(const char *s, size_t l) {

        /* Check for NUL chars in the string */
        if (memchr(s, 0, l))
                return false;

        /* Check for NUL termination */
        if (s[l] != 0)
                return false;

        return true;
}

static bool validate_string(const char *s, size_t l) {

        if (!validate_nul(s, l))
                return false;

        /* Check if valid UTF8 */
        if (!utf8_is_valid(s))
                return false;

        return true;
}

static bool validate_signature(const char *s, size_t l) {

        if (!validate_nul(s, l))
                return false;

        /* Check if valid signature */
        if (!signature_is_valid(s, true))
                return false;

        return true;
}

static bool validate_object_path(const char *s, size_t l) {

        if (!validate_nul(s, l))
                return false;

        if (!object_path_is_valid(s))
                return false;

        return true;
}

int sd_bus_message_read_basic(sd_bus_message *m, char type, void *p) {
        struct bus_container *c;
        int r;
        void *q;

        if (!m)
                return -EINVAL;
        if (!m->sealed)
                return -EPERM;
        if (!bus_type_is_basic(type))
                return -EINVAL;

        c = message_get_container(m);

        if (!c->signature || c->signature[c->index] == 0)
                return 0;

        if (c->signature[c->index] != type)
                return -ENXIO;

        switch (type) {

        case SD_BUS_TYPE_STRING:
        case SD_BUS_TYPE_OBJECT_PATH: {
                uint32_t l;
                size_t rindex;

                rindex = m->rindex;
                r = message_peek_body(m, &rindex, 4, 4, &q);
                if (r <= 0)
                        return r;

                l = BUS_MESSAGE_BSWAP32(m, *(uint32_t*) q);
                r = message_peek_body(m, &rindex, 1, l+1, &q);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -EBADMSG;

                if (type == SD_BUS_TYPE_OBJECT_PATH) {
                        if (!validate_object_path(q, l))
                                return -EBADMSG;
                } else {
                        if (!validate_string(q, l))
                                return -EBADMSG;
                }

                m->rindex = rindex;
                *(const char**) p = q;
                break;
        }

        case SD_BUS_TYPE_SIGNATURE: {
                uint8_t l;
                size_t rindex;

                rindex = m->rindex;
                r = message_peek_body(m, &rindex, 1, 1, &q);
                if (r <= 0)
                        return r;

                l = *(uint8_t*) q;
                r = message_peek_body(m, &rindex, 1, l+1, &q);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -EBADMSG;

                if (!validate_signature(q, l))
                        return -EBADMSG;

                m->rindex = rindex;
                *(const char**) p = q;
                break;
        }

        default: {
                size_t sz, align;

                align = bus_type_get_alignment(type);
                sz = bus_type_get_size(type);

                r = message_peek_body(m, &m->rindex, align, sz, &q);
                if (r <= 0)
                        return r;

                switch (type) {

                case SD_BUS_TYPE_BYTE:
                        *(uint8_t*) p = *(uint8_t*) q;
                        break;

                case SD_BUS_TYPE_BOOLEAN:
                        *(int*) p = !!*(uint32_t*) q;
                        break;

                case SD_BUS_TYPE_INT16:
                case SD_BUS_TYPE_UINT16:
                        *(uint16_t*) p = BUS_MESSAGE_BSWAP16(m, *(uint16_t*) q);
                        break;

                case SD_BUS_TYPE_INT32:
                case SD_BUS_TYPE_UINT32:
                        *(uint32_t*) p = BUS_MESSAGE_BSWAP32(m, *(uint32_t*) q);
                        break;

                case SD_BUS_TYPE_INT64:
                case SD_BUS_TYPE_UINT64:
                case SD_BUS_TYPE_DOUBLE:
                        *(uint64_t*) p = BUS_MESSAGE_BSWAP64(m, *(uint64_t*) q);
                        break;

                default:
                        assert_not_reached("Unknown basic type...");
                }

                break;
        }
        }

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index++;

        return 1;
}

static int bus_message_enter_array(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents,
                uint32_t **array_size) {

        size_t rindex;
        void *q;
        int r, alignment;

        assert(m);
        assert(c);
        assert(contents);
        assert(array_size);

        if (!signature_is_single(contents))
                return -EINVAL;

        alignment = bus_type_get_alignment(contents[0]);
        if (alignment < 0)
                return alignment;

        if (!c->signature || c->signature[c->index] == 0)
                return 0;

        if (c->signature[c->index] != SD_BUS_TYPE_ARRAY)
                return -ENXIO;

        if (!startswith(c->signature + c->index + 1, contents))
                return -ENXIO;

        rindex = m->rindex;
        r = message_peek_body(m, &rindex, 4, 4, &q);
        if (r <= 0)
                return r;

        if (BUS_MESSAGE_BSWAP32(m, *(uint32_t*) q) > BUS_ARRAY_MAX_SIZE)
                return -EBADMSG;

        r = message_peek_body(m, &rindex, alignment, 0, NULL);
        if (r < 0)
                return r;
        if (r == 0)
                return -EBADMSG;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index += 1 + strlen(contents);

        m->rindex = rindex;

        *array_size = (uint32_t*) q;

        return 1;
}

static int bus_message_enter_variant(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents) {

        size_t rindex;
        uint8_t l;
        void *q;
        int r;

        assert(m);
        assert(c);
        assert(contents);

        if (!signature_is_single(contents))
                return -EINVAL;

        if (*contents == SD_BUS_TYPE_DICT_ENTRY_BEGIN)
                return -EINVAL;

        if (!c->signature || c->signature[c->index] == 0)
                return 0;

        if (c->signature[c->index] != SD_BUS_TYPE_VARIANT)
                return -ENXIO;

        rindex = m->rindex;
        r = message_peek_body(m, &rindex, 1, 1, &q);
        if (r <= 0)
                return r;

        l = *(uint8_t*) q;
        r = message_peek_body(m, &rindex, 1, l+1, &q);
        if (r < 0)
                return r;
        if (r == 0)
                return -EBADMSG;

        if (!validate_signature(q, l))
                return -EBADMSG;

        if (!streq(q, contents))
                return -ENXIO;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index++;

        m->rindex = rindex;

        return 1;
}

static int bus_message_enter_struct(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents) {

        size_t l;
        int r;

        assert(m);
        assert(c);
        assert(contents);

        if (!signature_is_valid(contents, false))
                return -EINVAL;

        if (!c->signature || c->signature[c->index] == 0)
                return 0;

        l = strlen(contents);

        if (c->signature[c->index] != SD_BUS_TYPE_STRUCT_BEGIN ||
            !startswith(c->signature + c->index + 1, contents) ||
            c->signature[c->index + 1 + l] != SD_BUS_TYPE_STRUCT_END)
                return -ENXIO;

        r = message_peek_body(m, &m->rindex, 8, 0, NULL);
        if (r <= 0)
                return r;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index += 1 + l + 1;

        return 1;
}

static int bus_message_enter_dict_entry(
                sd_bus_message *m,
                struct bus_container *c,
                const char *contents) {

        size_t l;
        int r;

        assert(m);
        assert(c);
        assert(contents);

        if (!signature_is_pair(contents))
                return -EINVAL;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                return -ENXIO;

        if (!c->signature || c->signature[c->index] == 0)
                return 0;

        l = strlen(contents);

        if (c->signature[c->index] != SD_BUS_TYPE_DICT_ENTRY_BEGIN ||
            !startswith(c->signature + c->index + 1, contents) ||
            c->signature[c->index + 1 + l] != SD_BUS_TYPE_DICT_ENTRY_END)
                return -ENXIO;

        r = message_peek_body(m, &m->rindex, 8, 0, NULL);
        if (r <= 0)
                return r;

        if (c->enclosing != SD_BUS_TYPE_ARRAY)
                c->index += 1 + l + 1;

        return 1;
}

int sd_bus_message_enter_container(sd_bus_message *m, char type, const char *contents) {
        struct bus_container *c, *w;
        uint32_t *array_size = NULL;
        char *signature;
        int r;

        if (!m)
                return -EINVAL;
        if (!m->sealed)
                return -EPERM;
        if (!contents)
                return -EINVAL;

        /*
         * We enforce a global limit on container depth, that is much
         * higher than the 32 structs and 32 arrays the specification
         * mandates. This is simpler to implement for us, and we need
         * this only to ensure our container array doesn't grow
         * without bounds. We are happy to return any data from a
         * message as long as the data itself is valid, even if the
         * overall message might be not.
         *
         * Note that the message signature is validated when
         * parsing the headers, and that validation does check the
         * 32/32 limit.
         *
         * Note that the specification defines no limits on the depth
         * of stacked variants, but we do.
         */
        if (m->n_containers >= BUS_CONTAINER_DEPTH)
                return -EBADMSG;

        w = realloc(m->containers, sizeof(struct bus_container) * (m->n_containers + 1));
        if (!w)
                return -ENOMEM;
        m->containers = w;

        c = message_get_container(m);

        if (!c->signature || c->signature[c->index] == 0)
                return 0;

        signature = strdup(contents);
        if (!signature)
                return -ENOMEM;

        if (type == SD_BUS_TYPE_ARRAY)
                r = bus_message_enter_array(m, c, contents, &array_size);
        else if (type == SD_BUS_TYPE_VARIANT)
                r = bus_message_enter_variant(m, c, contents);
        else if (type == SD_BUS_TYPE_STRUCT)
                r = bus_message_enter_struct(m, c, contents);
        else if (type == SD_BUS_TYPE_DICT_ENTRY)
                r = bus_message_enter_dict_entry(m, c, contents);
        else
                r = -EINVAL;

        if (r <= 0) {
                free(signature);
                return r;
        }

        /* OK, let's fill it in */
        w += m->n_containers++;
        w->enclosing = type;
        w->signature = signature;
        w->index = 0;
        w->array_size = array_size;
        w->begin = m->rindex;

        return 1;
}

int sd_bus_message_exit_container(sd_bus_message *m) {
        struct bus_container *c;

        if (!m)
                return -EINVAL;
        if (!m->sealed)
                return -EPERM;
        if (m->n_containers <= 0)
                return -EINVAL;

        c = message_get_container(m);
        if (c->enclosing == SD_BUS_TYPE_ARRAY) {
                uint32_t l;

                l = BUS_MESSAGE_BSWAP32(m, *c->array_size);
                if (c->begin + l != m->rindex)
                        return -EBUSY;

        } else {
                if (c->signature && c->signature[c->index] != 0)
                        return -EINVAL;
        }

        free(c->signature);
        m->n_containers--;

        return 1;
}

int sd_bus_message_peek_type(sd_bus_message *m, char *type, const char **contents) {
        struct bus_container *c;
        int r;

        if (!m)
                return -EINVAL;
        if (!m->sealed)
                return -EPERM;

        c = message_get_container(m);

        if (!c->signature || c->signature[c->index] == 0)
                goto eof;

        if (message_end_of_array(m, m->rindex))
                goto eof;

        if (bus_type_is_basic(c->signature[c->index])) {
                if (contents)
                        *contents = NULL;
                if (type)
                        *type = c->signature[c->index];
                return 1;
        }

        if (c->signature[c->index] == SD_BUS_TYPE_ARRAY) {

                if (contents) {
                        size_t l;
                        char *sig;

                        r = signature_element_length(c->signature+c->index+1, &l);
                        if (r < 0)
                                return r;

                        assert(l >= 1);

                        sig = strndup(c->signature + c->index + 1, l);
                        if (!sig)
                                return -ENOMEM;

                        free(m->peeked_signature);
                        m->peeked_signature = sig;

                        *contents = sig;
                }

                if (type)
                        *type = SD_BUS_TYPE_ARRAY;

                return 1;
        }

        if (c->signature[c->index] == SD_BUS_TYPE_STRUCT_BEGIN ||
            c->signature[c->index] == SD_BUS_TYPE_DICT_ENTRY_BEGIN) {

                if (contents) {
                        size_t l;
                        char *sig;

                        r = signature_element_length(c->signature+c->index, &l);
                        if (r < 0)
                                return r;

                        assert(l >= 2);
                        sig = strndup(c->signature + c->index + 1, l - 2);
                        if (!sig)
                                return -ENOMEM;

                        free(m->peeked_signature);
                        m->peeked_signature = sig;

                        *contents = sig;
                }

                if (type)
                        *type = c->signature[c->index] == SD_BUS_TYPE_STRUCT_BEGIN ? SD_BUS_TYPE_STRUCT : SD_BUS_TYPE_DICT_ENTRY;

                return 1;
        }

        if (c->signature[c->index] == SD_BUS_TYPE_VARIANT) {
                if (contents) {
                        size_t rindex, l;
                        void *q;

                        rindex = m->rindex;
                        r = message_peek_body(m, &rindex, 1, 1, &q);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                goto eof;

                        l = *(uint8_t*) q;
                        r = message_peek_body(m, &rindex, 1, l+1, &q);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                return -EBADMSG;

                        if (!validate_signature(q, l))
                                return -EBADMSG;

                        *contents = q;
                }

                if (type)
                        *type = SD_BUS_TYPE_VARIANT;

                return 1;
        }

        return -EINVAL;

eof:
        if (type)
                *type = c->enclosing;
        if (contents)
                *contents = NULL;
        return 0;
}

int sd_bus_message_rewind(sd_bus_message *m, int complete) {
        struct bus_container *c;

        if (!m)
                return -EINVAL;
        if (!m->sealed)
                return -EPERM;

        if (complete) {
                reset_containers(m);
                m->rindex = 0;
                m->root_container.index = 0;

                c = message_get_container(m);
        } else {
                c = message_get_container(m);

                c->index = 0;
                m->rindex = c->begin;
        }

        return !isempty(c->signature);
}

static int message_read_ap(sd_bus_message *m, const char *types, va_list ap) {
        const char *t;
        int r;

        assert(m);
        assert(types);

        for (t = types; *t; t++) {
                switch (*t) {

                case SD_BUS_TYPE_BYTE:
                case SD_BUS_TYPE_BOOLEAN:
                case SD_BUS_TYPE_INT16:
                case SD_BUS_TYPE_UINT16:
                case SD_BUS_TYPE_INT32:
                case SD_BUS_TYPE_UINT32:
                case SD_BUS_TYPE_INT64:
                case SD_BUS_TYPE_UINT64:
                case SD_BUS_TYPE_DOUBLE:
                case SD_BUS_TYPE_STRING:
                case SD_BUS_TYPE_OBJECT_PATH:
                case SD_BUS_TYPE_SIGNATURE: {
                        void *p;

                        p = va_arg(ap, void*);
                        r = sd_bus_message_read_basic(m, *t, p);
                        break;
                }

                case SD_BUS_TYPE_ARRAY: {
                        size_t k;

                        r = signature_element_length(t + 1, &k);
                        if (r < 0)
                                return r;

                        {
                                unsigned i, n;
                                char s[k + 1];

                                memcpy(s, t + 1, k);
                                s[k] = 0;
                                t += k;

                                r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, s);
                                if (r < 0)
                                        return r;
                                if (r == 0)
                                        return -ENXIO;

                                n = va_arg(ap, unsigned);
                                for (i = 0; i < n; i++) {
                                        r = message_read_ap(m, s, ap);
                                        if (r < 0)
                                                return r;
                                }

                                r = sd_bus_message_exit_container(m);
                        }

                        break;
                }

                case SD_BUS_TYPE_VARIANT: {
                        const char *s;

                        s = va_arg(ap, const char *);
                        if (!s)
                                return -EINVAL;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, s);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                return -ENXIO;

                        r = message_read_ap(m, s, ap);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                return -ENXIO;

                        r = sd_bus_message_exit_container(m);
                        break;
                }

                case SD_BUS_TYPE_STRUCT_BEGIN:
                case SD_BUS_TYPE_DICT_ENTRY_BEGIN: {
                        size_t k;

                        r = signature_element_length(t, &k);
                        if (r < 0)
                                return r;

                        {
                                char s[k - 1];
                                memcpy(s, t + 1, k - 2);
                                s[k - 2] = 0;

                                r = sd_bus_message_enter_container(m, *t == SD_BUS_TYPE_STRUCT_BEGIN ? SD_BUS_TYPE_STRUCT : SD_BUS_TYPE_DICT_ENTRY, s);
                                if (r < 0)
                                        return r;
                                if (r == 0)
                                        return -ENXIO;

                                t += k - 1;

                                r = message_read_ap(m, s, ap);
                                if (r < 0)
                                        return r;
                                if (r == 0)
                                        return -ENXIO;

                                r = sd_bus_message_exit_container(m);
                        }

                        break;
                }

                default:
                        r = -EINVAL;
                }

                if (r < 0)
                        return r;
                if (r == 0)
                        return -ENXIO;
        }

        return 1;
}

int sd_bus_message_read(sd_bus_message *m, const char *types, ...) {
        va_list ap;
        int r;

        if (!m)
                return -EINVAL;
        if (!m->sealed)
                return -EPERM;
        if (!types)
                return -EINVAL;

        va_start(ap, types);
        r = message_read_ap(m, types, ap);
        va_end(ap);

        return r;
}

static int message_peek_fields(
                sd_bus_message *m,
                size_t *rindex,
                size_t align,
                size_t nbytes,
                void **ret) {

        assert(m);
        assert(rindex);
        assert(align > 0);

        return buffer_peek(m->fields, BUS_MESSAGE_FIELDS_SIZE(m), rindex, align, nbytes, ret);
}

static int message_peek_field_uint32(
                sd_bus_message *m,
                size_t *ri,
                uint32_t *ret) {

        int r;
        void *q;

        assert(m);
        assert(ri);

        r = message_peek_fields(m, ri, 4, 4, &q);
        if (r < 0)
                return r;

        if (ret)
                *ret = BUS_MESSAGE_BSWAP32(m, *(uint32_t*) q);

        return 0;
}

static int message_peek_field_string(
                sd_bus_message *m,
                bool (*validate)(const char *p),
                size_t *ri,
                const char **ret) {

        uint32_t l;
        int r;
        void *q;

        assert(m);
        assert(ri);

        r = message_peek_field_uint32(m, ri, &l);
        if (r < 0)
                return r;

        r = message_peek_fields(m, ri, 1, l+1, &q);
        if (r < 0)
                return r;

        if (validate) {
                if (!validate_nul(q, l))
                        return -EBADMSG;

                if (!validate(q))
                        return -EBADMSG;
        } else {
                if (!validate_string(q, l))
                        return -EBADMSG;
        }

        if (ret)
                *ret = q;

        return 0;
}

static int message_peek_field_signature(
                sd_bus_message *m,
                size_t *ri,
                const char **ret) {

        size_t l;
        int r;
        void *q;

        assert(m);
        assert(ri);

        r = message_peek_fields(m, ri, 1, 1, &q);
        if (r < 0)
                return r;

        l = *(uint8_t*) q;
        r = message_peek_fields(m, ri, 1, l+1, &q);
        if (r < 0)
                return r;

        if (!validate_signature(q, l))
                return -EBADMSG;

        if (ret)
                *ret = q;

        return 0;
}

static int message_skip_fields(
                sd_bus_message *m,
                size_t *ri,
                uint32_t array_size,
                const char **signature) {

        size_t original_index;
        int r;

        assert(m);
        assert(ri);
        assert(signature);

        original_index = *ri;

        for (;;) {
                char t;
                size_t l;

                if (array_size != (uint32_t) -1 &&
                    array_size <= *ri - original_index)
                        return 0;

                t = **signature;
                if (!t)
                        return 0;

                if (t == SD_BUS_TYPE_STRING) {

                        r = message_peek_field_string(m, NULL, ri, NULL);
                        if (r < 0)
                                return r;

                        (*signature)++;

                } else if (t == SD_BUS_TYPE_OBJECT_PATH) {

                        r = message_peek_field_string(m, object_path_is_valid, ri, NULL);
                        if (r < 0)
                                return r;

                        (*signature)++;

                } else if (t == SD_BUS_TYPE_SIGNATURE) {

                        r = message_peek_field_signature(m, ri, NULL);
                        if (r < 0)
                                return r;

                        (*signature)++;

                } else if (bus_type_is_basic(t)) {
                        size_t align, k;

                        align = bus_type_get_alignment(t);
                        k = bus_type_get_size(t);

                        r = message_peek_fields(m, ri, align, k, NULL);
                        if (r < 0)
                                return r;

                        (*signature)++;

                } else if (t == SD_BUS_TYPE_ARRAY) {

                        r = signature_element_length(*signature+1, &l);
                        if (r < 0)
                                return r;

                        assert(l >= 1);
                        {
                                char sig[l-1], *s;
                                uint32_t nas;
                                int alignment;

                                strncpy(sig, *signature + 1, l-1);
                                s = sig;

                                alignment = bus_type_get_alignment(sig[0]);
                                if (alignment < 0)
                                        return alignment;

                                r = message_peek_field_uint32(m, ri, &nas);
                                if (r < 0)
                                        return r;
                                if (nas > BUS_ARRAY_MAX_SIZE)
                                        return -EBADMSG;

                                r = message_peek_fields(m, ri, alignment, 0, NULL);
                                if (r < 0)
                                        return r;

                                r = message_skip_fields(m, ri, nas, (const char**) &s);
                                if (r < 0)
                                        return r;
                        }

                        (*signature) += 1 + l;

                } else if (t == SD_BUS_TYPE_VARIANT) {
                        const char *s;

                        r = message_peek_field_signature(m, ri, &s);
                        if (r < 0)
                                return r;

                        r = message_skip_fields(m, ri, (uint32_t) -1, (const char**) &s);
                        if (r < 0)
                                return r;

                        (*signature)++;

                } else if (t == SD_BUS_TYPE_STRUCT ||
                           t == SD_BUS_TYPE_DICT_ENTRY) {

                        r = signature_element_length(*signature, &l);
                        if (r < 0)
                                return r;

                        assert(l >= 2);
                        {
                                char sig[l-1], *s;
                                strncpy(sig, *signature + 1, l-1);
                                s = sig;

                                r = message_skip_fields(m, ri, (uint32_t) -1, (const char**) &s);
                                if (r < 0)
                                        return r;
                        }

                        *signature += l;
                } else
                        return -EINVAL;
        }
}

static int message_parse_fields(sd_bus_message *m) {
        size_t ri;
        int r;

        assert(m);

        for (ri = 0; ri < BUS_MESSAGE_FIELDS_SIZE(m); ) {
                const char *signature;
                uint8_t *header;

                r = message_peek_fields(m, &ri, 8, 1, (void**) &header);
                if (r < 0)
                        return r;

                r = message_peek_field_signature(m, &ri, &signature);
                if (r < 0)
                        return r;

                switch (*header) {
                case _SD_BUS_MESSAGE_HEADER_INVALID:
                        return -EBADMSG;

                case SD_BUS_MESSAGE_HEADER_PATH:
                        if (!streq(signature, "o"))
                                return -EBADMSG;

                        r = message_peek_field_string(m, object_path_is_valid, &ri, &m->path);
                        break;

                case SD_BUS_MESSAGE_HEADER_INTERFACE:
                        if (!streq(signature, "s"))
                                return -EBADMSG;

                        r = message_peek_field_string(m, interface_name_is_valid, &ri, &m->interface);
                        break;

                case SD_BUS_MESSAGE_HEADER_MEMBER:
                        if (!streq(signature, "s"))
                                return -EBADMSG;

                        r = message_peek_field_string(m, member_name_is_valid, &ri, &m->member);
                        break;

                case SD_BUS_MESSAGE_HEADER_ERROR_NAME:
                        if (!streq(signature, "s"))
                                return -EBADMSG;

                        r = message_peek_field_string(m, error_name_is_valid, &ri, &m->error.name);
                        break;

                case SD_BUS_MESSAGE_HEADER_DESTINATION:
                        if (!streq(signature, "s"))
                                return -EBADMSG;

                        r = message_peek_field_string(m, service_name_is_valid, &ri, &m->destination);
                        break;

                case SD_BUS_MESSAGE_HEADER_SENDER:
                        if (!streq(signature, "s"))
                                return -EBADMSG;

                        r = message_peek_field_string(m, service_name_is_valid, &ri, &m->sender);
                        break;


                case SD_BUS_MESSAGE_HEADER_SIGNATURE: {
                        const char *s;
                        char *c;

                        if (!streq(signature, "g"))
                                return -EBADMSG;

                        r = message_peek_field_signature(m, &ri, &s);
                        if (r < 0)
                                return r;

                        c = strdup(s);
                        if (!c)
                                return -ENOMEM;

                        free(m->root_container.signature);
                        m->root_container.signature = c;
                        break;
                }

                case SD_BUS_MESSAGE_HEADER_REPLY_SERIAL:
                        if (!streq(signature, "u"))
                                return -EBADMSG;

                        r = message_peek_field_uint32(m, &ri, &m->reply_serial);
                        if (r < 0)
                                return r;

                        if (m->reply_serial == 0)
                                return -EBADMSG;

                        break;

                default:
                        r = message_skip_fields(m, &ri, (uint32_t) -1, (const char **) &signature);
                }

                if (r < 0)
                        return r;
        }

        if (isempty(m->root_container.signature) != (BUS_MESSAGE_BODY_SIZE(m) == 0))
                return -EBADMSG;

        switch (m->header->type) {

        case SD_BUS_MESSAGE_TYPE_SIGNAL:
                if (!m->path || !m->interface || !m->member)
                        return -EBADMSG;
                break;

        case SD_BUS_MESSAGE_TYPE_METHOD_CALL:

                if (!m->path || !m->member)
                        return -EBADMSG;

                break;

        case SD_BUS_MESSAGE_TYPE_METHOD_RETURN:

                if (m->reply_serial == 0)
                        return -EBADMSG;
                break;

        case SD_BUS_MESSAGE_TYPE_METHOD_ERROR:

                if (m->reply_serial == 0 || !m->error.name)
                        return -EBADMSG;
                break;
        }

        /* Try to read the error message, but if we can't it's a non-issue */
        if (m->header->type == SD_BUS_MESSAGE_TYPE_METHOD_ERROR)
                sd_bus_message_read(m, "s", &m->error.message);

        return 0;
}

static void setup_iovec(sd_bus_message *m) {
        assert(m);
        assert(m->sealed);

        m->n_iovec = 0;
        m->size = 0;

        m->iovec[m->n_iovec].iov_base = m->header;
        m->iovec[m->n_iovec].iov_len = sizeof(*m->header);
        m->size += m->iovec[m->n_iovec].iov_len;
        m->n_iovec++;

        if (m->fields) {
                m->iovec[m->n_iovec].iov_base = m->fields;
                m->iovec[m->n_iovec].iov_len = m->header->fields_size;
                m->size += m->iovec[m->n_iovec].iov_len;
                m->n_iovec++;

                if (m->header->fields_size % 8 != 0) {
                        static const uint8_t padding[7] = { 0, 0, 0, 0, 0, 0, 0 };

                        m->iovec[m->n_iovec].iov_base = (void*) padding;
                        m->iovec[m->n_iovec].iov_len = 8 - m->header->fields_size % 8;
                        m->size += m->iovec[m->n_iovec].iov_len;
                        m->n_iovec++;
                }
        }

        if (m->body) {
                m->iovec[m->n_iovec].iov_base = m->body;
                m->iovec[m->n_iovec].iov_len = m->header->body_size;
                m->size += m->iovec[m->n_iovec].iov_len;
                m->n_iovec++;
        }
}

int bus_message_seal(sd_bus_message *m, uint64_t serial) {
        int r;

        assert(m);

        if (m->sealed)
                return -EPERM;

        if (m->n_containers > 0)
                return -EBADMSG;

        /* If there's a non-trivial signature set, then add it in here */
        if (!isempty(m->root_container.signature)) {
                r = message_append_field_signature(m, SD_BUS_MESSAGE_HEADER_SIGNATURE, m->root_container.signature, NULL);
                if (r < 0)
                        return r;
        }

        if (m->n_fds > 0) {
                r = message_append_field_uint32(m, SD_BUS_MESSAGE_HEADER_UNIX_FDS, m->n_fds);
                if (r < 0)
                        return r;
        }

        m->header->serial = serial;
        m->sealed = true;

        setup_iovec(m);

        return 0;
}

int sd_bus_message_set_destination(sd_bus_message *m, const char *destination) {
        if (!m)
                return -EINVAL;
        if (!destination)
                return -EINVAL;
        if (m->sealed)
                return -EPERM;
        if (m->destination)
                return -EEXIST;

        return message_append_field_string(m, SD_BUS_MESSAGE_HEADER_DESTINATION, SD_BUS_TYPE_STRING, destination, &m->destination);
}

int bus_message_dump(sd_bus_message *m) {
        unsigned level = 1;
        int r;

        assert(m);

        printf("Message %p\n"
               "\tn_ref=%u\n"
               "\tendian=%c\n"
               "\ttype=%i\n"
               "\tflags=%u\n"
               "\tversion=%u\n"
               "\tserial=%u\n"
               "\tfields_size=%u\n"
               "\tbody_size=%u\n"
               "\tpath=%s\n"
               "\tinterface=%s\n"
               "\tmember=%s\n"
               "\tdestination=%s\n"
               "\tsender=%s\n"
               "\tsignature=%s\n"
               "\treply_serial=%u\n"
               "\terror.name=%s\n"
               "\terror.message=%s\n"
               "\tsealed=%s\n",
               m,
               m->n_ref,
               m->header->endian,
               m->header->type,
               m->header->flags,
               m->header->version,
               BUS_MESSAGE_SERIAL(m),
               BUS_MESSAGE_FIELDS_SIZE(m),
               BUS_MESSAGE_BODY_SIZE(m),
               strna(m->path),
               strna(m->interface),
               strna(m->member),
               strna(m->destination),
               strna(m->sender),
               strna(m->root_container.signature),
               m->reply_serial,
               strna(m->error.name),
               strna(m->error.message),
               yes_no(m->sealed));

        r = sd_bus_message_rewind(m, true);
        if (r < 0) {
                log_error("Failed to rewind: %s", strerror(-r));
                return r;
        }

        printf("BEGIN_MESSAGE \"%s\" {\n", strempty(m->root_container.signature));

        for(;;) {
                _cleanup_free_ char *prefix = NULL;
                const char *contents = NULL;
                char type;
                union {
                        uint8_t u8;
                        uint16_t u16;
                        int16_t s16;
                        uint32_t u32;
                        int32_t s32;
                        uint64_t u64;
                        int64_t s64;
                        double d64;
                        const char *string;
                        int i;
                } basic;

                r = sd_bus_message_peek_type(m, &type, &contents);
                if (r < 0) {
                        log_error("Failed to peek type: %s", strerror(-r));
                        return r;
                }
                if (r == 0) {
                        if (level <= 1)
                                break;

                        r = sd_bus_message_exit_container(m);
                        if (r < 0) {
                                log_error("Failed to exit container: %s", strerror(-r));
                                return r;
                        }

                        level--;

                        prefix = strrep("\t", level);
                        if (!prefix)
                                return log_oom();

                        if (type == SD_BUS_TYPE_ARRAY)
                                printf("%s} END_ARRAY \n", prefix);
                        else if (type == SD_BUS_TYPE_VARIANT)
                                printf("%s} END_VARIANT\n", prefix);
                        else if (type == SD_BUS_TYPE_STRUCT)
                                printf("%s} END_STRUCT\n", prefix);
                        else if (type == SD_BUS_TYPE_DICT_ENTRY)
                                printf("%s} END_DICT_ENTRY\n", prefix);

                        continue;
                }

                prefix = strrep("\t", level);
                if (!prefix)
                        return log_oom();

                if (bus_type_is_container(type) > 0) {
                        r = sd_bus_message_enter_container(m, type, contents);
                        if (r < 0) {
                                log_error("Failed to enter container: %s", strerror(-r));
                                return r;
                        }

                        if (type == SD_BUS_TYPE_ARRAY)
                                printf("%sBEGIN_ARRAY \"%s\" {\n", prefix, contents);
                        else if (type == SD_BUS_TYPE_VARIANT)
                                printf("%sBEGIN_VARIANT \"%s\" {\n", prefix, contents);
                        else if (type == SD_BUS_TYPE_STRUCT)
                                printf("%sBEGIN_STRUCT \"%s\" {\n", prefix, contents);
                        else if (type == SD_BUS_TYPE_DICT_ENTRY)
                                printf("%sBEGIN_DICT_ENTRY \"%s\" {\n", prefix, contents);

                        level ++;

                        continue;
                }

                r = sd_bus_message_read_basic(m, type, &basic);
                if (r < 0) {
                        log_error("Failed to get basic: %s", strerror(-r));
                        return r;
                }

                switch (type) {

                case SD_BUS_TYPE_BYTE:
                        printf("%sBYTE: %u\n", prefix, basic.u8);
                        break;

                case SD_BUS_TYPE_BOOLEAN:
                        printf("%sBOOLEAN: %s\n", prefix, yes_no(basic.i));
                        break;

                case SD_BUS_TYPE_INT16:
                        printf("%sINT16: %i\n", prefix, basic.s16);
                        break;

                case SD_BUS_TYPE_UINT16:
                        printf("%sUINT16: %u\n", prefix, basic.u16);
                        break;

                case SD_BUS_TYPE_INT32:
                        printf("%sINT32: %i\n", prefix, basic.s32);
                        break;

                case SD_BUS_TYPE_UINT32:
                        printf("%sUINT32: %u\n", prefix, basic.u32);
                        break;

                case SD_BUS_TYPE_INT64:
                        printf("%sINT64: %lli\n", prefix, (long long) basic.s64);
                        break;

                case SD_BUS_TYPE_UINT64:
                        printf("%sUINT64: %llu\n", prefix, (unsigned long long) basic.u64);
                        break;

                case SD_BUS_TYPE_DOUBLE:
                        printf("%sDOUBLE: %g\n", prefix, basic.d64);
                        break;

                case SD_BUS_TYPE_STRING:
                        printf("%sSTRING: \"%s\"\n", prefix, basic.string);
                        break;

                case SD_BUS_TYPE_OBJECT_PATH:
                        printf("%sOBJECT_PATH: \"%s\"\n", prefix, basic.string);
                        break;

                case SD_BUS_TYPE_SIGNATURE:
                        printf("%sSIGNATURE: \"%s\"\n", prefix, basic.string);
                        break;

                case SD_BUS_TYPE_UNIX_FD:
                        printf("%sUNIX_FD: %i\n", prefix, basic.i);
                        break;

                default:
                        assert_not_reached("Unknown basic type.");
                }
        }

        printf("} END_MESSAGE\n");
        return 0;
}

int bus_message_get_blob(sd_bus_message *m, void **buffer, size_t *sz) {
        size_t total;
        unsigned i;
        void *p, *e;

        assert(m);
        assert(buffer);
        assert(sz);

        for (i = 0, total = 0; i < m->n_iovec; i++)
                total += m->iovec[i].iov_len;

        p = malloc(total);
        if (!p)
                return -ENOMEM;

        for (i = 0, e = p; i < m->n_iovec; i++)
                e = mempcpy(e, m->iovec[i].iov_base, m->iovec[i].iov_len);

        *buffer = p;
        *sz = total;

        return 0;
}

int bus_message_read_strv_extend(sd_bus_message *m, char ***l) {
        int r;

        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "s");
        if (r < 0)
                return r;

        for (;;) {
                const char *s;

                r = sd_bus_message_read_basic(m, 's', &s);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = strv_extend(l, s);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}
