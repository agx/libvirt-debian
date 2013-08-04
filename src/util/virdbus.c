/*
 * virdbus.c: helper for using DBus
 *
 * Copyright (C) 2012-2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "virdbuspriv.h"
#include "viralloc.h"
#include "virerror.h"
#include "virlog.h"
#include "virthread.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_DBUS

#ifdef WITH_DBUS

static DBusConnection *systembus = NULL;
static DBusConnection *sessionbus = NULL;
static virOnceControl systemonce = VIR_ONCE_CONTROL_INITIALIZER;
static virOnceControl sessiononce = VIR_ONCE_CONTROL_INITIALIZER;
static DBusError systemdbuserr;
static DBusError sessiondbuserr;

static dbus_bool_t virDBusAddWatch(DBusWatch *watch, void *data);
static void virDBusRemoveWatch(DBusWatch *watch, void *data);
static void virDBusToggleWatch(DBusWatch *watch, void *data);

static DBusConnection *virDBusBusInit(DBusBusType type, DBusError *dbuserr)
{
    DBusConnection *bus;

    /* Allocate and initialize a new HAL context */
    dbus_connection_set_change_sigpipe(FALSE);
    dbus_threads_init_default();

    dbus_error_init(dbuserr);
    if (!(bus = dbus_bus_get(type, dbuserr)))
        return NULL;

    dbus_connection_set_exit_on_disconnect(bus, FALSE);

    /* Register dbus watch callbacks */
    if (!dbus_connection_set_watch_functions(bus,
                                             virDBusAddWatch,
                                             virDBusRemoveWatch,
                                             virDBusToggleWatch,
                                             bus, NULL)) {
        return NULL;
    }
    return bus;
}

static void virDBusSystemBusInit(void)
{
    systembus = virDBusBusInit(DBUS_BUS_SYSTEM, &systemdbuserr);
}

DBusConnection *virDBusGetSystemBus(void)
{
    if (virOnce(&systemonce, virDBusSystemBusInit) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to run one time DBus initializer"));
        return NULL;
    }

    if (!systembus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to get DBus system bus connection: %s"),
                       systemdbuserr.message ? systemdbuserr.message : "watch setup failed");
        return NULL;
    }

    return systembus;
}


static void virDBusSessionBusInit(void)
{
    sessionbus = virDBusBusInit(DBUS_BUS_SESSION, &sessiondbuserr);
}

DBusConnection *virDBusGetSessionBus(void)
{
    if (virOnce(&sessiononce, virDBusSessionBusInit) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to run one time DBus initializer"));
        return NULL;
    }

    if (!sessionbus) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to get DBus session bus connection: %s"),
                       sessiondbuserr.message ? sessiondbuserr.message : "watch setup failed");
        return NULL;
    }

    return sessionbus;
}

struct virDBusWatch
{
    int watch;
    DBusConnection *bus;
};

static void virDBusWatchCallback(int fdatch ATTRIBUTE_UNUSED,
                                 int fd ATTRIBUTE_UNUSED,
                                 int events, void *opaque)
{
    DBusWatch *watch = opaque;
    struct virDBusWatch *info;
    int dbus_flags = 0;

    info = dbus_watch_get_data(watch);

    if (events & VIR_EVENT_HANDLE_READABLE)
        dbus_flags |= DBUS_WATCH_READABLE;
    if (events & VIR_EVENT_HANDLE_WRITABLE)
        dbus_flags |= DBUS_WATCH_WRITABLE;
    if (events & VIR_EVENT_HANDLE_ERROR)
        dbus_flags |= DBUS_WATCH_ERROR;
    if (events & VIR_EVENT_HANDLE_HANGUP)
        dbus_flags |= DBUS_WATCH_HANGUP;

    (void)dbus_watch_handle(watch, dbus_flags);

    while (dbus_connection_dispatch(info->bus) == DBUS_DISPATCH_DATA_REMAINS)
        /* keep dispatching while data remains */;
}


static int virDBusTranslateWatchFlags(int dbus_flags)
{
    unsigned int flags = 0;
    if (dbus_flags & DBUS_WATCH_READABLE)
        flags |= VIR_EVENT_HANDLE_READABLE;
    if (dbus_flags & DBUS_WATCH_WRITABLE)
        flags |= VIR_EVENT_HANDLE_WRITABLE;
    if (dbus_flags & DBUS_WATCH_ERROR)
        flags |= VIR_EVENT_HANDLE_ERROR;
    if (dbus_flags & DBUS_WATCH_HANGUP)
        flags |= VIR_EVENT_HANDLE_HANGUP;
    return flags;
}


static void virDBusWatchFree(void *data) {
    struct virDBusWatch *info = data;
    VIR_FREE(info);
}

static dbus_bool_t virDBusAddWatch(DBusWatch *watch,
                                   void *data)
{
    int flags = 0;
    int fd;
    struct virDBusWatch *info;

    if (VIR_ALLOC(info) < 0)
        return 0;

    if (dbus_watch_get_enabled(watch))
        flags = virDBusTranslateWatchFlags(dbus_watch_get_flags(watch));

# if HAVE_DBUS_WATCH_GET_UNIX_FD
    fd = dbus_watch_get_unix_fd(watch);
# else
    fd = dbus_watch_get_fd(watch);
# endif
    info->bus = (DBusConnection *)data;
    info->watch = virEventAddHandle(fd, flags,
                                    virDBusWatchCallback,
                                    watch, NULL);
    if (info->watch < 0) {
        VIR_FREE(info);
        return 0;
    }
    dbus_watch_set_data(watch, info, virDBusWatchFree);

    return 1;
}


static void virDBusRemoveWatch(DBusWatch *watch,
                               void *data ATTRIBUTE_UNUSED)
{
    struct virDBusWatch *info;

    info = dbus_watch_get_data(watch);

    (void)virEventRemoveHandle(info->watch);
}


static void virDBusToggleWatch(DBusWatch *watch,
                               void *data ATTRIBUTE_UNUSED)
{
    int flags = 0;
    struct virDBusWatch *info;

    if (dbus_watch_get_enabled(watch))
        flags = virDBusTranslateWatchFlags(dbus_watch_get_flags(watch));

    info = dbus_watch_get_data(watch);

    (void)virEventUpdateHandle(info->watch, flags);
}

# define VIR_DBUS_TYPE_STACK_MAX_DEPTH 32

static const char virDBusBasicTypes[] = {
    DBUS_TYPE_BYTE,
    DBUS_TYPE_BOOLEAN,
    DBUS_TYPE_INT16,
    DBUS_TYPE_UINT16,
    DBUS_TYPE_INT32,
    DBUS_TYPE_UINT32,
    DBUS_TYPE_INT64,
    DBUS_TYPE_UINT64,
    DBUS_TYPE_DOUBLE,
    DBUS_TYPE_STRING,
    DBUS_TYPE_OBJECT_PATH,
    DBUS_TYPE_SIGNATURE,
};

static bool virDBusIsBasicType(char c) {
    return !!memchr(virDBusBasicTypes, c, ARRAY_CARDINALITY(virDBusBasicTypes));
}

/*
 * All code related to virDBusMessageIterEncode and
 * virDBusMessageIterDecode is derived from systemd
 * bus_message_append_ap()/message_read_ap() in
 * bus-message.c under the terms of the LGPLv2+
 */
static int
virDBusSignatureLengthInternal(const char *s,
                               bool allowDict,
                               unsigned arrayDepth,
                               unsigned structDepth,
                               size_t *l)
{
    if (virDBusIsBasicType(*s) || *s == DBUS_TYPE_VARIANT) {
        *l = 1;
        return 0;
    }

    if (*s == DBUS_TYPE_ARRAY) {
        size_t t;

        if (arrayDepth >= VIR_DBUS_TYPE_STACK_MAX_DEPTH) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Signature '%s' too deeply nested"),
                           s);
            return -1;
        }

        if (virDBusSignatureLengthInternal(s + 1,
                                           true,
                                           arrayDepth + 1,
                                           structDepth,
                                           &t) < 0)
            return -1;

        *l = t + 1;
        return 0;
    }

    if (*s == DBUS_STRUCT_BEGIN_CHAR) {
        const char *p = s + 1;

        if (structDepth >= VIR_DBUS_TYPE_STACK_MAX_DEPTH) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Signature '%s' too deeply nested"),
                           s);
            return -1;
        }

        while (*p != DBUS_STRUCT_END_CHAR) {
            size_t t;

            if (virDBusSignatureLengthInternal(p,
                                               false,
                                               arrayDepth,
                                               structDepth + 1,
                                               &t) < 0)
                return -1;

            p += t;
        }

        *l = p - s + 1;
        return 0;
    }

    if (*s == DBUS_DICT_ENTRY_BEGIN_CHAR && allowDict) {
        const char *p = s + 1;
        unsigned n = 0;
        if (structDepth >= VIR_DBUS_TYPE_STACK_MAX_DEPTH) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Signature '%s' too deeply nested"),
                           s);
            return -1;
        }

        while (*p != DBUS_DICT_ENTRY_END_CHAR) {
            size_t t;

            if (n == 0 && !virDBusIsBasicType(*p)) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Dict entry in signature '%s' must be a basic type"),
                               s);
                return -1;
            }

            if (virDBusSignatureLengthInternal(p,
                                               false,
                                               arrayDepth,
                                               structDepth + 1,
                                               &t) < 0)
                return -1;

            p += t;
            n++;
        }

        if (n != 2) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Dict entry in signature '%s' is wrong size"),
                           s);
            return -1;
        }

        *l = p - s + 1;
        return 0;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Unexpected signature '%s'"), s);
    return -1;
}


static int virDBusSignatureLength(const char *s, size_t *l)
{
    return virDBusSignatureLengthInternal(s, true, 0, 0, l);
}



/* Ideally, we'd just call ourselves recursively on every
 * complex type. However, the state of a va_list that is
 * passed to a function is undefined after that function
 * returns. This means we need to decode the va_list linearly
 * in a single stackframe. We hence implement our own
 * home-grown stack in an array. */

typedef struct _virDBusTypeStack virDBusTypeStack;
struct _virDBusTypeStack {
    const char *types;
    size_t nstruct;
    size_t narray;
    DBusMessageIter *iter;
};

static int virDBusTypeStackPush(virDBusTypeStack **stack,
                                size_t *nstack,
                                DBusMessageIter *iter,
                                const char *types,
                                size_t nstruct,
                                size_t narray)
{
    if (*nstack >= VIR_DBUS_TYPE_STACK_MAX_DEPTH) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DBus type too deeply nested"));
        return -1;
    }

    if (VIR_EXPAND_N(*stack, *nstack, 1) < 0)
        return -1;

    (*stack)[(*nstack) - 1].iter = iter;
    (*stack)[(*nstack) - 1].types = types;
    (*stack)[(*nstack) - 1].nstruct = nstruct;
    (*stack)[(*nstack) - 1].narray = narray;
    VIR_DEBUG("Pushed '%s'", types);
    return 0;
}


static int virDBusTypeStackPop(virDBusTypeStack **stack,
                               size_t *nstack,
                               DBusMessageIter **iter,
                               const char **types,
                               size_t *nstruct,
                               size_t *narray)
{
    if (*nstack == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DBus type stack is empty"));
        return -1;
    }

    *iter = (*stack)[(*nstack) - 1].iter;
    *types = (*stack)[(*nstack) - 1].types;
    *nstruct = (*stack)[(*nstack) - 1].nstruct;
    *narray = (*stack)[(*nstack) - 1].narray;
    VIR_DEBUG("Popped '%s'", *types);
    VIR_SHRINK_N(*stack, *nstack, 1);

    return 0;
}


static void virDBusTypeStackFree(virDBusTypeStack **stack,
                                 size_t *nstack)
{
    size_t i;
    /* The iter in the first level of the stack is the
     * root iter which must not be freed
     */
    for (i = 1; i < *nstack; i++) {
        VIR_FREE((*stack)[i].iter);
    }
    VIR_FREE(*stack);
}


# define SET_NEXT_VAL(dbustype, vargtype, sigtype, fmt)                 \
    do {                                                                \
        dbustype x = (dbustype)va_arg(args, vargtype);                  \
        if (!dbus_message_iter_append_basic(iter, sigtype, &x)) {       \
            virReportError(VIR_ERR_INTERNAL_ERROR,                      \
                           _("Cannot append basic type %s"), #vargtype); \
            goto cleanup;                                               \
        }                                                               \
        VIR_DEBUG("Appended basic type '" #dbustype "' varg '" #vargtype \
                  "' sig '%c' val '" fmt "'", sigtype, (vargtype)x);    \
    } while (0)

static int
virDBusMessageIterEncode(DBusMessageIter *rootiter,
                         const char *types,
                         va_list args)
{
    int ret = -1;
    size_t narray;
    size_t nstruct;
    virDBusTypeStack *stack = NULL;
    size_t nstack = 0;
    size_t siglen;
    char *contsig = NULL;
    const char *vsig;
    DBusMessageIter *newiter = NULL;
    DBusMessageIter *iter = rootiter;

    VIR_DEBUG("rootiter=%p types=%s", rootiter, types);

    if (!types)
        return 0;

    narray = (size_t)-1;
    nstruct = strlen(types);

    for (;;) {
        const char *t;

        VIR_DEBUG("Loop stack=%zu array=%zu struct=%zu type='%s'",
                  nstack, narray, nstruct, types);
        if (narray == 0 ||
            (narray == (size_t)-1 &&
             nstruct == 0)) {
            DBusMessageIter *thisiter = iter;
            VIR_DEBUG("Popping iter=%p", iter);
            if (nstack == 0)
                break;
            if (virDBusTypeStackPop(&stack, &nstack, &iter,
                                    &types, &nstruct, &narray) < 0)
                goto cleanup;
            VIR_DEBUG("Popped iter=%p", iter);

            if (!dbus_message_iter_close_container(iter, thisiter)) {
                if (thisiter != rootiter)
                    VIR_FREE(thisiter);
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Cannot close container iterator"));
                goto cleanup;
            }
            if (thisiter != rootiter)
                VIR_FREE(thisiter);
            continue;
        }

        t = types;
        if (narray != (size_t)-1) {
            narray--;
        } else {
            types++;
            nstruct--;
        }

        switch (*t) {
        case DBUS_TYPE_BYTE:
            SET_NEXT_VAL(unsigned char, int, *t, "%d");
            break;

        case DBUS_TYPE_BOOLEAN:
            SET_NEXT_VAL(dbus_bool_t, int, *t, "%d");
            break;

        case DBUS_TYPE_INT16:
            SET_NEXT_VAL(dbus_int16_t, int, *t, "%d");
            break;

        case DBUS_TYPE_UINT16:
            SET_NEXT_VAL(dbus_uint16_t, unsigned int, *t, "%d");
            break;

        case DBUS_TYPE_INT32:
            SET_NEXT_VAL(dbus_int32_t, int, *t, "%d");
            break;

        case DBUS_TYPE_UINT32:
            SET_NEXT_VAL(dbus_uint32_t, unsigned int, *t, "%u");
            break;

        case DBUS_TYPE_INT64:
            SET_NEXT_VAL(dbus_int64_t, long long, *t, "%lld");
            break;

        case DBUS_TYPE_UINT64:
            SET_NEXT_VAL(dbus_uint64_t, unsigned long long, *t, "%llu");
            break;

        case DBUS_TYPE_DOUBLE:
            SET_NEXT_VAL(double, double, *t, "%lf");
            break;

        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
            SET_NEXT_VAL(char *, char *, *t, "%s");
            break;

        case DBUS_TYPE_ARRAY:
            if (virDBusSignatureLength(t + 1, &siglen) < 0)
                goto cleanup;

            if (VIR_STRNDUP(contsig, t + 1, siglen) < 0)
                goto cleanup;

            if (narray == (size_t)-1) {
                types += siglen;
                nstruct -= siglen;
            }

            if (VIR_ALLOC(newiter) < 0)
                goto cleanup;
            VIR_DEBUG("Contsig '%s' '%zu'", contsig, siglen);
            if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                                                  contsig, newiter))
                goto cleanup;
            if (virDBusTypeStackPush(&stack, &nstack,
                                     iter, types,
                                     nstruct, narray) < 0)
                goto cleanup;
            VIR_FREE(contsig);
            iter = newiter;
            newiter = NULL;
            types = t + 1;
            nstruct = siglen;
            narray = va_arg(args, int);
            break;

        case DBUS_TYPE_VARIANT:
            vsig = va_arg(args, const char *);
            if (!vsig) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Missing variant type signature"));
                goto cleanup;
            }
            if (VIR_ALLOC(newiter) < 0)
                goto cleanup;
            if (!dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
                                                  vsig, newiter))
                goto cleanup;
            if (virDBusTypeStackPush(&stack, &nstack,
                                     iter, types,
                                     nstruct, narray) < 0)
                goto cleanup;
            iter = newiter;
            newiter = NULL;
            types = vsig;
            nstruct = strlen(types);
            narray = (size_t)-1;
            break;

        case DBUS_STRUCT_BEGIN_CHAR:
        case DBUS_DICT_ENTRY_BEGIN_CHAR:
            if (virDBusSignatureLength(t, &siglen) < 0)
                goto cleanup;

            if (VIR_STRNDUP(contsig, t + 1, siglen - 1) < 0)
                goto cleanup;

            if (VIR_ALLOC(newiter) < 0)
                goto cleanup;
            VIR_DEBUG("Contsig '%s' '%zu'", contsig, siglen);
            if (!dbus_message_iter_open_container(iter,
                                                  *t == DBUS_STRUCT_BEGIN_CHAR ?
                                                  DBUS_TYPE_STRUCT : DBUS_TYPE_DICT_ENTRY,
                                                  NULL, newiter))
                goto cleanup;
            if (narray == (size_t)-1) {
                types += siglen - 1;
                nstruct -= siglen - 1;
            }

            if (virDBusTypeStackPush(&stack, &nstack,
                                     iter, types,
                                     nstruct, narray) < 0)
                goto cleanup;
            VIR_FREE(contsig);
            iter = newiter;
            newiter = NULL;
            types = t + 1;
            nstruct = siglen - 2;
            narray = (size_t)-1;

            break;

        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown type in signature '%s'"),
                           types);
        }
    }

    ret = 0;

cleanup:
    virDBusTypeStackFree(&stack, &nstack);
    VIR_FREE(contsig);
    VIR_FREE(newiter);
    return ret;
}
# undef SET_NEXT_VAL


# define GET_NEXT_VAL(dbustype, vargtype, fmt)                          \
    do {                                                                \
        dbustype *x = (dbustype *)va_arg(args, vargtype *);             \
        dbus_message_iter_get_basic(iter, x);                           \
        VIR_DEBUG("Read basic type '" #dbustype "' varg '" #vargtype    \
                  "' val '" fmt "'", (vargtype)*x);                     \
    } while (0)


static int
virDBusMessageIterDecode(DBusMessageIter *rootiter,
                         const char *types,
                         va_list args)
{
    int ret = -1;
    size_t narray;
    size_t nstruct;
    virDBusTypeStack *stack = NULL;
    size_t nstack = 0;
    size_t siglen;
    char *contsig = NULL;
    const char *vsig;
    DBusMessageIter *newiter = NULL;
    DBusMessageIter *iter = rootiter;

    VIR_DEBUG("rootiter=%p types=%s", rootiter, types);

    if (!types)
        return 0;

    narray = (size_t)-1;
    nstruct = strlen(types);

    for (;;) {
        const char *t;
        bool advanceiter = true;

        VIR_DEBUG("Loop stack=%zu array=%zu struct=%zu type='%s'",
                  nstack, narray, nstruct, types);
        if (narray == 0 ||
            (narray == (size_t)-1 &&
             nstruct == 0)) {
            DBusMessageIter *thisiter = iter;
            VIR_DEBUG("Popping iter=%p", iter);
            if (nstack == 0)
                break;
            if (virDBusTypeStackPop(&stack, &nstack, &iter,
                                    &types, &nstruct, &narray) < 0)
                goto cleanup;
            VIR_DEBUG("Popped iter=%p types=%s", iter, types);
            if (thisiter != rootiter)
                VIR_FREE(thisiter);
            if (!(narray == 0 ||
                  (narray == (size_t)-1 &&
                   nstruct == 0)) &&
                !dbus_message_iter_next(iter)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Not enough fields in message for signature"));
                goto cleanup;
            }
            continue;
        }

        t = types;
        if (narray != (size_t)-1) {
            narray--;
        } else {
            types++;
            nstruct--;
        }

        switch (*t) {
        case DBUS_TYPE_BYTE:
            GET_NEXT_VAL(unsigned char, unsigned char, "%d");
            break;

        case DBUS_TYPE_BOOLEAN:
            GET_NEXT_VAL(dbus_bool_t, int, "%d");
            break;

        case DBUS_TYPE_INT16:
            GET_NEXT_VAL(dbus_int16_t, short, "%d");
            break;

        case DBUS_TYPE_UINT16:
            GET_NEXT_VAL(dbus_uint16_t, unsigned short, "%d");
            break;

        case DBUS_TYPE_INT32:
            GET_NEXT_VAL(dbus_uint32_t, int, "%d");
            break;

        case DBUS_TYPE_UINT32:
            GET_NEXT_VAL(dbus_uint32_t, unsigned int, "%u");
            break;

        case DBUS_TYPE_INT64:
            GET_NEXT_VAL(dbus_uint64_t, long long, "%lld");
            break;

        case DBUS_TYPE_UINT64:
            GET_NEXT_VAL(dbus_uint64_t, unsigned long long, "%llu");
            break;

        case DBUS_TYPE_DOUBLE:
            GET_NEXT_VAL(double, double, "%lf");
            break;

        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
            do {
                char **x = (char **)va_arg(args, char **);
                char *s;
                dbus_message_iter_get_basic(iter, &s);
                if (VIR_STRDUP(*x, s) < 0)
                    goto cleanup;
                VIR_DEBUG("Read basic type 'char *' varg 'char **'"
                          "' val '%s'", *x);
            } while (0);
            break;

        case DBUS_TYPE_ARRAY:
            advanceiter = false;
            if (virDBusSignatureLength(t + 1, &siglen) < 0)
                goto cleanup;

            if (VIR_STRNDUP(contsig, t + 1, siglen) < 0)
                goto cleanup;

            if (narray == (size_t)-1) {
                types += siglen;
                nstruct -= siglen;
            }

            if (VIR_ALLOC(newiter) < 0)
                goto cleanup;
            VIR_DEBUG("Contsig '%s' '%zu' '%s'", contsig, siglen, types);
            dbus_message_iter_recurse(iter, newiter);
            if (virDBusTypeStackPush(&stack, &nstack,
                                     iter, types,
                                     nstruct, narray) < 0)
                goto cleanup;
            VIR_FREE(contsig);
            iter = newiter;
            newiter = NULL;
            types = t + 1;
            nstruct = siglen;
            narray = va_arg(args, int);
            break;

        case DBUS_TYPE_VARIANT:
            advanceiter = false;
            vsig = va_arg(args, const char *);
            if (!vsig) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Missing variant type signature"));
                goto cleanup;
            }
            if (VIR_ALLOC(newiter) < 0)
                goto cleanup;
            dbus_message_iter_recurse(iter, newiter);
            if (virDBusTypeStackPush(&stack, &nstack,
                                     iter, types,
                                     nstruct, narray) < 0) {
                VIR_DEBUG("Push failed");
                goto cleanup;
            }
            iter = newiter;
            newiter = NULL;
            types = vsig;
            nstruct = strlen(types);
            narray = (size_t)-1;
            break;

        case DBUS_STRUCT_BEGIN_CHAR:
        case DBUS_DICT_ENTRY_BEGIN_CHAR:
            advanceiter = false;
            if (virDBusSignatureLength(t, &siglen) < 0)
                goto cleanup;

            if (VIR_STRNDUP(contsig, t + 1, siglen - 1) < 0)
                goto cleanup;

            if (VIR_ALLOC(newiter) < 0)
                goto cleanup;
            VIR_DEBUG("Contsig '%s' '%zu'", contsig, siglen);
            dbus_message_iter_recurse(iter, newiter);
            if (narray == (size_t)-1) {
                types += siglen - 1;
                nstruct -= siglen - 1;
            }

            if (virDBusTypeStackPush(&stack, &nstack,
                                     iter, types,
                                     nstruct, narray) < 0)
                goto cleanup;
            VIR_FREE(contsig);
            iter = newiter;
            newiter = NULL;
            types = t + 1;
            nstruct = siglen - 2;
            narray = (size_t)-1;

            break;

        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown type in signature '%s'"),
                           types);
        }

        VIR_DEBUG("After stack=%zu array=%zu struct=%zu type='%s'",
                  nstack, narray, nstruct, types);
        if (advanceiter &&
            !(narray == 0 ||
              (narray == (size_t)-1 &&
               nstruct == 0)) &&
            !dbus_message_iter_next(iter)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Not enough fields in message for signature"));
            goto cleanup;
        }
    }

    if (dbus_message_iter_has_next(iter)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Too many fields in message for signature"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    virDBusTypeStackFree(&stack, &nstack);
    VIR_FREE(contsig);
    VIR_FREE(newiter);
    return ret;
}
# undef GET_NEXT_VAL

int
virDBusMessageEncodeArgs(DBusMessage* msg,
                         const char *types,
                         va_list args)
{
    DBusMessageIter iter;
    int ret = -1;

    memset(&iter, 0, sizeof(iter));

    dbus_message_iter_init_append(msg, &iter);

    ret = virDBusMessageIterEncode(&iter, types, args);

    return ret;
}


int virDBusMessageDecodeArgs(DBusMessage* msg,
                             const char *types,
                             va_list args)
{
    DBusMessageIter iter;
    int ret = -1;

    if (!dbus_message_iter_init(msg, &iter)) {
        if (*types != '\0') {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("No args present for signature %s"),
                           types);
        } else {
            ret = 0;
        }
        goto cleanup;
    }

    ret = virDBusMessageIterDecode(&iter, types, args);

cleanup:
    return ret;
}


int virDBusMessageEncode(DBusMessage* msg,
                         const char *types,
                         ...)
{
    int ret;
    va_list args;
    va_start(args, types);
    ret = virDBusMessageEncodeArgs(msg, types, args);
    va_end(args);
    return ret;
}


int virDBusMessageDecode(DBusMessage* msg,
                         const char *types,
                         ...)
{
    int ret;
    va_list args;
    va_start(args, types);
    ret = virDBusMessageDecodeArgs(msg, types, args);
    va_end(args);
    return ret;
}

# define VIR_DBUS_METHOD_CALL_TIMEOUT_MILLIS 30 * 1000

/**
 * virDBusCallMethod:
 * @conn: a DBus connection
 * @replyout: pointer to receive reply message, or NULL
 * @destination: bus identifier of the target service
 * @path: object path of the target service
 * @iface: the interface of the object
 * @member: the name of the method in the interface
 * @types: type signature for following method arguments
 * @...: method arguments
 *
 * This invokes a method on a remote service on the
 * DBus bus @conn. The @destination, @path, @iface
 * and @member parameters identify the object method to
 * be invoked. The optional @replyout parameter will be
 * filled with any reply to the method call. The
 * virDBusMethodReply method can be used to decode the
 * return values.
 *
 * The @types parameter is a DBus signature describing
 * the method call parameters which will be provided
 * as variadic args. Each character in @types must
 * correspond to one of the following DBus codes for
 * basic types:
 *
 * 'y' - 8-bit byte, promoted to an 'int'
 * 'b' - bool value, promoted to an 'int'
 * 'n' - 16-bit signed integer, promoted to an 'int'
 * 'q' - 16-bit unsigned integer, promoted to an 'int'
 * 'i' - 32-bit signed integer, passed as an 'int'
 * 'u' - 32-bit unsigned integer, passed as an 'int'
 * 'x' - 64-bit signed integer, passed as a 'long long'
 * 't' - 64-bit unsigned integer, passed as an 'unsigned long long'
 * 'd' - 8-byte floating point, passed as a 'double'
 * 's' - NUL-terminated string, in UTF-8
 * 'o' - NUL-terminated string, representing a valid object path
 * 'g' - NUL-terminated string, representing a valid type signature
 *
 * or use one of the compound types
 *
 * 'a' - array of values
 * 'v' - a variadic type.
 * '(' - start of a struct
 * ')' - end of a struct
 * '{' - start of a dictionary entry (pair of types)
 * '}' - start of a dictionary entry (pair of types)
 *
 * At this time, there is no support for Unix fd's ('h'), which only
 * newer DBus supports.
 *
 * Passing values in variadic args for basic types is
 * simple, the value is just passed directly using the
 * corresponding C type listed against the type code
 * above. Note how any integer value smaller than an
 * 'int' is promoted to an 'int' by the C rules for
 * variadic args.
 *
 * Passing values in variadic args for compound types
 * requires a little further explanation.
 *
 * - Variant: the first arg is a string containing
 *   the type signature for the values to be stored
 *   inside the variant. This is then followed by
 *   the values corresponding to the type signature
 *   in the normal manner.
 *
 * - Array: when 'a' appears in a type signature, it
 *   must be followed by a single type describing the
 *   array element type. For example 'as' is an array
 *   of strings. 'a(is)' is an array of structs, each
 *   struct containing an int and a string.
 *
 *   The first variadic arg for an array, is an 'int'
 *   specifying the number of elements in the array.
 *   This is then followed by the values for the array
 *
 * - Struct: when a '(' appears in a type signature,
 *   it must be followed by one or more types describing
 *   the elements in the array, terminated by a ')'.
 *
 * - Dict entry: when a '{' appears in a type signature it
 *   must be followed by exactly two types, one describing
 *   the type of the hash key, the other describing the
 *   type of the hash entry. The hash key type must be
 *   a basic type, not a compound type.
 *
 * Example signatures, with their corresponding variadic
 * args:
 *
 * - "biiss" - some basic types
 *
 *     (true, 7, 42, "hello", "world")
 *
 * - "as" - an array with a basic type element
 *
 *     (3, "one", "two", "three")
 *
 * - "a(is)" - an array with a struct element
 *
 *     (3, 1, "one", 2, "two", 3, "three")
 *
 * - "svs" - some basic types with a variant as an int
 *
 *     ("hello", "i", 3, "world")
 *
 * - "svs" - some basic types with a variant as an array of ints
 *
 *     ("hello", "ai", 4, 1, 2, 3, 4, "world")
 *
 * - "a{ss}" - a hash table (aka array + dict entry)
 *
 *     (3, "title", "Mr", "forename", "Joe", "surname", "Bloggs")
 *
 * - "a{sv}" - a hash table (aka array + dict entry)
 *
 *     (3, "email", "s", "joe@blogs.com", "age", "i", 35,
 *      "address", "as", 3, "Some house", "Some road", "some city")
 */

int virDBusCallMethod(DBusConnection *conn,
                      DBusMessage **replyout,
                      const char *destination,
                      const char *path,
                      const char *iface,
                      const char *member,
                      const char *types, ...)
{
    DBusMessage *call = NULL;
    DBusMessage *reply = NULL;
    DBusError error;
    int ret = -1;
    va_list args;

    dbus_error_init(&error);

    if (!(call = dbus_message_new_method_call(destination,
                                              path,
                                              iface,
                                              member))) {
        virReportOOMError();
        goto cleanup;
    }

    va_start(args, types);
    ret = virDBusMessageEncodeArgs(call, types, args);
    va_end(args);
    if (ret < 0)
        goto cleanup;

    ret = -1;

    if (!(reply = dbus_connection_send_with_reply_and_block(conn,
                                                            call,
                                                            VIR_DBUS_METHOD_CALL_TIMEOUT_MILLIS,
                                                            &error))) {
        virReportDBusServiceError(error.message ? error.message : "unknown error",
                                  error.name);
        goto cleanup;
    }

    if (dbus_set_error_from_message(&error,
                                    reply)) {
        virReportDBusServiceError(error.message ? error.message : "unknown error",
                                  error.name);
        goto cleanup;
    }

    ret = 0;

cleanup:
    dbus_error_free(&error);
    if (call)
        dbus_message_unref(call);
    if (reply) {
        if (ret == 0 && replyout)
            *replyout = reply;
        else
            dbus_message_unref(reply);
    }
    return ret;
}


/**
 * virDBusMessageRead:
 * @msg: the reply to decode
 * @types: type signature for following return values
 * @...: pointers in which to store return values
 *
 * The @types type signature is the same format as
 * that used for the virDBusCallMethod. The difference
 * is that each variadic parameter must be a pointer to
 * be filled with the values. eg instead of passing an
 * 'int', pass an 'int *'.
 *
 */
int virDBusMessageRead(DBusMessage *msg,
                       const char *types, ...)
{
    va_list args;
    int ret;

    va_start(args, types);
    ret = virDBusMessageDecodeArgs(msg, types, args);
    va_end(args);

    dbus_message_unref(msg);
    return ret;
}


#else /* ! WITH_DBUS */
DBusConnection *virDBusGetSystemBus(void)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("DBus support not compiled into this binary"));
    return NULL;
}

DBusConnection *virDBusGetSessionBus(void)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("DBus support not compiled into this binary"));
    return NULL;
}

int virDBusCallMethod(DBusConnection *conn ATTRIBUTE_UNUSED,
                      DBusMessage **reply ATTRIBUTE_UNUSED,
                      const char *destination ATTRIBUTE_UNUSED,
                      const char *path ATTRIBUTE_UNUSED,
                      const char *iface ATTRIBUTE_UNUSED,
                      const char *member ATTRIBUTE_UNUSED,
                      const char *types ATTRIBUTE_UNUSED, ...)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("DBus support not compiled into this binary"));
    return -1;
}

int virDBusMessageRead(DBusMessage *msg ATTRIBUTE_UNUSED,
                       const char *types ATTRIBUTE_UNUSED, ...)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("DBus support not compiled into this binary"));
    return -1;
}

int virDBusMessageEncode(DBusMessage* msg ATTRIBUTE_UNUSED,
                         const char *types ATTRIBUTE_UNUSED,
                         ...)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("DBus support not compiled into this binary"));
    return -1;
}

int virDBusMessageDecode(DBusMessage* msg ATTRIBUTE_UNUSED,
                         const char *types ATTRIBUTE_UNUSED,
                         ...)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("DBus support not compiled into this binary"));
    return -1;
}

#endif /* ! WITH_DBUS */
