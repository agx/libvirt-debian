/*
 * qemu_security.c: QEMU security management
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

#include <config.h>

#include "qemu_domain.h"
#include "qemu_security.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_process");

struct qemuSecuritySetRestoreAllLabelData {
    bool set;
    virQEMUDriverPtr driver;
    virDomainObjPtr vm;
    const char *stdin_path;
    bool migrated;
};


int
qemuSecuritySetAllLabel(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        const char *stdin_path)
{
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT) &&
        virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetAllLabel(driver->securityManager,
                                      vm->def,
                                      stdin_path) < 0)
        goto cleanup;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT) &&
        virSecurityManagerTransactionCommit(driver->securityManager,
                                            vm->pid) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


void
qemuSecurityRestoreAllLabel(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            bool migrated)
{
    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT) &&
        virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreAllLabel(driver->securityManager,
                                          vm->def,
                                          migrated) < 0)
        goto cleanup;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT) &&
        virSecurityManagerTransactionCommit(driver->securityManager,
                                            vm->pid) < 0)
        goto cleanup;

 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
}


int
qemuSecuritySetDiskLabel(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virDomainDiskDefPtr disk)
{
    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT)) {
        /* Already handled by namespace code. */
        return 0;
    }

    return virSecurityManagerSetDiskLabel(driver->securityManager,
                                          vm->def,
                                          disk);
}


int
qemuSecurityRestoreDiskLabel(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             virDomainDiskDefPtr disk)
{
    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT)) {
        /* Already handled by namespace code. */
        return 0;
    }

    return virSecurityManagerRestoreDiskLabel(driver->securityManager,
                                              vm->def,
                                              disk);
}


int
qemuSecuritySetHostdevLabel(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainHostdevDefPtr hostdev)
{
    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT)) {
        /* Already handled by namespace code. */
        return 0;
    }

    return virSecurityManagerSetHostdevLabel(driver->securityManager,
                                             vm->def,
                                             hostdev,
                                             NULL);
}


int
qemuSecurityRestoreHostdevLabel(virQEMUDriverPtr driver,
                                virDomainObjPtr vm,
                                virDomainHostdevDefPtr hostdev)
{
    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT)) {
        /* Already handled by namespace code. */
        return 0;
    }

    return virSecurityManagerRestoreHostdevLabel(driver->securityManager,
                                                 vm->def,
                                                 hostdev,
                                                 NULL);
}
