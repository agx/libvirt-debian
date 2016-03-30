/*
 * libvirt_nss: Name Service Switch plugin
 *
 * The aim is to enable users and applications to translate
 * domain names into IP addresses. However, this is currently
 * available only for those domains which gets their IP addresses
 * from a libvirt managed network.
 *
 * Copyright (C) 2016 Red Hat, Inc.
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
 * Authors:
 *     Michal Privoznik <mprivozn@redhat.com>
 */

#ifndef __LIBVIRT_NSS_H__
# define __LIBVIRT_NSS_H__

# include <nss.h>
# include <netdb.h>

enum nss_status
_nss_libvirt_gethostbyname_r(const char *name, struct hostent *result,
                             char *buffer, size_t buflen, int *errnop,
                             int *herrnop);

enum nss_status
_nss_libvirt_gethostbyname2_r(const char *name, int af, struct hostent *result,
                              char *buffer, size_t buflen, int *errnop,
                              int *herrnop);
enum nss_status
_nss_libvirt_gethostbyname3_r(const char *name, int af, struct hostent *result,
                              char *buffer, size_t buflen, int *errnop,
                              int *herrnop, int32_t *ttlp, char **canonp);
enum nss_status
_nss_libvirt_gethostbyname4_r(const char *name, struct gaih_addrtuple **pat,
                              char *buffer, size_t buflen, int *errnop,
                              int *herrnop, int32_t *ttlp);
#endif /* __LIBVIRT_NSS_H__ */
