/*
 * network.c: network helper APIs for libvirt
 *
 * Copyright (C) 2009-2010 Red Hat, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Daniel Veillard <veillard@redhat.com>
 */

#include <config.h>
#include <arpa/inet.h>

#include "memory.h"
#include "network.h"
#include "util.h"
#include "virterror_internal.h"

#define VIR_FROM_THIS VIR_FROM_NONE
#define virSocketError(code, ...)                                       \
    virReportErrorHelper(NULL, VIR_FROM_THIS, code, __FILE__,           \
                         __FUNCTION__, __LINE__, __VA_ARGS__)

/*
 * Helpers to extract the IP arrays from the virSocketAddrPtr
 * That part is the less portable of the module
 */
typedef unsigned char virIPv4Addr[4];
typedef virIPv4Addr *virIPv4AddrPtr;
typedef unsigned short virIPv6Addr[8];
typedef virIPv6Addr *virIPv6AddrPtr;

static int getIPv4Addr(virSocketAddrPtr addr, virIPv4AddrPtr tab) {
    unsigned long val;
    int i;

    if ((addr == NULL) || (tab == NULL) || (addr->data.stor.ss_family != AF_INET))
        return(-1);

    val = ntohl(addr->data.inet4.sin_addr.s_addr);

    for (i = 0;i < 4;i++) {
        (*tab)[3 - i] = val & 0xFF;
        val >>= 8;
    }

    return(0);
}

static int getIPv6Addr(virSocketAddrPtr addr, virIPv6AddrPtr tab) {
    int i;

    if ((addr == NULL) || (tab == NULL) || (addr->data.stor.ss_family != AF_INET6))
        return(-1);

    for (i = 0;i < 8;i++) {
        (*tab)[i] = ((addr->data.inet6.sin6_addr.s6_addr[2 * i] << 8) |
                     addr->data.inet6.sin6_addr.s6_addr[2 * i + 1]);
    }

    return(0);
}

/**
 * virSocketParseAddr:
 * @val: a numeric network address IPv4 or IPv6
 * @addr: where to store the return value, optional.
 * @family: address family to pass down to getaddrinfo
 *
 * Mostly a wrapper for getaddrinfo() extracting the address storage
 * from the numeric string like 1.2.3.4 or 2001:db8:85a3:0:0:8a2e:370:7334
 *
 * Returns the length of the network address or -1 in case of error.
 */
int
virSocketParseAddr(const char *val, virSocketAddrPtr addr, int family) {
    int len;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int err;

    if (val == NULL) {
        virSocketError(VIR_ERR_INVALID_ARG, _("Missing address"));
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_flags = AI_NUMERICHOST;
    if ((err = getaddrinfo(val, NULL, &hints, &res)) != 0) {
        virSocketError(VIR_ERR_SYSTEM_ERROR,
                       _("Cannot parse socket address '%s': %s"),
                       val, gai_strerror(err));
        return -1;
    }

    if (res == NULL) {
        virSocketError(VIR_ERR_SYSTEM_ERROR,
                       _("No socket addresses found for '%s'"),
                       val);
        return -1;
    }

    len = res->ai_addrlen;
    if (addr != NULL) {
        memcpy(&addr->data.stor, res->ai_addr, len);
        addr->len = res->ai_addrlen;
    }

    freeaddrinfo(res);
    return(len);
}

/*
 * virSocketParseIpv4Addr:
 * @val: an IPv4 numeric address
 * @addr: the location to store the result
 *
 * Extract the address storage from an IPv4 numeric address
 *
 * Returns the length of the network address or -1 in case of error.
 */
int
virSocketParseIpv4Addr(const char *val, virSocketAddrPtr addr) {
    return(virSocketParseAddr(val, addr, AF_INET));
}

/*
 * virSocketParseIpv6Addr:
 * @val: an IPv6 numeric address
 * @addr: the location to store the result
 *
 * Extract the address storage from an IPv6 numeric address
 *
 * Returns the length of the network address or -1 in case of error.
 */
int
virSocketParseIpv6Addr(const char *val, virSocketAddrPtr addr) {
    return(virSocketParseAddr(val, addr, AF_INET6));
}

/*
 * virSocketFormatAddr:
 * @addr: an initialized virSocketAddrPtr
 *
 * Returns a string representation of the given address
 * Returns NULL on any error
 * Caller must free the returned string
 */
char *
virSocketFormatAddr(virSocketAddrPtr addr) {
    return virSocketFormatAddrFull(addr, false, NULL);
}


/*
 * virSocketFormatAddrFull:
 * @addr: an initialized virSocketAddrPtr
 * @withService: if true, then service info is appended
 * @separator: separator between hostname & service.
 *
 * Returns a string representation of the given address
 * Returns NULL on any error
 * Caller must free the returned string
 */
char *
virSocketFormatAddrFull(virSocketAddrPtr addr,
                        bool withService,
                        const char *separator)
{
    char host[NI_MAXHOST], port[NI_MAXSERV];
    char *addrstr;
    int err;

    if (addr == NULL) {
        virSocketError(VIR_ERR_INVALID_ARG, _("Missing address"));
        return NULL;
    }

    /* Short-circuit since getnameinfo doesn't work
     * nicely for UNIX sockets */
    if (addr->data.sa.sa_family == AF_UNIX) {
        if (withService) {
            if (virAsprintf(&addrstr, "127.0.0.1%s0",
                            separator ? separator : ":") < 0)
                goto no_memory;
        } else {
            if (!(addrstr = strdup("127.0.0.1")))
                goto no_memory;
        }
        return addrstr;
    }

    if ((err = getnameinfo(&addr->data.sa,
                           addr->len,
                           host, sizeof(host),
                           port, sizeof(port),
                           NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
        virSocketError(VIR_ERR_SYSTEM_ERROR,
                       _("Cannot convert socket address to string: %s"),
                       gai_strerror(err));
        return NULL;
    }

    if (withService) {
        if (virAsprintf(&addrstr, "%s%s%s", host, separator, port) == -1)
            goto no_memory;
    } else {
        if (!(addrstr = strdup(host)))
            goto no_memory;
    }

    return addrstr;

no_memory:
    virReportOOMError();
    return NULL;
}


/*
 * virSocketSetPort:
 * @addr: an initialized virSocketAddrPtr
 * @port: the port number to set
 *
 * Set the transport layer port of the given virtSocketAddr
 *
 * Returns 0 on success, -1 on failure
 */
int
virSocketSetPort(virSocketAddrPtr addr, int port) {
    if (addr == NULL)
        return -1;

    port = htons(port);

    if(addr->data.stor.ss_family == AF_INET) {
        addr->data.inet4.sin_port = port;
    }

    else if(addr->data.stor.ss_family == AF_INET6) {
        addr->data.inet6.sin6_port = port;
    }

    else {
        return -1;
    }

    return 0;
}

/*
 * virSocketGetPort:
 * @addr: an initialized virSocketAddrPtr
 *
 * Returns the transport layer port of the given virtSocketAddr
 * Returns -1 if @addr is invalid
 */
int
virSocketGetPort(virSocketAddrPtr addr) {
    if (addr == NULL)
        return -1;

    if(addr->data.stor.ss_family == AF_INET) {
        return ntohs(addr->data.inet4.sin_port);
    }

    else if(addr->data.stor.ss_family == AF_INET6) {
        return ntohs(addr->data.inet6.sin6_port);
    }

    return -1;
}

/**
 * virSocketAddrIsNetmask:
 * @netmask: the netmask address
 *
 * Check that @netmask is a proper network mask
 *
 * Returns 0 in case of success and -1 in case of error
 */
int virSocketAddrIsNetmask(virSocketAddrPtr netmask) {
    int n = virSocketGetNumNetmaskBits(netmask);
    if (n < 0)
        return -1;
    return 0;
}

/**
 * virSocketCheckNetmask:
 * @addr1: a first network address
 * @addr2: a second network address
 * @netmask: the netmask address
 *
 * Check that @addr1 and @addr2 pertain to the same @netmask address
 * range and returns the size of the range
 *
 * Returns 1 in case of success and 0 in case of failure and
 *         -1 in case of error
 */
int virSocketCheckNetmask(virSocketAddrPtr addr1, virSocketAddrPtr addr2,
                          virSocketAddrPtr netmask) {
    int i;

    if ((addr1 == NULL) || (addr2 == NULL) || (netmask == NULL))
        return(-1);
    if ((addr1->data.stor.ss_family != addr2->data.stor.ss_family) ||
        (addr1->data.stor.ss_family != netmask->data.stor.ss_family))
        return(-1);

    if (virSocketAddrIsNetmask(netmask) != 0)
        return(-1);

    if (addr1->data.stor.ss_family == AF_INET) {
        virIPv4Addr t1, t2, tm;

        if ((getIPv4Addr(addr1, &t1) < 0) ||
            (getIPv4Addr(addr2, &t2) < 0) ||
            (getIPv4Addr(netmask, &tm) < 0))
            return(-1);

        for (i = 0;i < 4;i++) {
            if ((t1[i] & tm[i]) != (t2[i] & tm[i]))
                return(0);
        }

    } else if (addr1->data.stor.ss_family == AF_INET6) {
        virIPv6Addr t1, t2, tm;

        if ((getIPv6Addr(addr1, &t1) < 0) ||
            (getIPv6Addr(addr2, &t2) < 0) ||
            (getIPv6Addr(netmask, &tm) < 0))
            return(-1);

        for (i = 0;i < 8;i++) {
            if ((t1[i] & tm[i]) != (t2[i] & tm[i]))
                return(0);
        }

    } else {
        return(-1);
    }
    return(1);
}

/**
 * virSocketGetRange:
 * @start: start of an IP range
 * @end: end of an IP range
 *
 * Check the order of the 2 addresses and compute the range, this
 * will return 1 for identical addresses. Errors can come from incompatible
 * addresses type, excessive range (>= 2^^16) where the two addresses are
 * unrelated or inverted start and end.
 *
 * Returns the size of the range or -1 in case of failure
 */
int virSocketGetRange(virSocketAddrPtr start, virSocketAddrPtr end) {
    int ret = 0, i;

    if ((start == NULL) || (end == NULL))
        return(-1);
    if (start->data.stor.ss_family != end->data.stor.ss_family)
        return(-1);

    if (start->data.stor.ss_family == AF_INET) {
        virIPv4Addr t1, t2;

        if ((getIPv4Addr(start, &t1) < 0) ||
            (getIPv4Addr(end, &t2) < 0))
            return(-1);

        for (i = 0;i < 2;i++) {
            if (t1[i] != t2[i])
                return(-1);
        }
        ret = (t2[2] - t1[2]) * 256 + (t2[3] - t1[3]);
        if (ret < 0)
            return(-1);
        ret++;
    } else if (start->data.stor.ss_family == AF_INET6) {
        virIPv6Addr t1, t2;

        if ((getIPv6Addr(start, &t1) < 0) ||
            (getIPv6Addr(end, &t2) < 0))
            return(-1);

        for (i = 0;i < 7;i++) {
            if (t1[i] != t2[i])
                return(-1);
        }
        ret = t2[7] - t1[7];
        if (ret < 0)
            return(-1);
        ret++;
    } else {
        return(-1);
    }
    return(ret);
}


/**
 * virGetNumNetmaskBits
 * @netmask: the presumed netmask
 *
 * Get the number of netmask bits in a netmask.
 *
 * Returns the number of bits in the netmask or -1 if an error occurred
 * or the netmask is invalid.
 */
int virSocketGetNumNetmaskBits(const virSocketAddrPtr netmask)
{
    int i, j;
    int c = 0;

    if (netmask->data.stor.ss_family == AF_INET) {
        virIPv4Addr tm;
        uint8_t bit;

        if (getIPv4Addr(netmask, &tm) < 0)
            return -1;

        for (i = 0; i < 4; i++)
            if (tm[i] == 0xff)
                c += 8;
            else
                break;

        if (c == 8 * 4)
            return c;

        j = i << 3;
        while (j < (8 * 4)) {
            bit = 1 << (7 - (j & 7));
            if ((tm[j >> 3] & bit)) {
                c++;
            } else
                break;
            j++;
        }

        while (j < (8 * 4)) {
            bit = 1 << (7 - (j & 7));
            if ((tm[j >> 3] & bit))
                return -1;
            j++;
        }

        return c;
    } else if (netmask->data.stor.ss_family == AF_INET6) {
        virIPv6Addr tm;
        uint16_t bit;

        if (getIPv6Addr(netmask, &tm) < 0)
            return -1;

        for (i = 0; i < 8; i++)
            if (tm[i] == 0xffff)
                c += 16;
            else
                break;

        if (c == 16 * 8)
            return c;

        j = i << 4;
        while (j < (16 * 8)) {
            bit = 1 << (15 - (j & 0xf));
            if ((tm[j >> 4] & bit)) {
                c++;
            } else
                break;
            j++;
        }

        while (j < (16 * 8)) {
            bit = 1 << (15 - (j & 0xf));
            if ((tm[j >> 4]) & bit)
                return -1;
            j++;
        }

        return c;
    }
    return -1;
}