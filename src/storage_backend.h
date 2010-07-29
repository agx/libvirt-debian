/*
 * storage_backend.h: internal storage driver backend contract
 *
 * Copyright (C) 2007-2008 Red Hat, Inc.
 * Copyright (C) 2007-2008 Daniel P. Berrange
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#ifndef __VIR_STORAGE_BACKEND_H__
#define __VIR_STORAGE_BACKEND_H__

#include <libvirt/libvirt.h>
#include "storage_conf.h"


typedef const char *(*virStorageVolFormatToString)(virConnectPtr conn,
                                                   int format);
typedef int (*virStorageVolFormatFromString)(virConnectPtr conn,
                                             const char *format);

typedef const char *(*virStoragePoolFormatToString)(virConnectPtr conn,
                                                    int format);
typedef int (*virStoragePoolFormatFromString)(virConnectPtr conn,
                                              const char *format);


typedef struct _virStorageBackendVolOptions virStorageBackendVolOptions;
typedef virStorageBackendVolOptions *virStorageBackendVolOptionsPtr;
struct _virStorageBackendVolOptions {
    virStorageVolFormatToString formatToString;
    virStorageVolFormatFromString formatFromString;
};


/* Flags to indicate mandatory components in the pool source */
enum {
    VIR_STORAGE_BACKEND_POOL_SOURCE_HOST    = (1<<0),
    VIR_STORAGE_BACKEND_POOL_SOURCE_DEVICE  = (1<<1),
    VIR_STORAGE_BACKEND_POOL_SOURCE_DIR     = (1<<2),
    VIR_STORAGE_BACKEND_POOL_SOURCE_ADAPTER = (1<<3),
    VIR_STORAGE_BACKEND_POOL_SOURCE_NAME    = (1<<4),
};

typedef struct _virStorageBackendPoolOptions virStorageBackendPoolOptions;
typedef virStorageBackendPoolOptions *virStorageBackendPoolOptionsPtr;
struct _virStorageBackendPoolOptions {
    int flags;
    virStoragePoolFormatToString formatToString;
    virStoragePoolFormatFromString formatFromString;
};

#define SOURCES_START_TAG "<sources>"
#define SOURCES_END_TAG "</sources>"

typedef char * (*virStorageBackendFindPoolSources)(virConnectPtr conn, const char *srcSpec, unsigned int flags);
typedef int (*virStorageBackendStartPool)(virConnectPtr conn, virStoragePoolObjPtr pool);
typedef int (*virStorageBackendBuildPool)(virConnectPtr conn, virStoragePoolObjPtr pool, unsigned int flags);
typedef int (*virStorageBackendRefreshPool)(virConnectPtr conn, virStoragePoolObjPtr pool);
typedef int (*virStorageBackendStopPool)(virConnectPtr conn, virStoragePoolObjPtr pool);
typedef int (*virStorageBackendDeletePool)(virConnectPtr conn, virStoragePoolObjPtr pool, unsigned int flags);

typedef int (*virStorageBackendCreateVol)(virConnectPtr conn, virStoragePoolObjPtr pool, virStorageVolDefPtr vol);
typedef int (*virStorageBackendRefreshVol)(virConnectPtr conn, virStoragePoolObjPtr pool, virStorageVolDefPtr vol);
typedef int (*virStorageBackendDeleteVol)(virConnectPtr conn, virStoragePoolObjPtr pool, virStorageVolDefPtr vol, unsigned int flags);


typedef struct _virStorageBackend virStorageBackend;
typedef virStorageBackend *virStorageBackendPtr;

struct _virStorageBackend {
    int type;

    virStorageBackendFindPoolSources findPoolSources;
    virStorageBackendStartPool startPool;
    virStorageBackendBuildPool buildPool;
    virStorageBackendRefreshPool refreshPool;
    virStorageBackendStopPool stopPool;
    virStorageBackendDeletePool deletePool;

    virStorageBackendCreateVol createVol;
    virStorageBackendRefreshVol refreshVol;
    virStorageBackendDeleteVol deleteVol;

    virStorageBackendPoolOptions poolOptions;
    virStorageBackendVolOptions volOptions;

    int volType;
};


virStorageBackendPtr virStorageBackendForType(int type);
virStorageBackendPoolOptionsPtr virStorageBackendPoolOptionsForType(int type);
virStorageBackendVolOptionsPtr virStorageBackendVolOptionsForType(int type);
int virStorageBackendFromString(const char *type);
const char *virStorageBackendToString(int type);

int virStorageBackendUpdateVolInfo(virConnectPtr conn,
                                   virStorageVolDefPtr vol,
                                   int withCapacity);

int virStorageBackendUpdateVolInfoFD(virConnectPtr conn,
                                     virStorageVolDefPtr vol,
                                     int fd,
                                     int withCapacity);

char *virStorageBackendStablePath(virConnectPtr conn,
                                  virStoragePoolObjPtr pool,
                                  char *devpath);

typedef int (*virStorageBackendListVolRegexFunc)(virConnectPtr conn,
                                                 virStoragePoolObjPtr pool,
                                                 char **const groups,
                                                 void *data);
typedef int (*virStorageBackendListVolNulFunc)(virConnectPtr conn,
                                               virStoragePoolObjPtr pool,
                                               size_t n_tokens,
                                               char **const groups,
                                               void *data);

int virStorageBackendRunProgRegex(virConnectPtr conn,
                                  virStoragePoolObjPtr pool,
                                  const char *const*prog,
                                  int nregex,
                                  const char **regex,
                                  int *nvars,
                                  virStorageBackendListVolRegexFunc func,
                                  void *data,
                                  int *exitstatus);

int virStorageBackendRunProgNul(virConnectPtr conn,
                                virStoragePoolObjPtr pool,
                                const char **prog,
                                size_t n_columns,
                                virStorageBackendListVolNulFunc func,
                                void *data);

#endif /* __VIR_STORAGE_BACKEND_H__ */