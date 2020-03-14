/*
 * driver.h: description of the set of interfaces provided by a
 *           entry point to the virtualization engine
 *
 * Copyright (C) 2006-2014 Red Hat, Inc.
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
 */

#pragma once

#include <unistd.h>

#include "internal.h"
#include "libvirt_internal.h"
#include "viruri.h"


/* Status codes returned from driver open call. */
typedef enum {
    /* Opened successfully. */
    VIR_DRV_OPEN_SUCCESS = 0,

    /* 'name' is not for us. */
    VIR_DRV_OPEN_DECLINED = -1,

    /* 'name' is for us, but there was some error.  virConnectOpen will
     * return an error rather than continue probing the other drivers.
     */
    VIR_DRV_OPEN_ERROR = -2,
} virDrvOpenStatus;


/* Internal feature-detection macro.  Don't call drv->supports_feature
 * directly if you don't have to, because it may be NULL, use this macro
 * instead.
 *
 * Note that this treats a possible error returned by drv->supports_feature
 * the same as not supported. If you care about the error, call
 * drv->supports_feature directly.
 *
 * Returns:
 *   != 0  Feature is supported.
 *   0     Feature is not supported.
 */
#define VIR_DRV_SUPPORTS_FEATURE(drv, conn, feature) \
    ((drv)->connectSupportsFeature ? \
        (drv)->connectSupportsFeature((conn), (feature)) > 0 : 0)


#define __VIR_DRIVER_H_INCLUDES___

#include "driver-hypervisor.h"
#include "driver-interface.h"
#include "driver-network.h"
#include "driver-nodedev.h"
#include "driver-nwfilter.h"
#include "driver-secret.h"
#include "driver-state.h"
#include "driver-stream.h"
#include "driver-storage.h"

#undef __VIR_DRIVER_H_INCLUDES___

typedef struct _virConnectDriver virConnectDriver;
typedef virConnectDriver *virConnectDriverPtr;

struct _virConnectDriver {
    /* Whether driver permits a server in the URI */
    bool localOnly;
    /* Whether driver needs a server in the URI */
    bool remoteOnly;
    /*
     * NULL terminated list of supported URI schemes.
     *  - Single element { NULL } list indicates no supported schemes
     *  - NULL list indicates wildcard supporting all schemes
     */
    const char **uriSchemes;
    virHypervisorDriverPtr hypervisorDriver;
    virInterfaceDriverPtr interfaceDriver;
    virNetworkDriverPtr networkDriver;
    virNodeDeviceDriverPtr nodeDeviceDriver;
    virNWFilterDriverPtr nwfilterDriver;
    virSecretDriverPtr secretDriver;
    virStorageDriverPtr storageDriver;
};

int virRegisterConnectDriver(virConnectDriverPtr driver,
                             bool setSharedDrivers) G_GNUC_WARN_UNUSED_RESULT;
int virRegisterStateDriver(virStateDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;

int virSetSharedInterfaceDriver(virInterfaceDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;
int virSetSharedNetworkDriver(virNetworkDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;
int virSetSharedNodeDeviceDriver(virNodeDeviceDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;
int virSetSharedNWFilterDriver(virNWFilterDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;
int virSetSharedSecretDriver(virSecretDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;
int virSetSharedStorageDriver(virStorageDriverPtr driver) G_GNUC_WARN_UNUSED_RESULT;

bool virHasDriverForURIScheme(const char *scheme);

int virDriverLoadModule(const char *name,
                        const char *regfunc,
                        bool required);

int virDriverShouldAutostart(const char *name,
                             bool *autostart);

virConnectPtr virGetConnectInterface(void);
virConnectPtr virGetConnectNetwork(void);
virConnectPtr virGetConnectNWFilter(void);
virConnectPtr virGetConnectNodeDev(void);
virConnectPtr virGetConnectSecret(void);
virConnectPtr virGetConnectStorage(void);

int virSetConnectInterface(virConnectPtr conn);
int virSetConnectNetwork(virConnectPtr conn);
int virSetConnectNWFilter(virConnectPtr conn);
int virSetConnectNodeDev(virConnectPtr conn);
int virSetConnectSecret(virConnectPtr conn);
int virSetConnectStorage(virConnectPtr conn);

bool virConnectValidateURIPath(const char *uriPath,
                               const char *entityName,
                               bool privileged);
