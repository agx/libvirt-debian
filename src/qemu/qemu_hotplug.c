/*
 * qemu_hotplug.h: QEMU device hotplug management
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */


#include <config.h>

#include "qemu_hotplug.h"
#include "qemu_capabilities.h"
#include "qemu_domain.h"
#include "qemu_command.h"
#include "qemu_bridge_filter.h"
#include "qemu_hostdev.h"
#include "domain_audit.h"
#include "domain_nwfilter.h"
#include "logging.h"
#include "datatypes.h"
#include "virterror_internal.h"
#include "memory.h"
#include "pci.h"
#include "virfile.h"
#include "qemu_cgroup.h"
#include "locking/domain_lock.h"
#include "network/bridge_driver.h"
#include "virnetdev.h"
#include "virnetdevbridge.h"
#include "virnetdevtap.h"
#include "device_conf.h"
#include "storage_file.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

int qemuDomainChangeEjectableMedia(struct qemud_driver *driver,
                                   virDomainObjPtr vm,
                                   virDomainDiskDefPtr disk,
                                   bool force)
{
    virDomainDiskDefPtr origdisk = NULL;
    int i;
    int ret;
    char *driveAlias = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (vm->def->disks[i]->bus == disk->bus &&
            STREQ(vm->def->disks[i]->dst, disk->dst)) {
            origdisk = vm->def->disks[i];
            break;
        }
    }

    if (!origdisk) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No device with bus '%s' and target '%s'"),
                       virDomainDiskBusTypeToString(disk->bus),
                       disk->dst);
        return -1;
    }

    if (!origdisk->info.alias) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("missing disk device alias name for %s"), origdisk->dst);
        return -1;
    }

    if (origdisk->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
        origdisk->device != VIR_DOMAIN_DISK_DEVICE_CDROM) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Removable media not supported for %s device"),
                        virDomainDiskDeviceTypeToString(disk->device));
        return -1;
    }

    if (virDomainLockDiskAttach(driver->lockManager, driver->uri,
                                vm, disk) < 0)
        return -1;

    if (virSecurityManagerSetImageLabel(driver->securityManager,
                                        vm->def, disk) < 0) {
        if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
            VIR_WARN("Unable to release lock on %s", disk->src);
        return -1;
    }

    if (!(driveAlias = qemuDeviceDriveHostAlias(origdisk, priv->caps)))
        goto error;

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (disk->src) {
        const char *format = NULL;
        if (disk->type != VIR_DOMAIN_DISK_TYPE_DIR) {
            if (disk->format > 0)
                format = virStorageFileFormatTypeToString(disk->format);
            else if (origdisk->format > 0)
                format = virStorageFileFormatTypeToString(origdisk->format);
        }
        ret = qemuMonitorChangeMedia(priv->mon,
                                     driveAlias,
                                     disk->src, format);
    } else {
        ret = qemuMonitorEjectMedia(priv->mon, driveAlias, force);
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditDisk(vm, origdisk->src, disk->src, "update", ret >= 0);

    if (ret < 0)
        goto error;

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, origdisk) < 0)
        VIR_WARN("Unable to restore security label on ejected image %s", origdisk->src);

    if (virDomainLockDiskDetach(driver->lockManager, vm, origdisk) < 0)
        VIR_WARN("Unable to release lock on disk %s", origdisk->src);

    VIR_FREE(origdisk->src);
    origdisk->src = disk->src;
    disk->src = NULL;
    origdisk->type = disk->type;

    VIR_FREE(driveAlias);

    virDomainDiskDefFree(disk);

    return ret;

error:
    VIR_FREE(driveAlias);

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, disk) < 0)
        VIR_WARN("Unable to restore security label on new media %s", disk->src);

    if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
        VIR_WARN("Unable to release lock on %s", disk->src);

    return -1;
}

int
qemuDomainCheckEjectableMedia(struct qemud_driver *driver,
                             virDomainObjPtr vm,
                             enum qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virHashTablePtr table = NULL;
    int ret = -1;
    int i;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) == 0) {
        table = qemuMonitorGetBlockInfo(priv->mon);
        qemuDomainObjExitMonitorWithDriver(driver, vm);
    }

    if (!table)
        goto cleanup;

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDefPtr disk = vm->def->disks[i];
        struct qemuDomainDiskInfo *info;

        if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK ||
            disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
                 continue;
        }

        info = qemuMonitorBlockInfoLookup(table, disk->info.alias);
        if (!info)
            goto cleanup;

        if (info->tray_open && disk->src)
            VIR_FREE(disk->src);
    }

    ret = 0;

cleanup:
    virHashFree(table);
    return ret;
}


int qemuDomainAttachPciDiskDevice(virConnectPtr conn,
                                  struct qemud_driver *driver,
                                  virDomainObjPtr vm,
                                  virDomainDiskDefPtr disk)
{
    int i, ret;
    const char* type = virDomainDiskBusTypeToString(disk->bus);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *devstr = NULL;
    char *drivestr = NULL;
    bool releaseaddr = false;

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (STREQ(vm->def->disks[i]->dst, disk->dst)) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("target %s already exists"), disk->dst);
            return -1;
        }
    }

    if (virDomainLockDiskAttach(driver->lockManager, driver->uri,
                                vm, disk) < 0)
        return -1;

    if (virSecurityManagerSetImageLabel(driver->securityManager,
                                        vm->def, disk) < 0) {
        if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
            VIR_WARN("Unable to release lock on %s", disk->src);
        return -1;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuDomainPCIAddressEnsureAddr(priv->pciaddrs, &disk->info) < 0)
            goto error;
        releaseaddr = true;
        if (qemuAssignDeviceDiskAlias(vm->def, disk, priv->caps) < 0)
            goto error;

        if (!(drivestr = qemuBuildDriveStr(conn, disk, false, priv->caps)))
            goto error;

        if (!(devstr = qemuBuildDriveDevStr(NULL, disk, 0, priv->caps)))
            goto error;
    }

    if (VIR_REALLOC_N(vm->def->disks, vm->def->ndisks+1) < 0) {
        virReportOOMError();
        goto error;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        ret = qemuMonitorAddDrive(priv->mon, drivestr);
        if (ret == 0) {
            ret = qemuMonitorAddDevice(priv->mon, devstr);
            if (ret < 0) {
                virErrorPtr orig_err = virSaveLastError();
                if (qemuMonitorDriveDel(priv->mon, drivestr) < 0) {
                    VIR_WARN("Unable to remove drive %s (%s) after failed "
                             "qemuMonitorAddDevice",
                             drivestr, devstr);
                }
                if (orig_err) {
                    virSetError(orig_err);
                    virFreeError(orig_err);
                }
            }
        }
    } else {
        virDevicePCIAddress guestAddr = disk->info.addr.pci;
        ret = qemuMonitorAddPCIDisk(priv->mon,
                                    disk->src,
                                    type,
                                    &guestAddr);
        if (ret == 0) {
            disk->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
            memcpy(&disk->info.addr.pci, &guestAddr, sizeof(guestAddr));
        }
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditDisk(vm, NULL, disk->src, "attach", ret >= 0);

    if (ret < 0)
        goto error;

    virDomainDiskInsertPreAlloced(vm->def, disk);

    VIR_FREE(devstr);
    VIR_FREE(drivestr);

    return 0;

error:
    VIR_FREE(devstr);
    VIR_FREE(drivestr);

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        (disk->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) &&
        releaseaddr &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        disk->info.addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on %s", disk->src);

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, disk) < 0)
        VIR_WARN("Unable to restore security label on %s", disk->src);

    if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
        VIR_WARN("Unable to release lock on %s", disk->src);

    return -1;
}


int qemuDomainAttachPciControllerDevice(struct qemud_driver *driver,
                                        virDomainObjPtr vm,
                                        virDomainControllerDefPtr controller)
{
    int ret = -1;
    const char* type = virDomainControllerTypeToString(controller->type);
    char *devstr = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool releaseaddr = false;

    if (virDomainControllerFind(vm->def, controller->type, controller->idx) > 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("target %s:%d already exists"),
                       type, controller->idx);
        return -1;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuDomainPCIAddressEnsureAddr(priv->pciaddrs, &controller->info) < 0)
            goto cleanup;
        releaseaddr = true;
        if (qemuAssignDeviceControllerAlias(controller) < 0)
            goto cleanup;

        if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            controller->model == -1 &&
            !qemuCapsGet(priv->caps, QEMU_CAPS_PIIX3_USB_UHCI)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("USB controller hotplug unsupported in this QEMU binary"));
            goto cleanup;
        }

        if (!(devstr = qemuBuildControllerDevStr(vm->def, controller, priv->caps, NULL))) {
            goto cleanup;
        }
    }

    if (VIR_REALLOC_N(vm->def->controllers, vm->def->ncontrollers+1) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        ret = qemuMonitorAddDevice(priv->mon, devstr);
    } else {
        ret = qemuMonitorAttachPCIDiskController(priv->mon,
                                                 type,
                                                 &controller->info.addr.pci);
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    if (ret == 0) {
        controller->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        virDomainControllerInsertPreAlloced(vm->def, controller);
    }

cleanup:
    if ((ret != 0) &&
        qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        (controller->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) &&
        releaseaddr &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        controller->info.addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on controller");

    VIR_FREE(devstr);
    return ret;
}


static virDomainControllerDefPtr
qemuDomainFindOrCreateSCSIDiskController(struct qemud_driver *driver,
                                         virDomainObjPtr vm,
                                         int controller)
{
    int i;
    virDomainControllerDefPtr cont;

    for (i = 0 ; i < vm->def->ncontrollers ; i++) {
        cont = vm->def->controllers[i];

        if (cont->type != VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
            continue;

        if (cont->idx == controller)
            return cont;
    }

    /* No SCSI controller present, for backward compatibility we
     * now hotplug a controller */
    if (VIR_ALLOC(cont) < 0) {
        virReportOOMError();
        return NULL;
    }
    cont->type = VIR_DOMAIN_CONTROLLER_TYPE_SCSI;
    cont->idx = controller;
    cont->model = -1;

    VIR_INFO("No SCSI controller present, hotplugging one");
    if (qemuDomainAttachPciControllerDevice(driver,
                                            vm, cont) < 0) {
        VIR_FREE(cont);
        return NULL;
    }

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        /* cont doesn't need freeing here, since the reference
         * now held in def->controllers */
        return NULL;
    }

    return cont;
}


int qemuDomainAttachSCSIDisk(virConnectPtr conn,
                             struct qemud_driver *driver,
                             virDomainObjPtr vm,
                             virDomainDiskDefPtr disk)
{
    int i;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainControllerDefPtr cont = NULL;
    char *drivestr = NULL;
    char *devstr = NULL;
    int ret = -1;

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (STREQ(vm->def->disks[i]->dst, disk->dst)) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("target %s already exists"), disk->dst);
            return -1;
        }
    }

    if (virDomainLockDiskAttach(driver->lockManager, driver->uri,
                                vm, disk) < 0)
        return -1;

    if (virSecurityManagerSetImageLabel(driver->securityManager,
                                        vm->def, disk) < 0) {
        if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
            VIR_WARN("Unable to release lock on %s", disk->src);
        return -1;
    }

    /* We should have an address already, so make sure */
    if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected disk address type %s"),
                       virDomainDeviceAddressTypeToString(disk->info.type));
        goto error;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceDiskAlias(vm->def, disk, priv->caps) < 0)
            goto error;
        if (!(devstr = qemuBuildDriveDevStr(vm->def, disk, 0, priv->caps)))
            goto error;
    }

    if (!(drivestr = qemuBuildDriveStr(conn, disk, false, priv->caps)))
        goto error;

    for (i = 0 ; i <= disk->info.addr.drive.controller ; i++) {
        cont = qemuDomainFindOrCreateSCSIDiskController(driver, vm, i);
        if (!cont)
            goto error;
    }

    /* Tell clang that "cont" is non-NULL.
       This is because disk->info.addr.driver.controller is unsigned,
       and hence the above loop must iterate at least once.  */
    sa_assert (cont);

    if (cont->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("SCSI controller %d was missing its PCI address"), cont->idx);
        goto error;
    }

    if (VIR_REALLOC_N(vm->def->disks, vm->def->ndisks+1) < 0) {
        virReportOOMError();
        goto error;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        ret = qemuMonitorAddDrive(priv->mon, drivestr);
        if (ret == 0) {
            ret = qemuMonitorAddDevice(priv->mon, devstr);
            if (ret < 0) {
                VIR_WARN("qemuMonitorAddDevice failed on %s (%s)",
                         drivestr, devstr);
                /* XXX should call 'drive_del' on error but this does not
                   exist yet */
            }
        }
    } else {
        virDomainDeviceDriveAddress driveAddr;
        ret = qemuMonitorAttachDrive(priv->mon,
                                     drivestr,
                                     &cont->info.addr.pci,
                                     &driveAddr);
        if (ret == 0) {
            /* XXX we should probably validate that the addr matches
             * our existing defined addr instead of overwriting */
            disk->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE;
            disk->info.addr.drive.bus = driveAddr.bus;
            disk->info.addr.drive.unit = driveAddr.unit;
        }
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditDisk(vm, NULL, disk->src, "attach", ret >= 0);

    if (ret < 0)
        goto error;

    virDomainDiskInsertPreAlloced(vm->def, disk);

    VIR_FREE(devstr);
    VIR_FREE(drivestr);

    return 0;

error:
    VIR_FREE(devstr);
    VIR_FREE(drivestr);

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, disk) < 0)
        VIR_WARN("Unable to restore security label on %s", disk->src);

    if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
        VIR_WARN("Unable to release lock on %s", disk->src);

    return -1;
}


int qemuDomainAttachUsbMassstorageDevice(virConnectPtr conn,
                                         struct qemud_driver *driver,
                                         virDomainObjPtr vm,
                                         virDomainDiskDefPtr disk)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int i, ret;
    char *drivestr = NULL;
    char *devstr = NULL;

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (STREQ(vm->def->disks[i]->dst, disk->dst)) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("target %s already exists"), disk->dst);
            return -1;
        }
    }

    if (virDomainLockDiskAttach(driver->lockManager, driver->uri,
                                vm, disk) < 0)
        return -1;

    if (virSecurityManagerSetImageLabel(driver->securityManager,
                                        vm->def, disk) < 0) {
        if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
            VIR_WARN("Unable to release lock on %s", disk->src);
        return -1;
    }

    /* XXX not correct once we allow attaching a USB CDROM */
    if (!disk->src) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("disk source path is missing"));
        goto error;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceDiskAlias(vm->def, disk, priv->caps) < 0)
            goto error;
        if (!(drivestr = qemuBuildDriveStr(conn, disk, false, priv->caps)))
            goto error;
        if (!(devstr = qemuBuildDriveDevStr(NULL, disk, 0, priv->caps)))
            goto error;
    }

    if (VIR_REALLOC_N(vm->def->disks, vm->def->ndisks+1) < 0) {
        virReportOOMError();
        goto error;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        ret = qemuMonitorAddDrive(priv->mon, drivestr);
        if (ret == 0) {
            ret = qemuMonitorAddDevice(priv->mon, devstr);
            if (ret < 0) {
                VIR_WARN("qemuMonitorAddDevice failed on %s (%s)",
                         drivestr, devstr);
                /* XXX should call 'drive_del' on error but this does not
                   exist yet */
            }
        }
    } else {
        ret = qemuMonitorAddUSBDisk(priv->mon, disk->src);
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditDisk(vm, NULL, disk->src, "attach", ret >= 0);

    if (ret < 0)
        goto error;

    virDomainDiskInsertPreAlloced(vm->def, disk);

    VIR_FREE(devstr);
    VIR_FREE(drivestr);

    return 0;

error:
    VIR_FREE(devstr);
    VIR_FREE(drivestr);

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, disk) < 0)
        VIR_WARN("Unable to restore security label on %s", disk->src);

    if (virDomainLockDiskDetach(driver->lockManager, vm, disk) < 0)
        VIR_WARN("Unable to release lock on %s", disk->src);

    return -1;
}


/* XXX conn required for network -> bridge resolution */
int qemuDomainAttachNetDevice(virConnectPtr conn,
                              struct qemud_driver *driver,
                              virDomainObjPtr vm,
                              virDomainNetDefPtr net)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *tapfd_name = NULL;
    int tapfd = -1;
    char *vhostfd_name = NULL;
    int vhostfd = -1;
    char *nicstr = NULL;
    char *netstr = NULL;
    virNetDevVPortProfilePtr vport = NULL;
    int ret = -1;
    virDevicePCIAddress guestAddr;
    int vlan;
    bool releaseaddr = false;
    bool iface_connected = false;
    int actualType;

    /* preallocate new slot for device */
    if (VIR_REALLOC_N(vm->def->nets, vm->def->nnets+1) < 0) {
        virReportOOMError();
        return -1;
    }

    /* If appropriate, grab a physical device from the configured
     * network's pool of devices, or resolve bridge device name
     * to the one defined in the network definition.
     */
    if (networkAllocateActualDevice(net) < 0)
        return -1;

    actualType = virDomainNetGetActualType(net);

    if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* This is really a "smart hostdev", so it should be attached
         * as a hostdev (the hostdev code will reach over into the
         * netdev-specific code as appropriate), then also added to
         * the nets list (see cleanup:) if successful.
         */
        ret = qemuDomainAttachHostDevice(driver, vm,
                                         virDomainNetGetActualHostdev(net));
        goto cleanup;
    }

    if (!qemuCapsGet(priv->caps, QEMU_CAPS_HOST_NET_ADD)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("installed qemu version does not support host_net_add"));
        goto cleanup;
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
        actualType == VIR_DOMAIN_NET_TYPE_NETWORK) {
        /*
         * If type=bridge then we attempt to allocate the tap fd here only if
         * running under a privilged user or -netdev bridge option is not
         * supported.
         */
        if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
            driver->privileged ||
            (!qemuCapsGet (priv->caps, QEMU_CAPS_NETDEV_BRIDGE))) {
            if ((tapfd = qemuNetworkIfaceConnect(vm->def, conn, driver, net,
                                                 priv->caps)) < 0)
                goto cleanup;
            iface_connected = true;
            if (qemuOpenVhostNet(vm->def, net, priv->caps, &vhostfd) < 0)
                goto cleanup;
        }
    } else if (actualType == VIR_DOMAIN_NET_TYPE_DIRECT) {
        if ((tapfd = qemuPhysIfaceConnect(vm->def, driver, net,
                                          priv->caps,
                                          VIR_NETDEV_VPORT_PROFILE_OP_CREATE)) < 0)
            goto cleanup;
        iface_connected = true;
        if (qemuOpenVhostNet(vm->def, net, priv->caps, &vhostfd) < 0)
            goto cleanup;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_NET_NAME) ||
        qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceNetAlias(vm->def, net, -1) < 0)
            goto cleanup;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        qemuDomainPCIAddressEnsureAddr(priv->pciaddrs, &net->info) < 0)
        goto cleanup;

    releaseaddr = true;

    if (qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV) &&
        qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        vlan = -1;
    } else {
        vlan = qemuDomainNetVLAN(net);

        if (vlan < 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Unable to attach network devices without vlan"));
            goto cleanup;
        }
    }

    if (tapfd != -1) {
        if (virAsprintf(&tapfd_name, "fd-%s", net->info.alias) < 0)
            goto no_memory;
    }

    if (vhostfd != -1) {
        if (virAsprintf(&vhostfd_name, "vhostfd-%s", net->info.alias) < 0)
            goto no_memory;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV) &&
        qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (!(netstr = qemuBuildHostNetStr(net, driver, priv->caps,
                                           ',', -1, tapfd_name,
                                           vhostfd_name)))
            goto cleanup;
    } else {
        if (!(netstr = qemuBuildHostNetStr(net, driver, priv->caps,
                                           ' ', vlan, tapfd_name,
                                           vhostfd_name)))
            goto cleanup;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV) &&
        qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuMonitorAddNetdev(priv->mon, netstr, tapfd, tapfd_name,
                                 vhostfd, vhostfd_name) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, NULL, net, "attach", false);
            goto cleanup;
        }
    } else {
        if (qemuMonitorAddHostNetwork(priv->mon, netstr, tapfd, tapfd_name,
                                      vhostfd, vhostfd_name) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, NULL, net, "attach", false);
            goto cleanup;
        }
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    VIR_FORCE_CLOSE(tapfd);
    VIR_FORCE_CLOSE(vhostfd);

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        goto cleanup;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (!(nicstr = qemuBuildNicDevStr(net, vlan, 0, priv->caps)))
            goto try_remove;
    } else {
        if (!(nicstr = qemuBuildNicStr(net, NULL, vlan)))
            goto try_remove;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuMonitorAddDevice(priv->mon, nicstr) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, NULL, net, "attach", false);
            goto try_remove;
        }
    } else {
        guestAddr = net->info.addr.pci;
        if (qemuMonitorAddPCINetwork(priv->mon, nicstr,
                                     &guestAddr) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, NULL, net, "attach", false);
            goto try_remove;
        }
        net->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        memcpy(&net->info.addr.pci, &guestAddr, sizeof(guestAddr));
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    /* set link state */
    if (net->linkstate == VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN) {
        if (!net->info.alias) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("device alias not found: cannot set link state to down"));
        } else {
            qemuDomainObjEnterMonitorWithDriver(driver, vm);

            if (qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV)) {
                if (qemuMonitorSetLink(priv->mon, net->info.alias, VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN) < 0) {
                    qemuDomainObjExitMonitorWithDriver(driver, vm);
                    virDomainAuditNet(vm, NULL, net, "attach", false);
                    goto try_remove;
                }
            } else {
                virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                               _("setting of link state not supported: Link is up"));
            }

            qemuDomainObjExitMonitorWithDriver(driver, vm);
        }
        /* link set to down */
    }

    virDomainAuditNet(vm, NULL, net, "attach", true);

    ret = 0;

cleanup:
    if (!ret) {
        vm->def->nets[vm->def->nnets++] = net;
    } else {
        if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
            (net->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) &&
            releaseaddr &&
            qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                            net->info.addr.pci.slot) < 0)
            VIR_WARN("Unable to release PCI address on NIC");

        if (iface_connected) {
            virDomainConfNWFilterTeardown(net);

            vport = virDomainNetGetActualVirtPortProfile(net);
            if (vport && vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH)
               ignore_value(virNetDevOpenvswitchRemovePort(
                               virDomainNetGetActualBridgeName(net), net->ifname));
        }

        networkReleaseActualDevice(net);
    }

    VIR_FREE(nicstr);
    VIR_FREE(netstr);
    VIR_FREE(tapfd_name);
    VIR_FORCE_CLOSE(tapfd);
    VIR_FREE(vhostfd_name);
    VIR_FORCE_CLOSE(vhostfd);

    return ret;

try_remove:
    if (!virDomainObjIsActive(vm))
        goto cleanup;

    if (vlan < 0) {
        if (qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV) &&
            qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
            char *netdev_name;
            if (virAsprintf(&netdev_name, "host%s", net->info.alias) < 0)
                goto no_memory;
            qemuDomainObjEnterMonitorWithDriver(driver, vm);
            if (qemuMonitorRemoveNetdev(priv->mon, netdev_name) < 0)
                VIR_WARN("Failed to remove network backend for netdev %s",
                         netdev_name);
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            VIR_FREE(netdev_name);
        } else {
            VIR_WARN("Unable to remove network backend");
        }
    } else {
        char *hostnet_name;
        if (virAsprintf(&hostnet_name, "host%s", net->info.alias) < 0)
            goto no_memory;
        qemuDomainObjEnterMonitorWithDriver(driver, vm);
        if (qemuMonitorRemoveHostNetwork(priv->mon, vlan, hostnet_name) < 0)
            VIR_WARN("Failed to remove network backend for vlan %d, net %s",
                     vlan, hostnet_name);
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        VIR_FREE(hostnet_name);
    }
    goto cleanup;

no_memory:
    virReportOOMError();
    goto cleanup;
}


int qemuDomainAttachHostPciDevice(struct qemud_driver *driver,
                                  virDomainObjPtr vm,
                                  virDomainHostdevDefPtr hostdev)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;
    char *devstr = NULL;
    int configfd = -1;
    char *configfd_name = NULL;
    bool releaseaddr = false;

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs+1) < 0) {
        virReportOOMError();
        return -1;
    }

    if (qemuPrepareHostdevPCIDevices(driver, vm->def->name, vm->def->uuid,
                                     &hostdev, 1) < 0)
        return -1;

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceHostdevAlias(vm->def, hostdev, -1) < 0)
            goto error;
        if (qemuDomainPCIAddressEnsureAddr(priv->pciaddrs, hostdev->info) < 0)
            goto error;
        releaseaddr = true;
        if (qemuCapsGet(priv->caps, QEMU_CAPS_PCI_CONFIGFD)) {
            configfd = qemuOpenPCIConfig(hostdev);
            if (configfd >= 0) {
                if (virAsprintf(&configfd_name, "fd-%s",
                                hostdev->info->alias) < 0) {
                    virReportOOMError();
                    goto error;
                }
            }
        }

        if (!virDomainObjIsActive(vm)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("guest unexpectedly quit during hotplug"));
            goto error;
        }

        if (!(devstr = qemuBuildPCIHostdevDevStr(hostdev, configfd_name,
                                                 priv->caps)))
            goto error;

        qemuDomainObjEnterMonitorWithDriver(driver, vm);
        ret = qemuMonitorAddDeviceWithFd(priv->mon, devstr,
                                         configfd, configfd_name);
        qemuDomainObjExitMonitorWithDriver(driver, vm);
    } else {
        virDevicePCIAddress guestAddr = hostdev->info->addr.pci;

        qemuDomainObjEnterMonitorWithDriver(driver, vm);
        ret = qemuMonitorAddPCIHostDevice(priv->mon,
                                          &hostdev->source.subsys.u.pci,
                                          &guestAddr);
        qemuDomainObjExitMonitorWithDriver(driver, vm);

        hostdev->info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        memcpy(&hostdev->info->addr.pci, &guestAddr, sizeof(guestAddr));
    }
    virDomainAuditHostdev(vm, hostdev, "attach", ret == 0);
    if (ret < 0)
        goto error;

    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;

    VIR_FREE(devstr);
    VIR_FREE(configfd_name);
    VIR_FORCE_CLOSE(configfd);

    return 0;

error:
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        (hostdev->info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) &&
        releaseaddr &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        hostdev->info->addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on host device");

    qemuDomainReAttachHostdevDevices(driver, vm->def->name, &hostdev, 1);

    VIR_FREE(devstr);
    VIR_FREE(configfd_name);
    VIR_FORCE_CLOSE(configfd);

    return -1;
}


int qemuDomainAttachRedirdevDevice(struct qemud_driver *driver,
                                   virDomainObjPtr vm,
                                   virDomainRedirdevDefPtr redirdev)
{
    int ret;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    char *devstr = NULL;

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceRedirdevAlias(vm->def, redirdev, -1) < 0)
            goto error;
        if (!(devstr = qemuBuildRedirdevDevStr(def, redirdev, priv->caps)))
            goto error;
    }

    if (VIR_REALLOC_N(vm->def->redirdevs, vm->def->nredirdevs+1) < 0) {
        virReportOOMError();
        goto error;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE))
        ret = qemuMonitorAddDevice(priv->mon, devstr);
    else
        goto error;

    qemuDomainObjExitMonitorWithDriver(driver, vm);
    virDomainAuditRedirdev(vm, redirdev, "attach", ret == 0);
    if (ret < 0)
        goto error;

    vm->def->redirdevs[vm->def->nredirdevs++] = redirdev;

    VIR_FREE(devstr);

    return 0;

error:
    VIR_FREE(devstr);
    return -1;

}

int qemuDomainAttachHostUsbDevice(struct qemud_driver *driver,
                                  virDomainObjPtr vm,
                                  virDomainHostdevDefPtr hostdev)
{
    int ret;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *devstr = NULL;

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceHostdevAlias(vm->def, hostdev, -1) < 0)
            goto error;
        if (!(devstr = qemuBuildUSBHostdevDevStr(hostdev, priv->caps)))
            goto error;
    }

    if (VIR_REALLOC_N(vm->def->hostdevs, vm->def->nhostdevs+1) < 0) {
        virReportOOMError();
        goto error;
    }

    if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_DEVICES)) {
        virCgroupPtr cgroup = NULL;
        usbDevice *usb;
        qemuCgroupData data;

        if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) !=0 ) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to find cgroup for %s"),
                           vm->def->name);
            goto error;
        }

        if ((usb = usbGetDevice(hostdev->source.subsys.u.usb.bus,
                                hostdev->source.subsys.u.usb.device)) == NULL)
            goto error;

        data.vm = vm;
        data.cgroup = cgroup;
        if (usbDeviceFileIterate(usb, qemuSetupHostUsbDeviceCgroup, &data) < 0)
            goto error;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE))
        ret = qemuMonitorAddDevice(priv->mon, devstr);
    else
        ret = qemuMonitorAddUSBDeviceExact(priv->mon,
                                           hostdev->source.subsys.u.usb.bus,
                                           hostdev->source.subsys.u.usb.device);
    qemuDomainObjExitMonitorWithDriver(driver, vm);
    virDomainAuditHostdev(vm, hostdev, "attach", ret == 0);
    if (ret < 0)
        goto error;

    vm->def->hostdevs[vm->def->nhostdevs++] = hostdev;

    VIR_FREE(devstr);

    return 0;

error:
    VIR_FREE(devstr);
    return -1;
}

int qemuDomainAttachHostDevice(struct qemud_driver *driver,
                               virDomainObjPtr vm,
                               virDomainHostdevDefPtr hostdev)
{
    usbDeviceList *list;
    usbDevice *usb = NULL;

    if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hostdev mode '%s' not supported"),
                       virDomainHostdevModeTypeToString(hostdev->mode));
        return -1;
    }

    if (!(list = usbDeviceListNew()))
        goto cleanup;

    if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) {
        if (qemuFindHostdevUSBDevice(hostdev, true, &usb) < 0)
            goto cleanup;

        if (usbDeviceListAdd(list, usb) < 0) {
            usbFreeDevice(usb);
            usb = NULL;
            goto cleanup;
        }

        if (qemuPrepareHostdevUSBDevices(driver, vm->def->name, list) < 0) {
            usb = NULL;
            goto cleanup;
        }

        usbDeviceListSteal(list, usb);
    }

    if (virSecurityManagerSetHostdevLabel(driver->securityManager,
                                          vm->def, hostdev) < 0)
        goto cleanup;

    switch (hostdev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        if (qemuDomainAttachHostPciDevice(driver, vm,
                                          hostdev) < 0)
            goto error;
        break;

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        if (qemuDomainAttachHostUsbDevice(driver, vm,
                                          hostdev) < 0)
            goto error;
        break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hostdev subsys type '%s' not supported"),
                       virDomainHostdevSubsysTypeToString(hostdev->source.subsys.type));
        goto error;
    }

    usbDeviceListFree(list);
    return 0;

error:
    if (virSecurityManagerRestoreHostdevLabel(driver->securityManager,
                                              vm->def, hostdev) < 0)
        VIR_WARN("Unable to restore host device labelling on hotplug fail");

cleanup:
    usbDeviceListFree(list);
    if (usb)
        usbDeviceListSteal(driver->activeUsbHostdevs, usb);
    return -1;
}

static virDomainNetDefPtr *qemuDomainFindNet(virDomainObjPtr vm,
                                             virDomainNetDefPtr dev)
{
    int i;

    for (i = 0; i < vm->def->nnets; i++) {
        if (virMacAddrCmp(&vm->def->nets[i]->mac, &dev->mac) == 0)
            return &vm->def->nets[i];
    }

    return NULL;
}

static char *
qemuDomainNetGetBridgeName(virConnectPtr conn, virDomainNetDefPtr net)
{
    char *brname = NULL;
    int actualType = virDomainNetGetActualType(net);

    if (actualType == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        const char *tmpbr = virDomainNetGetActualBridgeName(net);
        if (!tmpbr) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("interface is missing bridge name"));
            goto cleanup;
        }
        /* we need a copy, not just a pointer to the original */
        if (!(brname = strdup(tmpbr))) {
            virReportOOMError();
            goto cleanup;
        }
    } else if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK) {
        int active;
        virErrorPtr errobj;
        virNetworkPtr network;

        if (!(network = virNetworkLookupByName(conn, net->data.network.name))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Couldn't find network '%s'"),
                           net->data.network.name);
            goto cleanup;
        }

        active = virNetworkIsActive(network);
        if (active == 1) {
            brname = virNetworkGetBridgeName(network);
        } else if (active == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Network '%s' is not active."),
                           net->data.network.name);
        }

        /* Make sure any above failure is preserved */
        errobj = virSaveLastError();
        virNetworkFree(network);
        virSetError(errobj);
        virFreeError(errobj);
        goto cleanup;

    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Network type %d is not supported"),
                   virDomainNetGetActualType(net));
cleanup:
    return brname;
}

static int
qemuDomainChangeNetBridge(virConnectPtr conn,
                          virDomainObjPtr vm,
                          virDomainNetDefPtr olddev,
                          virDomainNetDefPtr newdev)
{
    int ret = -1;
    char *oldbridge = NULL, *newbridge = NULL;

    if (!(oldbridge = qemuDomainNetGetBridgeName(conn, olddev)))
        goto cleanup;

    if (!(newbridge = qemuDomainNetGetBridgeName(conn, newdev)))
        goto cleanup;

    VIR_DEBUG("Change bridge for interface %s: %s -> %s",
              olddev->ifname, oldbridge, newbridge);

    if (virNetDevExists(newbridge) != 1) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("bridge %s doesn't exist"), newbridge);
        goto cleanup;
    }

    if (oldbridge) {
        ret = virNetDevBridgeRemovePort(oldbridge, olddev->ifname);
        virDomainAuditNet(vm, olddev, NULL, "detach", ret == 0);
        if (ret < 0)
            goto cleanup;
    }

    ret = virNetDevBridgeAddPort(newbridge, olddev->ifname);
    virDomainAuditNet(vm, NULL, newdev, "attach", ret == 0);
    if (ret < 0) {
        ret = virNetDevBridgeAddPort(oldbridge, olddev->ifname);
        virDomainAuditNet(vm, NULL, olddev, "attach", ret == 0);
        if (ret < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("unable to recover former state by adding port "
                             "to bridge %s"), oldbridge);
        }
        goto cleanup;
    }
    /* caller will replace entire olddev with newdev in domain nets list */
    ret = 0;
cleanup:
    VIR_FREE(oldbridge);
    VIR_FREE(newbridge);
    return ret;
}

int qemuDomainChangeNetLinkState(struct qemud_driver *driver,
                                 virDomainObjPtr vm,
                                 virDomainNetDefPtr dev,
                                 int linkstate)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    VIR_DEBUG("dev: %s, state: %d", dev->info.alias, linkstate);

    if (!dev->info.alias) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("can't change link state: device alias not found"));
        return -1;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);

    ret = qemuMonitorSetLink(priv->mon, dev->info.alias, linkstate);
    if (ret < 0)
        goto cleanup;

    /* modify the device configuration */
    dev->linkstate = linkstate;

cleanup:
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    return ret;
}

int
qemuDomainChangeNet(struct qemud_driver *driver,
                    virDomainObjPtr vm,
                    virDomainPtr dom,
                    virDomainDeviceDefPtr dev)
{
    virDomainNetDefPtr newdev = dev->data.net;
    virDomainNetDefPtr *devslot = qemuDomainFindNet(vm, newdev);
    virDomainNetDefPtr olddev;
    int oldType, newType;
    bool needReconnect = false;
    bool needBridgeChange = false;
    bool needLinkStateChange = false;
    bool needReplaceDevDef = false;
    int ret = -1;

    if (!devslot || !(olddev = *devslot)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot find existing network device to modify"));
        goto cleanup;
    }

    oldType = virDomainNetGetActualType(olddev);
    if (oldType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* no changes are possible to a type='hostdev' interface */
        virReportError(VIR_ERR_NO_SUPPORT,
                       _("cannot change config of '%s' network type"),
                       virDomainNetTypeToString(oldType));
        goto cleanup;
    }

    /* Check individual attributes for changes that can't be done to a
     * live netdev. These checks *mostly* go in order of the
     * declarations in virDomainNetDef in order to assure nothing is
     * omitted. (exceptiong where noted in comments - in particular,
     * some things require that a new "actual device" be allocated
     * from the network driver first, but we delay doing that until
     * after we've made as many other checks as possible)
     */

    /* type: this can change (with some restrictions), but the actual
     * type of the new device connection isn't known until after we
     * allocate the "actual" device.
     */

    if (virMacAddrCmp(&olddev->mac, &newdev->mac)) {
        char oldmac[VIR_MAC_STRING_BUFLEN], newmac[VIR_MAC_STRING_BUFLEN];

        virReportError(VIR_ERR_NO_SUPPORT,
                       _("cannot change network interface mac address "
                         "from %s to %s"),
                       virMacAddrFormat(&olddev->mac, oldmac),
                       virMacAddrFormat(&newdev->mac, newmac));
        goto cleanup;
    }

    if (STRNEQ_NULLABLE(olddev->model, newdev->model)) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       _("cannot modify network device model from %s to %s"),
                       olddev->model ? olddev->model : "(default)",
                       newdev->model ? newdev->model : "(default)");
        goto cleanup;
    }

    if (olddev->model && STREQ(olddev->model, "virtio") &&
        (olddev->driver.virtio.name != newdev->driver.virtio.name ||
         olddev->driver.virtio.txmode != newdev->driver.virtio.txmode ||
         olddev->driver.virtio.ioeventfd != newdev->driver.virtio.ioeventfd ||
         olddev->driver.virtio.event_idx != newdev->driver.virtio.event_idx)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify virtio network device driver attributes"));
        goto cleanup;
    }

    /* data: this union will be examined later, after allocating new actualdev */
    /* virtPortProfile: will be examined later, after allocating new actualdev */

    if (olddev->tune.sndbuf_specified != newdev->tune.sndbuf_specified ||
        olddev->tune.sndbuf != newdev->tune.sndbuf) {
        needReconnect = true;
    }

    if (STRNEQ_NULLABLE(olddev->script, newdev->script)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network device script attribute"));
        goto cleanup;
    }

    /* ifname: check if it's set in newdev. If not, retain the autogenerated one */
    if (!(newdev->ifname ||
          (newdev->ifname = strdup(olddev->ifname)))) {
        virReportOOMError();
        goto cleanup;
    }
    if (STRNEQ_NULLABLE(olddev->ifname, newdev->ifname)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network device tap name"));
        goto cleanup;
    }

    /* info: if newdev->info is empty, fill it in from olddev,
     * otherwise verify that it matches - nothing is allowed to
     * change. (There is no helper function to do this, so
     * individually check the few feidls of virDomainDeviceInfo that
     * are relevant in this case).
     */
    if (!virDomainDeviceAddressIsValid(&newdev->info,
                                       VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) &&
        virDomainDeviceInfoCopy(&newdev->info, &olddev->info) < 0) {
        goto cleanup;
    }
    if (!virDevicePCIAddressEqual(&olddev->info.addr.pci,
                                  &newdev->info.addr.pci)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network device guest PCI address"));
        goto cleanup;
    }
    /* grab alias from olddev if not set in newdev */
    if (!(newdev->info.alias ||
          (newdev->info.alias = strdup(olddev->info.alias)))) {
        virReportOOMError();
        goto cleanup;
    }
    if (STRNEQ_NULLABLE(olddev->info.alias, newdev->info.alias)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network device alias"));
        goto cleanup;
    }
    if (olddev->info.rombar != newdev->info.rombar) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network device rom bar setting"));
        goto cleanup;
    }
    if (STRNEQ_NULLABLE(olddev->info.romfile, newdev->info.romfile)) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network rom file"));
        goto cleanup;
    }
    if (olddev->info.bootIndex != newdev->info.bootIndex) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("cannot modify network device boot index setting"));
        goto cleanup;
    }
    /* (end of device info checks) */

    if (STRNEQ_NULLABLE(olddev->filter, newdev->filter))
        needReconnect = true;

    /* bandwidth can be modified, and will be checked later */
    /* vlan can be modified, and will be checked later */
    /* linkstate can be modified */

    /* allocate new actual device to compare to old - we will need to
     * free it if we fail for any reason
     */
    if (newdev->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
        networkAllocateActualDevice(newdev) < 0) {
        goto cleanup;
    }

    newType = virDomainNetGetActualType(newdev);

    if (newType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* can't turn it into a type='hostdev' interface */
        virReportError(VIR_ERR_NO_SUPPORT,
                       _("cannot change network interface type to '%s'"),
                       virDomainNetTypeToString(newType));
        goto cleanup;
    }

    if (olddev->type == newdev->type && oldType == newType) {

        /* if type hasn't changed, check the relevant fields for the type */
        switch (newdev->type) {
        case VIR_DOMAIN_NET_TYPE_USER:
            break;

        case VIR_DOMAIN_NET_TYPE_ETHERNET:
            if (STRNEQ_NULLABLE(olddev->data.ethernet.dev,
                                newdev->data.ethernet.dev) ||
                STRNEQ_NULLABLE(olddev->data.ethernet.ipaddr,
                                newdev->data.ethernet.ipaddr)) {
                needReconnect = true;
            }
        break;

        case VIR_DOMAIN_NET_TYPE_SERVER:
        case VIR_DOMAIN_NET_TYPE_CLIENT:
        case VIR_DOMAIN_NET_TYPE_MCAST:
            if (STRNEQ_NULLABLE(olddev->data.socket.address,
                                newdev->data.socket.address) ||
                olddev->data.socket.port != newdev->data.socket.port) {
                needReconnect = true;
            }
            break;

        case VIR_DOMAIN_NET_TYPE_NETWORK:
            if (STRNEQ(olddev->data.network.name, newdev->data.network.name)) {
                if (virDomainNetGetActualVirtPortProfile(newdev))
                    needReconnect = true;
                else
                    needBridgeChange = true;
            }
            /* other things handled in common code directly below this switch */
            break;

        case VIR_DOMAIN_NET_TYPE_BRIDGE:
            /* all handled in bridge name checked in common code below */
            break;

        case VIR_DOMAIN_NET_TYPE_INTERNAL:
            if (STRNEQ_NULLABLE(olddev->data.internal.name,
                                newdev->data.internal.name)) {
                needReconnect = true;
            }
            break;

        case VIR_DOMAIN_NET_TYPE_DIRECT:
            /* all handled in common code directly below this switch */
            break;

        default:
            virReportError(VIR_ERR_NO_SUPPORT,
                           _("unable to change config on '%s' network type"),
                           virDomainNetTypeToString(newdev->type));
            break;

        }
    } else {
        /* interface type has changed. There are a few special cases
         * where this can only require a minor (or even no) change,
         * but in most cases we need to do a full reconnection.
         *
         * If we switch (in either direction) between type='bridge'
         * and type='network' (for a traditional managed virtual
         * network that uses a host bridge, i.e. forward
         * mode='route|nat'), we just need to change the bridge.
         */
        if ((oldType == VIR_DOMAIN_NET_TYPE_NETWORK &&
             newType == VIR_DOMAIN_NET_TYPE_BRIDGE) ||
            (oldType == VIR_DOMAIN_NET_TYPE_BRIDGE &&
             newType == VIR_DOMAIN_NET_TYPE_NETWORK)) {

            needBridgeChange = true;

        } else if (oldType == VIR_DOMAIN_NET_TYPE_DIRECT &&
                   newType == VIR_DOMAIN_NET_TYPE_DIRECT) {

            /* this is the case of switching from type='direct' to
             * type='network' for a network that itself uses direct
             * (macvtap) devices. If the physical device and mode are
             * the same, this doesn't require any actual setup
             * change. If the physical device or mode *does* change,
             * that will be caught in the common section below */

        } else {

            /* for all other combinations, we'll need a full reconnect */
            needReconnect = true;

        }
    }

    /* now several things that are in multiple (but not all)
     * different types, and can be safely compared even for those
     * cases where they don't apply to a particular type.
     */
    if (STRNEQ_NULLABLE(virDomainNetGetActualBridgeName(olddev),
                        virDomainNetGetActualBridgeName(newdev))) {
        if (virDomainNetGetActualVirtPortProfile(newdev))
            needReconnect = true;
        else
            needBridgeChange = true;
    }

    if (STRNEQ_NULLABLE(virDomainNetGetActualDirectDev(olddev),
                        virDomainNetGetActualDirectDev(newdev)) ||
        virDomainNetGetActualDirectMode(olddev) != virDomainNetGetActualDirectMode(olddev) ||
        !virNetDevVPortProfileEqual(virDomainNetGetActualVirtPortProfile(olddev),
                                    virDomainNetGetActualVirtPortProfile(newdev)) ||
        !virNetDevBandwidthEqual(virDomainNetGetActualBandwidth(olddev),
                                 virDomainNetGetActualBandwidth(newdev)) ||
        !virNetDevVlanEqual(virDomainNetGetActualVlan(olddev),
                            virDomainNetGetActualVlan(newdev))) {
        needReconnect = true;
    }

    if (olddev->linkstate != newdev->linkstate)
        needLinkStateChange = true;

    /* FINALLY - actually perform the required actions */

    if (needReconnect) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       _("unable to change config on '%s' network type"),
                       virDomainNetTypeToString(newdev->type));
        goto cleanup;
    }

    if (needBridgeChange) {
        if (qemuDomainChangeNetBridge(dom->conn, vm, olddev, newdev) < 0)
            goto cleanup;
        /* we successfully switched to the new bridge, and we've
         * determined that the rest of newdev is equivalent to olddev,
         * so move newdev into place, so that the  */
        needReplaceDevDef = true;
    }

    if (needLinkStateChange &&
        qemuDomainChangeNetLinkState(driver, vm, olddev, newdev->linkstate) < 0) {
        goto cleanup;
    }

    if (needReplaceDevDef) {
        /* the changes above warrant replacing olddev with newdev in
         * the domain's nets list.
         */
        networkReleaseActualDevice(olddev);
        virDomainNetDefFree(olddev);
        /* move newdev into the nets list, and NULL it out from the
         * virDomainDeviceDef that we were given so that the caller
         * won't delete it on return.
         */
        *devslot = newdev;
        newdev = dev->data.net = NULL;
        dev->type = VIR_DOMAIN_DEVICE_NONE;
    }

    ret = 0;
cleanup:
    /* When we get here, we will be in one of these two states:
     *
     * 1) newdev has been moved into the domain's list of nets and
     *    newdev set to NULL, and dev->data.net will be NULL (and
     *    dev->type is NONE). olddev will have been completely
     *    released and freed. (aka success) In this case no extra
     *    cleanup is needed.
     *
     * 2) newdev has *not* been moved into the domain's list of nets,
     *    and dev->data.net == newdev (and dev->type == NET). In this *
     *    case, we need to at least release the "actual device" from *
     *    newdev (the caller will free dev->data.net a.k.a. newdev, and
     *    the original olddev is still in used)
     *
     * Note that case (2) isn't necessarily a failure. It may just be
     * that the changes were minor enough that we didn't need to
     * replace the entire device object.
     */
    if (newdev)
        networkReleaseActualDevice(newdev);

    return ret;
}



static virDomainGraphicsDefPtr qemuDomainFindGraphics(virDomainObjPtr vm,
                                                      virDomainGraphicsDefPtr dev)
{
    int i;

    for (i = 0 ; i < vm->def->ngraphics ; i++) {
        if (vm->def->graphics[i]->type == dev->type)
            return vm->def->graphics[i];
    }

    return NULL;
}


int
qemuDomainChangeGraphics(struct qemud_driver *driver,
                         virDomainObjPtr vm,
                         virDomainGraphicsDefPtr dev)
{
    virDomainGraphicsDefPtr olddev = qemuDomainFindGraphics(vm, dev);
    const char *oldListenAddr, *newListenAddr;
    const char *oldListenNetwork, *newListenNetwork;
    int ret = -1;

    if (!olddev) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot find existing graphics device to modify"));
        return -1;
    }

    oldListenAddr = virDomainGraphicsListenGetAddress(olddev, 0);
    newListenAddr = virDomainGraphicsListenGetAddress(dev, 0);
    oldListenNetwork = virDomainGraphicsListenGetNetwork(olddev, 0);
    newListenNetwork = virDomainGraphicsListenGetNetwork(dev, 0);

    switch (dev->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        if ((olddev->data.vnc.autoport != dev->data.vnc.autoport) ||
            (!dev->data.vnc.autoport &&
             (olddev->data.vnc.port != dev->data.vnc.port))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change port settings on vnc graphics"));
            return -1;
        }
        if (STRNEQ_NULLABLE(oldListenAddr,newListenAddr)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change listen address setting on vnc graphics"));
            return -1;
        }
        if (STRNEQ_NULLABLE(oldListenNetwork,newListenNetwork)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change listen network setting on vnc graphics"));
            return -1;
        }
        if (STRNEQ_NULLABLE(olddev->data.vnc.keymap, dev->data.vnc.keymap)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change keymap setting on vnc graphics"));
            return -1;
        }

        /* If a password lifetime was, or is set, or action if connected has
         * changed, then we must always run, even if new password matches
         * old password */
        if (olddev->data.vnc.auth.expires ||
            dev->data.vnc.auth.expires ||
            olddev->data.vnc.auth.connected != dev->data.vnc.auth.connected ||
            STRNEQ_NULLABLE(olddev->data.vnc.auth.passwd,
                            dev->data.vnc.auth.passwd)) {
            VIR_DEBUG("Updating password on VNC server %p %p",
                      dev->data.vnc.auth.passwd, driver->vncPassword);
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_VNC,
                                                    &dev->data.vnc.auth,
                                                    driver->vncPassword);
            if (ret < 0)
                return ret;

            /* Steal the new dev's  char * reference */
            VIR_FREE(olddev->data.vnc.auth.passwd);
            olddev->data.vnc.auth.passwd = dev->data.vnc.auth.passwd;
            dev->data.vnc.auth.passwd = NULL;
            olddev->data.vnc.auth.validTo = dev->data.vnc.auth.validTo;
            olddev->data.vnc.auth.expires = dev->data.vnc.auth.expires;
            olddev->data.vnc.auth.connected = dev->data.vnc.auth.connected;
        } else {
            ret = 0;
        }
        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        if ((olddev->data.spice.autoport != dev->data.spice.autoport) ||
            (!dev->data.spice.autoport &&
             (olddev->data.spice.port != dev->data.spice.port)) ||
            (!dev->data.spice.autoport &&
             (olddev->data.spice.tlsPort != dev->data.spice.tlsPort))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change port settings on spice graphics"));
            return -1;
        }
        if (STRNEQ_NULLABLE(oldListenAddr, newListenAddr)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change listen address setting on spice graphics"));
            return -1;
        }
        if (STRNEQ_NULLABLE(oldListenNetwork, newListenNetwork)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change listen network setting on spice graphics"));
            return -1;
        }
        if (STRNEQ_NULLABLE(olddev->data.spice.keymap,
                            dev->data.spice.keymap)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("cannot change keymap setting on spice graphics"));
            return -1;
        }

        /* We must reset the password if it has changed but also if:
         * - password lifetime is or was set
         * - the requested action has changed
         * - the action is "disconnect"
         */
        if (olddev->data.spice.auth.expires ||
            dev->data.spice.auth.expires ||
            olddev->data.spice.auth.connected != dev->data.spice.auth.connected ||
            dev->data.spice.auth.connected ==
            VIR_DOMAIN_GRAPHICS_AUTH_CONNECTED_DISCONNECT ||
            STRNEQ_NULLABLE(olddev->data.spice.auth.passwd,
                            dev->data.spice.auth.passwd)) {
            VIR_DEBUG("Updating password on SPICE server %p %p",
                      dev->data.spice.auth.passwd, driver->spicePassword);
            ret = qemuDomainChangeGraphicsPasswords(driver, vm,
                                                    VIR_DOMAIN_GRAPHICS_TYPE_SPICE,
                                                    &dev->data.spice.auth,
                                                    driver->spicePassword);

            if (ret < 0)
                return ret;

            /* Steal the new dev's char * reference */
            VIR_FREE(olddev->data.spice.auth.passwd);
            olddev->data.spice.auth.passwd = dev->data.spice.auth.passwd;
            dev->data.spice.auth.passwd = NULL;
            olddev->data.spice.auth.validTo = dev->data.spice.auth.validTo;
            olddev->data.spice.auth.expires = dev->data.spice.auth.expires;
            olddev->data.spice.auth.connected = dev->data.spice.auth.connected;
        } else {
            VIR_DEBUG("Not updating since password didn't change");
            ret = 0;
        }
        break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to change config on '%s' graphics type"),
                       virDomainGraphicsTypeToString(dev->type));
        break;
    }

    return ret;
}


static inline int qemuFindDisk(virDomainDefPtr def, const char *dst)
{
    int i;

    for (i = 0 ; i < def->ndisks ; i++) {
        if (STREQ(def->disks[i]->dst, dst)) {
            return i;
        }
    }

    return -1;
}

static int qemuComparePCIDevice(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                virDomainDeviceDefPtr device ATTRIBUTE_UNUSED,
                                virDomainDeviceInfoPtr info1,
                                void *opaque)
{
    virDomainDeviceInfoPtr info2 = opaque;

    if (info1->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI ||
        info2->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)
        return 0;

    if (info1->addr.pci.slot == info2->addr.pci.slot &&
        info1->addr.pci.function != info2->addr.pci.function)
        return -1;
    return 0;
}

static bool qemuIsMultiFunctionDevice(virDomainDefPtr def,
                                      virDomainDeviceInfoPtr dev)
{
    if (virDomainDeviceInfoIterate(def, qemuComparePCIDevice, dev) < 0)
        return true;
    return false;
}


int qemuDomainDetachPciDiskDevice(struct qemud_driver *driver,
                                  virDomainObjPtr vm,
                                  virDomainDeviceDefPtr dev)
{
    int i, ret = -1;
    virDomainDiskDefPtr detach = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCgroupPtr cgroup = NULL;
    char *drivestr = NULL;

    i = qemuFindDisk(vm->def, dev->data.disk->dst);

    if (i < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("disk %s not found"), dev->data.disk->dst);
        goto cleanup;
    }

    detach = vm->def->disks[i];

    if (qemuIsMultiFunctionDevice(vm->def, &detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %s"),
                       dev->data.disk->dst);
        goto cleanup;
    }

    if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_DEVICES)) {
        if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to find cgroup for %s"),
                           vm->def->name);
            goto cleanup;
        }
    }

    if (!virDomainDeviceAddressIsValid(&detach->info,
                                       VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("device cannot be detached without a PCI address"));
        goto cleanup;
    }

    /* build the actual drive id string as the disk->info.alias doesn't
     * contain the QEMU_DRIVE_HOST_PREFIX that is passed to qemu */
    if (virAsprintf(&drivestr, "%s%s",
                    QEMU_DRIVE_HOST_PREFIX, detach->info.alias) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuMonitorDelDevice(priv->mon, detach->info.alias) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditDisk(vm, detach->src, NULL, "detach", false);
            goto cleanup;
        }
    } else {
        if (qemuMonitorRemovePCIDevice(priv->mon,
                                       &detach->info.addr.pci) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditDisk(vm, detach->src, NULL, "detach", false);
            goto cleanup;
        }
    }

    /* disconnect guest from host device */
    qemuMonitorDriveDel(priv->mon, drivestr);

    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditDisk(vm, detach->src, NULL, "detach", true);

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        detach->info.addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on %s", dev->data.disk->src);

    virDomainDiskRemove(vm->def, i);

    virDomainDiskDefFree(detach);

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, dev->data.disk) < 0)
        VIR_WARN("Unable to restore security label on %s", dev->data.disk->src);

    if (cgroup != NULL) {
        if (qemuTeardownDiskCgroup(vm, cgroup, dev->data.disk) < 0)
            VIR_WARN("Failed to teardown cgroup for disk path %s",
                     NULLSTR(dev->data.disk->src));
    }

    if (virDomainLockDiskDetach(driver->lockManager, vm, dev->data.disk) < 0)
        VIR_WARN("Unable to release lock on %s", dev->data.disk->src);

    ret = 0;

cleanup:
    virCgroupFree(&cgroup);
    VIR_FREE(drivestr);
    return ret;
}

int qemuDomainDetachDiskDevice(struct qemud_driver *driver,
                               virDomainObjPtr vm,
                               virDomainDeviceDefPtr dev)
{
    int i, ret = -1;
    virDomainDiskDefPtr detach = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCgroupPtr cgroup = NULL;
    char *drivestr = NULL;

    i = qemuFindDisk(vm->def, dev->data.disk->dst);

    if (i < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("disk %s not found"), dev->data.disk->dst);
        goto cleanup;
    }

    if (!qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Underlying qemu does not support %s disk removal"),
                       virDomainDiskBusTypeToString(dev->data.disk->bus));
        goto cleanup;
    }

    detach = vm->def->disks[i];

    if (detach->mirror) {
        virReportError(VIR_ERR_BLOCK_COPY_ACTIVE,
                       _("disk '%s' is in an active block copy job"),
                       detach->dst);
        goto cleanup;
    }

    if (qemuCgroupControllerActive(driver, VIR_CGROUP_CONTROLLER_DEVICES)) {
        if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to find cgroup for %s"),
                           vm->def->name);
            goto cleanup;
        }
    }

    /* build the actual drive id string as the disk->info.alias doesn't
     * contain the QEMU_DRIVE_HOST_PREFIX that is passed to qemu */
    if (virAsprintf(&drivestr, "%s%s",
                    QEMU_DRIVE_HOST_PREFIX, detach->info.alias) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuMonitorDelDevice(priv->mon, detach->info.alias) < 0) {
        qemuDomainObjExitMonitorWithDriver(driver, vm);
        virDomainAuditDisk(vm, detach->src, NULL, "detach", false);
        goto cleanup;
    }

    /* disconnect guest from host device */
    qemuMonitorDriveDel(priv->mon, drivestr);

    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditDisk(vm, detach->src, NULL, "detach", true);

    virDomainDiskRemove(vm->def, i);

    virDomainDiskDefFree(detach);

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, dev->data.disk) < 0)
        VIR_WARN("Unable to restore security label on %s", dev->data.disk->src);

    if (cgroup != NULL) {
        if (qemuTeardownDiskCgroup(vm, cgroup, dev->data.disk) < 0)
            VIR_WARN("Failed to teardown cgroup for disk path %s",
                     NULLSTR(dev->data.disk->src));
    }

    if (virDomainLockDiskDetach(driver->lockManager, vm, dev->data.disk) < 0)
        VIR_WARN("Unable to release lock on disk %s", dev->data.disk->src);

    ret = 0;

cleanup:
    VIR_FREE(drivestr);
    virCgroupFree(&cgroup);
    return ret;
}

static bool qemuDomainDiskControllerIsBusy(virDomainObjPtr vm,
                                           virDomainControllerDefPtr detach)
{
    int i;
    virDomainDiskDefPtr disk;

    for (i = 0; i < vm->def->ndisks; i++) {
        disk = vm->def->disks[i];
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE)
            /* the disk does not use disk controller */
            continue;

        /* check whether the disk uses this type controller */
        if (disk->bus == VIR_DOMAIN_DISK_BUS_IDE &&
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_IDE)
            continue;
        if (disk->bus == VIR_DOMAIN_DISK_BUS_FDC &&
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_FDC)
            continue;
        if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI &&
            detach->type != VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
            continue;

        if (disk->info.addr.drive.controller == detach->idx)
            return true;
    }

    return false;
}

static bool qemuDomainControllerIsBusy(virDomainObjPtr vm,
                                       virDomainControllerDefPtr detach)
{
    switch (detach->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        return qemuDomainDiskControllerIsBusy(vm, detach);

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
    default:
        /* libvirt does not support sata controller, and does not support to
         * detach virtio and smart card controller.
         */
        return true;
    }
}

int qemuDomainDetachPciControllerDevice(struct qemud_driver *driver,
                                        virDomainObjPtr vm,
                                        virDomainDeviceDefPtr dev)
{
    int idx, ret = -1;
    virDomainControllerDefPtr detach = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if ((idx = virDomainControllerFind(vm->def,
                                       dev->data.controller->type,
                                       dev->data.controller->idx)) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("disk controller %s:%d not found"),
                       virDomainControllerTypeToString(dev->data.controller->type),
                       dev->data.controller->idx);
        goto cleanup;
    }

    detach = vm->def->controllers[idx];

    if (!virDomainDeviceAddressIsValid(&detach->info,
                                       VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("device cannot be detached without a PCI address"));
        goto cleanup;
    }

    if (qemuIsMultiFunctionDevice(vm->def, &detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %s"),
                       dev->data.disk->dst);
        goto cleanup;
    }

    if (qemuDomainControllerIsBusy(vm, detach)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("device cannot be detached: device is busy"));
        goto cleanup;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuAssignDeviceControllerAlias(detach) < 0)
            goto cleanup;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuMonitorDelDevice(priv->mon, detach->info.alias)) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            goto cleanup;
        }
    } else {
        if (qemuMonitorRemovePCIDevice(priv->mon,
                                       &detach->info.addr.pci) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            goto cleanup;
        }
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainControllerRemove(vm->def, idx);
    virDomainControllerDefFree(detach);

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        detach->info.addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on controller");

    ret = 0;

cleanup:
    return ret;
}

static int
qemuDomainDetachHostPciDevice(struct qemud_driver *driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr detach)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainHostdevSubsysPtr subsys = &detach->source.subsys;
    int ret;
    pciDevice *pci;
    pciDevice *activePci;

    if (qemuIsMultiFunctionDevice(vm->def, detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device: %.4x:%.2x:%.2x.%.1x"),
                       subsys->u.pci.domain, subsys->u.pci.bus,
                       subsys->u.pci.slot,   subsys->u.pci.function);
        return -1;
    }

    if (!virDomainDeviceAddressIsValid(detach->info,
                                       VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached without a PCI address"));
        return -1;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);
    } else {
        ret = qemuMonitorRemovePCIDevice(priv->mon, &detach->info->addr.pci);
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);
    virDomainAuditHostdev(vm, detach, "detach", ret == 0);
    if (ret < 0)
        return -1;

    /*
     * For SRIOV net host devices, unset mac and port profile before
     * reset and reattach device
     */
     if (detach->parent.data.net)
         qemuDomainHostdevNetConfigRestore(detach, driver->stateDir);

    pci = pciGetDevice(subsys->u.pci.domain, subsys->u.pci.bus,
                       subsys->u.pci.slot,   subsys->u.pci.function);
    if (pci) {
        activePci = pciDeviceListSteal(driver->activePciHostdevs, pci);
        if (activePci &&
            pciResetDevice(activePci, driver->activePciHostdevs,
                           driver->inactivePciHostdevs) == 0) {
            qemuReattachPciDevice(activePci, driver);
        } else {
            /* reset of the device failed, treat it as if it was returned */
            pciFreeDevice(activePci);
            ret = -1;
        }
        pciFreeDevice(pci);
    } else {
        ret = -1;
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        detach->info->addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on host device");

    return ret;
}

static int
qemuDomainDetachHostUsbDevice(struct qemud_driver *driver,
                              virDomainObjPtr vm,
                              virDomainHostdevDefPtr detach)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainHostdevSubsysPtr subsys = &detach->source.subsys;
    usbDevice *usb;
    int ret;

    if (!detach->info->alias) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached without a device alias"));
        return -1;
    }

    if (!qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached with this QEMU version"));
        return -1;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    ret = qemuMonitorDelDevice(priv->mon, detach->info->alias);
    qemuDomainObjExitMonitorWithDriver(driver, vm);
    virDomainAuditHostdev(vm, detach, "detach", ret == 0);
    if (ret < 0)
        return -1;

    usb = usbGetDevice(subsys->u.usb.bus, subsys->u.usb.device);
    if (usb) {
        usbDeviceListDel(driver->activeUsbHostdevs, usb);
        usbFreeDevice(usb);
    } else {
        VIR_WARN("Unable to find device %03d.%03d in list of used USB devices",
                 subsys->u.usb.bus, subsys->u.usb.device);
    }
    return ret;
}

static
int qemuDomainDetachThisHostDevice(struct qemud_driver *driver,
                                   virDomainObjPtr vm,
                                   virDomainHostdevDefPtr detach,
                                   int idx)
{
    int ret = -1;

    if (idx < 0) {
        /* caller didn't know index of hostdev in hostdevs list, so we
         * need to find it.
         */
        for (idx = 0; idx < vm->def->nhostdevs; idx++) {
            if (vm->def->hostdevs[idx] == detach)
                break;
        }
        if (idx >= vm->def->nhostdevs) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("device not found in hostdevs list (%zu entries)"),
                           vm->def->nhostdevs);
            return ret;
        }
    }

    switch (detach->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
        ret = qemuDomainDetachHostPciDevice(driver, vm, detach);
        break;
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
        ret = qemuDomainDetachHostUsbDevice(driver, vm, detach);
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hostdev subsys type '%s' not supported"),
                       virDomainHostdevSubsysTypeToString(detach->source.subsys.type));
        return -1;
    }

    if (!ret) {
        if (virSecurityManagerRestoreHostdevLabel(driver->securityManager,
                                                  vm->def, detach) < 0) {
            VIR_WARN("Failed to restore host device labelling");
        }
        virDomainHostdevRemove(vm->def, idx);
        virDomainHostdevDefFree(detach);
    }
    return ret;
}

/* search for a hostdev matching dev and detach it */
int qemuDomainDetachHostDevice(struct qemud_driver *driver,
                               virDomainObjPtr vm,
                               virDomainDeviceDefPtr dev)
{
    virDomainHostdevDefPtr hostdev = dev->data.hostdev;
    virDomainHostdevSubsysPtr subsys = &hostdev->source.subsys;
    virDomainHostdevDefPtr detach = NULL;
    int idx;

    if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hostdev mode '%s' not supported"),
                       virDomainHostdevModeTypeToString(hostdev->mode));
        return -1;
    }

    idx = virDomainHostdevFind(vm->def, hostdev, &detach);

    if (idx < 0) {
        switch(subsys->type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("host pci device %.4x:%.2x:%.2x.%.1x not found"),
                           subsys->u.pci.domain, subsys->u.pci.bus,
                           subsys->u.pci.slot, subsys->u.pci.function);
            break;
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
            if (subsys->u.usb.bus && subsys->u.usb.device) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("host usb device %03d.%03d not found"),
                               subsys->u.usb.bus, subsys->u.usb.device);
            } else {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("host usb device vendor=0x%.4x product=0x%.4x not found"),
                               subsys->u.usb.vendor, subsys->u.usb.product);
            }
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected hostdev type %d"), subsys->type);
            break;
        }
        return -1;
    }

    /* If this is a network hostdev, we need to use the higher-level detach
     * function so that mac address / virtualport are reset
     */
    if (detach->parent.type == VIR_DOMAIN_DEVICE_NET)
        return qemuDomainDetachNetDevice(driver, vm, &detach->parent);
    else
        return qemuDomainDetachThisHostDevice(driver, vm, detach, idx);
}

int
qemuDomainDetachNetDevice(struct qemud_driver *driver,
                          virDomainObjPtr vm,
                          virDomainDeviceDefPtr dev)
{
    int detachidx, ret = -1;
    virDomainNetDefPtr detach = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int vlan;
    char *hostnet_name = NULL;
    char mac[VIR_MAC_STRING_BUFLEN];
    virNetDevVPortProfilePtr vport = NULL;

    detachidx = virDomainNetFindIdx(vm->def, dev->data.net);
    if (detachidx == -2) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("multiple devices matching mac address %s found"),
                       virMacAddrFormat(&dev->data.net->mac, mac));
        goto cleanup;
        }
    else if (detachidx < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("network device %s not found"),
                       virMacAddrFormat(&dev->data.net->mac, mac));
        goto cleanup;
    }
    detach = vm->def->nets[detachidx];

    if (virDomainNetGetActualType(detach) == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        ret = qemuDomainDetachThisHostDevice(driver, vm,
                                             virDomainNetGetActualHostdev(detach),
                                             -1);
        goto cleanup;
    }

    if (!virDomainDeviceAddressIsValid(&detach->info,
                                       VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("device cannot be detached without a PCI address"));
        goto cleanup;
    }

    if (qemuIsMultiFunctionDevice(vm->def, &detach->info)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot hot unplug multifunction PCI device :%s"),
                       dev->data.disk->dst);
        goto cleanup;
    }

    if ((vlan = qemuDomainNetVLAN(detach)) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("unable to determine original VLAN"));
        goto cleanup;
    }

    if (virAsprintf(&hostnet_name, "host%s", detach->info.alias) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuMonitorDelDevice(priv->mon, detach->info.alias) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, detach, NULL, "detach", false);
            goto cleanup;
        }
    } else {
        if (qemuMonitorRemovePCIDevice(priv->mon,
                                       &detach->info.addr.pci) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, detach, NULL, "detach", false);
            goto cleanup;
        }
    }

    if (qemuCapsGet(priv->caps, QEMU_CAPS_NETDEV) &&
        qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE)) {
        if (qemuMonitorRemoveNetdev(priv->mon, hostnet_name) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, detach, NULL, "detach", false);
            goto cleanup;
        }
    } else {
        if (qemuMonitorRemoveHostNetwork(priv->mon, vlan, hostnet_name) < 0) {
            qemuDomainObjExitMonitorWithDriver(driver, vm);
            virDomainAuditNet(vm, detach, NULL, "detach", false);
            goto cleanup;
        }
    }
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    virDomainAuditNet(vm, detach, NULL, "detach", true);

    if (qemuCapsGet(priv->caps, QEMU_CAPS_DEVICE) &&
        qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                        detach->info.addr.pci.slot) < 0)
        VIR_WARN("Unable to release PCI address on NIC");

    virDomainConfNWFilterTeardown(detach);

    if (virDomainNetGetActualType(detach) == VIR_DOMAIN_NET_TYPE_DIRECT) {
        ignore_value(virNetDevMacVLanDeleteWithVPortProfile(
                         detach->ifname, &detach->mac,
                         virDomainNetGetActualDirectDev(detach),
                         virDomainNetGetActualDirectMode(detach),
                         virDomainNetGetActualVirtPortProfile(detach),
                         driver->stateDir));
        VIR_FREE(detach->ifname);
    }

    if ((driver->macFilter) && (detach->ifname != NULL)) {
        if ((errno = networkDisallowMacOnPort(driver,
                                              detach->ifname,
                                              &detach->mac))) {
            virReportSystemError(errno,
             _("failed to remove ebtables rule on '%s'"),
                                 detach->ifname);
        }
    }

    vport = virDomainNetGetActualVirtPortProfile(detach);
    if (vport && vport->virtPortType == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH)
        ignore_value(virNetDevOpenvswitchRemovePort(
                        virDomainNetGetActualBridgeName(detach),
                        detach->ifname));
    ret = 0;
cleanup:
    if (!ret) {
        networkReleaseActualDevice(detach);
        virDomainNetRemove(vm->def, detachidx);
        virDomainNetDefFree(detach);
    }
    VIR_FREE(hostnet_name);
    return ret;
}

int
qemuDomainChangeGraphicsPasswords(struct qemud_driver *driver,
                                  virDomainObjPtr vm,
                                  int type,
                                  virDomainGraphicsAuthDefPtr auth,
                                  const char *defaultPasswd)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    time_t now = time(NULL);
    char expire_time [64];
    const char *connected = NULL;
    int ret;

    if (!auth->passwd && !driver->vncPassword)
        return 0;

    if (auth->connected)
        connected = virDomainGraphicsAuthConnectedTypeToString(auth->connected);

    qemuDomainObjEnterMonitorWithDriver(driver, vm);
    ret = qemuMonitorSetPassword(priv->mon,
                                 type,
                                 auth->passwd ? auth->passwd : defaultPasswd,
                                 connected);

    if (ret == -2) {
        if (type != VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Graphics password only supported for VNC"));
            ret = -1;
        } else {
            ret = qemuMonitorSetVNCPassword(priv->mon,
                                            auth->passwd ? auth->passwd : defaultPasswd);
        }
    }
    if (ret != 0)
        goto cleanup;

    if (auth->expires) {
        time_t lifetime = auth->validTo - now;
        if (lifetime <= 0)
            snprintf(expire_time, sizeof(expire_time), "now");
        else
            snprintf(expire_time, sizeof(expire_time), "%lu", (long unsigned)auth->validTo);
    } else {
        snprintf(expire_time, sizeof(expire_time), "never");
    }

    ret = qemuMonitorExpirePassword(priv->mon, type, expire_time);

    if (ret == -2) {
        /* XXX we could fake this with a timer */
        if (auth->expires) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Expiry of passwords is not supported"));
            ret = -1;
        } else {
            ret = 0;
        }
    }

cleanup:
    qemuDomainObjExitMonitorWithDriver(driver, vm);

    return ret;
}

int qemuDomainAttachLease(struct qemud_driver *driver,
                          virDomainObjPtr vm,
                          virDomainLeaseDefPtr lease)
{
    if (virDomainLeaseInsertPreAlloc(vm->def) < 0)
        return -1;

    if (virDomainLockLeaseAttach(driver->lockManager, driver->uri,
                                 vm, lease) < 0) {
        virDomainLeaseInsertPreAlloced(vm->def, NULL);
        return -1;
    }

    virDomainLeaseInsertPreAlloced(vm->def, lease);
    return 0;
}

int qemuDomainDetachLease(struct qemud_driver *driver,
                          virDomainObjPtr vm,
                          virDomainLeaseDefPtr lease)
{
    virDomainLeaseDefPtr det_lease;
    int i;

    if ((i = virDomainLeaseIndex(vm->def, lease)) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Lease %s in lockspace %s does not exist"),
                       lease->key, NULLSTR(lease->lockspace));
        return -1;
    }

    if (virDomainLockLeaseDetach(driver->lockManager, vm, lease) < 0)
        return -1;

    det_lease = virDomainLeaseRemoveAt(vm->def, i);
    virDomainLeaseDefFree(det_lease);
    return 0;
}
