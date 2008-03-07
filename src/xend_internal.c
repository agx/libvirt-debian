/*
 * xend_internal.c: access to Xen though the Xen Daemon interface
 *
 * Copyright (C) 2005
 *
 *      Anthony Liguori <aliguori@us.ibm.com>
 *
 *  This file is subject to the terms and conditions of the GNU Lesser General
 *  Public License. See the file COPYING.LIB in the main directory of this
 *  archive for more details.
 */

#ifdef WITH_XEN
#include <config.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <stdarg.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <libxml/uri.h>
#include <ctype.h>
#include <errno.h>

#include "libvirt/libvirt.h"
#include "driver.h"
#include "util.h"
#include "sexpr.h"
#include "xml.h"
#include "buf.h"
#include "capabilities.h"
#include "uuid.h"
#include "xen_unified.h"
#include "xend_internal.h"
#include "xen_internal.h" /* for DOM0_INTERFACE_VERSION */
#include "xs_internal.h" /* To extract VNC port & Serial console TTY */

/* required for cpumap_t */
#include <xen/dom0_ops.h>

#define DEBUG(fmt,...) VIR_DEBUG(__FILE__, fmt,__VA_ARGS__)
#define DEBUG0(msg) VIR_DEBUG(__FILE__, "%s", msg)

#ifndef PROXY
static int xenDaemonListDomains(virConnectPtr conn, int *ids, int maxids);
static int xenDaemonNumOfDomains(virConnectPtr conn);
static int xenDaemonListDefinedDomains(virConnectPtr conn, char **const names, int maxnames);
static int xenDaemonNumOfDefinedDomains(virConnectPtr conn);
static virDomainPtr xenDaemonCreateLinux(virConnectPtr conn,
                                         const char *xmlDesc,
					 unsigned int flags);
static char *xenDaemonDomainGetOSType(virDomainPtr domain);
static int xenDaemonAttachDevice(virDomainPtr domain, const char *xml);
static int xenDaemonDetachDevice(virDomainPtr domain, const char *xml);
static int xenDaemonDomainCoreDump(virDomainPtr domain, const char *filename,
                                   int flags);
#endif /* PROXY */

#ifndef PROXY
struct xenUnifiedDriver xenDaemonDriver = {
    xenDaemonOpen, /* open */
    xenDaemonClose, /* close */
    xenDaemonGetVersion, /* version */
    NULL, /* hostname */
    NULL, /* URI */
    xenDaemonNodeGetInfo, /* nodeGetInfo */
    NULL, /* getCapabilities */
    xenDaemonListDomains, /* listDomains */
    xenDaemonNumOfDomains, /* numOfDomains */
    xenDaemonCreateLinux, /* domainCreateLinux */
    xenDaemonDomainSuspend, /* domainSuspend */
    xenDaemonDomainResume, /* domainResume */
    xenDaemonDomainShutdown, /* domainShutdown */
    xenDaemonDomainReboot, /* domainReboot */
    xenDaemonDomainDestroy, /* domainDestroy */
    xenDaemonDomainGetOSType, /* domainGetOSType */
    xenDaemonDomainGetMaxMemory, /* domainGetMaxMemory */
    xenDaemonDomainSetMaxMemory, /* domainSetMaxMemory */
    xenDaemonDomainSetMemory, /* domainMaxMemory */
    xenDaemonDomainGetInfo, /* domainGetInfo */
    xenDaemonDomainSave, /* domainSave */
    xenDaemonDomainRestore, /* domainRestore */
    xenDaemonDomainCoreDump, /* domainCoreDump */
    xenDaemonDomainSetVcpus, /* domainSetVcpus */
    xenDaemonDomainPinVcpu, /* domainPinVcpu */
    xenDaemonDomainGetVcpus, /* domainGetVcpus */
    NULL, /* domainGetMaxVcpus */
    xenDaemonListDefinedDomains, /* listDefinedDomains */
    xenDaemonNumOfDefinedDomains, /* numOfDefinedDomains */
    xenDaemonDomainCreate, /* domainCreate */
    xenDaemonDomainDefineXML, /* domainDefineXML */
    xenDaemonDomainUndefine, /* domainUndefine */
    xenDaemonAttachDevice, /* domainAttachDevice */
    xenDaemonDetachDevice, /* domainDetachDevice */
    NULL, /* domainGetAutostart */
    NULL, /* domainSetAutostart */
    NULL, /* domainGetSchedulerType */
    NULL, /* domainGetSchedulerParameters */
    NULL, /* domainSetSchedulerParameters */
};

/**
 * xenDaemonInit:
 *
 * Initialise the xenDaemon driver.
 */
int
xenDaemonInit (void)
{
    return 0;
}
#endif /* !PROXY */

/**
 * xend_connection_type:
 *
 * The connection to the Xen Daemon can be done either though a normal TCP
 * socket or a local domain direct connection.
 */
enum xend_connection_type {
    XEND_DOMAIN,
    XEND_TCP,
};

/**
 * xend:
 *
 * Structure associated to a connection to a Xen daemon
 */
struct xend {
    int len;
    int type;
    struct sockaddr *addr;
    struct sockaddr_un addr_un;
    struct sockaddr_in addr_in;
};


/**
 * virXendError:
 * @conn: the connection if available
 * @error: the error noumber
 * @info: extra information string
 *
 * Handle an error at the xend daemon interface
 */
static void
virXendError(virConnectPtr conn, virErrorNumber error, const char *info)
{
    const char *errmsg;

    if (error == VIR_ERR_OK)
        return;

    errmsg = __virErrorMsg(error, info);
    __virRaiseError(conn, NULL, NULL, VIR_FROM_XEND, error, VIR_ERR_ERROR,
                    errmsg, info, NULL, 0, 0, errmsg, info);
}

/**
 * virXendErrorInt:
 * @conn: the connection if available
 * @error: the error noumber
 * @val: extra integer information
 *
 * Handle an error at the xend daemon interface
 */
static void
virXendErrorInt(virConnectPtr conn, virErrorNumber error, int val)
{
    const char *errmsg;

    if (error == VIR_ERR_OK)
        return;

    errmsg = __virErrorMsg(error, NULL);
    __virRaiseError(conn, NULL, NULL, VIR_FROM_XEND, error, VIR_ERR_ERROR,
                    errmsg, NULL, NULL, val, 0, errmsg, val);
}


#define foreach(iterator, start) \
       	for (_for_i = (start), *iterator = (start)->u.s.car; \
             _for_i->kind == SEXPR_CONS; \
             _for_i = _for_i->u.s.cdr, iterator = _for_i->u.s.car)

#define foreach_node(iterator, start, path) \
        foreach(iterator, start) \
            if (sexpr_lookup(iterator, path))

/**
 * do_connect:
 * @xend: pointer to the Xen Daemon structure
 *
 * Internal routine to (re)connect to the daemon
 *
 * Returns the socket file descriptor or -1 in case of error
 */
static int
do_connect(virConnectPtr xend)
{
    int s;
    int serrno;
    int no_slow_start = 1;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) xend->privateData;

    s = socket(priv->type, SOCK_STREAM, 0);
    if (s == -1) {
        virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                     _("failed to create a socket"));
        return -1;
    }

    /*
     * try to desactivate slow-start
     */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (void *)&no_slow_start,
               sizeof(no_slow_start));


    if (connect(s, priv->addr, priv->len) == -1) {
        serrno = errno;
        close(s);
        errno = serrno;
        s = -1;
	/*
	 * not being able to connect via the socket as a normal user
	 * is rather normal, this should fallback to the proxy (or
	 * remote) mechanism.
	 */
	if ((getuid() == 0) || (xend->flags & VIR_CONNECT_RO)) {
	    virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                     _("failed to connect to xend"));
        }
    }

    return s;
}

/**
 * wr_sync:
 * @xend: the xend connection object
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @size: the size of the I/O
 * @do_read: write operation if 0, read operation otherwise
 *
 * Do a synchronous read or write on the file descriptor
 *
 * Returns the number of bytes exchanged, or -1 in case of error
 */
static size_t
wr_sync(virConnectPtr xend, int fd, void *buffer, size_t size, int do_read)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t len;

        if (do_read) {
            len = read(fd, ((char *) buffer) + offset, size - offset);
        } else {
            len = write(fd, ((char *) buffer) + offset, size - offset);
        }

        /* recoverable error, retry  */
        if ((len == -1) && ((errno == EAGAIN) || (errno == EINTR))) {
            continue;
        }

        /* eof */
        if (len == 0) {
            break;
        }

        /* unrecoverable error */
        if (len == -1) {
            if (do_read)
                virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                             _("failed to read from Xen Daemon"));
            else
                virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                             _("failed to read from Xen Daemon"));

            return (-1);
        }

        offset += len;
    }

    return offset;
}

/**
 * sread:
 * @xend: the xend connection object
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @size: the size of the I/O
 *
 * Internal routine to do a synchronous read
 *
 * Returns the number of bytes read, or -1 in case of error
 */
static ssize_t
sread(virConnectPtr xend, int fd, void *buffer, size_t size)
{
    return wr_sync(xend, fd, buffer, size, 1);
}

/**
 * swrite:
 * @xend: the xend connection object
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @size: the size of the I/O
 *
 * Internal routine to do a synchronous write
 *
 * Returns the number of bytes written, or -1 in case of error
 */
static ssize_t
swrite(virConnectPtr xend, int fd, const void *buffer, size_t size)
{
    return wr_sync(xend, fd, (void *) buffer, size, 0);
}

/**
 * swrites:
 * @xend: the xend connection object
 * @fd:  the file descriptor
 * @string: the string to write
 *
 * Internal routine to do a synchronous write of a string
 *
 * Returns the number of bytes written, or -1 in case of error
 */
static ssize_t
swrites(virConnectPtr xend, int fd, const char *string)
{
    return swrite(xend, fd, string, strlen(string));
}

/**
 * sreads:
 * @xend: the xend connection object
 * @fd:  the file descriptor
 * @buffer: the I/O buffer
 * @n_buffer: the size of the I/O buffer
 *
 * Internal routine to do a synchronous read of a line
 *
 * Returns the number of bytes read, or -1 in case of error
 */
static ssize_t
sreads(virConnectPtr xend, int fd, char *buffer, size_t n_buffer)
{
    size_t offset;

    if (n_buffer < 1)
        return (-1);

    for (offset = 0; offset < (n_buffer - 1); offset++) {
        ssize_t ret;

        ret = sread(xend, fd, buffer + offset, 1);
        if (ret == 0)
            break;
        else if (ret == -1)
            return ret;

        if (buffer[offset] == '\n') {
            offset++;
            break;
        }
    }
    buffer[offset] = 0;

    return offset;
}

static int
istartswith(const char *haystack, const char *needle)
{
    return (strncasecmp(haystack, needle, strlen(needle)) == 0);
}


/**
 * xend_req:
 * @xend: the xend connection object
 * @fd: the file descriptor
 * @content: the buffer to store the content
 * @n_content: the size of the buffer
 *
 * Read the HTTP response from a Xen Daemon request.
 *
 * Returns the HTTP return code.
 */
static int
xend_req(virConnectPtr xend, int fd, char *content, size_t n_content)
{
    char buffer[4096];
    int content_length = -1;
    int retcode = 0;

    while (sreads(xend, fd, buffer, sizeof(buffer)) > 0) {
        if (strcmp(buffer, "\r\n") == 0)
            break;

        if (istartswith(buffer, "Content-Length: "))
            content_length = atoi(buffer + 16);
        else if (istartswith(buffer, "HTTP/1.1 "))
            retcode = atoi(buffer + 9);
    }

    if (content_length > -1) {
        ssize_t ret;

        if ((unsigned int) content_length > (n_content + 1))
            content_length = n_content - 1;

        ret = sread(xend, fd, content, content_length);
        if (ret < 0)
            return -1;

        content[ret] = 0;
    } else {
        content[0] = 0;
    }

    return retcode;
}

/**
 * xend_get:
 * @xend: pointer to the Xen Daemon structure
 * @path: the path used for the HTTP request
 * @content: the buffer to store the content
 * @n_content: the size of the buffer
 *
 * Do an HTTP GET RPC with the Xen Daemon
 *
 * Returns the HTTP return code or -1 in case or error.
 */
static int
xend_get(virConnectPtr xend, const char *path,
         char *content, size_t n_content)
{
    int ret;
    int s = do_connect(xend);

    if (s == -1)
        return s;

    swrites(xend, s, "GET ");
    swrites(xend, s, path);
    swrites(xend, s, " HTTP/1.1\r\n");

    swrites(xend, s,
            "Host: localhost:8000\r\n"
            "Accept-Encoding: identity\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n" "\r\n");

    ret = xend_req(xend, s, content, n_content);
    close(s);

    if (((ret < 0) || (ret >= 300)) &&
        ((ret != 404) || (strncmp(path, "/xend/domain/", 13)))) {
        virXendError(xend, VIR_ERR_GET_FAILED, content);
    }

    return ret;
}

#ifndef PROXY
/**
 * xend_post:
 * @xend: pointer to the Xen Daemon structure
 * @path: the path used for the HTTP request
 * @ops: the information sent for the POST
 * @content: the buffer to store the content
 * @n_content: the size of the buffer
 *
 * Do an HTTP POST RPC with the Xen Daemon, this usually makes changes at the
 * Xen level.
 *
 * Returns the HTTP return code or -1 in case or error.
 */
static int
xend_post(virConnectPtr xend, const char *path, const char *ops,
          char *content, size_t n_content)
{
    char buffer[100];
    int ret;
    int s = do_connect(xend);

    if (s == -1)
        return s;

    swrites(xend, s, "POST ");
    swrites(xend, s, path);
    swrites(xend, s, " HTTP/1.1\r\n");

    swrites(xend, s,
            "Host: localhost:8000\r\n"
            "Accept-Encoding: identity\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: ");
    snprintf(buffer, sizeof(buffer), "%d", (int) strlen(ops));
    swrites(xend ,s, buffer);
    swrites(xend, s, "\r\n\r\n");
    swrites(xend, s, ops);

    ret = xend_req(xend, s, content, n_content);
    close(s);

    if ((ret < 0) || (ret >= 300)) {
        virXendError(xend, VIR_ERR_POST_FAILED, content);
    } else if ((ret == 202) && (strstr(content, "failed") != NULL)) {
        virXendError(xend, VIR_ERR_POST_FAILED, content);
        ret = -1;
    } else if (((ret >= 200) && (ret <= 202)) && (strstr(content, "xend.err") != NULL)) {
        /* This is to catch case of things like 'virsh dump Domain-0 foo'
         * which returns a success code, but the word 'xend.err'
         * in body to indicate error :-(
         */
        virXendError(xend, VIR_ERR_POST_FAILED, content);
        ret = -1;
    }

    return ret;
}
#endif /* ! PROXY */


/**
 * http2unix:
 * @xend: the xend connection object
 * @ret: the http return code
 *
 * Convert the HTTP return code to 0/-1 and set errno if needed
 *
 * Return -1 in case of error code 0 otherwise
 */
static int
http2unix(virConnectPtr xend, int ret)
{
    switch (ret) {
        case -1:
            break;
        case 200:
        case 201:
        case 202:
            return 0;
        case 404:
            errno = ESRCH;
            break;
        case 500:
            errno = EIO;
            break;
        default:
            virXendErrorInt(xend, VIR_ERR_HTTP_ERROR, ret);
            errno = EINVAL;
            break;
    }
    return -1;
}

#ifndef PROXY
/**
 * xend_op_ext2:
 * @xend: pointer to the Xen Daemon structure
 * @path: path for the object
 * @error: buffer for the error output
 * @n_error: size of @error
 * @key: the key for the operation
 * @ap: input values to pass to the operation
 *
 * internal routine to run a POST RPC operation to the Xen Daemon
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xend_op_ext2(virConnectPtr xend, const char *path, char *error,
             size_t n_error, const char *key, va_list ap)
{
    const char *k = key, *v;
    virBuffer buf;
    int ret;

    buf.content = malloc(1000);
    if (buf.content == NULL) {
	virXendError(xend, VIR_ERR_NO_MEMORY, _("allocate new buffer"));
        return -1;
    }
    buf.size = 1000;
    buf.use = 0;

    while (k) {
        v = va_arg(ap, const char *);

        virBufferVSprintf(&buf, "%s", k);
        virBufferVSprintf(&buf, "%s", "=");
        virBufferVSprintf(&buf, "%s", v);
        k = va_arg(ap, const char *);

        if (k)
            virBufferVSprintf(&buf, "%s", "&");
    }

    ret = http2unix(xend, xend_post(xend, path, buf.content, error, n_error));
    free(buf.content);

    return ret;
}


/**
 * xend_node_op:
 * @xend: pointer to the Xen Daemon structure
 * @path: path for the object
 * @key: the key for the operation
 * @...: input values to pass to the operation
 *
 * internal routine to run a POST RPC operation to the Xen Daemon
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xend_node_op(virConnectPtr xend, const char *path, const char *key, ...)
{
    va_list ap;
    int ret;
    char error[1024];

    va_start(ap, key);
    ret = xend_op_ext2(xend, path, error, sizeof(error), key, ap);
    va_end(ap);

    return ret;
}


/**
 * xend_op_ext:
 * @xend: pointer to the Xen Daemon structure
 * @name: the domain name target of this operation
 * @error: buffer for the error output
 * @n_error: size of @error
 * @key: the key for the operation
 * @ap: input values to pass to the operation
 * @...: input values to pass to the operation
 *
 * internal routine to run a POST RPC operation to the Xen Daemon targetting
 * a given domain.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xend_op(virConnectPtr xend, const char *name, const char *key, ...)
{
    char buffer[1024];
    char error[1024];
    va_list ap;
    int ret;

    snprintf(buffer, sizeof(buffer), "/xend/domain/%s", name);

    va_start(ap, key);
    ret = xend_op_ext2(xend, buffer, error, sizeof(error), key, ap);
    va_end(ap);

    return ret;
}

#endif /* ! PROXY */

/**
 * sexpr_get:
 * @xend: pointer to the Xen Daemon structure
 * @fmt: format string for the path of the operation
 * @...: extra data to build the path of the operation
 *
 * Internal routine to run a simple GET RPC operation to the Xen Daemon
 *
 * Returns a parsed S-Expression in case of success, NULL in case of failure
 */
static struct sexpr *sexpr_get(virConnectPtr xend, const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf,2,3);

static struct sexpr *
sexpr_get(virConnectPtr xend, const char *fmt, ...)
{
    char buffer[4096];
    char path[1024];
    va_list ap;
    int ret;

    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);

    ret = xend_get(xend, path, buffer, sizeof(buffer));
    ret = http2unix(xend ,ret);
    if (ret == -1)
        return NULL;

    return string2sexpr(buffer);
}

/**
 * sexpr_int:
 * @sexpr: an S-Expression
 * @name: the name for the value
 *
 * convenience function to lookup an int value in the S-Expression
 *
 * Returns the value found or 0 if not found (but may not be an error)
 */
static int
sexpr_int(const struct sexpr *sexpr, const char *name)
{
    const char *value = sexpr_node(sexpr, name);

    if (value) {
        return strtol(value, NULL, 0);
    }
    return 0;
}


/**
 * sexpr_float:
 * @sexpr: an S-Expression
 * @name: the name for the value
 *
 * convenience function to lookup a float value in the S-Expression
 *
 * Returns the value found or 0 if not found (but may not be an error)
 */
static double
sexpr_float(const struct sexpr *sexpr, const char *name)
{
    const char *value = sexpr_node(sexpr, name);

    if (value) {
        return strtod(value, NULL);
    }
    return 0;
}

/**
 * sexpr_u64:
 * @sexpr: an S-Expression
 * @name: the name for the value
 *
 * convenience function to lookup a 64bits unsigned int value in the
 * S-Expression
 *
 * Returns the value found or 0 if not found (but may not be an error)
 */
static uint64_t
sexpr_u64(const struct sexpr *sexpr, const char *name)
{
    const char *value = sexpr_node(sexpr, name);

    if (value) {
        return strtoll(value, NULL, 0);
    }
    return 0;
}


/**
 * sexpr_uuid:
 * @ptr: where to store the UUID, incremented
 * @sexpr: an S-Expression
 * @name: the name for the value
 *
 * convenience function to lookup an UUID value from the S-Expression
 *
 * Returns a -1 on error, 0 on success
 */
static int
sexpr_uuid(unsigned char *ptr, const struct sexpr *node, const char *path)
{
    const char *r = sexpr_node(node, path);
    if (!r)
        return -1;
    return virUUIDParse(r, ptr);
}


#ifndef PROXY
/**
 * urlencode:
 * @string: the input URL
 *
 * Encode an URL see RFC 2396 and following
 *
 * Returns the new string or NULL in case of error.
 */
static char *
urlencode(const char *string)
{
    size_t len = strlen(string);
    char *buffer = malloc(len * 3 + 1);
    char *ptr = buffer;
    size_t i;

    if (buffer == NULL) {
	virXendError(NULL, VIR_ERR_NO_MEMORY, _("allocate new buffer"));
        return (NULL);
    }
    for (i = 0; i < len; i++) {
        switch (string[i]) {
            case ' ':
            case '\n':
                snprintf(ptr, 4, "%%%02x", string[i]);
                ptr += 3;
                break;
            default:
                *ptr = string[i];
                ptr++;
        }
    }

    *ptr = 0;

    return buffer;
}
#endif /* ! PROXY */

/* PUBLIC FUNCTIONS */

/**
 * xenDaemonOpen_unix:
 * @conn: an existing virtual connection block
 * @path: the path for the Xen Daemon socket
 *
 * Creates a localhost Xen Daemon connection
 * Note: this doesn't try to check if the connection actually works
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenDaemonOpen_unix(virConnectPtr conn, const char *path)
{
    struct sockaddr_un *addr;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    if ((conn == NULL) || (path == NULL))
        return (-1);

    addr = &priv->addr_un;
    addr->sun_family = AF_UNIX;
    memset(addr->sun_path, 0, sizeof(addr->sun_path));
    strncpy(addr->sun_path, path, sizeof(addr->sun_path));

    priv->len = sizeof(addr->sun_family) + strlen(addr->sun_path);
    if ((unsigned int) priv->len > sizeof(addr->sun_path))
        priv->len = sizeof(addr->sun_path);

    priv->addr = (struct sockaddr *) addr;
    priv->type = PF_UNIX;

    return (0);
}

#ifndef PROXY
/**
 * xenDaemonOpen_tcp:
 * @conn: an existing virtual connection block
 * @host: the host name for the Xen Daemon
 * @port: the port
 *
 * Creates a possibly remote Xen Daemon connection
 * Note: this doesn't try to check if the connection actually works
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenDaemonOpen_tcp(virConnectPtr conn, const char *host, int port)
{
    struct in_addr ip;
    struct hostent *pent;
    xenUnifiedPrivatePtr priv;

    if ((conn == NULL) || (host == NULL) || (port <= 0))
        return (-1);

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    pent = gethostbyname(host);
    if (pent == NULL) {
        if (inet_aton(host, &ip) == 0) {
            virXendError(NULL, VIR_ERR_UNKNOWN_HOST, host);
            errno = ESRCH;
            return (-1);
        }
    } else {
        memcpy(&ip, pent->h_addr_list[0], sizeof(ip));
    }

    priv->len = sizeof(struct sockaddr_in);
    priv->addr = (struct sockaddr *) &priv->addr_in;
    priv->type = PF_INET;

    priv->addr_in.sin_family = AF_INET;
    priv->addr_in.sin_port = htons(port);
    memcpy(&priv->addr_in.sin_addr, &ip, sizeof(ip));

    return (0);
}


/**
 * xend_wait_for_devices:
 * @xend: pointer to the Xem Daemon block
 * @name: name for the domain
 *
 * Block the domain until all the virtual devices are ready. This operation
 * is needed when creating a domain before resuming it.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xend_wait_for_devices(virConnectPtr xend, const char *name)
{
    return xend_op(xend, name, "op", "wait_for_devices", NULL);
}


#endif /* PROXY */


/**
 * xenDaemonListDomainsOld:
 * @xend: pointer to the Xem Daemon block
 *
 * This method will return an array of names of currently running
 * domains.  The memory should be released will a call to free().
 *
 * Returns a list of names or NULL in case of error.
 */
char **
xenDaemonListDomainsOld(virConnectPtr xend)
{
    size_t extra = 0;
    struct sexpr *root = NULL;
    char **ret = NULL;
    int count = 0;
    int i;
    char *ptr;
    struct sexpr *_for_i, *node;

    root = sexpr_get(xend, "/xend/domain");
    if (root == NULL)
        goto error;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        extra += strlen(node->u.value) + 1;
        count++;
    }

    ptr = malloc((count + 1) * sizeof(char *) + extra);
    if (ptr == NULL)
        goto error;

    ret = (char **) ptr;
    ptr += sizeof(char *) * (count + 1);

    i = 0;
    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        ret[i] = ptr;
        strcpy(ptr, node->u.value);
        ptr += strlen(node->u.value) + 1;
        i++;
    }

    ret[i] = NULL;

  error:
    sexpr_free(root);
    return ret;
}

#ifndef PROXY
/**
 * xenDaemonDomainCreateLinux:
 * @xend: A xend instance
 * @sexpr: An S-Expr description of the domain.
 *
 * This method will create a domain based the passed in description.  The
 * domain will be paused after creation and must be unpaused with
 * xenDaemonResumeDomain() to begin execution.
 * This method may be deprecated once switching to XML-RPC based communcations
 * with xend.
 *
 * Returns 0 for success, -1 (with errno) on error
 */

int
xenDaemonDomainCreateLinux(virConnectPtr xend, const char *sexpr)
{
    int ret, serrno;
    char *ptr;

    ptr = urlencode(sexpr);
    if (ptr == NULL) {
        /* this should be caught at the interface but ... */
        virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                     _("failed to urlencode the create S-Expr"));
        return (-1);
    }

    ret = xend_op(xend, "", "op", "create", "config", ptr, NULL);

    serrno = errno;
    free(ptr);
    errno = serrno;

    return ret;
}
#endif /* ! PROXY */

/**
 * xenDaemonDomainLookupByName_ids:
 * @xend: A xend instance
 * @domname: The name of the domain
 * @uuid: return value for the UUID if not NULL
 *
 * This method looks up the id of a domain
 *
 * Returns the id on success; -1 (with errno) on error
 */
int
xenDaemonDomainLookupByName_ids(virConnectPtr xend, const char *domname,
				unsigned char *uuid)
{
    struct sexpr *root;
    const char *value;
    int ret = -1;

    if (uuid != NULL)
        memset(uuid, 0, VIR_UUID_BUFLEN);
    root = sexpr_get(xend, "/xend/domain/%s?detail=1", domname);
    if (root == NULL)
        goto error;

    value = sexpr_node(root, "domain/domid");
    if (value == NULL) {
        virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                     _("domain information incomplete, missing domid"));
        goto error;
    }
    ret = strtol(value, NULL, 0);
    if ((ret == 0) && (value[0] != '0')) {
        virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                     _("domain information incorrect domid not numeric"));
        ret = -1;
    } else if (uuid != NULL) {
        if (sexpr_uuid(uuid, root, "domain/uuid") < 0) {
            virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                         _("domain information incomplete, missing uuid"));
        }
    }

  error:
    sexpr_free(root);
    return (ret);
}


/**
 * xenDaemonDomainLookupByID:
 * @xend: A xend instance
 * @id: The id of the domain
 * @name: return value for the name if not NULL
 * @uuid: return value for the UUID if not NULL
 *
 * This method looks up the name of a domain based on its id
 *
 * Returns the 0 on success; -1 (with errno) on error
 */
int
xenDaemonDomainLookupByID(virConnectPtr xend,
			  int id,
			  char **domname,
			  unsigned char *uuid)
{
    const char *name = NULL;
    struct sexpr *root;

    memset(uuid, 0, VIR_UUID_BUFLEN);

    root = sexpr_get(xend, "/xend/domain/%d?detail=1", id);
    if (root == NULL)
      goto error;

    name = sexpr_node(root, "domain/name");
    if (name == NULL) {
      virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                   _("domain information incomplete, missing name"));
      goto error;
    }
    if (domname)
      *domname = strdup(name);

    if (sexpr_uuid(uuid, root, "domain/uuid") < 0) {
      virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                   _("domain information incomplete, missing uuid"));
      goto error;
    }

    sexpr_free(root);
    return (0);

error:
    sexpr_free(root);
    if (domname && *domname) {
      free(*domname);
      *domname = NULL;
    }
    return (-1);
}


#ifndef PROXY
static int
xend_detect_config_version(virConnectPtr conn) {
    struct sexpr *root;
    const char *value;
    xenUnifiedPrivatePtr priv;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL)
        return (-1);

    value = sexpr_node(root, "node/xend_config_format");

    if (value) {
        priv->xendConfigVersion = strtol(value, NULL, 10);
    }  else {
        /* Xen prior to 3.0.3 did not have the xend_config_format
           field, and is implicitly version 1. */
        priv->xendConfigVersion = 1;
    }
    sexpr_free(root);
    return (0);
}

/**
 * xend_node_shutdown:
 * @xend: A xend instance
 *
 * This method shuts down the physical machine running Xen.
 *
 * Returns 0 on success; -1 (with errno) on error
 */
int
xend_node_shutdown(virConnectPtr xend)
{
    return xend_node_op(xend, "/xend/node/", "op", "halt", NULL);
}

/**
 * xend_node_restart:
 * @xend: A xend instance
 *
 * This method restarts the physical machine running Xen.
 *
 * Returns 0 on success; -1 (with errno) on error
 */
int
xend_node_restart(virConnectPtr xend)
{
    return xend_node_op(xend, "/xend/node/", "op", "restart", NULL);
}


/**
 * xend_dmesg:
 * @xend: A xend instance
 * @buffer: A buffer to hold the messages
 * @n_buffer: Size of buffer (including null terminator)
 *
 * This function will place the debugging messages from the
 * hypervisor into a buffer with a null terminator.
 *
 * Returns 0 on success; -1 (with errno) on error
 */
int
xend_dmesg(virConnectPtr xend, char *buffer, size_t n_buffer)
{
    return http2unix(xend, xend_get(xend, "/xend/node/dmesg", buffer, n_buffer));
}

/**
 * xend_dmesg_clear:
 * @xend: A xend instance
 *
 * This function will clear the debugging message ring queue
 * in the hypervisor.
 *
 * Returns 0 on success; -1 (with errno) on error
 */
int
xend_dmesg_clear(virConnectPtr xend)
{
    return xend_node_op(xend, "/xend/node/dmesg", "op", "clear", NULL);
}

/**
 * xend_log:
 * @xend: A xend instance
 * @buffer: The buffer to hold the messages
 * @n_buffer: Size of buffer (including null terminator)
 *
 * This function will place the Xend debugging messages into
 * a buffer with a null terminator.
 *
 * Returns 0 on success; -1 (with errno) on error
 */
int
xend_log(virConnectPtr xend, char *buffer, size_t n_buffer)
{
    return http2unix(xend, xend_get(xend, "/xend/node/log", buffer, n_buffer));
}
#endif /* PROXY */

/*****************************************************************
 ******
 ******
 ******
 ******
             Needed helper code
 ******
 ******
 ******
 ******
 *****************************************************************/

/**
 * xend_parse_sexp_desc_os:
 * @xend: the xend connection object
 * @node: the root of the parsed S-Expression
 * @buf: output buffer object
 * @hvm: true or 1 if no contains HVM S-Expression
 * @bootloader: true or 1 if a bootloader is defined
 *
 * Parse the xend sexp for description of os and append it to buf.
 *
 * Returns 0 in case of success and -1 in case of error
 */
static int
xend_parse_sexp_desc_os(virConnectPtr xend, struct sexpr *node, virBufferPtr buf, int hvm, int bootloader)
{
    const char *loader = NULL;
    const char *kernel = NULL;
    const char *initrd = NULL;
    const char *cmdline = NULL;
    const char *root = NULL;

    if (node == NULL || buf == NULL) {
       return(-1);
    }

    virBufferAddLit(buf, "  <os>\n");
    if (hvm)
        virBufferAddLit(buf, "    <type>hvm</type>\n");
    else
        virBufferAddLit(buf, "    <type>linux</type>\n");

    if (hvm) {
        loader = sexpr_node(node, "domain/image/hvm/loader");
        if (loader == NULL) {
            loader = sexpr_node(node, "domain/image/hvm/kernel");

            if (loader == NULL) {
                virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                             _("domain information incomplete, missing HVM loader"));
                return(-1);
            }
        } else {
            kernel = sexpr_node(node, "domain/image/hvm/kernel");
            initrd = sexpr_node(node, "domain/image/hvm/ramdisk");
            cmdline = sexpr_node(node, "domain/image/hvm/args");
            root = sexpr_node(node, "domain/image/hvm/root");
        }
    } else {
        kernel = sexpr_node(node, "domain/image/linux/kernel");
        initrd = sexpr_node(node, "domain/image/linux/ramdisk");
        cmdline = sexpr_node(node, "domain/image/linux/args");
        root = sexpr_node(node, "domain/image/linux/root");
    }

    if (hvm)
        virBufferVSprintf(buf, "    <loader>%s</loader>\n", loader);

    if (kernel) {
        virBufferVSprintf(buf, "    <kernel>%s</kernel>\n", kernel);
        if (initrd && initrd[0])
            virBufferVSprintf(buf, "    <initrd>%s</initrd>\n", initrd);
        if (root && root[0])
            virBufferVSprintf(buf, "    <root>%s</root>\n", root);
        if (cmdline && cmdline[0])
            virBufferEscapeString(buf, "    <cmdline>%s</cmdline>\n", cmdline);
    } else {
        if (hvm) {
            const char *boot = sexpr_node(node, "domain/image/hvm/boot");
            if ((boot != NULL) && (boot[0] != 0)) {
                while (*boot) {
                    if (*boot == 'a')
                        /* XXX no way to deal with boot from 2nd floppy */
                        virBufferAddLit(buf, "    <boot dev='fd'/>\n");
                    else if (*boot == 'c')
                        /*
                         * Don't know what to put here.  Say the vm has been given 3
                         * disks - hda, hdb, hdc.  How does one identify the boot disk?
                         * We're going to assume that first disk is the boot disk since
                         * this is most common practice
                         */
                        virBufferAddLit(buf, "    <boot dev='hd'/>\n");
                    else if (*boot == 'd')
                        virBufferAddLit(buf, "    <boot dev='cdrom'/>\n");
                    else if (*boot == 'n')
                        virBufferAddLit(buf, "    <boot dev='network'/>\n");
                    boot++;
                }
            }
        } else if (!bootloader) {
            virXendError(xend, VIR_ERR_INTERNAL_ERROR,
                         _("domain information incomplete, missing kernel & bootloader"));
            return(-1);
        }
    }

    virBufferAddLit(buf, "  </os>\n");
    return(0);
}

/**
 * xend_parse_sexp_desc:
 * @conn: the connection associated with the XML
 * @root: the root of the parsed S-Expression
 * @xendConfigVersion: version of xend
 * @flags: a combination of virDomainXMLFlags
 * @cpus: set of cpus the domain may be pinned to
 *
 * Parse the xend sexp description and turn it into the XML format similar
 * to the one unsed for creation.
 *
 * Returns the 0 terminated XML string or NULL in case of error.
 *         the caller must free() the returned value.
 */
static char *
xend_parse_sexp_desc(virConnectPtr conn, struct sexpr *root,
                     int xendConfigVersion, int flags, const char *cpus)
{
    struct sexpr *cur, *node;
    const char *tmp;
    char *tty;
    virBuffer buf;
    int hvm = 0, bootloader = 0, vfb = 0;
    int domid = -1;
    int max_mem, cur_mem;
    unsigned char uuid[VIR_UUID_BUFLEN];
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    int vif_index = 0;

    if (root == NULL) {
        /* ERROR */
        return (NULL);
    }
    buf.content = malloc(4000);
    if (buf.content == NULL)
        return (NULL);
    buf.size = 4000;
    buf.use = 0;

    tmp = sexpr_node(root, "domain/domid");
    if (tmp == NULL && xendConfigVersion < 3) { /* Old XenD, domid was mandatory */
        virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                     _("domain information incomplete, missing id"));
        goto error;
    }
    if (tmp)
        domid = sexpr_int(root, "domain/domid");
    else
        domid = -1;
    virBufferVSprintf(&buf, "<domain type='xen' id='%d'>\n", domid);

    tmp = sexpr_node(root, "domain/name");
    if (tmp == NULL) {
        virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                     _("domain information incomplete, missing name"));
        goto error;
    }
    virBufferVSprintf(&buf, "  <name>%s</name>\n", tmp);
    tmp = sexpr_node(root, "domain/uuid");
    if (tmp == NULL) {
        virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                     _("domain information incomplete, missing name"));
        goto error;
    }
    virUUIDParse(tmp, uuid);
    virUUIDFormat(uuid, uuidstr);
    virBufferVSprintf(&buf, "  <uuid>%s</uuid>\n", uuidstr);

    hvm = sexpr_lookup(root, "domain/image/hvm") ? 1 : 0;
    if (!hvm) {
        tmp = sexpr_node(root, "domain/bootloader");
        if (tmp != NULL) {
            bootloader = 1;
            virBufferVSprintf(&buf, "  <bootloader>%s</bootloader>\n", tmp);
        } else if (sexpr_has(root, "domain/bootloader")) {
            bootloader = 1;
            virBufferAddLit(&buf, "  <bootloader/>\n");
        }
        tmp = sexpr_node(root, "domain/bootloader_args");
        if (tmp != NULL && bootloader) {
            /*
             * Only insert bootloader_args if there is also a bootloader param
             */
            virBufferEscapeString(&buf, "  <bootloader_args>%s</bootloader_args>\n", tmp);
        }
    }

    if (domid != 0) {
        if (sexpr_lookup(root, "domain/image")) {
            if (xend_parse_sexp_desc_os(conn, root, &buf, hvm, bootloader) < 0)
                goto error;
        }
    }

    max_mem = (int) (sexpr_u64(root, "domain/maxmem") << 10);
    cur_mem = (int) (sexpr_u64(root, "domain/memory") << 10);
    if (cur_mem > max_mem)
        max_mem = cur_mem;
    virBufferVSprintf(&buf, "  <memory>%d</memory>\n", max_mem);
    if ((cur_mem >= MIN_XEN_GUEST_SIZE) && (cur_mem != max_mem))
        virBufferVSprintf(&buf, "  <currentMemory>%d</currentMemory>\n",
                          cur_mem);

    virBufferAddLit(&buf, "  <vcpu");
    if (cpus != NULL) {
        virBufferVSprintf(&buf, " cpuset='%s'", cpus);
    }
    virBufferVSprintf(&buf, ">%d</vcpu>\n",
                      sexpr_int(root, "domain/vcpus"));
    /* TODO if need to output the cpus values,
     * - parse the cpus values if xend exports
     * or
     * - analyze the cpus values extracted by xenDaemonDomainGetVcpus
     */
    tmp = sexpr_node(root, "domain/on_poweroff");
    if (tmp != NULL)
        virBufferVSprintf(&buf, "  <on_poweroff>%s</on_poweroff>\n", tmp);
    tmp = sexpr_node(root, "domain/on_reboot");
    if (tmp != NULL)
        virBufferVSprintf(&buf, "  <on_reboot>%s</on_reboot>\n", tmp);
    tmp = sexpr_node(root, "domain/on_crash");
    if (tmp != NULL)
        virBufferVSprintf(&buf, "  <on_crash>%s</on_crash>\n", tmp);

    if (hvm) {
        int clockLocal;

        virBufferAddLit(&buf, "  <features>\n");
        if (sexpr_int(root, "domain/image/hvm/acpi"))
            virBufferAddLit(&buf, "    <acpi/>\n");
        if (sexpr_int(root, "domain/image/hvm/apic"))
            virBufferAddLit(&buf, "    <apic/>\n");
        if (sexpr_int(root, "domain/image/hvm/pae"))
            virBufferAddLit(&buf, "    <pae/>\n");
        virBufferAddLit(&buf, "  </features>\n");

        clockLocal = sexpr_int(root, "domain/image/hvm/localtime");
        virBufferVSprintf(&buf, "  <clock offset='%s'/>\n", clockLocal ? "localtime" : "utc");
    }

    virBufferAddLit(&buf, "  <devices>\n");

    if (hvm)
        tmp = sexpr_node(root, "domain/image/hvm/device_model");
    else
        tmp = sexpr_node(root, "domain/image/linux/device_model");
    if ((tmp != NULL) && (tmp[0] != 0))
        virBufferVSprintf(&buf, "    <emulator>%s</emulator>\n", tmp);

    for (cur = root; cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
        node = cur->u.s.car;
        /* Normally disks are in a (device (vbd ...)) block
           but blktap disks ended up in a differently named
           (device (tap ....)) block.... */
        if (sexpr_lookup(node, "device/vbd") ||
            sexpr_lookup(node, "device/tap")) {
            char *offset;
            int isBlock = 0;
            int cdrom = 0;
            int isNoSrcCdrom = 0;
            char *drvName = NULL;
            char *drvType = NULL;
            const char *src = NULL;
            const char *dst = NULL;
            const char *mode = NULL;

            /* Again dealing with (vbd...) vs (tap ...) differences */
            if (sexpr_lookup(node, "device/vbd")) {
                src = sexpr_node(node, "device/vbd/uname");
                dst = sexpr_node(node, "device/vbd/dev");
                mode = sexpr_node(node, "device/vbd/mode");
            } else {
                src = sexpr_node(node, "device/tap/uname");
                dst = sexpr_node(node, "device/tap/dev");
                mode = sexpr_node(node, "device/tap/mode");
            }

            if (dst == NULL) {
                virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                             _("domain information incomplete, vbd has no dev"));
                goto bad_parse;
            }

            if (src == NULL) {
                /* There is a case without the uname to the CD-ROM device */
                offset = strchr(dst, ':');
                if (offset) {
                    if (hvm && !strcmp( offset , ":cdrom")) {
                        isNoSrcCdrom = 1;
                    }
                    offset[0] = '\0';
                }
                if (!isNoSrcCdrom) {
                    virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                                 _("domain information incomplete, vbd has no src"));
                    goto bad_parse;
                }
            }

            if (!isNoSrcCdrom) {
                offset = strchr(src, ':');
                if (!offset) {
                    virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                                 _("cannot parse vbd filename, missing driver name"));
                    goto bad_parse;
                }

                drvName = malloc((offset-src)+1);
                if (!drvName) {
                    virXendError(conn, VIR_ERR_NO_MEMORY,
                                 _("allocate new buffer"));
                    goto bad_parse;
                }
                strncpy(drvName, src, (offset-src));
                drvName[offset-src] = '\0';

                src = offset + 1;

                if (!strcmp(drvName, "tap")) {
                    offset = strchr(src, ':');
                    if (!offset) {
                        virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                                     _("cannot parse vbd filename, missing driver type"));
                        goto bad_parse;
                    }

                    drvType = malloc((offset-src)+1);
                    if (!drvType) {
                        virXendError(conn, VIR_ERR_NO_MEMORY,
                                     _("allocate new buffer"));
                        goto bad_parse;
                    }
                    strncpy(drvType, src, (offset-src));
                    drvType[offset-src] = '\0';
                    src = offset + 1;
                    /* Its possible to use blktap driver for block devs
                       too, but kinda pointless because blkback is better,
                       so we assume common case here. If blktap becomes
                       omnipotent, we can revisit this, perhaps stat()'ing
                       the src file in question */
                    isBlock = 0;
                } else if (!strcmp(drvName, "phy")) {
                    isBlock = 1;
                } else if (!strcmp(drvName, "file")) {
                    isBlock = 0;
                }
            }

            if (!strncmp(dst, "ioemu:", 6))
                dst += 6;

            /* New style disk config from Xen >= 3.0.3 */
            if (xendConfigVersion > 1) {
                offset = strrchr(dst, ':');
                if (offset) {
                    if (!strcmp(offset, ":cdrom")) {
                        cdrom = 1;
                    } else if (!strcmp(offset, ":disk")) {
                        /* The default anyway */
                    } else {
                        /* Unknown, lets pretend its a disk too */
                    }
                    offset[0] = '\0';
                }
            }

            if (!isNoSrcCdrom) {
                virBufferVSprintf(&buf, "    <disk type='%s' device='%s'>\n",
                                  isBlock ? "block" : "file",
                                  cdrom ? "cdrom" : "disk");
                if (drvType) {
                    virBufferVSprintf(&buf, "      <driver name='%s' type='%s'/>\n", drvName, drvType);
                } else {
                    virBufferVSprintf(&buf, "      <driver name='%s'/>\n", drvName);
                }
                if (isBlock) {
                    virBufferVSprintf(&buf, "      <source dev='%s'/>\n", src);
                } else {
                    virBufferVSprintf(&buf, "      <source file='%s'/>\n", src);
                }
            } else {
                /* This case is the cdrom device only */
                virBufferAddLit(&buf, "    <disk device='cdrom'>\n");
            }
            virBufferVSprintf(&buf, "      <target dev='%s'/>\n", dst);


            /* XXX should we force mode == r, if cdrom==1, or assume
               xend has already done this ? */
            if ((mode != NULL) && (!strcmp(mode, "r")))
                virBufferAddLit(&buf, "      <readonly/>\n");
	    else if ((mode != NULL) && (!strcmp(mode, "w!")))
                virBufferAddLit(&buf, "      <shareable/>\n");
            virBufferAddLit(&buf, "    </disk>\n");

            bad_parse:
            free(drvName);
            free(drvType);
        } else if (sexpr_lookup(node, "device/vif")) {
            const char *tmp2;
            tmp2 = sexpr_node(node, "device/vif/script");
            tmp = sexpr_node(node, "device/vif/bridge");
            if ((tmp2 && strstr(tmp2, "bridge")) || tmp) {
                virBufferAddLit(&buf, "    <interface type='bridge'>\n");
                if (tmp != NULL)
                    virBufferVSprintf(&buf, "      <source bridge='%s'/>\n",
                                      tmp);
            } else {
                virBufferAddLit(&buf, "    <interface type='ethernet'>\n");
            }

            tmp = sexpr_node(node, "device/vif/vifname");
            if (tmp)
                virBufferVSprintf(&buf, "      <target dev='%s'/>\n",
                                  tmp);
            else
                virBufferVSprintf(&buf, "      <target dev='vif%d.%d'/>\n",
                                  domid, vif_index);
            tmp = sexpr_node(node, "device/vif/mac");
            if (tmp)
                virBufferVSprintf(&buf, "      <mac address='%s'/>\n",
                                  tmp);
            tmp = sexpr_node(node, "device/vif/ip");
            if (tmp)
                virBufferVSprintf(&buf, "      <ip address='%s'/>\n",
                                  tmp);
            if (tmp2)
                virBufferVSprintf(&buf, "      <script path='%s'/>\n",
                                  tmp2);

            virBufferAddLit(&buf, "    </interface>\n");
            vif_index++;
        } else if (sexpr_lookup(node, "device/vfb")) {
            /* New style graphics config for PV guests in >= 3.0.4,
             * or for HVM guests in >= 3.0.5 */
            tmp = sexpr_node(node, "device/vfb/type");

            if (tmp && !strcmp(tmp, "sdl")) {
                vfb = 1;
                virBufferVSprintf(&buf, "    <input type='mouse' bus='%s'/>\n", hvm ? "ps2": "xen");
                virBufferAddLit(&buf, "    <graphics type='sdl'/>\n");
            } else if (tmp && !strcmp(tmp, "vnc")) {
                int port = xenStoreDomainGetVNCPort(conn, domid);
                const char *listenAddr = sexpr_node(node, "device/vfb/vnclisten");
                const char *vncPasswd = NULL;
                const char *keymap = sexpr_node(node, "device/vfb/keymap");
                vfb = 1;
                virBufferVSprintf(&buf, "    <input type='mouse' bus='%s'/>\n", hvm ? "ps2": "xen");
                virBufferVSprintf(&buf, "    <graphics type='vnc' port='%d'", port);
                if (listenAddr)
                    virBufferVSprintf(&buf, " listen='%s'", listenAddr);
		if (flags & VIR_DOMAIN_XML_SECURE) {
                    vncPasswd = sexpr_node(node, "device/vfb/vncpasswd");
		    if (vncPasswd)
			virBufferVSprintf(&buf, " passwd='%s'", vncPasswd);
		}
                if (keymap)
                    virBufferVSprintf(&buf, " keymap='%s'", keymap);
                virBufferAddLit(&buf, "/>\n");
            }
        }
    }

    if (hvm) {
        tmp = sexpr_node(root, "domain/image/hvm/fda");
        if ((tmp != NULL) && (tmp[0] != 0)) {
            virBufferAddLit(&buf, "    <disk type='file' device='floppy'>\n");
            virBufferVSprintf(&buf, "      <source file='%s'/>\n", tmp);
            virBufferAddLit(&buf, "      <target dev='fda'/>\n");
            virBufferAddLit(&buf, "    </disk>\n");
        }
        tmp = sexpr_node(root, "domain/image/hvm/fdb");
        if ((tmp != NULL) && (tmp[0] != 0)) {
            virBufferAddLit(&buf, "    <disk type='file' device='floppy'>\n");
            virBufferVSprintf(&buf, "      <source file='%s'/>\n", tmp);
            virBufferAddLit(&buf, "      <target dev='fdb'/>\n");
            virBufferAddLit(&buf, "    </disk>\n");
        }

        /* Old style cdrom config from Xen <= 3.0.2 */
        if (xendConfigVersion == 1) {
            tmp = sexpr_node(root, "domain/image/hvm/cdrom");
            if ((tmp != NULL) && (tmp[0] != 0)) {
                virBufferAddLit(&buf, "    <disk type='file' device='cdrom'>\n");
                virBufferAddLit(&buf, "      <driver name='file'/>\n");
                virBufferVSprintf(&buf, "      <source file='%s'/>\n", tmp);
                virBufferAddLit(&buf, "      <target dev='hdc'/>\n");
                virBufferAddLit(&buf, "      <readonly/>\n");
                virBufferAddLit(&buf, "    </disk>\n");
            }
        }
    }

    /* in case of HVM we have devices emulation */
    if (hvm) {
        for (cur = sexpr_lookup(root, "domain/image/hvm"); cur && cur->kind == SEXPR_CONS; cur = cur->u.s.cdr) {
            node = cur->u.s.car;
            if (sexpr_lookup(node, "usbdevice")) {
                tmp = sexpr_node(node, "usbdevice");
                if (tmp && *tmp) {
                    if (!strcmp(tmp, "tablet"))
                        virBufferAddLit(&buf, "    <input type='tablet' bus='usb'/>\n");
                    else if (!strcmp(tmp, "mouse"))
                        virBufferAddLit(&buf, "    <input type='mouse' bus='usb'/>\n");
                }
            }
        }
    }

    /* Graphics device (HVM <= 3.0.4, or PV <= 3.0.3) vnc config */
    if (!vfb) {
        tmp = sexpr_fmt_node(root, "domain/image/%s/vnc", hvm ? "hvm" : "linux");
        if (tmp != NULL) {
            if (tmp[0] == '1') {
                int port = xenStoreDomainGetVNCPort(conn, domid);
                const char *listenAddr = sexpr_fmt_node(root, "domain/image/%s/vnclisten", hvm ? "hvm" : "linux");
                const char *vncPasswd = NULL;
                const char *keymap = sexpr_fmt_node(root, "domain/image/%s/keymap", hvm ? "hvm" : "linux");
                /* For Xen >= 3.0.3, don't generate a fixed port mapping
                 * because it will almost certainly be wrong ! Just leave
                 * it as -1 which lets caller see that the VNC server isn't
                 * present yet. Subsquent dumps of the XML will eventually
                 * find the port in XenStore once VNC server has started
                 */
                if (port == -1 && xendConfigVersion < 2)
                    port = 5900 + domid;
                virBufferVSprintf(&buf, "    <input type='mouse' bus='%s'/>\n", hvm ? "ps2" : "xen");
                virBufferVSprintf(&buf, "    <graphics type='vnc' port='%d'", port);
                if (listenAddr)
                    virBufferVSprintf(&buf, " listen='%s'", listenAddr);
		if (flags & VIR_DOMAIN_XML_SECURE) {
		    vncPasswd = sexpr_fmt_node(root, "domain/image/%s/vncpasswd", hvm ? "hvm" : "linux");
		    if (vncPasswd)
			virBufferVSprintf(&buf, " passwd='%s'", vncPasswd);
		}
                if (keymap)
                    virBufferVSprintf(&buf, " keymap='%s'", keymap);
                virBufferAddLit(&buf, "/>\n");
            }
        }

        /* Graphics device (HVM, or old (pre-3.0.4) style PV sdl config) */
        tmp = sexpr_fmt_node(root, "domain/image/%s/sdl", hvm ? "hvm" : "linux");
        if (tmp != NULL) {
            if (tmp[0] == '1') {
                virBufferVSprintf(&buf, "    <input type='mouse' bus='%s'/>\n", hvm ? "ps2" : "xen");
                virBufferAddLit(&buf, "    <graphics type='sdl'/>\n");
            }
        }
    }

    tty = xenStoreDomainGetConsolePath(conn, domid);
    if (tty) {
        virBufferVSprintf(&buf, "    <console tty='%s'/>\n", tty);
        free(tty);
    }

    virBufferAddLit(&buf, "  </devices>\n");
    virBufferAddLit(&buf, "</domain>\n");

    buf.content[buf.use] = 0;
    return (buf.content);

  error:
    free(buf.content);
    return (NULL);
}

char *
xend_parse_domain_sexp(virConnectPtr conn, char *sexpr, int xendConfigVersion) {
  struct sexpr *root = string2sexpr(sexpr);
  char *data;

  if (!root)
      return NULL;

  data = xend_parse_sexp_desc(conn, root, xendConfigVersion, 0, NULL);

  sexpr_free(root);

  return data;
}

/**
 * sexpr_to_xend_domain_info:
 * @root: an S-Expression describing a domain
 * @info: a info data structure to fill=up
 *
 * Internal routine filling up the info structure with the values from
 * the domain root provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
sexpr_to_xend_domain_info(virDomainPtr domain, const struct sexpr *root,
                          virDomainInfoPtr info)
{
    const char *flags;


    if ((root == NULL) || (info == NULL))
        return (-1);

    info->memory = sexpr_u64(root, "domain/memory") << 10;
    info->maxMem = sexpr_u64(root, "domain/maxmem") << 10;
    flags = sexpr_node(root, "domain/state");

    if (flags) {
        if (strchr(flags, 'c'))
            info->state = VIR_DOMAIN_CRASHED;
        else if (strchr(flags, 's'))
            info->state = VIR_DOMAIN_SHUTOFF;
        else if (strchr(flags, 'd'))
            info->state = VIR_DOMAIN_SHUTDOWN;
        else if (strchr(flags, 'p'))
            info->state = VIR_DOMAIN_PAUSED;
        else if (strchr(flags, 'b'))
            info->state = VIR_DOMAIN_BLOCKED;
        else if (strchr(flags, 'r'))
            info->state = VIR_DOMAIN_RUNNING;
    } else {
        /* Inactive domains don't have a state reported, so
           mark them SHUTOFF, rather than NOSTATE */
        if (domain->id < 0)
            info->state = VIR_DOMAIN_SHUTOFF;
        else
            info->state = VIR_DOMAIN_NOSTATE;
    }
    info->cpuTime = sexpr_float(root, "domain/cpu_time") * 1000000000;
    info->nrVirtCpu = sexpr_int(root, "domain/vcpus");
    return (0);
}

/**
 * sexpr_to_xend_node_info:
 * @root: an S-Expression describing a domain
 * @info: a info data structure to fill up
 *
 * Internal routine filling up the info structure with the values from
 * the node root provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
sexpr_to_xend_node_info(const struct sexpr *root, virNodeInfoPtr info)
{
    const char *machine;


    if ((root == NULL) || (info == NULL))
        return (-1);

    machine = sexpr_node(root, "node/machine");
    if (machine == NULL) {
        info->model[0] = 0;
    } else {
        snprintf(&info->model[0], sizeof(info->model) - 1, "%s", machine);
        info->model[sizeof(info->model) - 1] = 0;
    }
    info->memory = (unsigned long) sexpr_u64(root, "node/total_memory") << 10;

    info->cpus = sexpr_int(root, "node/nr_cpus");
    info->mhz = sexpr_int(root, "node/cpu_mhz");
    info->nodes = sexpr_int(root, "node/nr_nodes");
    info->sockets = sexpr_int(root, "node/sockets_per_node");
    info->cores = sexpr_int(root, "node/cores_per_socket");
    info->threads = sexpr_int(root, "node/threads_per_core");

    /* Xen 3.2.0 replaces sockets_per_node with 'nr_cpus'.
     * Old Xen calculated sockets_per_node using its internal
     * nr_cpus / (nodes*cores*threads), so fake it ourselves
     * in the same way
     */
    if (info->sockets == 0) {
        int nr_cpus = sexpr_int(root, "node/nr_cpus");
        int procs = info->nodes * info->cores * info->threads;
        if (procs == 0) /* Sanity check in case of Xen bugs in futures..*/
            return (-1);
        info->sockets = nr_cpus / procs;
        /* Should already be fine, but for further sanity make
         * sure we have at least one socket
         */
        if (info->sockets == 0)
            info->sockets = 1;
    }
    return (0);
}


/**
 * sexpr_to_xend_topology
 * @root: an S-Expression describing a node
 * @caps: capability info
 *
 * Internal routine populating capability info with
 * NUMA node mapping details
 *
 * Returns 0 in case of success, -1 in case of error
 */
static int
sexpr_to_xend_topology(virConnectPtr conn,
                       const struct sexpr *root,
                       virCapsPtr caps)
{
    const char *nodeToCpu;
    const char *cur;
    char *cpuset = NULL;
    int *cpuNums = NULL;
    int cell, cpu, nb_cpus;
    int n = 0;
    int numCpus;

    nodeToCpu = sexpr_node(root, "node/node_to_cpu");
    if (nodeToCpu == NULL) {
        virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                     _("failed to parse topology information"));
        return -1;
    }

    numCpus = sexpr_int(root, "node/nr_cpus");


    cpuset = malloc(numCpus * sizeof(*cpuset));
    if (cpuset == NULL)
        goto memory_error;
    cpuNums = malloc(numCpus * sizeof(*cpuNums));
    if (cpuNums == NULL)
        goto memory_error;

    cur = nodeToCpu;
    while (*cur != 0) {
        /*
         * Find the next NUMA cell described in the xend output
         */
        cur = strstr(cur, "node");
        if (cur == NULL)
            break;
        cur += 4;
        cell = virParseNumber(&cur);
        if (cell < 0)
            goto parse_error;
        virSkipSpaces(&cur);
        if (*cur != ':')
            goto parse_error;
        cur++;
        virSkipSpaces(&cur);
        if (!strncmp(cur, "no cpus", 7)) {
            nb_cpus = 0;
            for (cpu = 0; cpu < numCpus; cpu++)
                cpuset[cpu] = 0;
        } else {
            nb_cpus = virParseCpuSet(conn, &cur, 'n', cpuset, numCpus);
            if (nb_cpus < 0)
                goto error;
        }

        for (n = 0, cpu = 0; cpu < numCpus; cpu++)
            if (cpuset[cpu] == 1)
                cpuNums[n++] = cpu;

        if (virCapabilitiesAddHostNUMACell(caps,
                                           cell,
                                           nb_cpus,
                                           cpuNums) < 0)
            goto memory_error;
    }
    free(cpuNums);
    free(cpuset);
    return (0);

  parse_error:
    virXendError(conn, VIR_ERR_XEN_CALL, _("topology syntax error"));
  error:
    free(cpuNums);
    free(cpuset);

    return (-1);

  memory_error:
    free(cpuNums);
    free(cpuset);
    virXendError(conn, VIR_ERR_NO_MEMORY, _("allocate buffer"));
    return (-1);
}


#ifndef PROXY
/**
 * sexpr_to_domain:
 * @conn: an existing virtual connection block
 * @root: an S-Expression describing a domain
 *
 * Internal routine returning the associated virDomainPtr for this domain
 *
 * Returns the domain pointer or NULL in case of error.
 */
static virDomainPtr
sexpr_to_domain(virConnectPtr conn, const struct sexpr *root)
{
    virDomainPtr ret = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    const char *name;
    const char *tmp;
    xenUnifiedPrivatePtr priv;

    if ((conn == NULL) || (root == NULL))
        return(NULL);

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (sexpr_uuid(uuid, root, "domain/uuid") < 0)
        goto error;
    name = sexpr_node(root, "domain/name");
    if (name == NULL)
        goto error;

    ret = virGetDomain(conn, name, uuid);
    if (ret == NULL) return NULL;

    tmp = sexpr_node(root, "domain/domid");
    /* New 3.0.4 XenD will not report a domid for inactive domains,
     * so only error out for old XenD
     */
    if (!tmp && priv->xendConfigVersion < 3)
        goto error;

    if (tmp)
        ret->id = sexpr_int(root, "domain/domid");
    else
        ret->id = -1; /* An inactive domain */

    return (ret);

error:
    virXendError(conn, VIR_ERR_INTERNAL_ERROR,
                 _("failed to parse Xend domain information"));
    if (ret != NULL)
        virUnrefDomain(ret);
    return(NULL);
}
#endif /* !PROXY */

/*****************************************************************
 ******
 ******
 ******
 ******
             Refactored
 ******
 ******
 ******
 ******
 *****************************************************************/
#ifndef PROXY
/**
 * xenDaemonOpen:
 * @conn: an existing virtual connection block
 * @name: optional argument to select a connection type
 * @flags: combination of virDrvOpenFlag(s)
 *
 * Creates a localhost Xen Daemon connection
 * Note: this doesn't try to check if the connection actually works
 *
 * Returns 0 in case of success, -1 in case of error.
 */
int
xenDaemonOpen(virConnectPtr conn,
              xmlURIPtr uri,
              virConnectAuthPtr auth ATTRIBUTE_UNUSED,
              int flags ATTRIBUTE_UNUSED)
{
    int ret;

    /* Switch on the scheme, which we expect to be NULL (file),
     * "http" or "xen".
     */
    if (uri->scheme == NULL) {
        /* It should be a file access */
        if (uri->path == NULL) {
            virXendError(NULL, VIR_ERR_NO_CONNECT, __FUNCTION__);
            goto failed;
        }
        ret = xenDaemonOpen_unix(conn, uri->path);
        if (ret < 0)
            goto failed;

        ret = xend_detect_config_version(conn);
        if (ret == -1)
            goto failed;
    }
    else if (STRCASEEQ (uri->scheme, "xen")) {
        /*
         * try first to open the unix socket
         */
        ret = xenDaemonOpen_unix(conn, "/var/lib/xend/xend-socket");
        if (ret < 0)
            goto try_http;
        ret = xend_detect_config_version(conn);
        if (ret != -1)
            goto done;

    try_http:
        /*
         * try though http on port 8000
         */
        ret = xenDaemonOpen_tcp(conn, "localhost", 8000);
        if (ret < 0)
            goto failed;
        ret = xend_detect_config_version(conn);
        if (ret == -1)
            goto failed;
    } else if (STRCASEEQ (uri->scheme, "http")) {
        ret = xenDaemonOpen_tcp(conn, uri->server, uri->port);
        if (ret < 0)
            goto failed;
        ret = xend_detect_config_version(conn);
        if (ret == -1)
            goto failed;
    } else {
        virXendError(NULL, VIR_ERR_NO_CONNECT, __FUNCTION__);
        goto failed;
    }

 done:
    return(ret);

failed:
    return(-1);
}


/**
 * xenDaemonClose:
 * @conn: an existing virtual connection block
 *
 * This method should be called when a connection to xend instance
 * initialized with xenDaemonOpen is no longer needed
 * to free the associated resources.
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xenDaemonClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

/**
 * xenDaemonDomainSuspend:
 * @domain: pointer to the Domain block
 *
 * Pause the domain, the domain is not scheduled anymore though its resources
 * are preserved. Use xenDaemonDomainResume() to resume execution.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainSuspend(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }
    if (domain->id < 0)
        return(-1);
    return xend_op(domain->conn, domain->name, "op", "pause", NULL);
}

/**
 * xenDaemonDomainResume:
 * @xend: pointer to the Xem Daemon block
 * @name: name for the domain
 *
 * Resume the domain after xenDaemonDomainSuspend() has been called
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainResume(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }
    if (domain->id < 0)
        return(-1);
    return xend_op(domain->conn, domain->name, "op", "unpause", NULL);
}

/**
 * xenDaemonDomainShutdown:
 * @domain: pointer to the Domain block
 *
 * Shutdown the domain, the OS is requested to properly shutdown
 * and the domain may ignore it.  It will return immediately
 * after queuing the request.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainShutdown(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }
    if (domain->id < 0)
        return(-1);
    return xend_op(domain->conn, domain->name, "op", "shutdown", "reason", "halt", NULL);
}

/**
 * xenDaemonDomainReboot:
 * @domain: pointer to the Domain block
 * @flags: extra flags for the reboot operation, not used yet
 *
 * Reboot the domain, the OS is requested to properly shutdown
 * and restart but the domain may ignore it.  It will return immediately
 * after queuing the request.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainReboot(virDomainPtr domain, unsigned int flags ATTRIBUTE_UNUSED)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }
    if (domain->id < 0)
        return(-1);
    return xend_op(domain->conn, domain->name, "op", "shutdown", "reason", "reboot", NULL);
}

/**
 * xenDaemonDomainDestroy:
 * @domain: pointer to the Domain block
 *
 * Abruptly halt the domain, the OS is not properly shutdown and the
 * resources allocated for the domain are immediately freed, mounted
 * filesystems will be marked as uncleanly shutdown.
 * After calling this function, the domain's status will change to
 * dying and will go away completely once all of the resources have been
 * unmapped (usually from the backend devices).
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainDestroy(virDomainPtr domain)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }
    if (domain->id < 0)
        return(-1);
    return xend_op(domain->conn, domain->name, "op", "destroy", NULL);
}

/**
 * xenDaemonDomainGetOSType:
 * @domain: a domain object
 *
 * Get the type of domain operation system.
 *
 * Returns the new string or NULL in case of error, the string must be
 *         freed by the caller.
 */
static char *
xenDaemonDomainGetOSType(virDomainPtr domain)
{
    char *type;
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(NULL);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(NULL);

    /* can we ask for a subset ? worth it ? */
    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return(NULL);

    if (sexpr_lookup(root, "domain/image/hvm")) {
        type = strdup("hvm");
    } else {
        type = strdup("linux");
    }

    sexpr_free(root);

    return(type);
}

/**
 * xenDaemonDomainSave:
 * @domain: pointer to the Domain block
 * @filename: path for the output file
 *
 * This method will suspend a domain and save its memory contents to
 * a file on disk.  Use xenDaemonDomainRestore() to restore a domain after
 * saving.
 * Note that for remote Xen Daemon the file path will be interpreted in
 * the remote host.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainSave(virDomainPtr domain, const char *filename)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL) ||
        (filename == NULL) || (domain->id < 0)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }


    /* We can't save the state of Domain-0, that would mean stopping it too */
    if (domain->id == 0) {
        return(-1);
    }

    return xend_op(domain->conn, domain->name, "op", "save", "file", filename, NULL);
}

/**
 * xenDaemonDomainCoreDump:
 * @domain: pointer to the Domain block
 * @filename: path for the output file
 * @flags: extra flags, currently unused
 *
 * This method will dump the core of a domain on a given file for analysis.
 * Note that for remote Xen Daemon the file path will be interpreted in
 * the remote host.
 *
 * Returns 0 in case of success, -1 in case of error.
 */
static int
xenDaemonDomainCoreDump(virDomainPtr domain, const char *filename,
                        int flags ATTRIBUTE_UNUSED)
{
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL) ||
        (filename == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }
    if (domain->id < 0)
        return(-1);
    return xend_op(domain->conn, domain->name, "op", "dump", "file", filename,
                   "live", "1", "crash", "0", NULL);
}

/**
 * xenDaemonDomainRestore:
 * @conn: pointer to the Xem Daemon block
 * @filename: path for the output file
 *
 * This method will restore a domain saved to disk by xenDaemonDomainSave().
 * Note that for remote Xen Daemon the file path will be interpreted in
 * the remote host.
 *
 * Returns 0 in case of success, -1 (with errno) in case of error.
 */
int
xenDaemonDomainRestore(virConnectPtr conn, const char *filename)
{
    if ((conn == NULL) || (filename == NULL)) {
        /* this should be caught at the interface but ... */
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }
    return xend_op(conn, "", "op", "restore", "file", filename, NULL);
}
#endif /* !PROXY */

/**
 * xenDaemonDomainGetMaxMemory:
 * @domain: pointer to the domain block
 *
 * Ask the Xen Daemon for the maximum memory allowed for a domain
 *
 * Returns the memory size in kilobytes or 0 in case of error.
 */
unsigned long
xenDaemonDomainGetMaxMemory(virDomainPtr domain)
{
    unsigned long ret = 0;
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    /* can we ask for a subset ? worth it ? */
    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return(0);

    ret = (unsigned long) sexpr_u64(root, "domain/memory") << 10;
    sexpr_free(root);

    return(ret);
}

#ifndef PROXY
/**
 * xenDaemonDomainSetMaxMemory:
 * @domain: pointer to the Domain block
 * @memory: The maximum memory in kilobytes
 *
 * This method will set the maximum amount of memory that can be allocated to
 * a domain.  Please note that a domain is able to allocate up to this amount
 * on its own.
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainSetMaxMemory(virDomainPtr domain, unsigned long memory)
{
    char buf[1024];
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    snprintf(buf, sizeof(buf), "%lu", memory >> 10);
    return xend_op(domain->conn, domain->name, "op", "maxmem_set", "memory",
                   buf, NULL);
}

/**
 * xenDaemonDomainSetMemory:
 * @domain: pointer to the Domain block
 * @memory: The target memory in kilobytes
 *
 * This method will set a target memory allocation for a given domain and
 * request that the guest meet this target.  The guest may or may not actually
 * achieve this target.  When this function returns, it does not signify that
 * the domain has actually reached that target.
 *
 * Memory for a domain can only be allocated up to the maximum memory setting.
 * There is no safe guard for allocations that are too small so be careful
 * when using this function to reduce a domain's memory usage.
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainSetMemory(virDomainPtr domain, unsigned long memory)
{
    char buf[1024];
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    snprintf(buf, sizeof(buf), "%lu", memory >> 10);
    return xend_op(domain->conn, domain->name, "op", "mem_target_set",
                   "target", buf, NULL);
}

#endif /* ! PROXY */

/* XXX change proxy to use Name instead of ID, then
   dumpxml will work over proxy for inactive domains
   and this can be removed */
char *
xenDaemonDomainDumpXMLByID(virConnectPtr conn, int domid, int flags,
                           const char *cpus)
{
    char *ret = NULL;
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;

    root = sexpr_get(conn, "/xend/domain/%d?detail=1", domid);
    if (root == NULL) {
        virXendError (conn, VIR_ERR_XEN_CALL,
                      _("xenDaemonDomainDumpXMLByID failed to"
                        " find this domain"));
        return (NULL);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    ret = xend_parse_sexp_desc(conn, root, priv->xendConfigVersion,
                               flags, cpus);
    sexpr_free(root);

    return (ret);
}

char *
xenDaemonDomainDumpXMLByName(virConnectPtr conn, const char *name, int flags,
                             const char *cpus)
{
    char *ret = NULL;
    struct sexpr *root;
    xenUnifiedPrivatePtr priv;

    root = sexpr_get(conn, "/xend/domain/%s?detail=1", name);
    if (root == NULL) {
        virXendError (conn, VIR_ERR_XEN_CALL,
                      _("xenDaemonDomainDumpXMLByName failed to"
                        " find this domain"));
        return (NULL);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    ret = xend_parse_sexp_desc(conn, root, priv->xendConfigVersion,
                               flags, cpus);
    sexpr_free(root);

    return (ret);
}


#ifndef PROXY
/**
 * xenDaemonDomainDumpXML:
 * @domain: a domain object
 * @flags: potential dump flags
 * @cpus: list of cpu the domain is pinned to.
 *
 * Provide an XML description of the domain.
 *
 * Returns a 0 terminated UTF-8 encoded XML instance, or NULL in case of error.
 *         the caller must free() the returned value.
 */
char *
xenDaemonDomainDumpXML(virDomainPtr domain, int flags, const char *cpus)
{
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(NULL);
    }
    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3) {
	// fall-through to the next driver to handle
        return(NULL);
    }

    if (domain->id < 0)
        return xenDaemonDomainDumpXMLByName(domain->conn, domain->name, flags,
	                                    cpus);
    else
        return xenDaemonDomainDumpXMLByID(domain->conn, domain->id, flags,
	                                  cpus);
}
#endif /* !PROXY */

/**
 * xenDaemonDomainGetInfo:
 * @domain: a domain object
 * @info: pointer to a virDomainInfo structure allocated by the user
 *
 * This method looks up information about a domain and update the
 * information block provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xenDaemonDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    struct sexpr *root;
    int ret;
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL) ||
        (info == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    root = sexpr_get(domain->conn, "/xend/domain/%s?detail=1", domain->name);
    if (root == NULL)
        return (-1);

    ret = sexpr_to_xend_domain_info(domain, root, info);
    sexpr_free(root);
    return (ret);
}

#ifndef PROXY
/**
 * xenDaemonLookupByName:
 * @conn: A xend instance
 * @name: The name of the domain
 *
 * This method looks up information about a domain and returns
 * it in the form of a struct xend_domain.  This should be
 * free()'d when no longer needed.
 *
 * Returns domain info on success; NULL (with errno) on error
 */
virDomainPtr
xenDaemonLookupByName(virConnectPtr conn, const char *domname)
{
    struct sexpr *root;
    virDomainPtr ret = NULL;

    if ((conn == NULL) || (domname == NULL)) {
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return(NULL);
    }

    root = sexpr_get(conn, "/xend/domain/%s?detail=1", domname);
    if (root == NULL)
        goto error;

    ret = sexpr_to_domain(conn, root);

error:
    sexpr_free(root);
    return(ret);
}
#endif /* ! PROXY */

/**
 * xenDaemonNodeGetInfo:
 * @conn: pointer to the Xen Daemon block
 * @info: pointer to a virNodeInfo structure allocated by the user
 *
 * Extract hardware information about the node.
 *
 * Returns 0 in case of success and -1 in case of failure.
 */
int
xenDaemonNodeGetInfo(virConnectPtr conn, virNodeInfoPtr info) {
    int ret = -1;
    struct sexpr *root;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }
    if (info == NULL) {
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL)
        return (-1);

    ret = sexpr_to_xend_node_info(root, info);
    sexpr_free(root);
    return (ret);
}

/**
 * xenDaemonNodeGetTopology:
 * @conn: pointer to the Xen Daemon block
 * @caps: capabilities info
 *
 * This method retrieves a node's topology information.
 *
 * Returns -1 in case of error, 0 otherwise.
 */
int
xenDaemonNodeGetTopology(virConnectPtr conn,
                         virCapsPtr caps) {
    int ret = -1;
    struct sexpr *root;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    if (caps == NULL) {
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }

    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL) {
        return (-1);
    }

    ret = sexpr_to_xend_topology(conn, root, caps);
    sexpr_free(root);
    return (ret);
}

/**
 * xenDaemonGetVersion:
 * @conn: pointer to the Xen Daemon block
 * @hvVer: return value for the version of the running hypervisor (OUT)
 *
 * Get the version level of the Hypervisor running.
 *
 * Returns -1 in case of error, 0 otherwise. if the version can't be
 *    extracted by lack of capacities returns 0 and @hvVer is 0, otherwise
 *    @hvVer value is major * 1,000,000 + minor * 1,000 + release
 */
int
xenDaemonGetVersion(virConnectPtr conn, unsigned long *hvVer)
{
    struct sexpr *root;
    int major, minor;
    unsigned long version;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }
    if (hvVer == NULL) {
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (-1);
    }
    root = sexpr_get(conn, "/xend/node/");
    if (root == NULL)
	return(-1);

    major = sexpr_int(root, "node/xen_major");
    minor = sexpr_int(root, "node/xen_minor");
    sexpr_free(root);
    version = major * 1000000 + minor * 1000;
    *hvVer = version;
    return(0);
}

#ifndef PROXY
/**
 * xenDaemonListDomains:
 * @conn: pointer to the hypervisor connection
 * @ids: array to collect the list of IDs of active domains
 * @maxids: size of @ids
 *
 * Collect the list of active domains, and store their ID in @maxids
 * TODO: this is quite expensive at the moment since there isn't one
 *       xend RPC providing both name and id for all domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
static int
xenDaemonListDomains(virConnectPtr conn, int *ids, int maxids)
{
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;
    long id;

    if ((ids == NULL) || (maxids <= 0))
        goto error;
    root = sexpr_get(conn, "/xend/domain");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        id = xenDaemonDomainLookupByName_ids(conn, node->u.value, NULL);
        if (id >= 0)
            ids[ret++] = (int) id;
        if (ret >= maxids)
            break;
    }

error:
	sexpr_free(root);
    return(ret);
}

/**
 * xenDaemonNumOfDomains:
 * @conn: pointer to the hypervisor connection
 *
 * Provides the number of active domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
static int
xenDaemonNumOfDomains(virConnectPtr conn)
{
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;

    root = sexpr_get(conn, "/xend/domain");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
	ret++;
    }

error:
	sexpr_free(root);
    return(ret);
}
#endif /* ! PROXY */

#ifndef PROXY
/**
 * xenDaemonLookupByID:
 * @conn: pointer to the hypervisor connection
 * @id: the domain ID number
 *
 * Try to find a domain based on the hypervisor ID number
 *
 * Returns a new domain object or NULL in case of failure
 */
virDomainPtr
xenDaemonLookupByID(virConnectPtr conn, int id) {
    char *name = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virDomainPtr ret;

    if (xenDaemonDomainLookupByID(conn, id, &name, uuid) < 0) {
        goto error;
    }

    ret = virGetDomain(conn, name, uuid);
    if (ret == NULL) return NULL;

    ret->id = id;
    free(name);
    return (ret);

 error:
    free(name);
    return (NULL);
}

/**
 * xenDaemonDomainSetVcpus:
 * @domain: pointer to domain object
 * @nvcpus: the new number of virtual CPUs for this domain
 *
 * Dynamically change the number of virtual CPUs used by the domain.
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainSetVcpus(virDomainPtr domain, unsigned int vcpus)
{
    char buf[VIR_UUID_BUFLEN];
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)
     || (vcpus < 1)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (domain->id < 0 && priv->xendConfigVersion < 3)
        return(-1);

    snprintf(buf, sizeof(buf), "%d", vcpus);
    return(xend_op(domain->conn, domain->name, "op", "set_vcpus", "vcpus",
                   buf, NULL));
}

/**
 * xenDaemonDomainPinCpu:
 * @domain: pointer to domain object
 * @vcpu: virtual CPU number
 * @cpumap: pointer to a bit map of real CPUs (in 8-bit bytes)
 * @maplen: length of cpumap in bytes
 *
 * Dynamically change the real CPUs which can be allocated to a virtual CPU.
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int
xenDaemonDomainPinVcpu(virDomainPtr domain, unsigned int vcpu,
                     unsigned char *cpumap, int maplen)
{
    char buf[VIR_UUID_BUFLEN], mapstr[sizeof(cpumap_t) * 64] = "[";
    int i, j;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)
     || (cpumap == NULL) || (maplen < 1) || (maplen > (int)sizeof(cpumap_t))) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return (-1);
    }
    if (domain->id < 0)
        return(-1);

    /* from bit map, build character string of mapped CPU numbers */
    for (i = 0; i < maplen; i++) for (j = 0; j < 8; j++)
     if (cpumap[i] & (1 << j)) {
        snprintf(buf, sizeof(buf), "%d,", (8 * i) + j);
        strcat(mapstr, buf);
    }
    mapstr[strlen(mapstr) - 1] = ']';
    snprintf(buf, sizeof(buf), "%d", vcpu);
    return(xend_op(domain->conn, domain->name, "op", "pincpu", "vcpu", buf,
                  "cpumap", mapstr, NULL));
}

/**
 * virDomainGetVcpus:
 * @domain: pointer to domain object, or NULL for Domain0
 * @info: pointer to an array of virVcpuInfo structures (OUT)
 * @maxinfo: number of structures in info array
 * @cpumaps: pointer to an bit map of real CPUs for all vcpus of this domain (in 8-bit bytes) (OUT)
 *	If cpumaps is NULL, then no cupmap information is returned by the API.
 *	It's assumed there is <maxinfo> cpumap in cpumaps array.
 *	The memory allocated to cpumaps must be (maxinfo * maplen) bytes
 *	(ie: calloc(maxinfo, maplen)).
 *	One cpumap inside cpumaps has the format described in virDomainPinVcpu() API.
 * @maplen: number of bytes in one cpumap, from 1 up to size of CPU map in
 *	underlying virtualization system (Xen...).
 *
 * Extract information about virtual CPUs of domain, store it in info array
 * and also in cpumaps if this pointer is'nt NULL.
 *
 * Returns the number of info filled in case of success, -1 in case of failure.
 */
int
xenDaemonDomainGetVcpus(virDomainPtr domain, virVcpuInfoPtr info, int maxinfo,
                        unsigned char *cpumaps, int maplen)
{
    struct sexpr *root, *s, *t;
    virVcpuInfoPtr ipt = info;
    int nbinfo = 0, oln;
    unsigned char *cpumap;
    int vcpu, cpu;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)
        || (info == NULL) || (maxinfo < 1)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                     __FUNCTION__);
        return (-1);
    }
    if (cpumaps != NULL && maplen < 1) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                     __FUNCTION__);
        return (-1);
    }
    if (domain->id < 0)
        return(-1);

    root = sexpr_get(domain->conn, "/xend/domain/%s?op=vcpuinfo", domain->name);
    if (root == NULL)
        return (-1);

    if (cpumaps != NULL)
        memset(cpumaps, 0, maxinfo * maplen);

    /* scan the sexprs from "(vcpu (number x)...)" and get parameter values */
    for (s = root; s->kind == SEXPR_CONS; s = s->u.s.cdr) {
        if ((s->u.s.car->kind == SEXPR_CONS) &&
            (s->u.s.car->u.s.car->kind == SEXPR_VALUE) &&
            !strcmp(s->u.s.car->u.s.car->u.value, "vcpu")) {
            t = s->u.s.car;
            vcpu = ipt->number = sexpr_int(t, "vcpu/number");
            if ((oln = sexpr_int(t, "vcpu/online")) != 0) {
                if (sexpr_int(t, "vcpu/running")) ipt->state = VIR_VCPU_RUNNING;
                if (sexpr_int(t, "vcpu/blocked")) ipt->state = VIR_VCPU_BLOCKED;
            }
            else
                ipt->state = VIR_VCPU_OFFLINE;
            ipt->cpuTime = sexpr_float(t, "vcpu/cpu_time") * 1000000000;
            ipt->cpu = oln ? sexpr_int(t, "vcpu/cpu") : -1;

            if (cpumaps != NULL && vcpu >= 0 && vcpu < maxinfo) {
                cpumap = (unsigned char *) VIR_GET_CPUMAP(cpumaps, maplen, vcpu);
                /*
                 * get sexpr from "(cpumap (x y z...))" and convert values
                 * to bitmap
                 */
                for (t = t->u.s.cdr; t->kind == SEXPR_CONS; t = t->u.s.cdr)
                    if ((t->u.s.car->kind == SEXPR_CONS) &&
                        (t->u.s.car->u.s.car->kind == SEXPR_VALUE) &&
                        !strcmp(t->u.s.car->u.s.car->u.value, "cpumap") &&
                        (t->u.s.car->u.s.cdr->kind == SEXPR_CONS)) {
                        for (t = t->u.s.car->u.s.cdr->u.s.car; t->kind == SEXPR_CONS; t = t->u.s.cdr)
                            if (t->u.s.car->kind == SEXPR_VALUE
                                && virStrToLong_i(t->u.s.car->u.value, NULL, 10, &cpu) == 0
                                && cpu >= 0
                                && (VIR_CPU_MAPLEN(cpu+1) <= maplen)) {
                                VIR_USE_CPU(cpumap, cpu);
                            }
                        break;
                    }
            }

            if (++nbinfo == maxinfo) break;
            ipt++;
        }
    }
    sexpr_free(root);
    return(nbinfo);
}

/**
 * xenDaemonLookupByUUID:
 * @conn: pointer to the hypervisor connection
 * @uuid: the raw UUID for the domain
 *
 * Try to lookup a domain on xend based on its UUID.
 *
 * Returns a new domain object or NULL in case of failure
 */
virDomainPtr
xenDaemonLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    virDomainPtr ret;
    char *name = NULL;
    int id = -1;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    /* Old approach for xen <= 3.0.3 */
    if (priv->xendConfigVersion < 3) {
        char **names, **tmp;
        unsigned char ident[VIR_UUID_BUFLEN];
        names = xenDaemonListDomainsOld(conn);
        tmp = names;

        if (names == NULL) {
            return (NULL);
        }
        while (*tmp != NULL) {
            id = xenDaemonDomainLookupByName_ids(conn, *tmp, &ident[0]);
            if (id >= 0) {
                if (!memcmp(uuid, ident, VIR_UUID_BUFLEN)) {
                    name = strdup(*tmp);
                    break;
                }
            }
            tmp++;
        }
        free(names);
    } else { /* New approach for xen >= 3.0.4 */
        char *domname = NULL;
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        struct sexpr *root = NULL;

        virUUIDFormat(uuid, uuidstr);
        root = sexpr_get(conn, "/xend/domain/%s?detail=1", uuidstr);
        if (root == NULL)
            return (NULL);
        domname = (char*)sexpr_node(root, "domain/name");
        if (sexpr_node(root, "domain/domid")) /* only active domains have domid */
            id = sexpr_int(root, "domain/domid");
        else
            id = -1;
        name = domname ? strdup(domname) : NULL;
        sexpr_free(root);
    }

    if (name == NULL)
        return (NULL);

    ret = virGetDomain(conn, name, uuid);
    if (ret == NULL) return NULL;

    ret->id = id;
    free(name);
    return (ret);
}

/**
 * xenDaemonCreateLinux:
 * @conn: pointer to the hypervisor connection
 * @xmlDesc: an XML description of the domain
 * @flags: an optional set of virDomainFlags
 *
 * Launch a new Linux guest domain, based on an XML description similar
 * to the one returned by virDomainGetXMLDesc()
 * This function may requires priviledged access to the hypervisor.
 *
 * Returns a new domain object or NULL in case of failure
 */
static virDomainPtr
xenDaemonCreateLinux(virConnectPtr conn, const char *xmlDesc,
                     unsigned int flags ATTRIBUTE_UNUSED)
{
    int ret;
    char *sexpr;
    char *name = NULL;
    virDomainPtr dom = NULL;
    xenUnifiedPrivatePtr priv;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (xmlDesc == NULL) {
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    sexpr = virDomainParseXMLDesc(conn, xmlDesc, &name, priv->xendConfigVersion);
    if ((sexpr == NULL) || (name == NULL)) {
        virXendError(conn, VIR_ERR_XML_ERROR,
                     _("failed to parse domain description"));
        free(sexpr);
        free(name);

        return (NULL);
    }

    ret = xenDaemonDomainCreateLinux(conn, sexpr);
    free(sexpr);
    if (ret != 0) {
        goto error;
    }

    /* This comes before wait_for_devices, to ensure that latter
       cleanup will destroy the domain upon failure */
    if (!(dom = virDomainLookupByName(conn, name)))
        goto error;

    if ((ret = xend_wait_for_devices(conn, name)) < 0)
        goto error;

    if ((ret = xenDaemonDomainResume(dom)) < 0)
        goto error;

    free(name);

    return (dom);

  error:
    /* Make sure we don't leave a still-born domain around */
    if (dom != NULL) {
        xenDaemonDomainDestroy(dom);
        virUnrefDomain(dom);
    }
    free(name);
    return (NULL);
}

/**
 * xenDaemonAttachDevice:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 *
 * Create a virtual device attachment to backend.
 * XML description is translated into S-expression.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenDaemonAttachDevice(virDomainPtr domain, const char *xml)
{
    char *sexpr, *conf, *str;
    int hvm = 0, ret;
    xenUnifiedPrivatePtr priv;
    char class[8], ref[80];

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return (-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    /*
     * on older Xen without the inactive guests management
     * avoid doing this on inactive guests
     */
    if ((domain->id < 0) && (priv->xendConfigVersion < 3))
        return (-1);

    str = virDomainGetOSType(domain);
    if (strcmp(str, "linux"))
        hvm = 1;
    free(str);
    sexpr = virParseXMLDevice(domain->conn, xml, hvm, priv->xendConfigVersion);
    if (sexpr == NULL)
        return (-1);
    if (!memcmp(sexpr, "(device ", 8)) {
        conf = sexpr + 8;
        *(conf + strlen(conf) -1) = 0; /* suppress final ) */
    }
    else conf = sexpr;
    if (virDomainXMLDevID(domain, xml, class, ref, sizeof(ref))) {
        /* device doesn't exist, define it */
        ret = xend_op(domain->conn, domain->name, "op", "device_create",
                      "config", conf, NULL);
    }
    else {
        /* device exists, attempt to modify it */
        ret = xend_op(domain->conn, domain->name, "op", "device_configure",
                      "config", conf, "dev", ref, NULL);
    }
    free(sexpr);
    return ret;
}

/**
 * xenDaemonDetachDevice:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 *
 * Destroy a virtual device attachment to backend.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenDaemonDetachDevice(virDomainPtr domain, const char *xml)
{
    char class[8], ref[80];

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return (-1);
    }
    if (virDomainXMLDevID(domain, xml, class, ref, sizeof(ref)))
        return (-1);
    return(xend_op(domain->conn, domain->name, "op", "device_destroy",
        "type", class, "dev", ref, NULL));
}


int
xenDaemonDomainMigratePrepare (virConnectPtr dconn,
                               char **cookie ATTRIBUTE_UNUSED,
                               int *cookielen ATTRIBUTE_UNUSED,
                               const char *uri_in,
                               char **uri_out,
                               unsigned long flags ATTRIBUTE_UNUSED,
                               const char *dname ATTRIBUTE_UNUSED,
                               unsigned long resource ATTRIBUTE_UNUSED)
{
    int r;
    char hostname [HOST_NAME_MAX+1];

    /* If uri_in is NULL, get the current hostname as a best guess
     * of how the source host should connect to us.  Note that caller
     * deallocates this string.
     */
    if (uri_in == NULL) {
        r = gethostname (hostname, HOST_NAME_MAX+1);
        if (r == -1) {
            virXendError (dconn, VIR_ERR_SYSTEM_ERROR, strerror (errno));
            return -1;
        }
        *uri_out = strdup (hostname);
        if (*uri_out == NULL) {
            virXendError (dconn, VIR_ERR_SYSTEM_ERROR, strerror (errno));
            return -1;
        }
    }

    return 0;
}

int
xenDaemonDomainMigratePerform (virDomainPtr domain,
                               const char *cookie ATTRIBUTE_UNUSED,
                               int cookielen ATTRIBUTE_UNUSED,
                               const char *uri,
                               unsigned long flags,
                               const char *dname,
                               unsigned long bandwidth)
{
    /* Upper layers have already checked domain. */
    virConnectPtr conn = domain->conn;
    /* NB: Passing port=0 to xend means it ignores
     * the port.  However this is somewhat specific to
     * the internals of the xend Python code. (XXX).
     */
    char port[16] = "0";
    char live[2] = "0";
    int ret;
    char *p, *hostname = NULL;

    /* Xen doesn't support renaming domains during migration. */
    if (dname) {
        virXendError (conn, VIR_ERR_NO_SUPPORT,
                      _("xenDaemonDomainMigrate: Xen does not support"
                        " renaming domains during migration"));
        return -1;
    }

    /* Xen (at least up to 3.1.0) takes a resource parameter but
     * ignores it.
     */
    if (bandwidth) {
        virXendError (conn, VIR_ERR_NO_SUPPORT,
                      _("xenDaemonDomainMigrate: Xen does not support"
                        " bandwidth limits during migration"));
        return -1;
    }

    /* Check the flags. */
    if ((flags & VIR_MIGRATE_LIVE)) {
        strcpy (live, "1");
        flags &= ~VIR_MIGRATE_LIVE;
    }
    if (flags != 0) {
        virXendError (conn, VIR_ERR_NO_SUPPORT,
                      _("xenDaemonDomainMigrate: unsupported flag"));
        return -1;
    }

    /* Set hostname and port.
     *
     * URI is non-NULL (guaranteed by caller).  We expect either
     * "hostname", "hostname:port" or "xenmigr://hostname[:port]/".
     */
    if (strstr (uri, "//")) {   /* Full URI. */
        xmlURIPtr uriptr = xmlParseURI (uri);
        if (!uriptr) {
            virXendError (conn, VIR_ERR_INVALID_ARG,
                          _("xenDaemonDomainMigrate: invalid URI"));
            return -1;
        }
        if (uriptr->scheme && STRCASENEQ (uriptr->scheme, "xenmigr")) {
            virXendError (conn, VIR_ERR_INVALID_ARG,
                          _("xenDaemonDomainMigrate: only xenmigr://"
                            " migrations are supported by Xen"));
            xmlFreeURI (uriptr);
            return -1;
        }
        if (!uriptr->server) {
            virXendError (conn, VIR_ERR_INVALID_ARG,
                          _("xenDaemonDomainMigrate: a hostname must be"
                            " specified in the URI"));
            xmlFreeURI (uriptr);
            return -1;
        }
        hostname = strdup (uriptr->server);
        if (!hostname) {
            virXendError (conn, VIR_ERR_NO_MEMORY, _("strdup failed"));
            xmlFreeURI (uriptr);
            return -1;
        }
        if (uriptr->port)
            snprintf (port, sizeof port, "%d", uriptr->port);
        xmlFreeURI (uriptr);
    }
    else if ((p = strrchr (uri, ':')) != NULL) { /* "hostname:port" */
        int port_nr, n;

        if (sscanf (p+1, "%d", &port_nr) != 1) {
            virXendError (conn, VIR_ERR_INVALID_ARG,
                          _("xenDaemonDomainMigrate: invalid port number"));
            return -1;
        }
        snprintf (port, sizeof port, "%d", port_nr);

        /* Get the hostname. */
        n = p - uri; /* n = Length of hostname in bytes. */
        hostname = strdup (uri);
        if (!hostname) {
            virXendError (conn, VIR_ERR_NO_MEMORY, _("strdup failed"));
            return -1;
        }
        hostname[n] = '\0';
    }
    else {                      /* "hostname" (or IP address) */
        hostname = strdup (uri);
        if (!hostname) {
            virXendError (conn, VIR_ERR_NO_MEMORY, _("strdup failed"));
            return -1;
        }
    }

    DEBUG("hostname = %s, port = %s", hostname, port);

    /* Make the call. */
    ret = xend_op (domain->conn, domain->name,
                   "op", "migrate",
                   "destination", hostname,
                   "live", live,
                   "port", port,
                   "resource", "0", /* required, xend ignores it */
                   NULL);
    free (hostname);

    DEBUG0("migration done");

    return ret;
}

virDomainPtr xenDaemonDomainDefineXML(virConnectPtr conn, const char *xmlDesc) {
    int ret;
    char *sexpr;
    char *name = NULL;
    virDomainPtr dom;
    xenUnifiedPrivatePtr priv;

    if (!VIR_IS_CONNECT(conn)) {
        virXendError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (xmlDesc == NULL) {
        virXendError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(NULL);

    sexpr = virDomainParseXMLDesc(conn, xmlDesc, &name, priv->xendConfigVersion);
    if ((sexpr == NULL) || (name == NULL)) {
        virXendError(conn, VIR_ERR_XML_ERROR,
                     _("failed to parse domain description"));
        free(sexpr);
        free(name);

        return (NULL);
    }

    ret = xend_op(conn, "", "op", "new", "config", sexpr, NULL);
    free(sexpr);
    if (ret != 0) {
        fprintf(stderr, _("Failed to create inactive domain %s\n"), name);
        goto error;
    }

    dom = virDomainLookupByName(conn, name);
    if (dom == NULL) {
        goto error;
    }

    return (dom);
  error:
    free(name);
    return (NULL);
}
int xenDaemonDomainCreate(virDomainPtr domain)
{
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(-1);

    return xend_op(domain->conn, domain->name, "op", "start", NULL);
}

int xenDaemonDomainUndefine(virDomainPtr domain)
{
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        virXendError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
	             __FUNCTION__);
        return(-1);
    }

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(-1);

    return xend_op(domain->conn, domain->name, "op", "delete", NULL);
}

/**
 * xenDaemonNumOfDomains:
 * @conn: pointer to the hypervisor connection
 *
 * Provides the number of active domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
static int
xenDaemonNumOfDefinedDomains(virConnectPtr conn)
{
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    /* xm_internal.c (the support for defined domains from /etc/xen
     * config files used by old Xen) will handle this.
     */
    if (priv->xendConfigVersion < 3)
        return(-1);

    root = sexpr_get(conn, "/xend/domain?state=halted");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;
        ret++;
    }

error:
    sexpr_free(root);
    return(ret);
}

int xenDaemonListDefinedDomains(virConnectPtr conn, char **const names, int maxnames) {
    struct sexpr *root = NULL;
    int ret = -1;
    struct sexpr *_for_i, *node;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (priv->xendConfigVersion < 3)
        return(-1);

    if ((names == NULL) || (maxnames <= 0))
        goto error;
    root = sexpr_get(conn, "/xend/domain?state=halted");
    if (root == NULL)
        goto error;

    ret = 0;

    for (_for_i = root, node = root->u.s.car; _for_i->kind == SEXPR_CONS;
         _for_i = _for_i->u.s.cdr, node = _for_i->u.s.car) {
        if (node->kind != SEXPR_VALUE)
            continue;

        names[ret++] = strdup(node->u.value);
        if (ret >= maxnames)
            break;
    }

error:
    sexpr_free(root);
    return(ret);
}

#endif /* ! PROXY */
#endif /* WITH_XEN */

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
