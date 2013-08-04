/*
 * qemu_command.c: QEMU command generation
 *
 * Copyright (C) 2006-2013 Red Hat, Inc.
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

#include "qemu_command.h"
#include "qemu_hostdev.h"
#include "qemu_capabilities.h"
#include "qemu_bridge_filter.h"
#include "cpu/cpu.h"
#include "passfd.h"
#include "viralloc.h"
#include "virlog.h"
#include "virarch.h"
#include "virerror.h"
#include "virfile.h"
#include "virnetdev.h"
#include "virstring.h"
#include "viruuid.h"
#include "c-ctype.h"
#include "domain_nwfilter.h"
#include "domain_audit.h"
#include "domain_conf.h"
#include "snapshot_conf.h"
#include "storage_conf.h"
#include "network/bridge_driver.h"
#include "virnetdevtap.h"
#include "base64.h"
#include "device_conf.h"
#include "virstoragefile.h"
#include "virtpm.h"
#include "virscsi.h"
#if defined(__linux__)
# include <linux/capability.h>
#endif

#include <sys/stat.h>
#include <fcntl.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

#define VIO_ADDR_NET 0x1000ul
#define VIO_ADDR_SCSI 0x2000ul
#define VIO_ADDR_SERIAL 0x30000000ul
#define VIO_ADDR_NVRAM 0x3000ul

VIR_ENUM_DECL(virDomainDiskQEMUBus)
VIR_ENUM_IMPL(virDomainDiskQEMUBus, VIR_DOMAIN_DISK_BUS_LAST,
              "ide",
              "floppy",
              "scsi",
              "virtio",
              "xen",
              "usb",
              "uml",
              "sata")


VIR_ENUM_DECL(qemuDiskCacheV1)
VIR_ENUM_DECL(qemuDiskCacheV2)

VIR_ENUM_IMPL(qemuDiskCacheV1, VIR_DOMAIN_DISK_CACHE_LAST,
              "default",
              "off",
              "off",  /* writethrough not supported, so for safety, disable */
              "on",   /* Old 'on' was equivalent to 'writeback' */
              "off",  /* directsync not supported, for safety, disable */
              "off"); /* unsafe not supported, for safety, disable */

VIR_ENUM_IMPL(qemuDiskCacheV2, VIR_DOMAIN_DISK_CACHE_LAST,
              "default",
              "none",
              "writethrough",
              "writeback",
              "directsync",
              "unsafe");

VIR_ENUM_DECL(qemuVideo)

VIR_ENUM_IMPL(qemuVideo, VIR_DOMAIN_VIDEO_TYPE_LAST,
              "std",
              "cirrus",
              "vmware",
              "", /* no arg needed for xen */
              "", /* don't support vbox */
              "qxl");

VIR_ENUM_DECL(qemuDeviceVideo)

VIR_ENUM_IMPL(qemuDeviceVideo, VIR_DOMAIN_VIDEO_TYPE_LAST,
              "VGA",
              "cirrus-vga",
              "vmware-svga",
              "", /* no device for xen */
              "", /* don't support vbox */
              "qxl-vga");

VIR_ENUM_DECL(qemuSoundCodec)

VIR_ENUM_IMPL(qemuSoundCodec, VIR_DOMAIN_SOUND_CODEC_TYPE_LAST,
              "hda-duplex",
              "hda-micro");

VIR_ENUM_DECL(qemuControllerModelUSB)

VIR_ENUM_IMPL(qemuControllerModelUSB, VIR_DOMAIN_CONTROLLER_MODEL_USB_LAST,
              "piix3-usb-uhci",
              "piix4-usb-uhci",
              "usb-ehci",
              "ich9-usb-ehci1",
              "ich9-usb-uhci1",
              "ich9-usb-uhci2",
              "ich9-usb-uhci3",
              "vt82c686b-usb-uhci",
              "pci-ohci",
              "nec-usb-xhci",
              "none");

VIR_ENUM_DECL(qemuDomainFSDriver)
VIR_ENUM_IMPL(qemuDomainFSDriver, VIR_DOMAIN_FS_DRIVER_TYPE_LAST,
              "local",
              "local",
              "handle",
              NULL,
              NULL);


/**
 * qemuPhysIfaceConnect:
 * @def: the definition of the VM (needed by 802.1Qbh and audit)
 * @driver: pointer to the driver instance
 * @net: pointer to he VM's interface description with direct device type
 * @qemuCaps: flags for qemu
 * @vmop: VM operation type
 *
 * Returns a filedescriptor on success or -1 in case of error.
 */
int
qemuPhysIfaceConnect(virDomainDefPtr def,
                     virQEMUDriverPtr driver,
                     virDomainNetDefPtr net,
                     virQEMUCapsPtr qemuCaps,
                     enum virNetDevVPortProfileOp vmop)
{
    int rc;
    char *res_ifname = NULL;
    int vnet_hdr = 0;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNET_HDR) &&
        net->model && STREQ(net->model, "virtio"))
        vnet_hdr = 1;

    rc = virNetDevMacVLanCreateWithVPortProfile(
        net->ifname, &net->mac,
        virDomainNetGetActualDirectDev(net),
        virDomainNetGetActualDirectMode(net),
        true, vnet_hdr, def->uuid,
        virDomainNetGetActualVirtPortProfile(net),
        &res_ifname,
        vmop, cfg->stateDir,
        virDomainNetGetActualBandwidth(net));
    if (rc >= 0) {
        if (virSecurityManagerSetTapFDLabel(driver->securityManager,
                                            def, rc) < 0)
            goto error;

        virDomainAuditNetDevice(def, net, res_ifname, true);
        VIR_FREE(net->ifname);
        net->ifname = res_ifname;
    }

    virObjectUnref(cfg);
    return rc;

error:
    ignore_value(virNetDevMacVLanDeleteWithVPortProfile(
                     res_ifname, &net->mac,
                     virDomainNetGetActualDirectDev(net),
                     virDomainNetGetActualDirectMode(net),
                     virDomainNetGetActualVirtPortProfile(net),
                     cfg->stateDir));
    VIR_FREE(res_ifname);
    virObjectUnref(cfg);
    return -1;
}


/**
 * qemuCreateInBridgePortWithHelper:
 * @cfg: the configuration object in which the helper name is looked up
 * @brname: the bridge name
 * @ifname: the returned interface name
 * @macaddr: the returned MAC address
 * @tapfd: file descriptor return value for the new tap device
 * @flags: OR of virNetDevTapCreateFlags:

 *   VIR_NETDEV_TAP_CREATE_VNET_HDR
 *     - Enable IFF_VNET_HDR on the tap device
 *
 * This function creates a new tap device on a bridge using an external
 * helper.  The final name for the bridge will be stored in @ifname.
 *
 * Returns 0 in case of success or -1 on failure
 */
static int qemuCreateInBridgePortWithHelper(virQEMUDriverConfigPtr cfg,
                                            const char *brname,
                                            char **ifname,
                                            int *tapfd,
                                            unsigned int flags)
{
    virCommandPtr cmd;
    int status;
    int pair[2] = { -1, -1 };

    if ((flags & ~VIR_NETDEV_TAP_CREATE_VNET_HDR) != VIR_NETDEV_TAP_CREATE_IFUP)
        return -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        virReportSystemError(errno, "%s", _("failed to create socket"));
        return -1;
    }

    cmd = virCommandNew(cfg->bridgeHelperName);
    if (flags & VIR_NETDEV_TAP_CREATE_VNET_HDR)
        virCommandAddArgFormat(cmd, "--use-vnet");
    virCommandAddArgFormat(cmd, "--br=%s", brname);
    virCommandAddArgFormat(cmd, "--fd=%d", pair[1]);
    virCommandPassFD(cmd, pair[1],
                     VIR_COMMAND_PASS_FD_CLOSE_PARENT);
    virCommandClearCaps(cmd);
#ifdef CAP_NET_ADMIN
    virCommandAllowCap(cmd, CAP_NET_ADMIN);
#endif
    if (virCommandRunAsync(cmd, NULL) < 0) {
        *tapfd = -1;
        goto cleanup;
    }

    do {
        *tapfd = recvfd(pair[0], 0);
    } while (*tapfd < 0 && errno == EINTR);
    if (*tapfd < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to retrieve file descriptor for interface"));
        goto cleanup;
    }

    if (virNetDevTapGetName(*tapfd, ifname) < 0 ||
        virCommandWait(cmd, &status) < 0) {
        VIR_FORCE_CLOSE(*tapfd);
        *tapfd = -1;
    }

cleanup:
    virCommandFree(cmd);
    VIR_FORCE_CLOSE(pair[0]);
    return *tapfd < 0 ? -1 : 0;
}

int
qemuNetworkIfaceConnect(virDomainDefPtr def,
                        virConnectPtr conn,
                        virQEMUDriverPtr driver,
                        virDomainNetDefPtr net,
                        virQEMUCapsPtr qemuCaps,
                        int *tapfd,
                        int *tapfdSize)
{
    char *brname = NULL;
    int ret = -1;
    unsigned int tap_create_flags = VIR_NETDEV_TAP_CREATE_IFUP;
    bool template_ifname = false;
    int actualType = virDomainNetGetActualType(net);
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK) {
        int active;
        bool fail = false;
        virErrorPtr errobj;
        virNetworkPtr network = virNetworkLookupByName(conn,
                                                       net->data.network.name);
        if (!network)
            return ret;

        active = virNetworkIsActive(network);
        if (active != 1) {
            fail = true;

            if (active == 0)
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Network '%s' is not active."),
                               net->data.network.name);
        }

        if (!fail) {
            brname = virNetworkGetBridgeName(network);
            if (brname == NULL)
                fail = true;
        }

        /* Make sure any above failure is preserved */
        errobj = virSaveLastError();
        virNetworkFree(network);
        virSetError(errobj);
        virFreeError(errobj);

        if (fail)
            return ret;

    } else if (actualType == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        if (VIR_STRDUP(brname, virDomainNetGetActualBridgeName(net)) < 0)
            return ret;
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Network type %d is not supported"),
                       virDomainNetGetActualType(net));
        return ret;
    }

    if (!net->ifname ||
        STRPREFIX(net->ifname, VIR_NET_GENERATED_PREFIX) ||
        strchr(net->ifname, '%')) {
        VIR_FREE(net->ifname);
        if (VIR_STRDUP(net->ifname, VIR_NET_GENERATED_PREFIX "%d") < 0)
            goto cleanup;
        /* avoid exposing vnet%d in getXMLDesc or error outputs */
        template_ifname = true;
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNET_HDR) &&
        net->model && STREQ(net->model, "virtio")) {
        tap_create_flags |= VIR_NETDEV_TAP_CREATE_VNET_HDR;
    }

    if (cfg->privileged) {
        if (virNetDevTapCreateInBridgePort(brname, &net->ifname, &net->mac,
                                           def->uuid, tapfd, *tapfdSize,
                                           virDomainNetGetActualVirtPortProfile(net),
                                           virDomainNetGetActualVlan(net),
                                           tap_create_flags) < 0) {
            virDomainAuditNetDevice(def, net, "/dev/net/tun", false);
            goto cleanup;
        }
    } else {
        if (qemuCreateInBridgePortWithHelper(cfg, brname,
                                             &net->ifname,
                                             tapfd, tap_create_flags) < 0) {
            virDomainAuditNetDevice(def, net, "/dev/net/tun", false);
            goto cleanup;
        }
        /* qemuCreateInBridgePortWithHelper can only create a single FD */
        if (*tapfdSize > 1) {
            VIR_WARN("Ignoring multiqueue network request");
            *tapfdSize = 1;
        }
    }

    virDomainAuditNetDevice(def, net, "/dev/net/tun", true);

    if (cfg->macFilter &&
        (ret = networkAllowMacOnPort(driver, net->ifname, &net->mac)) < 0) {
        virReportSystemError(ret,
                             _("failed to add ebtables rule "
                               "to allow MAC address on '%s'"),
                             net->ifname);
    }

    if (virNetDevBandwidthSet(net->ifname,
                              virDomainNetGetActualBandwidth(net),
                              false) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot set bandwidth limits on %s"),
                       net->ifname);
        goto cleanup;
    }

    if (net->filter && net->ifname &&
        virDomainConfNWFilterInstantiate(conn, def->uuid, net) < 0) {
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (ret < 0) {
        size_t i;
        for (i = 0; i < *tapfdSize; i++)
            VIR_FORCE_CLOSE(tapfd[i]);
        if (template_ifname)
            VIR_FREE(net->ifname);
    }
    VIR_FREE(brname);
    virObjectUnref(cfg);

    return ret;
}


/**
 * qemuOpenVhostNet:
 * @def: domain definition
 * @net: network definition
 * @qemuCaps: qemu binary capabilities
 * @vhostfd: array of opened vhost-net device
 * @vhostfdSize: number of file descriptors in @vhostfd array
 *
 * Open vhost-net, multiple times - if requested.
 * In case, no vhost-net is needed, @vhostfdSize is set to 0
 * and 0 is returned.
 *
 * Returns: 0 on success
 *         -1 on failure
 */
int
qemuOpenVhostNet(virDomainDefPtr def,
                 virDomainNetDefPtr net,
                 virQEMUCapsPtr qemuCaps,
                 int *vhostfd,
                 int *vhostfdSize)
{
    size_t i;

    /* If the config says explicitly to not use vhost, return now */
    if (net->driver.virtio.name == VIR_DOMAIN_NET_BACKEND_TYPE_QEMU) {
        *vhostfdSize = 0;
        return 0;
    }

    /* If qemu doesn't support vhost-net mode (including the -netdev command
     * option), don't try to open the device.
     */
    if (!(virQEMUCapsGet(qemuCaps, QEMU_CAPS_VHOST_NET) &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_NETDEV) &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))) {
        if (net->driver.virtio.name == VIR_DOMAIN_NET_BACKEND_TYPE_VHOST) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("vhost-net is not supported with "
                                   "this QEMU binary"));
            return -1;
        }
        *vhostfdSize = 0;
        return 0;
    }

    /* If the nic model isn't virtio, don't try to open. */
    if (!(net->model && STREQ(net->model, "virtio"))) {
        if (net->driver.virtio.name == VIR_DOMAIN_NET_BACKEND_TYPE_VHOST) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("vhost-net is only supported for "
                                   "virtio network interfaces"));
            return -1;
        }
        *vhostfdSize = 0;
        return 0;
    }

    for (i = 0; i < *vhostfdSize; i++) {
        vhostfd[i] = open("/dev/vhost-net", O_RDWR);

        /* If the config says explicitly to use vhost and we couldn't open it,
         * report an error.
         */
        if (vhostfd[i] < 0) {
            virDomainAuditNetDevice(def, net, "/dev/vhost-net", false);
            if (net->driver.virtio.name == VIR_DOMAIN_NET_BACKEND_TYPE_VHOST) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("vhost-net was requested for an interface, "
                                       "but is unavailable"));
                goto error;
            }
            VIR_WARN("Unable to open vhost-net. Opened so far %zu, requested %d",
                     i, *vhostfdSize);
            *vhostfdSize = i;
            break;
        }
    }
    virDomainAuditNetDevice(def, net, "/dev/vhost-net", *vhostfdSize);
    return 0;

error:
    while (i--)
        VIR_FORCE_CLOSE(vhostfd[i]);

    return -1;
}

int
qemuNetworkPrepareDevices(virDomainDefPtr def)
{
    int ret = -1;
    size_t i;

    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];
        int actualType;

        /* If appropriate, grab a physical device from the configured
         * network's pool of devices, or resolve bridge device name
         * to the one defined in the network definition.
         */
        if (networkAllocateActualDevice(net) < 0)
            goto cleanup;

        actualType = virDomainNetGetActualType(net);
        if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
            net->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
            /* Each type='hostdev' network device must also have a
             * corresponding entry in the hostdevs array. For netdevs
             * that are hardcoded as type='hostdev', this is already
             * done by the parser, but for those allocated from a
             * network / determined at runtime, we need to do it
             * separately.
             */
            virDomainHostdevDefPtr hostdev = virDomainNetGetActualHostdev(net);

            if (virDomainHostdevFind(def, hostdev, NULL) >= 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("PCI device %04x:%02x:%02x.%x "
                                 "allocated from network %s is already "
                                 "in use by domain %s"),
                               hostdev->source.subsys.u.pci.addr.domain,
                               hostdev->source.subsys.u.pci.addr.bus,
                               hostdev->source.subsys.u.pci.addr.slot,
                               hostdev->source.subsys.u.pci.addr.function,
                               net->data.network.name,
                               def->name);
                goto cleanup;
            }
            if (virDomainHostdevInsert(def, hostdev) < 0)
                goto cleanup;
        }
    }
    ret = 0;
cleanup:
    return ret;
}

static int qemuDomainDeviceAliasIndex(virDomainDeviceInfoPtr info,
                                      const char *prefix)
{
    int idx;

    if (!info->alias)
        return -1;
    if (!STRPREFIX(info->alias, prefix))
        return -1;

    if (virStrToLong_i(info->alias + strlen(prefix), NULL, 10, &idx) < 0)
        return -1;

    return idx;
}


int qemuDomainNetVLAN(virDomainNetDefPtr def)
{
    return qemuDomainDeviceAliasIndex(&def->info, "net");
}


/* Names used before -drive existed */
static int qemuAssignDeviceDiskAliasLegacy(virDomainDiskDefPtr disk)
{
    char *dev_name;

    if (VIR_STRDUP(dev_name,
                   disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM &&
                   STREQ(disk->dst, "hdc") ? "cdrom" : disk->dst) < 0)
        return -1;
    disk->info.alias = dev_name;
    return 0;
}


char *qemuDeviceDriveHostAlias(virDomainDiskDefPtr disk,
                               virQEMUCapsPtr qemuCaps)
{
    char *ret;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        ignore_value(virAsprintf(&ret, "%s%s", QEMU_DRIVE_HOST_PREFIX,
                                 disk->info.alias));
    } else {
        ignore_value(VIR_STRDUP(ret, disk->info.alias));
    }
    return ret;
}


/* Names used before -drive supported the id= option */
static int qemuAssignDeviceDiskAliasFixed(virDomainDiskDefPtr disk)
{
    int busid, devid;
    int ret;
    char *dev_name;

    if (virDiskNameToBusDeviceIndex(disk, &busid, &devid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot convert disk '%s' to bus/device index"),
                       disk->dst);
        return -1;
    }

    switch (disk->bus) {
    case VIR_DOMAIN_DISK_BUS_IDE:
        if (disk->device== VIR_DOMAIN_DISK_DEVICE_DISK)
            ret = virAsprintf(&dev_name, "ide%d-hd%d", busid, devid);
        else
            ret = virAsprintf(&dev_name, "ide%d-cd%d", busid, devid);
        break;
    case VIR_DOMAIN_DISK_BUS_SCSI:
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK)
            ret = virAsprintf(&dev_name, "scsi%d-hd%d", busid, devid);
        else
            ret = virAsprintf(&dev_name, "scsi%d-cd%d", busid, devid);
        break;
    case VIR_DOMAIN_DISK_BUS_FDC:
        ret = virAsprintf(&dev_name, "floppy%d", devid);
        break;
    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        ret = virAsprintf(&dev_name, "virtio%d", devid);
        break;
    case VIR_DOMAIN_DISK_BUS_XEN:
        ret = virAsprintf(&dev_name, "xenblk%d", devid);
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported disk name mapping for bus '%s'"),
                       virDomainDiskBusTypeToString(disk->bus));
        return -1;
    }

    if (ret == -1)
        return -1;

    disk->info.alias = dev_name;

    return 0;
}

static int
qemuSetScsiControllerModel(virDomainDefPtr def,
                           virQEMUCapsPtr qemuCaps,
                           int *model)
{
    if (*model > 0) {
        switch (*model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_LSI)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support "
                                 "the LSI 53C895A SCSI controller"));
                return -1;
            }
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_SCSI)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support "
                                 "virtio scsi controller"));
                return -1;
            }
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
            /*TODO: need checking work here if necessary */
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_MEGASAS)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support "
                                 "the LSI SAS1078 controller"));
                return -1;
            }
            break;
        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported controller model: %s"),
                           virDomainControllerModelSCSITypeToString(*model));
            return -1;
        }
    } else {
        if ((def->os.arch == VIR_ARCH_PPC64) &&
            STREQ(def->os.machine, "pseries")) {
            *model = VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI;
        } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_LSI)) {
            *model = VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC;
        } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_SCSI)) {
            *model = VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI;
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unable to determine model for scsi controller"));
            return -1;
        }
    }

    return 0;
}

/* Our custom -drive naming scheme used with id= */
static int
qemuAssignDeviceDiskAliasCustom(virDomainDefPtr def,
                                virDomainDiskDefPtr disk,
                                virQEMUCapsPtr qemuCaps)
{
    const char *prefix = virDomainDiskBusTypeToString(disk->bus);
    int controllerModel = -1;

    if (disk->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
        if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI) {
            controllerModel =
                virDomainDeviceFindControllerModel(def, &disk->info,
                                                   VIR_DOMAIN_CONTROLLER_TYPE_SCSI);

            if ((qemuSetScsiControllerModel(def, qemuCaps, &controllerModel)) < 0)
                return -1;
        }

        if (disk->bus != VIR_DOMAIN_DISK_BUS_SCSI ||
            controllerModel == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
            if (virAsprintf(&disk->info.alias, "%s%d-%d-%d", prefix,
                            disk->info.addr.drive.controller,
                            disk->info.addr.drive.bus,
                            disk->info.addr.drive.unit) < 0)
                return -1;
        } else {
            if (virAsprintf(&disk->info.alias, "%s%d-%d-%d-%d", prefix,
                            disk->info.addr.drive.controller,
                            disk->info.addr.drive.bus,
                            disk->info.addr.drive.target,
                            disk->info.addr.drive.unit) < 0)
                return -1;
        }
    } else {
        int idx = virDiskNameToIndex(disk->dst);
        if (virAsprintf(&disk->info.alias, "%s-disk%d", prefix, idx) < 0)
            return -1;
    }

    return 0;
}


int
qemuAssignDeviceDiskAlias(virDomainDefPtr vmdef,
                          virDomainDiskDefPtr def,
                          virQEMUCapsPtr qemuCaps)
{
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE)) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
            return qemuAssignDeviceDiskAliasCustom(vmdef, def, qemuCaps);
        else
            return qemuAssignDeviceDiskAliasFixed(def);
    } else {
        return qemuAssignDeviceDiskAliasLegacy(def);
    }
}


int
qemuAssignDeviceNetAlias(virDomainDefPtr def, virDomainNetDefPtr net, int idx)
{
    if (idx == -1) {
        size_t i;
        idx = 0;
        for (i = 0; i < def->nnets; i++) {
            int thisidx;

            if (def->nets[i]->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
                /* type='hostdev' interfaces have a hostdev%d alias */
               continue;
            }
            if ((thisidx = qemuDomainDeviceAliasIndex(&def->nets[i]->info, "net")) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Unable to determine device index for network device"));
                return -1;
            }
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    if (virAsprintf(&net->info.alias, "net%d", idx) < 0)
        return -1;
    return 0;
}


int
qemuAssignDeviceHostdevAlias(virDomainDefPtr def, virDomainHostdevDefPtr hostdev, int idx)
{
    if (idx == -1) {
        size_t i;
        idx = 0;
        for (i = 0; i < def->nhostdevs; i++) {
            int thisidx;
            if ((thisidx = qemuDomainDeviceAliasIndex(def->hostdevs[i]->info, "hostdev")) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Unable to determine device index for hostdev device"));
                return -1;
            }
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    if (virAsprintf(&hostdev->info->alias, "hostdev%d", idx) < 0)
        return -1;

    return 0;
}


int
qemuAssignDeviceRedirdevAlias(virDomainDefPtr def, virDomainRedirdevDefPtr redirdev, int idx)
{
    if (idx == -1) {
        size_t i;
        idx = 0;
        for (i = 0; i < def->nredirdevs; i++) {
            int thisidx;
            if ((thisidx = qemuDomainDeviceAliasIndex(&def->redirdevs[i]->info, "redir")) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Unable to determine device index for redirected device"));
                return -1;
            }
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    if (virAsprintf(&redirdev->info.alias, "redir%d", idx) < 0)
        return -1;
    return 0;
}


int
qemuAssignDeviceControllerAlias(virDomainControllerDefPtr controller)
{
    const char *prefix = virDomainControllerTypeToString(controller->type);

    if (virAsprintf(&controller->info.alias, "%s%d", prefix,
                    controller->idx) < 0)
        return -1;
    return 0;
}

static ssize_t
qemuGetNextChrDevIndex(virDomainDefPtr def,
                       virDomainChrDefPtr chr,
                       const char *prefix)
{
    virDomainChrDefPtr **arrPtr;
    size_t *cntPtr;
    size_t i;
    ssize_t idx = 0;
    const char *prefix2 = NULL;

    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE)
        prefix2 = "serial";

    virDomainChrGetDomainPtrs(def, chr, &arrPtr, &cntPtr);

    for (i = 0; i < *cntPtr; i++) {
        ssize_t thisidx;
        if (((thisidx = qemuDomainDeviceAliasIndex(&(*arrPtr)[i]->info, prefix)) < 0) &&
            (prefix2 &&
             (thisidx = qemuDomainDeviceAliasIndex(&(*arrPtr)[i]->info, prefix2)) < 0)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unable to determine device index for character device"));
            return -1;
        }
        if (thisidx >= idx)
            idx = thisidx + 1;
    }

    return idx;
}


int
qemuAssignDeviceChrAlias(virDomainDefPtr def,
                         virDomainChrDefPtr chr,
                         ssize_t idx)
{
    const char *prefix = NULL;

    switch ((enum virDomainChrDeviceType) chr->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
        prefix = "parallel";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        prefix = "serial";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        prefix = "console";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        prefix = "channel";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        return -1;
    }

    if (idx == -1 && (idx = qemuGetNextChrDevIndex(def, chr, prefix)) < 0)
        return -1;

    return virAsprintf(&chr->info.alias, "%s%zd", prefix, idx);
}

int
qemuAssignDeviceAliases(virDomainDefPtr def, virQEMUCapsPtr qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (qemuAssignDeviceDiskAlias(def, def->disks[i], qemuCaps) < 0)
            return -1;
    }
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NET_NAME) ||
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        for (i = 0; i < def->nnets; i++) {
            /* type='hostdev' interfaces are also on the hostdevs list,
             * and will have their alias assigned with other hostdevs.
             */
            if ((def->nets[i]->type != VIR_DOMAIN_NET_TYPE_HOSTDEV) &&
                (qemuAssignDeviceNetAlias(def, def->nets[i], i) < 0)) {
                return -1;
            }
        }
    }

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
        return 0;

    for (i = 0; i < def->nfss; i++) {
        if (virAsprintf(&def->fss[i]->info.alias, "fs%zu", i) < 0)
            return -1;
    }
    for (i = 0; i < def->nsounds; i++) {
        if (virAsprintf(&def->sounds[i]->info.alias, "sound%zu", i) < 0)
            return -1;
    }
    for (i = 0; i < def->nhostdevs; i++) {
        if (qemuAssignDeviceHostdevAlias(def, def->hostdevs[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nredirdevs; i++) {
        if (qemuAssignDeviceRedirdevAlias(def, def->redirdevs[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nvideos; i++) {
        if (virAsprintf(&def->videos[i]->info.alias, "video%zu", i) < 0)
            return -1;
    }
    for (i = 0; i < def->ncontrollers; i++) {
        if (qemuAssignDeviceControllerAlias(def->controllers[i]) < 0)
            return -1;
    }
    for (i = 0; i < def->ninputs; i++) {
        if (virAsprintf(&def->inputs[i]->info.alias, "input%zu", i) < 0)
            return -1;
    }
    for (i = 0; i < def->nparallels; i++) {
        if (qemuAssignDeviceChrAlias(def, def->parallels[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nserials; i++) {
        if (qemuAssignDeviceChrAlias(def, def->serials[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nchannels; i++) {
        if (qemuAssignDeviceChrAlias(def, def->channels[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nconsoles; i++) {
        if (qemuAssignDeviceChrAlias(def, def->consoles[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nhubs; i++) {
        if (virAsprintf(&def->hubs[i]->info.alias, "hub%zu", i) < 0)
            return -1;
    }
    for (i = 0; i < def->nsmartcards; i++) {
        if (virAsprintf(&def->smartcards[i]->info.alias, "smartcard%zu", i) < 0)
            return -1;
    }
    if (def->watchdog) {
        if (virAsprintf(&def->watchdog->info.alias, "watchdog%d", 0) < 0)
            return -1;
    }
    if (def->memballoon) {
        if (virAsprintf(&def->memballoon->info.alias, "balloon%d", 0) < 0)
            return -1;
    }
    if (def->rng) {
        if (virAsprintf(&def->rng->info.alias, "rng%d", 0) < 0)
            return -1;
    }
    if (def->tpm) {
        if (virAsprintf(&def->tpm->info.alias, "tpm%d", 0) < 0)
            return -1;
    }

    return 0;
}

/* S390 ccw bus support */

struct _qemuDomainCCWAddressSet {
    virHashTablePtr defined;
    virDomainDeviceCCWAddress next;
};

static char*
qemuCCWAddressAsString(virDomainDeviceCCWAddressPtr addr)
{
    char *addrstr = NULL;

    ignore_value(virAsprintf(&addrstr, "%x.%x.%04x",
                             addr->cssid,
                             addr->ssid,
                             addr->devno));
    return addrstr;
}

static int
qemuCCWAdressIncrement(virDomainDeviceCCWAddressPtr addr)
{
    virDomainDeviceCCWAddress ccwaddr = *addr;

    /* We are not touching subchannel sets and channel subsystems */
    if (++ccwaddr.devno > VIR_DOMAIN_DEVICE_CCW_MAX_DEVNO)
        return -1;

    *addr = ccwaddr;
    return 0;
}

static void
qemuDomainCCWAddressSetFreeEntry(void *payload,
                                 const void *name ATTRIBUTE_UNUSED)
{
    VIR_FREE(payload);
}

int qemuDomainCCWAddressAssign(virDomainDeviceInfoPtr dev,
                               qemuDomainCCWAddressSetPtr addrs,
                               bool autoassign)
{
    int ret = -1;
    char *addr = NULL;

    if (dev->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
        return 0;

    if (!autoassign && dev->addr.ccw.assigned) {
        if (!(addr = qemuCCWAddressAsString(&dev->addr.ccw)))
            goto cleanup;

        if (virHashLookup(addrs->defined, addr)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("The CCW devno '%s' is in use already "),
                           addr);
            goto cleanup;
        }
    } else if (autoassign && !dev->addr.ccw.assigned) {
        if (!(addr = qemuCCWAddressAsString(&addrs->next)) < 0)
            goto cleanup;

        while (virHashLookup(addrs->defined, addr)) {
            if (qemuCCWAdressIncrement(&addrs->next) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("There are no more free CCW devnos."));
                goto cleanup;
            }
            VIR_FREE(addr);
            addr = qemuCCWAddressAsString(&addrs->next);
        }
        dev->addr.ccw = addrs->next;
        dev->addr.ccw.assigned = true;
    } else {
        return 0;
    }

    if (virHashAddEntry(addrs->defined,addr,addr) < 0)
        goto cleanup;
    else
        addr = NULL; /* memory will be freed by hash table */

    ret = 0;

cleanup:
    VIR_FREE(addr);
    return ret;
}

static void
qemuDomainPrimeS390VirtioDevices(virDomainDefPtr def,
                                 enum virDomainDeviceAddressType type)
{
    /*
       declare address-less virtio devices to be of address type 'type'
       disks, networks, consoles, controllers, memballoon and rng in this
       order
    */
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (def->disks[i]->bus == VIR_DOMAIN_DISK_BUS_VIRTIO &&
            def->disks[i]->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            def->disks[i]->info.type = type;
    }

    for (i = 0; i < def->nnets; i++) {
        if (STREQ(def->nets[i]->model,"virtio") &&
            def->nets[i]->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
            def->nets[i]->info.type = type;
        }
    }

    for (i = 0; i < def->ncontrollers; i++) {
        if ((def->controllers[i]->type ==
             VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL ||
             def->controllers[i]->type ==
             VIR_DOMAIN_CONTROLLER_TYPE_SCSI) &&
            def->controllers[i]->info.type ==
            VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            def->controllers[i]->info.type = type;
    }

    if (def->memballoon &&
        def->memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO &&
        def->memballoon->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        def->memballoon->info.type = type;

    if (def->rng &&
        def->rng->model == VIR_DOMAIN_RNG_MODEL_VIRTIO &&
        def->rng->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
        def->rng->info.type = type;
}

static int
qemuDomainCCWAddressAllocate(virDomainDefPtr def ATTRIBUTE_UNUSED,
                             virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                             virDomainDeviceInfoPtr info,
                             void *data)
{
    return qemuDomainCCWAddressAssign(info, data, true);
}

static int
qemuDomainCCWAddressValidate(virDomainDefPtr def ATTRIBUTE_UNUSED,
                             virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                             virDomainDeviceInfoPtr info,
                             void *data)
{
    return qemuDomainCCWAddressAssign(info, data, false);
}

static int
qemuDomainCCWAddressReleaseAddr(qemuDomainCCWAddressSetPtr addrs,
                                virDomainDeviceInfoPtr dev)
{
    char *addr;
    int ret;

    addr = qemuCCWAddressAsString(&(dev->addr.ccw));
    if (!addr)
        return -1;

    if ((ret = virHashRemoveEntry(addrs->defined, addr)) == 0 &&
        dev->addr.ccw.cssid == addrs->next.cssid &&
        dev->addr.ccw.ssid == addrs->next.ssid &&
        dev->addr.ccw.devno < addrs->next.devno) {
        addrs->next.devno = dev->addr.ccw.devno;
        addrs->next.assigned = false;
    }

    VIR_FREE(addr);

    return ret;
}

void qemuDomainCCWAddressSetFree(qemuDomainCCWAddressSetPtr addrs)
{
    if (!addrs)
        return;

    virHashFree(addrs->defined);
    VIR_FREE(addrs);
}

static qemuDomainCCWAddressSetPtr
qemuDomainCCWAddressSetCreate(void)
{
     qemuDomainCCWAddressSetPtr addrs = NULL;

    if (VIR_ALLOC(addrs) < 0)
        goto cleanup;

    if (!(addrs->defined = virHashCreate(10, qemuDomainCCWAddressSetFreeEntry)))
        goto cleanup;

    /* must use cssid = 0xfe (254) for virtio-ccw devices */
    addrs->next.cssid = 254;
    addrs->next.ssid = 0;
    addrs->next.devno = 0;
    addrs->next.assigned = 0;
    return addrs;

cleanup:
    qemuDomainCCWAddressSetFree(addrs);
    return addrs;
}

/*
 * Three steps populating CCW devnos
 * 1. Allocate empty address set
 * 2. Gather addresses with explicit devno
 * 3. Assign defaults to the rest
 */
static int
qemuDomainAssignS390Addresses(virDomainDefPtr def,
                              virQEMUCapsPtr qemuCaps,
                              virDomainObjPtr obj)
{
    int ret = -1;
    qemuDomainCCWAddressSetPtr addrs = NULL;
    qemuDomainObjPrivatePtr priv = NULL;

    if (STREQLEN(def->os.machine, "s390-ccw", 8) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_CCW)) {
        qemuDomainPrimeS390VirtioDevices(
            def, VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW);

        if (!(addrs = qemuDomainCCWAddressSetCreate()))
            goto cleanup;

        if (virDomainDeviceInfoIterate(def, qemuDomainCCWAddressValidate,
                                       addrs) < 0)
            goto cleanup;

        if (virDomainDeviceInfoIterate(def, qemuDomainCCWAddressAllocate,
                                       addrs) < 0)
            goto cleanup;
    } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_S390)) {
        /* deal with legacy virtio-s390 */
        qemuDomainPrimeS390VirtioDevices(
            def, VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390);
    }

    if (obj && obj->privateData) {
        priv = obj->privateData;
        if (addrs) {
            /* if this is the live domain object, we persist the CCW addresses*/
            qemuDomainCCWAddressSetFree(priv->ccwaddrs);
            priv->persistentAddrs = 1;
            priv->ccwaddrs = addrs;
            addrs = NULL;
        } else {
            priv->persistentAddrs = 0;
        }
    }
    ret = 0;

cleanup:
    qemuDomainCCWAddressSetFree(addrs);

    return ret;
}

static int
qemuSpaprVIOFindByReg(virDomainDefPtr def ATTRIBUTE_UNUSED,
                      virDomainDeviceDefPtr device ATTRIBUTE_UNUSED,
                      virDomainDeviceInfoPtr info, void *opaque)
{
    virDomainDeviceInfoPtr target = opaque;

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO)
        return 0;

    /* Match a dev that has a reg, is not us, and has a matching reg */
    if (info->addr.spaprvio.has_reg && info != target &&
        info->addr.spaprvio.reg == target->addr.spaprvio.reg)
        /* Has to be < 0 so virDomainDeviceInfoIterate() will exit */
        return -1;

    return 0;
}

static int
qemuAssignSpaprVIOAddress(virDomainDefPtr def, virDomainDeviceInfoPtr info,
                          unsigned long long default_reg)
{
    bool user_reg;
    int ret;

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO)
        return 0;

    /* Check if the user has assigned the reg already, if so use it */
    user_reg = info->addr.spaprvio.has_reg;
    if (!user_reg) {
        info->addr.spaprvio.reg = default_reg;
        info->addr.spaprvio.has_reg = true;
    }

    ret = virDomainDeviceInfoIterate(def, qemuSpaprVIOFindByReg, info);
    while (ret != 0) {
        if (user_reg) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("spapr-vio address %#llx already in use"),
                           info->addr.spaprvio.reg);
            return -EEXIST;
        }

        /* We assigned the reg, so try a new value */
        info->addr.spaprvio.reg += 0x1000;
        ret = virDomainDeviceInfoIterate(def, qemuSpaprVIOFindByReg, info);
    }

    return 0;
}

int qemuDomainAssignSpaprVIOAddresses(virDomainDefPtr def,
                                      virQEMUCapsPtr qemuCaps)
{
    size_t i;
    int ret = -1;
    int model;

    /* Default values match QEMU. See spapr_(llan|vscsi|vty).c */

    for (i = 0; i < def->nnets; i++) {
        if (def->nets[i]->model &&
            STREQ(def->nets[i]->model, "spapr-vlan"))
            def->nets[i]->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO;
        if (qemuAssignSpaprVIOAddress(def, &def->nets[i]->info,
                                      VIO_ADDR_NET) < 0)
            goto cleanup;
    }

    for (i = 0; i < def->ncontrollers; i++) {
        model = def->controllers[i]->model;
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
            if (qemuSetScsiControllerModel(def, qemuCaps, &model) < 0)
                goto cleanup;
        }

        if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI &&
            def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI)
            def->controllers[i]->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO;
        if (qemuAssignSpaprVIOAddress(def, &def->controllers[i]->info,
                                      VIO_ADDR_SCSI) < 0)
            goto cleanup;
    }

    for (i = 0; i < def->nserials; i++) {
        if (def->serials[i]->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
            (def->os.arch == VIR_ARCH_PPC64) &&
            STREQ(def->os.machine, "pseries"))
            def->serials[i]->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO;
        if (qemuAssignSpaprVIOAddress(def, &def->serials[i]->info,
                                      VIO_ADDR_SERIAL) < 0)
            goto cleanup;
    }

    if (def->nvram) {
        if (def->os.arch == VIR_ARCH_PPC64 &&
            STREQ(def->os.machine, "pseries"))
            def->nvram->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO;
        if (qemuAssignSpaprVIOAddress(def, &def->nvram->info,
                                      VIO_ADDR_NVRAM) < 0)
            goto cleanup;
    }

    /* No other devices are currently supported on spapr-vio */

    ret = 0;

cleanup:
    return ret;
}

#define QEMU_PCI_ADDRESS_SLOT_LAST 31
#define QEMU_PCI_ADDRESS_FUNCTION_LAST 7

typedef struct {
    virDomainControllerModelPCI model;
    /* flags an min/max can be computed from model, but
     * having them ready makes life easier.
     */
    qemuDomainPCIConnectFlags flags;
    size_t minSlot, maxSlot; /* usually 0,0 or 1,31 */
    /* Each bit in a slot represents one function on that slot. If the
     * bit is set, that function is in use by a device.
     */
    uint8_t slots[QEMU_PCI_ADDRESS_SLOT_LAST + 1];
} qemuDomainPCIAddressBus;
typedef qemuDomainPCIAddressBus *qemuDomainPCIAddressBusPtr;

struct _qemuDomainPCIAddressSet {
    qemuDomainPCIAddressBus *buses;
    size_t nbuses;
    virDevicePCIAddress lastaddr;
    bool dryRun;          /* on a dry run, new buses are auto-added
                             and addresses aren't saved in device infos */
};


/*
 * Check that the PCI address is valid for use
 * with the specified PCI address set.
 */
static bool
qemuPCIAddressValidate(qemuDomainPCIAddressSetPtr addrs ATTRIBUTE_UNUSED,
                       virDevicePCIAddressPtr addr,
                       qemuDomainPCIConnectFlags flags)
{
    qemuDomainPCIAddressBusPtr bus;

    if (addrs->nbuses == 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s", _("No PCI buses available"));
        return false;
    }
    if (addr->domain != 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Only PCI domain 0 is available"));
        return false;
    }
    if (addr->bus >= addrs->nbuses) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Only PCI buses up to %zu are available"),
                       addrs->nbuses - 1);
        return false;
    }

    bus = &addrs->buses[addr->bus];

    /* assure that at least one of the requested connection types is
     * provided by this bus
     */
    if (!(flags & bus->flags & QEMU_PCI_CONNECT_TYPES_MASK)) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid PCI address: The PCI controller "
                         "providing bus %04x doesn't support "
                         "connections appropriate for the device "
                         "(%x required vs. %x provided by bus)"),
                       addr->bus, flags & QEMU_PCI_CONNECT_TYPES_MASK,
                       bus->flags & QEMU_PCI_CONNECT_TYPES_MASK);
        return false;
    }
    /* make sure this bus allows hot-plug if the caller demands it */
    if ((flags & QEMU_PCI_CONNECT_HOTPLUGGABLE) &&
        !(bus->flags &  QEMU_PCI_CONNECT_HOTPLUGGABLE)) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid PCI address: hot-pluggable slot requested, "
                         "but bus %04x doesn't support hot-plug"), addr->bus);
        return false;
    }
    /* some "buses" are really just a single port */
    if (bus->minSlot && addr->slot < bus->minSlot) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid PCI address: slot must be >= %zu"),
                       bus->minSlot);
        return false;
    }
    if (addr->slot > bus->maxSlot) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid PCI address: slot must be <= %zu"),
                       bus->maxSlot);
        return false;
    }
    if (addr->function > QEMU_PCI_ADDRESS_FUNCTION_LAST) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid PCI address: function must be <= %u"),
                       QEMU_PCI_ADDRESS_FUNCTION_LAST);
        return false;
    }
    return true;
}


static int
qemuDomainPCIAddressBusSetModel(qemuDomainPCIAddressBusPtr bus,
                                virDomainControllerModelPCI model)
{
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        bus->flags = (QEMU_PCI_CONNECT_HOTPLUGGABLE |
                      QEMU_PCI_CONNECT_TYPE_PCI);
        bus->minSlot = 1;
        bus->maxSlot = QEMU_PCI_ADDRESS_SLOT_LAST;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid PCI controller model %d"), model);
        return -1;
    }

    bus->model = model;
    return 0;
}


/* Ensure addr fits in the address set, by expanding it if needed.
 * This will only grow if the flags say that we need a normal
 * hot-pluggable PCI slot. If we need a different type of slot, it
 * will fail.
 *
 * Return value:
 * -1 = OOM
 *  0 = no action performed
 * >0 = number of buses added
 */
static int
qemuDomainPCIAddressSetGrow(qemuDomainPCIAddressSetPtr addrs,
                            virDevicePCIAddressPtr addr,
                            qemuDomainPCIConnectFlags flags)
{
    int add;
    size_t i;

    add = addr->bus - addrs->nbuses + 1;
    i = addrs->nbuses;
    if (add <= 0)
        return 0;

    /* auto-grow only works when we're adding plain PCI devices */
    if (!(flags & QEMU_PCI_CONNECT_TYPE_PCI)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot automatically add a new PCI bus for a "
                         "device requiring a slot other than standard PCI."));
        return -1;
    }

    if (VIR_EXPAND_N(addrs->buses, addrs->nbuses, add) < 0)
        return -1;

    for (; i < addrs->nbuses; i++) {
        /* Any time we auto-add a bus, we will want a multi-slot
         * bus. Currently the only type of bus we will auto-add is a
         * pci-bridge, which is hot-pluggable and provides standard
         * PCI slots.
         */
        qemuDomainPCIAddressBusSetModel(&addrs->buses[i],
                                        VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE);
    }
    return add;
}


static char *
qemuPCIAddressAsString(virDevicePCIAddressPtr addr)
{
    char *str;

    ignore_value(virAsprintf(&str, "%.4x:%.2x:%.2x.%.1x",
                             addr->domain,
                             addr->bus,
                             addr->slot,
                             addr->function));
    return str;
}


static int
qemuCollectPCIAddress(virDomainDefPtr def ATTRIBUTE_UNUSED,
                      virDomainDeviceDefPtr device,
                      virDomainDeviceInfoPtr info,
                      void *opaque)
{
    int ret = -1;
    char *str = NULL;
    virDevicePCIAddressPtr addr = &info->addr.pci;
    qemuDomainPCIAddressSetPtr addrs = opaque;
    /* FIXME: flags should be set according to the requirements of @device */
    qemuDomainPCIConnectFlags flags = (QEMU_PCI_CONNECT_HOTPLUGGABLE |
                                       QEMU_PCI_CONNECT_TYPE_PCI);

    if ((info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)
        || ((device->type == VIR_DOMAIN_DEVICE_HOSTDEV) &&
            (device->data.hostdev->parent.type != VIR_DOMAIN_DEVICE_NONE))) {
        /* If a hostdev has a parent, its info will be a part of the
         * parent, and will have its address collected during the scan
         * of the parent's device type.
        */
        return 0;
    }

    /* Ignore implicit controllers on slot 0:0:1.0:
     * implicit IDE controller on 0:0:1.1 (no qemu command line)
     * implicit USB controller on 0:0:1.2 (-usb)
     *
     * If the machine does have a PCI bus, they will get reserved
     * in qemuAssignDevicePCISlots().
     *
     * FIXME: When we have support for a pcie-root controller at bus
     * 0, we will no longer be able to skip checking of these devices,
     * as they are PCI, and thus can't be connected to bus 0 if it is
     * PCIe rather than PCI.
     */
    if (device->type == VIR_DOMAIN_DEVICE_CONTROLLER && addr->domain == 0 &&
        addr->bus == 0 && addr->slot == 1) {
        virDomainControllerDefPtr cont = device->data.controller;
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_IDE && cont->idx == 0 &&
            addr->function == 1)
            return 0;
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB && cont->idx == 0 &&
            (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI ||
             cont->model == -1) && addr->function == 2)
            return 0;
    }

    /* add an additional bus of the correct type if needed */
    if (addrs->dryRun &&
        qemuDomainPCIAddressSetGrow(addrs, addr, flags) < 0)
        return -1;

    /* verify that the address is in bounds for the chosen bus, and
     * that the bus is of the correct type for the device (via
     * comparing the flags).
     */
    if (!qemuPCIAddressValidate(addrs, addr, flags))
        return -1;

    if (!(str = qemuPCIAddressAsString(addr)))
        goto cleanup;

    /* check if already in use */
    if (addrs->buses[addr->bus].slots[addr->slot] & (1 << addr->function)) {
        if (info->addr.pci.function != 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Attempted double use of PCI Address '%s' "
                             "(may need \"multifunction='on'\" for device on function 0)"),
                           str);
        } else {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Attempted double use of PCI Address '%s'"), str);
        }
        goto cleanup;
    }

    /* mark as in use */
    if ((info->addr.pci.function == 0) &&
        (info->addr.pci.multi != VIR_DEVICE_ADDRESS_PCI_MULTI_ON)) {
        /* a function 0 w/o multifunction=on must reserve the entire slot */
        if (addrs->buses[addr->bus].slots[addr->slot]) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Attempted double use of PCI Address on slot '%s' "
                             "(need \"multifunction='off'\" for device "
                             "on function 0)"),
                           str);
            goto cleanup;
        }
        addrs->buses[addr->bus].slots[addr->slot] = 0xFF;
        VIR_DEBUG("Remembering PCI slot: %s (multifunction=off)", str);
    } else {
        VIR_DEBUG("Remembering PCI addr: %s", str);
        addrs->buses[addr->bus].slots[addr->slot] |= 1 << addr->function;
    }
    ret = 0;
cleanup:
    VIR_FREE(str);
    return ret;
}


int
qemuDomainAssignPCIAddresses(virDomainDefPtr def,
                             virQEMUCapsPtr qemuCaps,
                             virDomainObjPtr obj)
{
    int ret = -1;
    qemuDomainPCIAddressSetPtr addrs = NULL;
    qemuDomainObjPrivatePtr priv = NULL;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        int max_idx = -1;
        int nbuses = 0;
        size_t i;
        int rv;
        qemuDomainPCIConnectFlags flags = (QEMU_PCI_CONNECT_HOTPLUGGABLE |
                                           QEMU_PCI_CONNECT_TYPE_PCI);

        for (i = 0; i < def->ncontrollers; i++) {
            if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI) {
                if ((int) def->controllers[i]->idx > max_idx)
                    max_idx = def->controllers[i]->idx;
            }
        }

        nbuses = max_idx + 1;

        if (nbuses > 0 &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_PCI_BRIDGE)) {
            virDomainDeviceInfo info;

            /* 1st pass to figure out how many PCI bridges we need */
            if (!(addrs = qemuDomainPCIAddressSetCreate(def, nbuses, true)))
                goto cleanup;
            if (qemuAssignDevicePCISlots(def, qemuCaps, addrs) < 0)
                goto cleanup;
            /* Reserve 1 extra slot for a (potential) bridge */
            if (qemuDomainPCIAddressSetNextAddr(addrs, &info, flags) < 0)
                goto cleanup;

            for (i = 1; i < addrs->nbuses; i++) {
                qemuDomainPCIAddressBusPtr bus = &addrs->buses[i];

                if ((rv = virDomainDefMaybeAddController(
                         def, VIR_DOMAIN_CONTROLLER_TYPE_PCI,
                         i, bus->model)) < 0)
                    goto cleanup;
                /* If we added a new bridge, we will need one more address */
                if (rv > 0 && qemuDomainPCIAddressSetNextAddr(addrs, &info, flags) < 0)
                        goto cleanup;
            }
            nbuses = addrs->nbuses;
            qemuDomainPCIAddressSetFree(addrs);
            addrs = NULL;

        } else if (max_idx > 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("PCI bridges are not supported "
                             "by this QEMU binary"));
            goto cleanup;
        }

        if (!(addrs = qemuDomainPCIAddressSetCreate(def, nbuses, false)))
            goto cleanup;

        if (qemuAssignDevicePCISlots(def, qemuCaps, addrs) < 0)
            goto cleanup;
    }

    if (obj && obj->privateData) {
        priv = obj->privateData;
        if (addrs) {
            /* if this is the live domain object, we persist the PCI addresses*/
            qemuDomainPCIAddressSetFree(priv->pciaddrs);
            priv->persistentAddrs = 1;
            priv->pciaddrs = addrs;
            addrs = NULL;
        } else {
            priv->persistentAddrs = 0;
        }
    }

    ret = 0;

cleanup:
    qemuDomainPCIAddressSetFree(addrs);

    return ret;
}

int qemuDomainAssignAddresses(virDomainDefPtr def,
                              virQEMUCapsPtr qemuCaps,
                              virDomainObjPtr obj)
{
    int rc;

    rc = qemuDomainAssignSpaprVIOAddresses(def, qemuCaps);
    if (rc)
        return rc;

    rc = qemuDomainAssignS390Addresses(def, qemuCaps, obj);
    if (rc)
        return rc;

    return qemuDomainAssignPCIAddresses(def, qemuCaps, obj);
}


qemuDomainPCIAddressSetPtr
qemuDomainPCIAddressSetCreate(virDomainDefPtr def,
                              unsigned int nbuses,
                              bool dryRun)
{
    qemuDomainPCIAddressSetPtr addrs;
    size_t i;

    if (VIR_ALLOC(addrs) < 0)
        goto error;

    if (VIR_ALLOC_N(addrs->buses, nbuses) < 0)
        goto error;

    addrs->nbuses = nbuses;
    addrs->dryRun = dryRun;

    /* As a safety measure, set default model='pci-root' for first pci
     * controller and 'pci-bridge' for all subsequent. After setting
     * those defaults, then scan the config and set the actual model
     * for all addrs[idx]->bus that already have a corresponding
     * controller in the config.
     *
     */
    if (nbuses > 0)
       qemuDomainPCIAddressBusSetModel(&addrs->buses[0],
                                       VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT);
    for (i = 1; i < nbuses; i++) {
        qemuDomainPCIAddressBusSetModel(&addrs->buses[i],
                                        VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE);
    }

    for (i = 0; i < def->ncontrollers; i++) {
        size_t idx = def->controllers[i]->idx;

        if (def->controllers[i]->type != VIR_DOMAIN_CONTROLLER_TYPE_PCI)
            continue;

        if (idx >= addrs->nbuses) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Inappropriate new pci controller index %zu "
                             "not found in addrs"), idx);
            goto error;
        }

        if (qemuDomainPCIAddressBusSetModel(&addrs->buses[idx],
                                            def->controllers[i]->model) < 0)
            goto error;
        }

    if (virDomainDeviceInfoIterate(def, qemuCollectPCIAddress, addrs) < 0)
        goto error;

    return addrs;

error:
    qemuDomainPCIAddressSetFree(addrs);
    return NULL;
}

/*
 * Check if the PCI slot is used by another device.
 */
static bool qemuDomainPCIAddressSlotInUse(qemuDomainPCIAddressSetPtr addrs,
                                          virDevicePCIAddressPtr addr)
{
    return !!addrs->buses[addr->bus].slots[addr->slot];
}


int
qemuDomainPCIAddressReserveAddr(qemuDomainPCIAddressSetPtr addrs,
                                virDevicePCIAddressPtr addr,
                                qemuDomainPCIConnectFlags flags)
{
    char *str;
    qemuDomainPCIAddressBusPtr bus;

    if (addrs->dryRun && qemuDomainPCIAddressSetGrow(addrs, addr, flags) < 0)
        return -1;

    if (!(str = qemuPCIAddressAsString(addr)))
        return -1;

    VIR_DEBUG("Reserving PCI addr %s", str);

    bus = &addrs->buses[addr->bus];
    if ((bus->minSlot && addr->slot < bus->minSlot) ||
        addr->slot > bus->maxSlot) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to reserve PCI address %s. "
                         "Slot %02x can't be used on bus %04x, "
                         "only slots %02zx - %02zx are permitted on this bus."),
                       str, addr->slot, addr->bus, bus->minSlot, bus->maxSlot);
    }

    if (bus->slots[addr->slot] & (1 << addr->function)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to reserve PCI address %s already in use"), str);
        VIR_FREE(str);
        return -1;
    }

    VIR_FREE(str);

    addrs->lastaddr = *addr;
    addrs->lastaddr.function = 0;
    addrs->lastaddr.multi = 0;
    bus->slots[addr->slot] |= 1 << addr->function;
    return 0;
}


int
qemuDomainPCIAddressReserveSlot(qemuDomainPCIAddressSetPtr addrs,
                                virDevicePCIAddressPtr addr,
                                qemuDomainPCIConnectFlags flags)
{
    char *str;

    if (addrs->dryRun && qemuDomainPCIAddressSetGrow(addrs, addr, flags) < 0)
        return -1;

    if (!(str = qemuPCIAddressAsString(addr)))
        return -1;

    VIR_DEBUG("Reserving PCI slot %s", str);

    if (addrs->buses[addr->bus].slots[addr->slot]) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to reserve PCI slot %s"), str);
        VIR_FREE(str);
        return -1;
    }

    VIR_FREE(str);
    addrs->buses[addr->bus].slots[addr->slot] = 0xFF;
    return 0;
}

int qemuDomainPCIAddressEnsureAddr(qemuDomainPCIAddressSetPtr addrs,
                                   virDomainDeviceInfoPtr dev)
{
    int ret = 0;
    /* FIXME: flags should be set according to the particular device */
    qemuDomainPCIConnectFlags flags = (QEMU_PCI_CONNECT_HOTPLUGGABLE |
                                       QEMU_PCI_CONNECT_TYPE_PCI);

    if (dev->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        /* We do not support hotplug multi-function PCI device now, so we should
         * reserve the whole slot. The function of the PCI device must be 0.
         */
        if (dev->addr.pci.function != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only PCI device addresses with function=0"
                             " are supported"));
            return -1;
        }

        if (!qemuPCIAddressValidate(addrs, &dev->addr.pci, flags))
            return -1;

        ret = qemuDomainPCIAddressReserveSlot(addrs, &dev->addr.pci, flags);
    } else {
        ret = qemuDomainPCIAddressSetNextAddr(addrs, dev, flags);
    }
    return ret;
}


int qemuDomainPCIAddressReleaseAddr(qemuDomainPCIAddressSetPtr addrs,
                                    virDevicePCIAddressPtr addr)
{
    addrs->buses[addr->bus].slots[addr->slot] &= ~(1 << addr->function);
    return 0;
}

static int
qemuDomainPCIAddressReleaseSlot(qemuDomainPCIAddressSetPtr addrs,
                                virDevicePCIAddressPtr addr)
{
    /* permit any kind of connection type in validation, since we
     * already had it, and are giving it back.
     */
    qemuDomainPCIConnectFlags flags = QEMU_PCI_CONNECT_TYPES_MASK;

    if (!qemuPCIAddressValidate(addrs, addr, flags))
        return -1;

    addrs->buses[addr->bus].slots[addr->slot] = 0;
    return 0;
}

void qemuDomainPCIAddressSetFree(qemuDomainPCIAddressSetPtr addrs)
{
    if (!addrs)
        return;

    VIR_FREE(addrs->buses);
    VIR_FREE(addrs);
}


static int
qemuDomainPCIAddressGetNextSlot(qemuDomainPCIAddressSetPtr addrs,
                                virDevicePCIAddressPtr next_addr,
                                qemuDomainPCIConnectFlags flags)
{
    virDevicePCIAddress a = addrs->lastaddr;

    if (addrs->nbuses == 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s", _("No PCI buses available"));
        return -1;
    }

    /* Start the search at the last used bus and slot */
    for (a.slot++; a.bus < addrs->nbuses; a.bus++) {
        for (; a.slot <= QEMU_PCI_ADDRESS_SLOT_LAST; a.slot++) {
            if (!qemuDomainPCIAddressSlotInUse(addrs, &a))
                goto success;

            VIR_DEBUG("PCI slot %.4x:%.2x:%.2x already in use",
                      a.domain, a.bus, a.slot);
        }
        a.slot = 1;
    }

    /* There were no free slots after the last used one */
    if (addrs->dryRun) {
        /* a is already set to the first new bus and slot 1 */
        if (qemuDomainPCIAddressSetGrow(addrs, &a, flags) < 0)
            return -1;
        goto success;
    } else {
        /* Check the buses from 0 up to the last used one */
        for (a.bus = 0; a.bus <= addrs->lastaddr.bus; a.bus++) {
            for (a.slot = 1; a.slot <= QEMU_PCI_ADDRESS_SLOT_LAST; a.slot++) {
                if (!qemuDomainPCIAddressSlotInUse(addrs, &a))
                    goto success;

                VIR_DEBUG("PCI slot %.4x:%.2x:%.2x already in use",
                          a.domain, a.bus, a.slot);
            }

        }
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("No more available PCI addresses"));
    return -1;

success:
    VIR_DEBUG("Found free PCI slot %.4x:%.2x:%.2x",
              a.domain, a.bus, a.slot);
    *next_addr = a;
    return 0;
}

int
qemuDomainPCIAddressSetNextAddr(qemuDomainPCIAddressSetPtr addrs,
                                virDomainDeviceInfoPtr dev,
                                qemuDomainPCIConnectFlags flags)
{
    virDevicePCIAddress addr;
    if (qemuDomainPCIAddressGetNextSlot(addrs, &addr, flags) < 0)
        return -1;

    if (qemuDomainPCIAddressReserveSlot(addrs, &addr, flags) < 0)
        return -1;

    if (!addrs->dryRun) {
        dev->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        dev->addr.pci = addr;
    }

    addrs->lastaddr = addr;
    return 0;
}


void
qemuDomainReleaseDeviceAddress(virDomainObjPtr vm,
                               virDomainDeviceInfoPtr info,
                               const char *devstr)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!devstr)
        devstr = info->alias;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW &&
        STREQLEN(vm->def->os.machine, "s390-ccw", 8) &&
        virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_VIRTIO_CCW) &&
        qemuDomainCCWAddressReleaseAddr(priv->ccwaddrs, info) < 0)
        VIR_WARN("Unable to release CCW address on %s",
                 NULLSTR(devstr));
    else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI &&
             virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE) &&
             qemuDomainPCIAddressReleaseSlot(priv->pciaddrs,
                                             &info->addr.pci) < 0)
        VIR_WARN("Unable to release PCI address on %s",
                 NULLSTR(devstr));
}


#define IS_USB2_CONTROLLER(ctrl) \
    (((ctrl)->type == VIR_DOMAIN_CONTROLLER_TYPE_USB) && \
     ((ctrl)->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1 || \
      (ctrl)->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1 || \
      (ctrl)->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2 || \
      (ctrl)->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3))


static int
qemuValidateDevicePCISlotsPIIX3(virDomainDefPtr def,
                                virQEMUCapsPtr qemuCaps,
                                qemuDomainPCIAddressSetPtr addrs)
{
    size_t i;
    virDevicePCIAddress tmp_addr;
    bool qemuDeviceVideoUsable = virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VIDEO_PRIMARY);
    virDevicePCIAddressPtr addrptr;
    qemuDomainPCIConnectFlags flags = QEMU_PCI_CONNECT_HOTPLUGGABLE | QEMU_PCI_CONNECT_TYPE_PCI;

    /* Verify that first IDE and USB controllers (if any) is on the PIIX3, fn 1 */
    for (i = 0; i < def->ncontrollers; i++) {
        /* First IDE controller lives on the PIIX3 at slot=1, function=1 */
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_IDE &&
            def->controllers[i]->idx == 0) {
            if (def->controllers[i]->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
                if (def->controllers[i]->info.addr.pci.domain != 0 ||
                    def->controllers[i]->info.addr.pci.bus != 0 ||
                    def->controllers[i]->info.addr.pci.slot != 1 ||
                    def->controllers[i]->info.addr.pci.function != 1) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("Primary IDE controller must have PCI address 0:0:1.1"));
                    goto error;
                }
            } else {
                def->controllers[i]->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
                def->controllers[i]->info.addr.pci.domain = 0;
                def->controllers[i]->info.addr.pci.bus = 0;
                def->controllers[i]->info.addr.pci.slot = 1;
                def->controllers[i]->info.addr.pci.function = 1;
            }
        } else if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
                   def->controllers[i]->idx == 0 &&
                   (def->controllers[i]->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI ||
                    def->controllers[i]->model == -1)) {
            if (def->controllers[i]->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
                if (def->controllers[i]->info.addr.pci.domain != 0 ||
                    def->controllers[i]->info.addr.pci.bus != 0 ||
                    def->controllers[i]->info.addr.pci.slot != 1 ||
                    def->controllers[i]->info.addr.pci.function != 2) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("PIIX3 USB controller must have PCI address 0:0:1.2"));
                    goto error;
                }
            } else {
                def->controllers[i]->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
                def->controllers[i]->info.addr.pci.domain = 0;
                def->controllers[i]->info.addr.pci.bus = 0;
                def->controllers[i]->info.addr.pci.slot = 1;
                def->controllers[i]->info.addr.pci.function = 2;
            }
        }
    }

    /* PIIX3 (ISA bridge, IDE controller, something else unknown, USB controller)
     * hardcoded slot=1, multifunction device
     */
    if (addrs->nbuses) {
        memset(&tmp_addr, 0, sizeof(tmp_addr));
        tmp_addr.slot = 1;
        if (qemuDomainPCIAddressReserveSlot(addrs, &tmp_addr, flags) < 0)
            goto error;
    }

    if (def->nvideos > 0) {
        virDomainVideoDefPtr primaryVideo = def->videos[0];
        if (primaryVideo->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
            primaryVideo->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
            primaryVideo->info.addr.pci.domain = 0;
            primaryVideo->info.addr.pci.bus = 0;
            primaryVideo->info.addr.pci.slot = 2;
            primaryVideo->info.addr.pci.function = 0;
            addrptr = &primaryVideo->info.addr.pci;

            if (!qemuPCIAddressValidate(addrs, addrptr, flags))
                goto error;

            if (qemuDomainPCIAddressSlotInUse(addrs, addrptr)) {
                if (qemuDeviceVideoUsable) {
                    virResetLastError();
                    if (qemuDomainPCIAddressSetNextAddr(addrs, &primaryVideo->info, flags) < 0)
                        goto error;;
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("PCI address 0:0:2.0 is in use, "
                                     "QEMU needs it for primary video"));
                    goto error;
                }
            } else if (qemuDomainPCIAddressReserveSlot(addrs, addrptr, flags) < 0) {
                goto error;
            }
        } else if (!qemuDeviceVideoUsable) {
            if (primaryVideo->info.addr.pci.domain != 0 ||
                primaryVideo->info.addr.pci.bus != 0 ||
                primaryVideo->info.addr.pci.slot != 2 ||
                primaryVideo->info.addr.pci.function != 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Primary video card must have PCI address 0:0:2.0"));
                goto error;
            }
            /* If TYPE==PCI, then qemuCollectPCIAddress() function
             * has already reserved the address, so we must skip */
        }
    } else if (addrs->nbuses && !qemuDeviceVideoUsable) {
        memset(&tmp_addr, 0, sizeof(tmp_addr));
        tmp_addr.slot = 2;

        if (qemuDomainPCIAddressSlotInUse(addrs, &tmp_addr)) {
            VIR_DEBUG("PCI address 0:0:2.0 in use, future addition of a video"
                      " device will not be possible without manual"
                      " intervention");
            virResetLastError();
        } else if (qemuDomainPCIAddressReserveSlot(addrs, &tmp_addr, flags) < 0) {
            goto error;
        }
    }
    return 0;

error:
    return -1;
}


/*
 * This assigns static PCI slots to all configured devices.
 * The ordering here is chosen to match the ordering used
 * with old QEMU < 0.12, so that if a user updates a QEMU
 * host from old QEMU to QEMU >= 0.12, their guests should
 * get PCI addresses in the same order as before.
 *
 * NB, if they previously hotplugged devices then all bets
 * are off. Hotplug for old QEMU was unfixably broken wrt
 * to stable PCI addressing.
 *
 * Order is:
 *
 *  - Host bridge (slot 0)
 *  - PIIX3 ISA bridge, IDE controller, something else unknown, USB controller (slot 1)
 *  - Video (slot 2)
 *
 * Incrementally assign slots from 3 onwards:
 *
 *  - Net
 *  - Sound
 *  - SCSI controllers
 *  - VirtIO block
 *  - VirtIO balloon
 *  - Host device passthrough
 *  - Watchdog (not IB700)
 *
 * Prior to this function being invoked, qemuCollectPCIAddress() will have
 * added all existing PCI addresses from the 'def' to 'addrs'. Thus this
 * function must only try to reserve addresses if info.type == NONE and
 * skip over info.type == PCI
 */
int
qemuAssignDevicePCISlots(virDomainDefPtr def,
                         virQEMUCapsPtr qemuCaps,
                         qemuDomainPCIAddressSetPtr addrs)
{
    size_t i, j;
    qemuDomainPCIConnectFlags flags;
    virDevicePCIAddress tmp_addr;

    if ((STRPREFIX(def->os.machine, "pc-0.") ||
        STRPREFIX(def->os.machine, "pc-1.") ||
        STRPREFIX(def->os.machine, "pc-i440") ||
        STREQ(def->os.machine, "pc") ||
        STRPREFIX(def->os.machine, "rhel")) &&
        qemuValidateDevicePCISlotsPIIX3(def, qemuCaps, addrs) < 0) {
        goto error;
    }

    flags = QEMU_PCI_CONNECT_HOTPLUGGABLE | QEMU_PCI_CONNECT_TYPE_PCI;

    /* PCI controllers */
    for (i = 0; i < def->ncontrollers; i++) {
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI) {
            if (def->controllers[i]->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT)
                continue;
            if (def->controllers[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
                continue;
            if (qemuDomainPCIAddressSetNextAddr(addrs, &def->controllers[i]->info, flags) < 0)
                goto error;
        }
    }

    for (i = 0; i < def->nfss; i++) {
        if (def->fss[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            continue;

        /* Only support VirtIO-9p-pci so far. If that changes,
         * we might need to skip devices here */
        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->fss[i]->info, flags) < 0)
            goto error;
    }

    /* Network interfaces */
    for (i = 0; i < def->nnets; i++) {
        /* type='hostdev' network devices might be USB, and are also
         * in hostdevs list anyway, so handle them with other hostdevs
         * instead of here.
         */
        if ((def->nets[i]->type == VIR_DOMAIN_NET_TYPE_HOSTDEV) ||
            (def->nets[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)) {
            continue;
        }
        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->nets[i]->info, flags) < 0)
            goto error;
    }

    /* Sound cards */
    for (i = 0; i < def->nsounds; i++) {
        if (def->sounds[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            continue;
        /* Skip ISA sound card, and PCSPK */
        if (def->sounds[i]->model == VIR_DOMAIN_SOUND_MODEL_SB16 ||
            def->sounds[i]->model == VIR_DOMAIN_SOUND_MODEL_PCSPK)
            continue;

        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->sounds[i]->info, flags) < 0)
            goto error;
    }

    /* Device controllers (SCSI, USB, but not IDE, FDC or CCID) */
    for (i = 0; i < def->ncontrollers; i++) {
        /* PCI controllers have been dealt with earlier */
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI)
            continue;

        /* USB controller model 'none' doesn't need a PCI address */
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
            def->controllers[i]->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE)
            continue;

        /* FDC lives behind the ISA bridge; CCID is a usb device */
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_FDC ||
            def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_CCID)
            continue;

        /* First IDE controller lives on the PIIX3 at slot=1, function=1,
           dealt with earlier on*/
        if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_IDE &&
            def->controllers[i]->idx == 0)
            continue;

        if (def->controllers[i]->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO)
            continue;
        if (def->controllers[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            continue;

        /* USB2 needs special handling to put all companions in the same slot */
        if (IS_USB2_CONTROLLER(def->controllers[i])) {
            virDevicePCIAddress addr = { 0, 0, 0, 0, false };
            memset(&tmp_addr, 0, sizeof(tmp_addr));
            for (j = 0; j < i; j++) {
                if (IS_USB2_CONTROLLER(def->controllers[j]) &&
                    def->controllers[j]->idx == def->controllers[i]->idx) {
                    addr = def->controllers[j]->info.addr.pci;
                    break;
                }
            }

            switch (def->controllers[i]->model) {
            case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1:
                addr.function = 7;
                break;
            case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1:
                addr.function = 0;
                addr.multi = VIR_DEVICE_ADDRESS_PCI_MULTI_ON;
                break;
            case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2:
                addr.function = 1;
                break;
            case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3:
                addr.function = 2;
                break;
            }

            if (addr.slot == 0) {
                /* This is the first part of the controller, so need
                 * to find a free slot & then reserve a function */
                if (qemuDomainPCIAddressGetNextSlot(addrs, &tmp_addr, flags) < 0)
                    goto error;

                addr.bus = tmp_addr.bus;
                addr.slot = tmp_addr.slot;
            }
            /* Finally we can reserve the slot+function */
            if (qemuDomainPCIAddressReserveAddr(addrs, &addr, flags) < 0)
                goto error;

            def->controllers[i]->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
            def->controllers[i]->info.addr.pci = addr;
        } else {
            if (qemuDomainPCIAddressSetNextAddr(addrs, &def->controllers[i]->info, flags) < 0)
                goto error;
        }
    }

    /* Disks (VirtIO only for now) */
    for (i = 0; i < def->ndisks; i++) {
        /* Only VirtIO disks use PCI addrs */
        if (def->disks[i]->bus != VIR_DOMAIN_DISK_BUS_VIRTIO)
            continue;

        /* don't touch s390 devices */
        if (def->disks[i]->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI ||
            def->disks[i]->info.type ==
            VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390 ||
            def->disks[i]->info.type ==
            VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
            continue;

        if (def->disks[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio only support device address type 'PCI'"));
            goto error;
        }

        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->disks[i]->info, flags) < 0)
            goto error;
    }

    /* Host PCI devices */
    for (i = 0; i < def->nhostdevs; i++) {
        if (def->hostdevs[i]->info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            continue;
        if (def->hostdevs[i]->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS ||
            def->hostdevs[i]->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            continue;

        if (qemuDomainPCIAddressSetNextAddr(addrs, def->hostdevs[i]->info, flags) < 0)
            goto error;
    }

    /* VirtIO balloon */
    if (def->memballoon &&
        def->memballoon->model == VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO &&
        def->memballoon->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->memballoon->info, flags) < 0)
            goto error;
    }

    /* VirtIO RNG */
    if (def->rng &&
        def->rng->model == VIR_DOMAIN_RNG_MODEL_VIRTIO &&
        def->rng->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->rng->info, flags) < 0)
            goto error;
    }

    /* A watchdog - skip IB700, it is not a PCI device */
    if (def->watchdog &&
        def->watchdog->model != VIR_DOMAIN_WATCHDOG_MODEL_IB700 &&
        def->watchdog->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->watchdog->info, flags) < 0)
            goto error;
    }

    /* Further non-primary video cards which have to be qxl type */
    for (i = 1; i < def->nvideos; i++) {
        if (def->videos[i]->type != VIR_DOMAIN_VIDEO_TYPE_QXL) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("non-primary video device must be type of 'qxl'"));
            goto error;
        }
        if (def->videos[i]->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE)
            continue;
        if (qemuDomainPCIAddressSetNextAddr(addrs, &def->videos[i]->info, flags) < 0)
            goto error;
    }
    for (i = 0; i < def->ninputs; i++) {
        /* Nada - none are PCI based (yet) */
    }
    for (i = 0; i < def->nparallels; i++) {
        /* Nada - none are PCI based (yet) */
    }
    for (i = 0; i < def->nserials; i++) {
        /* Nada - none are PCI based (yet) */
    }
    for (i = 0; i < def->nchannels; i++) {
        /* Nada - none are PCI based (yet) */
    }
    for (i = 0; i < def->nhubs; i++) {
        /* Nada - none are PCI based (yet) */
    }

    return 0;

error:
    return -1;
}

static void
qemuUsbId(virBufferPtr buf, int idx)
{
    if (idx == 0)
        virBufferAddLit(buf, "usb");
    else
        virBufferAsprintf(buf, "usb%d", idx);
}

static int
qemuBuildDeviceAddressStr(virBufferPtr buf,
                          virDomainDeviceInfoPtr info,
                          virQEMUCapsPtr qemuCaps)
{
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_MULTIFUNCTION)) {
            if (info->addr.pci.function != 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Only PCI device addresses with function=0 "
                                 "are supported with this QEMU binary"));
                return -1;
            }
            if (info->addr.pci.multi == VIR_DEVICE_ADDRESS_PCI_MULTI_ON) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("'multifunction=on' is not supported with "
                                 "this QEMU binary"));
                return -1;
            }
        }

        /*
         * PCI bridge support is required for multiple buses
         * 'pci.%u' is the ID of the bridge as specified in
         * qemuBuildControllerDevStr
         *
         * PCI_MULTIBUS capability indicates that the implicit
         * PCI bus is named 'pci.0' instead of 'pci'.
         */
        if (info->addr.pci.bus != 0) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_PCI_BRIDGE)) {
                virBufferAsprintf(buf, ",bus=pci.%u", info->addr.pci.bus);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Multiple PCI buses are not supported "
                                 "with this QEMU binary"));
                return -1;
            }
        } else {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_MULTIBUS))
                virBufferAddLit(buf, ",bus=pci.0");
            else
                virBufferAddLit(buf, ",bus=pci");
        }
        if (info->addr.pci.multi == VIR_DEVICE_ADDRESS_PCI_MULTI_ON)
            virBufferAddLit(buf, ",multifunction=on");
        else if (info->addr.pci.multi == VIR_DEVICE_ADDRESS_PCI_MULTI_OFF)
            virBufferAddLit(buf, ",multifunction=off");
        virBufferAsprintf(buf, ",addr=0x%x", info->addr.pci.slot);
        if (info->addr.pci.function != 0)
           virBufferAsprintf(buf, ".0x%x", info->addr.pci.function);
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        virBufferAddLit(buf, ",bus=");
        qemuUsbId(buf, info->addr.usb.bus);
        virBufferAsprintf(buf, ".0,port=%s", info->addr.usb.port);
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO) {
        if (info->addr.spaprvio.has_reg)
            virBufferAsprintf(buf, ",reg=0x%llx", info->addr.spaprvio.reg);
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
        if (info->addr.ccw.assigned)
            virBufferAsprintf(buf, ",devno=%x.%x.%04x",
                              info->addr.ccw.cssid,
                              info->addr.ccw.ssid,
                              info->addr.ccw.devno);
    }

    return 0;
}

static int
qemuBuildRomStr(virBufferPtr buf,
                virDomainDeviceInfoPtr info,
                virQEMUCapsPtr qemuCaps)
{
    if (info->rombar || info->romfile) {
        if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("rombar and romfile are supported only for PCI devices"));
            return -1;
        }
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_ROMBAR)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("rombar and romfile not supported in this QEMU binary"));
            return -1;
        }

        switch (info->rombar) {
        case VIR_DOMAIN_PCI_ROMBAR_OFF:
            virBufferAddLit(buf, ",rombar=0");
            break;
        case VIR_DOMAIN_PCI_ROMBAR_ON:
            virBufferAddLit(buf, ",rombar=1");
            break;
        default:
            break;
        }
        if (info->romfile)
           virBufferAsprintf(buf, ",romfile=%s", info->romfile);
    }
    return 0;
}

static int
qemuBuildIoEventFdStr(virBufferPtr buf,
                      enum virDomainIoEventFd use,
                      virQEMUCapsPtr qemuCaps)
{
    if (use && virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_IOEVENTFD))
        virBufferAsprintf(buf, ",ioeventfd=%s",
                          virDomainIoEventFdTypeToString(use));
    return 0;
}

#define QEMU_SERIAL_PARAM_ACCEPTED_CHARS \
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"

static int
qemuSafeSerialParamValue(const char *value)
{
    if (strspn(value, QEMU_SERIAL_PARAM_ACCEPTED_CHARS) != strlen(value)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("driver serial '%s' contains unsafe characters"),
                       value);
        return -1;
    }

    return 0;
}

static char *
qemuGetSecretString(virConnectPtr conn,
                    const char *scheme,
                    bool encoded,
                    int diskSecretType,
                    char *username,
                    unsigned char *uuid, char *usage,
                    virSecretUsageType secretUsageType)
{
    size_t secret_size;
    virSecretPtr sec = NULL;
    char *secret = NULL;

    /* look up secret */
    switch (diskSecretType) {
    case VIR_DOMAIN_DISK_SECRET_TYPE_UUID:
        sec = virSecretLookupByUUID(conn, uuid);
        break;
    case VIR_DOMAIN_DISK_SECRET_TYPE_USAGE:
        sec = virSecretLookupByUsage(conn, secretUsageType, usage);
        break;
    }

    if (!sec) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s username '%s' specified but secret not found"),
                       scheme, username);
        goto cleanup;
    }

    secret = (char *)conn->secretDriver->secretGetValue(sec, &secret_size, 0,
                                                        VIR_SECRET_GET_VALUE_INTERNAL_CALL);
    if (!secret) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not get value of the secret for username %s"),
                       username);
        goto cleanup;
    }

    if (encoded) {
        char *base64 = NULL;

        base64_encode_alloc(secret, secret_size, &base64);
        VIR_FREE(secret);
        if (!base64) {
            virReportOOMError();
            goto cleanup;
        }
        secret = base64;
    }

cleanup:
    virObjectUnref(sec);
    return secret;
}

static int
qemuBuildRBDString(virConnectPtr conn,
                   virDomainDiskDefPtr disk,
                   virBufferPtr opt)
{
    size_t i;
    int ret = 0;
    char *secret = NULL;

    if (strchr(disk->src, ':')) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("':' not allowed in RBD source volume name '%s'"),
                       disk->src);
        return -1;
    }

    virBufferEscape(opt, ',', ",", "rbd:%s", disk->src);
    if (disk->auth.username) {
        virBufferEscape(opt, '\\', ":", ":id=%s", disk->auth.username);
        /* Get the secret string using the virDomainDiskDef
         * NOTE: qemu/librbd wants it base64 encoded
         */
        if (!(secret = qemuGetSecretString(conn, "rbd", true,
                                           disk->auth.secretType,
                                           disk->auth.username,
                                           disk->auth.secret.uuid,
                                           disk->auth.secret.usage,
                                           VIR_SECRET_USAGE_TYPE_CEPH)))
            goto error;


        virBufferEscape(opt, '\\', ":",
                        ":key=%s:auth_supported=cephx\\;none",
                        secret);
    } else {
        virBufferAddLit(opt, ":auth_supported=none");
    }

    if (disk->nhosts > 0) {
        virBufferAddLit(opt, ":mon_host=");
        for (i = 0; i < disk->nhosts; ++i) {
            if (i) {
                virBufferAddLit(opt, "\\;");
            }

            /* assume host containing : is ipv6 */
            if (strchr(disk->hosts[i].name, ':')) {
                virBufferEscape(opt, '\\', ":", "[%s]", disk->hosts[i].name);
            } else {
                virBufferAsprintf(opt, "%s", disk->hosts[i].name);
            }
            if (disk->hosts[i].port) {
                virBufferAsprintf(opt, "\\:%s", disk->hosts[i].port);
            }
        }
    }

cleanup:
    VIR_FREE(secret);

    return ret;

error:
    ret = -1;
    goto cleanup;
}

static int qemuAddRBDHost(virDomainDiskDefPtr disk, char *hostport)
{
    char *port;
    size_t skip;
    char **parts;

    disk->nhosts++;
    if (VIR_REALLOC_N(disk->hosts, disk->nhosts) < 0)
        goto error;

    if ((port = strchr(hostport, ']'))) {
        /* ipv6, strip brackets */
        hostport += 1;
        skip = 3;
    } else {
        port = strstr(hostport, "\\:");
        skip = 2;
    }

    if (port) {
        *port = '\0';
        port += skip;
        if (VIR_STRDUP(disk->hosts[disk->nhosts - 1].port, port) < 0)
            goto error;
    } else {
        if (VIR_STRDUP(disk->hosts[disk->nhosts - 1].port, "6789") < 0)
            goto error;
    }

    parts = virStringSplit(hostport, "\\:", 0);
    if (!parts)
        goto error;
    disk->hosts[disk->nhosts-1].name = virStringJoin((const char **)parts, ":");
    virStringFreeList(parts);
    if (!disk->hosts[disk->nhosts-1].name)
        goto error;

    disk->hosts[disk->nhosts-1].transport = VIR_DOMAIN_DISK_PROTO_TRANS_TCP;
    disk->hosts[disk->nhosts-1].socket = NULL;

    return 0;

error:
    VIR_FREE(disk->hosts[disk->nhosts-1].port);
    VIR_FREE(disk->hosts[disk->nhosts-1].name);
    return -1;
}

/* disk->src initially has everything after the rbd: prefix */
static int qemuParseRBDString(virDomainDiskDefPtr disk)
{
    char *options = NULL;
    char *p, *e, *next;

    p = strchr(disk->src, ':');
    if (p) {
        if (VIR_STRDUP(options, p + 1) < 0)
            goto error;
        *p = '\0';
    }

    /* options */
    if (!options)
        return 0; /* all done */

    p = options;
    while (*p) {
        /* find : delimiter or end of string */
        for (e = p; *e && *e != ':'; ++e) {
            if (*e == '\\') {
                e++;
                if (*e == '\0')
                    break;
            }
        }
        if (*e == '\0') {
            next = e;    /* last kv pair */
        } else {
            next = e + 1;
            *e = '\0';
        }

        if (STRPREFIX(p, "id=") &&
            VIR_STRDUP(disk->auth.username, p + strlen("id=")) < 0)
            goto error;
        if (STRPREFIX(p, "mon_host=")) {
            char *h, *sep;

            h = p + strlen("mon_host=");
            while (h < e) {
                for (sep = h; sep < e; ++sep) {
                    if (*sep == '\\' && (sep[1] == ',' ||
                                         sep[1] == ';' ||
                                         sep[1] == ' ')) {
                        *sep = '\0';
                        sep += 2;
                        break;
                    }
                }
                if (qemuAddRBDHost(disk, h) < 0) {
                    return -1;
                }
                h = sep;
            }
        }

        p = next;
    }
    VIR_FREE(options);
    return 0;

error:
    VIR_FREE(options);
    return -1;
}

static int
qemuParseDriveURIString(virDomainDiskDefPtr def, virURIPtr uri,
                        const char *scheme)
{
    int ret = -1;
    char *transp = NULL;
    char *sock = NULL;
    char *volimg = NULL;
    char *secret = NULL;

    if (VIR_ALLOC(def->hosts) < 0)
        goto error;

    transp = strchr(uri->scheme, '+');
    if (transp)
        *transp++ = 0;

    if (!STREQ(uri->scheme, scheme)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid transport/scheme '%s'"), uri->scheme);
        goto error;
    }

    if (!transp) {
        def->hosts->transport = VIR_DOMAIN_DISK_PROTO_TRANS_TCP;
    } else {
        def->hosts->transport = virDomainDiskProtocolTransportTypeFromString(transp);
        if (def->hosts->transport < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid %s transport type '%s'"), scheme, transp);
            goto error;
        }
    }
    def->nhosts = 0; /* set to 1 once everything succeeds */

    if (def->hosts->transport != VIR_DOMAIN_DISK_PROTO_TRANS_UNIX) {
        if (VIR_STRDUP(def->hosts->name, uri->server) < 0)
            goto error;

        if (virAsprintf(&def->hosts->port, "%d", uri->port) < 0)
            goto error;
    } else {
        def->hosts->name = NULL;
        def->hosts->port = 0;
        if (uri->query) {
            if (STRPREFIX(uri->query, "socket=")) {
                sock = strchr(uri->query, '=') + 1;
                if (VIR_STRDUP(def->hosts->socket, sock) < 0)
                    goto error;
            } else {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Invalid query parameter '%s'"), uri->query);
                goto error;
            }
        }
    }
    if (uri->path) {
        volimg = uri->path + 1; /* skip the prefix slash */
        VIR_FREE(def->src);
        if (VIR_STRDUP(def->src, volimg) < 0)
            goto error;
    } else {
        VIR_FREE(def->src);
        def->src = NULL;
    }

    if (uri->user) {
        secret = strchr(uri->user, ':');
        if (secret)
            *secret = '\0';

        if (VIR_STRDUP(def->auth.username, uri->user) < 0)
            goto error;
    }

    def->nhosts = 1;
    ret = 0;

cleanup:
    virURIFree(uri);

    return ret;

error:
    virDomainDiskHostDefFree(def->hosts);
    VIR_FREE(def->hosts);
    goto cleanup;
}

static int
qemuParseGlusterString(virDomainDiskDefPtr def)
{
    virURIPtr uri = NULL;

    if (!(uri = virURIParse(def->src)))
        return -1;

    return qemuParseDriveURIString(def, uri, "gluster");
}

static int
qemuParseISCSIString(virDomainDiskDefPtr def)
{
    virURIPtr uri = NULL;
    char *slash;
    unsigned lun;

    if (!(uri = virURIParse(def->src)))
        return -1;

    if (uri->path &&
        (slash = strchr(uri->path + 1, '/')) != NULL) {

        if (slash[1] == '\0')
            *slash = '\0';
        else if (virStrToLong_ui(slash + 1, NULL, 10, &lun) == -1) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid name '%s' for iSCSI disk"), def->src);
            return -1;
        }
    }

    return qemuParseDriveURIString(def, uri, "iscsi");
}

static int
qemuParseNBDString(virDomainDiskDefPtr disk)
{
    virDomainDiskHostDefPtr h = NULL;
    char *host, *port;
    char *src;

    virURIPtr uri = NULL;

    if (strstr(disk->src, "://")) {
        uri = virURIParse(disk->src);
        if (uri)
            return qemuParseDriveURIString(disk, uri, "nbd");
    }

    if (VIR_ALLOC(h) < 0)
        goto error;

    host = disk->src + strlen("nbd:");
    if (STRPREFIX(host, "unix:/")) {
        src = strchr(host + strlen("unix:"), ':');
        if (src)
            *src++ = '\0';

        h->transport = VIR_DOMAIN_DISK_PROTO_TRANS_UNIX;
        if (VIR_STRDUP(h->socket, host + strlen("unix:")) < 0)
            goto error;
    } else {
        port = strchr(host, ':');
        if (!port) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot parse nbd filename '%s'"), disk->src);
            goto error;
        }

        *port++ = '\0';
        if (VIR_STRDUP(h->name, host) < 0)
            goto error;

        src = strchr(port, ':');
        if (src)
            *src++ = '\0';

        if (VIR_STRDUP(h->port, port) < 0)
            goto error;
    }

    if (src && STRPREFIX(src, "exportname=")) {
        if (VIR_STRDUP(src, strchr(src, '=') + 1) < 0)
            goto error;
    } else {
        src = NULL;
    }

    VIR_FREE(disk->src);
    disk->src = src;
    disk->nhosts = 1;
    disk->hosts = h;
    return 0;

error:
    virDomainDiskHostDefFree(h);
    VIR_FREE(h);
    return -1;
}

static int
qemuBuildDriveURIString(virConnectPtr conn,
                        virDomainDiskDefPtr disk, virBufferPtr opt,
                        const char *scheme, virSecretUsageType secretUsageType)
{
    int ret = -1;
    int port = 0;
    char *secret = NULL;

    char *tmpscheme = NULL;
    char *volimg = NULL;
    char *sock = NULL;
    char *user = NULL;
    char *builturi = NULL;
    const char *transp = NULL;
    virURI uri = {
        .port = port /* just to clear rest of bits */
    };

    if (disk->nhosts != 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s accepts only one host"), scheme);
        return -1;
    }

    virBufferAddLit(opt, "file=");
    transp = virDomainDiskProtocolTransportTypeToString(disk->hosts->transport);

    if (disk->hosts->transport == VIR_DOMAIN_DISK_PROTO_TRANS_TCP) {
        if (VIR_STRDUP(tmpscheme, scheme) < 0)
            goto cleanup;
    } else {
        if (virAsprintf(&tmpscheme, "%s+%s", scheme, transp) < 0)
            goto cleanup;
    }

    if (disk->src && virAsprintf(&volimg, "/%s", disk->src) < 0)
        goto cleanup;

    if (disk->hosts->port) {
        port = atoi(disk->hosts->port);
    }

    if (disk->hosts->socket &&
        virAsprintf(&sock, "socket=%s", disk->hosts->socket) < 0)
        goto cleanup;

    if (disk->auth.username && secretUsageType != VIR_SECRET_USAGE_TYPE_NONE) {
        /* Get the secret string using the virDomainDiskDef */
        if (!(secret = qemuGetSecretString(conn, scheme, false,
                                           disk->auth.secretType,
                                           disk->auth.username,
                                           disk->auth.secret.uuid,
                                           disk->auth.secret.usage,
                                           secretUsageType)))
            goto cleanup;
        if (virAsprintf(&user, "%s:%s", disk->auth.username, secret) < 0)
            goto cleanup;
    }

    uri.scheme = tmpscheme; /* gluster+<transport> */
    uri.server = disk->hosts->name;
    uri.user = user;
    uri.port = port;
    uri.path = volimg;
    uri.query = sock;

    builturi = virURIFormat(&uri);
    virBufferEscape(opt, ',', ",", "%s", builturi);

    ret = 0;

cleanup:
    VIR_FREE(builturi);
    VIR_FREE(tmpscheme);
    VIR_FREE(volimg);
    VIR_FREE(sock);
    VIR_FREE(secret);
    VIR_FREE(user);

    return ret;
}

static int
qemuBuildGlusterString(virConnectPtr conn, virDomainDiskDefPtr disk, virBufferPtr opt)
{
    return qemuBuildDriveURIString(conn, disk, opt, "gluster",
                                   VIR_SECRET_USAGE_TYPE_NONE);
}

#define QEMU_DEFAULT_NBD_PORT "10809"

static int
qemuBuildISCSIString(virConnectPtr conn, virDomainDiskDefPtr disk, virBufferPtr opt)
{
    return qemuBuildDriveURIString(conn, disk, opt, "iscsi",
                                   VIR_SECRET_USAGE_TYPE_ISCSI);
}

static int
qemuBuildNBDString(virConnectPtr conn, virDomainDiskDefPtr disk, virBufferPtr opt)
{
    const char *transp;

    if (disk->nhosts != 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("nbd accepts only one host"));
        return -1;
    }

    if ((disk->hosts->name && strchr(disk->hosts->name, ':'))
        || (disk->hosts->transport == VIR_DOMAIN_DISK_PROTO_TRANS_TCP
            && !disk->hosts->name)
        || (disk->hosts->transport == VIR_DOMAIN_DISK_PROTO_TRANS_UNIX
            && disk->hosts->socket && disk->hosts->socket[0] != '/'))
        return qemuBuildDriveURIString(conn, disk, opt, "nbd",
                                       VIR_SECRET_USAGE_TYPE_NONE);

    virBufferAddLit(opt, "file=nbd:");

    switch (disk->hosts->transport) {
    case VIR_DOMAIN_DISK_PROTO_TRANS_TCP:
        if (disk->hosts->name)
            virBufferEscape(opt, ',', ",", "%s", disk->hosts->name);
        virBufferEscape(opt, ',', ",", ":%s",
                        disk->hosts->port ? disk->hosts->port :
                        QEMU_DEFAULT_NBD_PORT);
        break;
    case VIR_DOMAIN_DISK_PROTO_TRANS_UNIX:
        if (!disk->hosts->socket) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("socket attribute required for unix transport"));
            return -1;
        }
        virBufferEscape(opt, ',', ",", "unix:%s", disk->hosts->socket);
        break;
    default:
        transp = virDomainDiskProtocolTransportTypeToString(disk->hosts->transport);
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("nbd does not support transport '%s'"), transp);
        break;
    }

    if (disk->src)
        virBufferEscape(opt, ',', ",", ":exportname=%s", disk->src);

    return 0;
}

static int
qemuBuildVolumeString(virConnectPtr conn,
                      virDomainDiskDefPtr disk,
                      virBufferPtr opt)
{
    int ret = -1;

    switch (disk->srcpool->voltype) {
    case VIR_STORAGE_VOL_DIR:
        if (!disk->readonly) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot create virtual FAT disks in read-write mode"));
            goto cleanup;
        }
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
            virBufferEscape(opt, ',', ",", "file=fat:floppy:%s,", disk->src);
        else
            virBufferEscape(opt, ',', ",", "file=fat:%s,", disk->src);
        break;
    case VIR_STORAGE_VOL_BLOCK:
        if (disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("tray status 'open' is invalid for "
                             "block type volume"));
            goto cleanup;
        }
        if (disk->srcpool->pooltype == VIR_STORAGE_POOL_ISCSI) {
            if (disk->srcpool->mode ==
                VIR_DOMAIN_DISK_SOURCE_POOL_MODE_DIRECT) {
                if (qemuBuildISCSIString(conn, disk, opt) < 0)
                    goto cleanup;
                virBufferAddChar(opt, ',');
            } else if (disk->srcpool->mode ==
                       VIR_DOMAIN_DISK_SOURCE_POOL_MODE_HOST) {
                virBufferEscape(opt, ',', ",", "file=%s,", disk->src);
            }
        } else {
            virBufferEscape(opt, ',', ",", "file=%s,", disk->src);
        }
        break;
    case VIR_STORAGE_VOL_FILE:
        if (disk->auth.username) {
            if (qemuBuildISCSIString(conn, disk, opt) < 0)
                goto cleanup;
            virBufferAddChar(opt, ',');
        } else {
            virBufferEscape(opt, ',', ",", "file=%s,", disk->src);
        }
        break;
    case VIR_STORAGE_VOL_NETWORK:
        /* Keep the compiler quite, qemuTranslateDiskSourcePool already
         * reported the unsupported error.
         */
        break;
    }

    ret = 0;

cleanup:
    return ret;
}

char *
qemuBuildDriveStr(virConnectPtr conn ATTRIBUTE_UNUSED,
                  virDomainDiskDefPtr disk,
                  bool bootable,
                  virQEMUCapsPtr qemuCaps)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *bus = virDomainDiskQEMUBusTypeToString(disk->bus);
    const char *trans =
        virDomainDiskGeometryTransTypeToString(disk->geometry.trans);
    int idx = virDiskNameToIndex(disk->dst);
    int busid = -1, unitid = -1;

    if (idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported disk type '%s'"), disk->dst);
        goto error;
    }

    switch (disk->bus) {
    case VIR_DOMAIN_DISK_BUS_SCSI:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for scsi disk"));
            goto error;
        }

        /* Setting bus= attr for SCSI drives, causes a controller
         * to be created. Yes this is slightly odd. It is not possible
         * to have > 1 bus on a SCSI controller (yet). */
        if (disk->info.addr.drive.bus != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("SCSI controller only supports 1 bus"));
            goto error;
        }
        busid = disk->info.addr.drive.controller;
        unitid = disk->info.addr.drive.unit;
        break;

    case VIR_DOMAIN_DISK_BUS_IDE:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for ide disk"));
            goto error;
        }
        /* We can only have 1 IDE controller (currently) */
        if (disk->info.addr.drive.controller != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Only 1 %s controller is supported"), bus);
            goto error;
        }
        busid = disk->info.addr.drive.bus;
        unitid = disk->info.addr.drive.unit;
        break;

    case VIR_DOMAIN_DISK_BUS_FDC:
        if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected address type for fdc disk"));
            goto error;
        }
        /* We can only have 1 FDC controller (currently) */
        if (disk->info.addr.drive.controller != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Only 1 %s controller is supported"), bus);
            goto error;
        }
        /* We can only have 1 FDC bus (currently) */
        if (disk->info.addr.drive.bus != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Only 1 %s bus is supported"), bus);
            goto error;
        }
        if (disk->info.addr.drive.target != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for controller fdc"));
            goto error;
        }
        unitid = disk->info.addr.drive.unit;

        break;

    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        idx = -1;
        break;

    case VIR_DOMAIN_DISK_BUS_XEN:
        /* Xen has no address type currently, so assign based on index */
        break;
    }

    /* disk->src is NULL when we use nbd disks */
    if ((disk->src ||
        (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK &&
         disk->protocol == VIR_DOMAIN_DISK_PROTOCOL_NBD)) &&
        !((disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY ||
           disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM) &&
          disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN)) {
        if (disk->type == VIR_DOMAIN_DISK_TYPE_DIR) {
            /* QEMU only supports magic FAT format for now */
            if (disk->format > 0 && disk->format != VIR_STORAGE_FILE_FAT) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unsupported disk driver type for '%s'"),
                               virStorageFileFormatTypeToString(disk->format));
                goto error;
            }
            if (!disk->readonly) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("cannot create virtual FAT disks in read-write mode"));
                goto error;
            }
            if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
                virBufferEscape(&opt, ',', ",", "file=fat:floppy:%s,",
                                disk->src);
            else
                virBufferEscape(&opt, ',', ",", "file=fat:%s,", disk->src);
        } else if (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK) {
            switch (disk->protocol) {
            case VIR_DOMAIN_DISK_PROTOCOL_NBD:
                if (qemuBuildNBDString(conn, disk, &opt) < 0)
                    goto error;
                virBufferAddChar(&opt, ',');
                break;
            case VIR_DOMAIN_DISK_PROTOCOL_RBD:
                virBufferAddLit(&opt, "file=");
                if (qemuBuildRBDString(conn, disk, &opt) < 0)
                    goto error;
                virBufferAddChar(&opt, ',');
                break;
            case VIR_DOMAIN_DISK_PROTOCOL_GLUSTER:
                if (qemuBuildGlusterString(conn, disk, &opt) < 0)
                    goto error;
                virBufferAddChar(&opt, ',');
                break;
            case VIR_DOMAIN_DISK_PROTOCOL_ISCSI:
                if (qemuBuildISCSIString(conn, disk, &opt) < 0)
                    goto error;
                virBufferAddChar(&opt, ',');
                break;

            case VIR_DOMAIN_DISK_PROTOCOL_SHEEPDOG:
                if (disk->nhosts == 0) {
                    virBufferEscape(&opt, ',', ",", "file=sheepdog:%s,",
                                    disk->src);
                } else {
                    /* only one host is supported now */
                    virBufferAsprintf(&opt, "file=sheepdog:%s:%s:",
                                      disk->hosts->name,
                                      disk->hosts->port ? disk->hosts->port : "7000");
                    virBufferEscape(&opt, ',', ",", "%s,", disk->src);
                }
                break;
            }
        } else if (disk->type == VIR_DOMAIN_DISK_TYPE_VOLUME) {
            if (qemuBuildVolumeString(conn, disk, &opt) < 0)
                goto error;
        } else {
            if ((disk->type == VIR_DOMAIN_DISK_TYPE_BLOCK) &&
                (disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("tray status 'open' is invalid for "
                                 "block type disk"));
                goto error;
            }
            virBufferEscape(&opt, ',', ",", "file=%s,", disk->src);
        }
    }
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
        virBufferAddLit(&opt, "if=none");
    else
        virBufferAsprintf(&opt, "if=%s", bus);

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM) {
        if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_CD))
                virBufferAddLit(&opt, ",media=cdrom");
        } else if (disk->bus == VIR_DOMAIN_DISK_BUS_IDE) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_IDE_CD))
                virBufferAddLit(&opt, ",media=cdrom");
        } else {
            virBufferAddLit(&opt, ",media=cdrom");
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        virBufferAsprintf(&opt, ",id=%s%s", QEMU_DRIVE_HOST_PREFIX, disk->info.alias);
    } else {
        if (busid == -1 && unitid == -1) {
            if (idx != -1)
                virBufferAsprintf(&opt, ",index=%d", idx);
        } else {
            if (busid != -1)
                virBufferAsprintf(&opt, ",bus=%d", busid);
            if (unitid != -1)
                virBufferAsprintf(&opt, ",unit=%d", unitid);
        }
    }
    if (bootable &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_BOOT) &&
        (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK ||
         disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) &&
        disk->bus != VIR_DOMAIN_DISK_BUS_IDE)
        virBufferAddLit(&opt, ",boot=on");
    if (disk->readonly &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_READONLY))
        virBufferAddLit(&opt, ",readonly=on");
    if (disk->transient) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("transient disks not supported yet"));
        goto error;
    }
    if (disk->format > 0 &&
        disk->type != VIR_DOMAIN_DISK_TYPE_DIR &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_FORMAT))
        virBufferAsprintf(&opt, ",format=%s",
                          virStorageFileFormatTypeToString(disk->format));

    /* generate geometry command string */
    if (disk->geometry.cylinders > 0 &&
        disk->geometry.heads > 0 &&
        disk->geometry.sectors > 0) {

        virBufferAsprintf(&opt, ",cyls=%u,heads=%u,secs=%u",
                          disk->geometry.cylinders,
                          disk->geometry.heads,
                          disk->geometry.sectors);

        if (disk->geometry.trans != VIR_DOMAIN_DISK_TRANS_DEFAULT)
            virBufferEscapeString(&opt, ",trans=%s", trans);
    }

    if (disk->serial &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_SERIAL)) {
        if (qemuSafeSerialParamValue(disk->serial) < 0)
            goto error;
        virBufferAsprintf(&opt, ",serial=%s", disk->serial);
    }

    if (disk->cachemode) {
        const char *mode = NULL;

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_V2)) {
            mode = qemuDiskCacheV2TypeToString(disk->cachemode);

            if (disk->cachemode == VIR_DOMAIN_DISK_CACHE_DIRECTSYNC &&
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("disk cache mode 'directsync' is not "
                                 "supported by this QEMU"));
                goto error;
            } else if (disk->cachemode == VIR_DOMAIN_DISK_CACHE_UNSAFE &&
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_UNSAFE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("disk cache mode 'unsafe' is not "
                                 "supported by this QEMU"));
                goto error;
            }
        } else {
            mode = qemuDiskCacheV1TypeToString(disk->cachemode);
        }

        virBufferAsprintf(&opt, ",cache=%s", mode);
    } else if (disk->shared && !disk->readonly) {
        virBufferAddLit(&opt, ",cache=off");
    }

    if (disk->copy_on_read) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_COPY_ON_READ)) {
            virBufferAsprintf(&opt, ",copy-on-read=%s",
                              virDomainDiskCopyOnReadTypeToString(disk->copy_on_read));
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("copy_on_read is not supported by this QEMU binary"));
            goto error;
        }
    }

    if (disk->discard) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_DISCARD)) {
            virBufferAsprintf(&opt, ",discard=%s",
                              virDomainDiskDiscardTypeToString(disk->discard));
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("discard is not supported by this QEMU binary"));
            goto error;
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MONITOR_JSON)) {
        const char *wpolicy = NULL, *rpolicy = NULL;

        if (disk->error_policy)
            wpolicy = virDomainDiskErrorPolicyTypeToString(disk->error_policy);
        if (disk->rerror_policy)
            rpolicy = virDomainDiskErrorPolicyTypeToString(disk->rerror_policy);

        if (disk->error_policy == VIR_DOMAIN_DISK_ERROR_POLICY_ENOSPACE) {
            /* in the case of enospace, the option is spelled
             * differently in qemu, and it's only valid for werror,
             * not for rerror, so leave leave rerror NULL.
             */
            wpolicy = "enospc";
        } else if (!rpolicy) {
            /* for other policies, rpolicy can match wpolicy */
            rpolicy = wpolicy;
        }

        if (wpolicy)
            virBufferAsprintf(&opt, ",werror=%s", wpolicy);
        if (rpolicy)
            virBufferAsprintf(&opt, ",rerror=%s", rpolicy);
    }

    if (disk->iomode) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_AIO)) {
            virBufferAsprintf(&opt, ",aio=%s",
                              virDomainDiskIoTypeToString(disk->iomode));
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk aio mode not supported with this "
                             "QEMU binary"));
            goto error;
        }
    }

    /* block I/O throttling */
    if ((disk->blkdeviotune.total_bytes_sec ||
         disk->blkdeviotune.read_bytes_sec ||
         disk->blkdeviotune.write_bytes_sec ||
         disk->blkdeviotune.total_iops_sec ||
         disk->blkdeviotune.read_iops_sec ||
         disk->blkdeviotune.write_iops_sec) &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("block I/O throttling not supported with this "
                         "QEMU binary"));
        goto error;
    }

    if (disk->blkdeviotune.total_bytes_sec) {
        virBufferAsprintf(&opt, ",bps=%llu",
                          disk->blkdeviotune.total_bytes_sec);
    }

    if (disk->blkdeviotune.read_bytes_sec) {
        virBufferAsprintf(&opt, ",bps_rd=%llu",
                          disk->blkdeviotune.read_bytes_sec);
    }

    if (disk->blkdeviotune.write_bytes_sec) {
        virBufferAsprintf(&opt, ",bps_wr=%llu",
                          disk->blkdeviotune.write_bytes_sec);
    }

    if (disk->blkdeviotune.total_iops_sec) {
        virBufferAsprintf(&opt, ",iops=%llu",
                          disk->blkdeviotune.total_iops_sec);
    }

    if (disk->blkdeviotune.read_iops_sec) {
        virBufferAsprintf(&opt, ",iops_rd=%llu",
                          disk->blkdeviotune.read_iops_sec);
    }

    if (disk->blkdeviotune.write_iops_sec) {
        virBufferAsprintf(&opt, ",iops_wr=%llu",
                          disk->blkdeviotune.write_iops_sec);
    }

    if (virBufferError(&opt)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&opt);

error:
    virBufferFreeAndReset(&opt);
    return NULL;
}

char *
qemuBuildDriveDevStr(virDomainDefPtr def,
                     virDomainDiskDefPtr disk,
                     int bootindex,
                     virQEMUCapsPtr qemuCaps)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *bus = virDomainDiskQEMUBusTypeToString(disk->bus);
    int idx = virDiskNameToIndex(disk->dst);
    int controllerModel;

    if (idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported disk type '%s'"), disk->dst);
        goto error;
    }

    if (disk->wwn) {
        if ((disk->bus != VIR_DOMAIN_DISK_BUS_IDE) &&
            (disk->bus != VIR_DOMAIN_DISK_BUS_SCSI)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Only ide and scsi disk support wwn"));
            goto error;
        }
    }

    if ((disk->vendor || disk->product) &&
        disk->bus != VIR_DOMAIN_DISK_BUS_SCSI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Only scsi disk supports vendor and product"));
            goto error;
    }

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
        /* make sure that both the bus and the qemu binary support
         *  type='lun' (SG_IO).
         */
        if (disk->bus != VIR_DOMAIN_DISK_BUS_VIRTIO &&
            disk->bus != VIR_DOMAIN_DISK_BUS_SCSI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("disk device='lun' is not supported for bus='%s'"),
                           bus);
            goto error;
        }
        if (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK) {
            if (disk->protocol != VIR_DOMAIN_DISK_PROTOCOL_ISCSI) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("disk device='lun' is not supported for protocol='%s'"),
                               virDomainDiskProtocolTypeToString(disk->protocol));
                goto error;
            }
        } else if (!virDomainDiskSourceIsBlockType(disk)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk device='lun' is only valid for block type disk source"));
            goto error;
        }
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SG_IO)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk device='lun' is not supported by this QEMU"));
            goto error;
        }
        if (disk->wwn) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting wwn is not supported for lun device"));
            goto error;
        }
        if (disk->vendor || disk->product) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting vendor or product is not supported "
                             "for lun device"));
            goto error;
        }
    }

    switch (disk->bus) {
    case VIR_DOMAIN_DISK_BUS_IDE:
        if (disk->info.addr.drive.target != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for ide controller"));
            goto error;
        }

        if (disk->wwn &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_IDE_DRIVE_WWN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting wwn for ide disk is not supported "
                             "by this QEMU"));
            goto error;
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_IDE_CD)) {
            if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
                virBufferAddLit(&opt, "ide-cd");
            else
                virBufferAddLit(&opt, "ide-hd");
        } else {
            virBufferAddLit(&opt, "ide-drive");
        }

        virBufferAsprintf(&opt, ",bus=ide.%d,unit=%d",
                          disk->info.addr.drive.bus,
                          disk->info.addr.drive.unit);
        break;
    case VIR_DOMAIN_DISK_BUS_SCSI:
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_BLOCK)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("This QEMU doesn't support scsi-block for "
                                 "lun passthrough"));
                goto error;
            }
        }

        if (disk->wwn &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_WWN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting wwn for scsi disk is not supported "
                             "by this QEMU"));
            goto error;
        }

        /* Properties wwn, vendor and product were introduced in the
         * same QEMU release (1.2.0).
         */
        if ((disk->vendor || disk->product) &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_WWN)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Setting vendor or product for scsi disk is not "
                             "supported by this QEMU"));
            goto error;
        }

        controllerModel =
            virDomainDeviceFindControllerModel(def, &disk->info,
                                               VIR_DOMAIN_CONTROLLER_TYPE_SCSI);
        if ((qemuSetScsiControllerModel(def, qemuCaps, &controllerModel)) < 0)
            goto error;

        if (controllerModel == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
            if (disk->info.addr.drive.target != 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("target must be 0 for controller "
                                 "model 'lsilogic'"));
                goto error;
            }

            if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
                virBufferAddLit(&opt, "scsi-block");
            } else {
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_CD)) {
                    if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
                        virBufferAddLit(&opt, "scsi-cd");
                    else
                        virBufferAddLit(&opt, "scsi-hd");
                } else {
                    virBufferAddLit(&opt, "scsi-disk");
                }
            }

            virBufferAsprintf(&opt, ",bus=scsi%d.%d,scsi-id=%d",
                              disk->info.addr.drive.controller,
                              disk->info.addr.drive.bus,
                              disk->info.addr.drive.unit);
        } else {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_DISK_CHANNEL)) {
                if (disk->info.addr.drive.target > 7) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("This QEMU doesn't support target "
                                     "greater than 7"));
                    goto error;
                }

                if ((disk->info.addr.drive.bus != disk->info.addr.drive.unit) &&
                    (disk->info.addr.drive.bus != 0)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("This QEMU only supports both bus and "
                                     "unit equal to 0"));
                    goto error;
                }
            }

            if (disk->device != VIR_DOMAIN_DISK_DEVICE_LUN) {
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_CD)) {
                    if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
                        virBufferAddLit(&opt, "scsi-cd");
                    else
                        virBufferAddLit(&opt, "scsi-hd");
                } else {
                    virBufferAddLit(&opt, "scsi-disk");
                }
            } else {
                virBufferAddLit(&opt, "scsi-block");
            }

            virBufferAsprintf(&opt, ",bus=scsi%d.0,channel=%d,scsi-id=%d,lun=%d",
                              disk->info.addr.drive.controller,
                              disk->info.addr.drive.bus,
                              disk->info.addr.drive.target,
                              disk->info.addr.drive.unit);
        }
        break;
    case VIR_DOMAIN_DISK_BUS_SATA:
        if (disk->info.addr.drive.bus != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("bus must be 0 for ide controller"));
            goto error;
        }
        if (disk->info.addr.drive.target != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for ide controller"));
            goto error;
        }

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_IDE_CD)) {
            if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM)
                virBufferAddLit(&opt, "ide-cd");
            else
                virBufferAddLit(&opt, "ide-hd");
        } else {
            virBufferAddLit(&opt, "ide-drive");
        }

        virBufferAsprintf(&opt, ",bus=ahci%d.%d",
                          disk->info.addr.drive.controller,
                          disk->info.addr.drive.unit);
        break;
    case VIR_DOMAIN_DISK_BUS_VIRTIO:
        if (disk->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
            virBufferAddLit(&opt, "virtio-blk-ccw");
        } else if (disk->info.type ==
                   VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390) {
            virBufferAddLit(&opt, "virtio-blk-s390");
        } else {
            virBufferAddLit(&opt, "virtio-blk-pci");
        }
        qemuBuildIoEventFdStr(&opt, disk->ioeventfd, qemuCaps);
        if (disk->event_idx &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_EVENT_IDX)) {
            virBufferAsprintf(&opt, ",event_idx=%s",
                              virDomainVirtioEventIdxTypeToString(disk->event_idx));
        }
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SCSI)) {
            /* if sg_io is true but the scsi option isn't supported,
             * that means it's just always on in this version of qemu.
             */
            virBufferAsprintf(&opt, ",scsi=%s",
                              (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN)
                              ? "on" : "off");
        }
        if (qemuBuildDeviceAddressStr(&opt, &disk->info, qemuCaps) < 0)
            goto error;
        break;
    case VIR_DOMAIN_DISK_BUS_USB:
        virBufferAddLit(&opt, "usb-storage");

        if (qemuBuildDeviceAddressStr(&opt, &disk->info, qemuCaps) < 0)
            goto error;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported disk bus '%s' with device setup"), bus);
        goto error;
    }
    virBufferAsprintf(&opt, ",drive=%s%s", QEMU_DRIVE_HOST_PREFIX, disk->info.alias);
    virBufferAsprintf(&opt, ",id=%s", disk->info.alias);
    if (bootindex && virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOTINDEX))
        virBufferAsprintf(&opt, ",bootindex=%d", bootindex);
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKIO)) {
        if (disk->blockio.logical_block_size > 0)
            virBufferAsprintf(&opt, ",logical_block_size=%u",
                              disk->blockio.logical_block_size);
        if (disk->blockio.physical_block_size > 0)
            virBufferAsprintf(&opt, ",physical_block_size=%u",
                              disk->blockio.physical_block_size);
    }

    if (disk->wwn) {
        if (STRPREFIX(disk->wwn, "0x"))
            virBufferAsprintf(&opt, ",wwn=%s", disk->wwn);
        else
            virBufferAsprintf(&opt, ",wwn=0x%s", disk->wwn);
    }

    if (disk->vendor)
        virBufferAsprintf(&opt, ",vendor=%s", disk->vendor);

    if (disk->product)
        virBufferAsprintf(&opt, ",product=%s", disk->product);

    if (virBufferError(&opt)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&opt);

error:
    virBufferFreeAndReset(&opt);
    return NULL;
}


char *qemuBuildFSStr(virDomainFSDefPtr fs,
                     virQEMUCapsPtr qemuCaps ATTRIBUTE_UNUSED)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *driver = qemuDomainFSDriverTypeToString(fs->fsdriver);
    const char *wrpolicy = virDomainFSWrpolicyTypeToString(fs->wrpolicy);

    if (fs->type != VIR_DOMAIN_FS_TYPE_MOUNT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only supports mount filesystem type"));
        goto error;
    }

    if (!driver) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Filesystem driver type not supported"));
        goto error;
    }
    virBufferAdd(&opt, driver, -1);

    if (fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_PATH ||
        fs->fsdriver == VIR_DOMAIN_FS_DRIVER_TYPE_DEFAULT) {
        if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_MAPPED) {
            virBufferAddLit(&opt, ",security_model=mapped");
        } else if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH) {
            virBufferAddLit(&opt, ",security_model=passthrough");
        } else if (fs->accessmode == VIR_DOMAIN_FS_ACCESSMODE_SQUASH) {
            virBufferAddLit(&opt, ",security_model=none");
        }
    } else {
        /* For other fs drivers, default(passthru) should always
         * be supported */
        if (fs->accessmode != VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("only supports passthrough accessmode"));
            goto error;
        }
    }

    if (fs->wrpolicy) {
       if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_FSDEV_WRITEOUT)) {
           virBufferAsprintf(&opt, ",writeout=%s", wrpolicy);
       } else {
           virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                          _("filesystem writeout not supported"));
           goto error;
       }
    }

    virBufferAsprintf(&opt, ",id=%s%s", QEMU_FSDEV_HOST_PREFIX, fs->info.alias);
    virBufferAsprintf(&opt, ",path=%s", fs->src);

    if (fs->readonly) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_FSDEV_READONLY)) {
            virBufferAddLit(&opt, ",readonly");
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("readonly filesystem is not supported by this "
                             "QEMU binary"));
            goto error;
        }
    }

    if (virBufferError(&opt)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&opt);

error:
    virBufferFreeAndReset(&opt);
    return NULL;
}


char *
qemuBuildFSDevStr(virDomainFSDefPtr fs,
                  virQEMUCapsPtr qemuCaps)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;

    if (fs->type != VIR_DOMAIN_FS_TYPE_MOUNT) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("can only passthrough directories"));
        goto error;
    }

    virBufferAddLit(&opt, "virtio-9p-pci");
    virBufferAsprintf(&opt, ",id=%s", fs->info.alias);
    virBufferAsprintf(&opt, ",fsdev=%s%s", QEMU_FSDEV_HOST_PREFIX, fs->info.alias);
    virBufferAsprintf(&opt, ",mount_tag=%s", fs->dst);

    if (qemuBuildDeviceAddressStr(&opt, &fs->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&opt)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&opt);

error:
    virBufferFreeAndReset(&opt);
    return NULL;
}


static int
qemuControllerModelUSBToCaps(int model)
{
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI:
        return QEMU_CAPS_PIIX3_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX4_UHCI:
        return QEMU_CAPS_PIIX4_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_EHCI:
        return QEMU_CAPS_USB_EHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3:
        return QEMU_CAPS_ICH9_USB_EHCI1;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_VT82C686B_UHCI:
        return QEMU_CAPS_VT82C686B_USB_UHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PCI_OHCI:
        return QEMU_CAPS_PCI_OHCI;
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI:
        return QEMU_CAPS_NEC_USB_XHCI;
    default:
        return -1;
    }
}


static int
qemuBuildUSBControllerDevStr(virDomainDefPtr domainDef,
                             virDomainControllerDefPtr def,
                             virQEMUCapsPtr qemuCaps,
                             virBuffer *buf)
{
    const char *smodel;
    int model, flags;

    model = def->model;

    if (model == -1) {
        if (domainDef->os.arch == VIR_ARCH_PPC64)
            model = VIR_DOMAIN_CONTROLLER_MODEL_USB_PCI_OHCI;
        else
            model = VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI;
    }

    smodel = qemuControllerModelUSBTypeToString(model);
    flags = qemuControllerModelUSBToCaps(model);

    if (flags == -1 || !virQEMUCapsGet(qemuCaps, flags)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("%s not supported in this QEMU binary"), smodel);
        return -1;
    }

    virBufferAsprintf(buf, "%s", smodel);

    if (def->info.mastertype == VIR_DOMAIN_CONTROLLER_MASTER_USB) {
        virBufferAddLit(buf, ",masterbus=");
        qemuUsbId(buf, def->idx);
        virBufferAsprintf(buf, ".0,firstport=%d", def->info.master.usb.startport);
    } else {
        virBufferAddLit(buf, ",id=");
        qemuUsbId(buf, def->idx);
    }

    return 0;
}

char *
qemuBuildControllerDevStr(virDomainDefPtr domainDef,
                          virDomainControllerDefPtr def,
                          virQEMUCapsPtr qemuCaps,
                          int *nusbcontroller)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int model;

    if (def->queues &&
        !(def->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI &&
          def->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("'queues' is only supported by virtio-scsi controller"));
        return NULL;
    }

    switch (def->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        model = def->model;
        if ((qemuSetScsiControllerModel(domainDef, qemuCaps, &model)) < 0)
            return NULL;

        switch (model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
            if (def->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
                virBufferAddLit(&buf, "virtio-scsi-ccw");
            else if (def->info.type ==
                     VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390)
                virBufferAddLit(&buf, "virtio-scsi-s390");
            else
                virBufferAddLit(&buf, "virtio-scsi-pci");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
            virBufferAddLit(&buf, "lsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
            virBufferAddLit(&buf, "spapr-vscsi");
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
            virBufferAddLit(&buf, "megasas");
            break;
        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported controller model: %s"),
                           virDomainControllerModelSCSITypeToString(def->model));
        }
        virBufferAsprintf(&buf, ",id=scsi%d", def->idx);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
        if (def->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
            virBufferAddLit(&buf, "virtio-serial-pci");
        } else if (def->info.type ==
                   VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
            virBufferAddLit(&buf, "virtio-serial-ccw");
        } else if (def->info.type ==
                   VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390) {
            virBufferAddLit(&buf, "virtio-serial-s390");
        } else {
            virBufferAddLit(&buf, "virtio-serial");
        }
        virBufferAsprintf(&buf, ",id=" QEMU_VIRTIO_SERIAL_PREFIX "%d",
                          def->idx);
        if (def->opts.vioserial.ports != -1) {
            virBufferAsprintf(&buf, ",max_ports=%d",
                              def->opts.vioserial.ports);
        }
        if (def->opts.vioserial.vectors != -1) {
            virBufferAsprintf(&buf, ",vectors=%d",
                              def->opts.vioserial.vectors);
        }
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
        virBufferAsprintf(&buf, "usb-ccid,id=ccid%d", def->idx);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
        virBufferAsprintf(&buf, "ahci,id=ahci%d", def->idx);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_USB:
        if (qemuBuildUSBControllerDevStr(domainDef, def, qemuCaps, &buf) == -1)
            goto error;

        if (nusbcontroller)
            *nusbcontroller += 1;

        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_PCI:
        switch (def->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
            if (def->idx == 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("PCI bridge index should be > 0"));
                goto error;
            }
            virBufferAsprintf(&buf, "pci-bridge,chassis_nr=%d,id=pci.%d",
                              def->idx, def->idx);
            break;
        case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("wrong function called for pci-root"));
            return NULL;
        }
        break;

    /* We always get an IDE controller, whether we want it or not. */
    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unknown controller type: %s"),
                       virDomainControllerTypeToString(def->type));
        goto error;
    }

    if (def->queues)
        virBufferAsprintf(&buf, ",num_queues=%u", def->queues);

    if (qemuBuildDeviceAddressStr(&buf, &def->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildNicStr(virDomainNetDefPtr net,
                const char *prefix,
                int vlan)
{
    char *str;
    char macaddr[VIR_MAC_STRING_BUFLEN];

    ignore_value(virAsprintf(&str,
                             "%smacaddr=%s,vlan=%d%s%s%s%s",
                             prefix ? prefix : "",
                             virMacAddrFormat(&net->mac, macaddr),
                             vlan,
                             (net->model ? ",model=" : ""),
                             (net->model ? net->model : ""),
                             (net->info.alias ? ",name=" : ""),
                             (net->info.alias ? net->info.alias : "")));
    return str;
}


char *
qemuBuildNicDevStr(virDomainNetDefPtr net,
                   int vlan,
                   int bootindex,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *nic = net->model;
    bool usingVirtio = false;
    char macaddr[VIR_MAC_STRING_BUFLEN];

    if (STREQ(net->model, "virtio")) {
        if (net->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
            nic = "virtio-net-ccw";
        else if (net->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390)
            nic = "virtio-net-s390";
        else
            nic = "virtio-net-pci";

        usingVirtio = true;
    }

    virBufferAdd(&buf, nic, -1);
    if (usingVirtio && net->driver.virtio.txmode) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_TX_ALG)) {
            virBufferAddLit(&buf, ",tx=");
            switch (net->driver.virtio.txmode) {
                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_IOTHREAD:
                    virBufferAddLit(&buf, "bh");
                    break;

                case VIR_DOMAIN_NET_VIRTIO_TX_MODE_TIMER:
                    virBufferAddLit(&buf, "timer");
                    break;
                default:
                    /* this should never happen, if it does, we need
                     * to add another case to this switch.
                     */
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("unrecognized virtio-net-pci 'tx' option"));
                    goto error;
            }
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio-net-pci 'tx' option not supported in this QEMU binary"));
            goto error;
        }
    }
    if (usingVirtio) {
        qemuBuildIoEventFdStr(&buf, net->driver.virtio.ioeventfd, qemuCaps);
        if (net->driver.virtio.event_idx &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_NET_EVENT_IDX)) {
            virBufferAsprintf(&buf, ",event_idx=%s",
                              virDomainVirtioEventIdxTypeToString(net->driver.virtio.event_idx));
        }
    }
    if (vlan == -1)
        virBufferAsprintf(&buf, ",netdev=host%s", net->info.alias);
    else
        virBufferAsprintf(&buf, ",vlan=%d", vlan);
    virBufferAsprintf(&buf, ",id=%s", net->info.alias);
    virBufferAsprintf(&buf, ",mac=%s",
                      virMacAddrFormat(&net->mac, macaddr));
    if (qemuBuildDeviceAddressStr(&buf, &net->info, qemuCaps) < 0)
        goto error;
    if (qemuBuildRomStr(&buf, &net->info, qemuCaps) < 0)
       goto error;
    if (bootindex && virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOTINDEX))
        virBufferAsprintf(&buf, ",bootindex=%d", bootindex);

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildHostNetStr(virDomainNetDefPtr net,
                    virQEMUDriverPtr driver,
                    char type_sep,
                    int vlan,
                    char **tapfd,
                    int tapfdSize,
                    char **vhostfd,
                    int vhostfdSize)
{
    bool is_tap = false;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    enum virDomainNetType netType = virDomainNetGetActualType(net);
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    size_t i;

    if (net->script && netType != VIR_DOMAIN_NET_TYPE_ETHERNET) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("scripts are not supported on interfaces of type %s"),
                       virDomainNetTypeToString(netType));
        virObjectUnref(cfg);
        return NULL;
    }

    switch (netType) {
    /*
     * If type='bridge', and we're running as privileged user
     * or -netdev bridge is not supported then it will fall
     * through, -net tap,fd
     */
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
        virBufferAsprintf(&buf, "tap%c", type_sep);
        /* for one tapfd 'fd=' shall be used,
         * for more than one 'fds=' is the right choice */
        if (tapfdSize == 1) {
            virBufferAsprintf(&buf, "fd=%s", tapfd[0]);
        } else {
            virBufferAddLit(&buf, "fds=");
            for (i = 0; i < tapfdSize; i++) {
                if (i)
                    virBufferAddChar(&buf, ':');
                virBufferAdd(&buf, tapfd[i], -1);
            }
        }
        type_sep = ',';
        is_tap = true;
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        virBufferAddLit(&buf, "tap");
        if (net->ifname) {
            virBufferAsprintf(&buf, "%cifname=%s", type_sep, net->ifname);
            type_sep = ',';
        }
        if (net->script) {
            virBufferAsprintf(&buf, "%cscript=%s", type_sep,
                              net->script);
            type_sep = ',';
        }
        is_tap = true;
        break;

    case VIR_DOMAIN_NET_TYPE_CLIENT:
       virBufferAsprintf(&buf, "socket%cconnect=%s:%d",
                         type_sep,
                         net->data.socket.address,
                         net->data.socket.port);
       type_sep = ',';
       break;

    case VIR_DOMAIN_NET_TYPE_SERVER:
       virBufferAsprintf(&buf, "socket%clisten=%s:%d",
                         type_sep,
                         net->data.socket.address,
                         net->data.socket.port);
       type_sep = ',';
       break;

    case VIR_DOMAIN_NET_TYPE_MCAST:
       virBufferAsprintf(&buf, "socket%cmcast=%s:%d",
                         type_sep,
                         net->data.socket.address,
                         net->data.socket.port);
       type_sep = ',';
       break;

    case VIR_DOMAIN_NET_TYPE_USER:
    default:
        virBufferAddLit(&buf, "user");
        break;
    }

    if (vlan >= 0) {
        virBufferAsprintf(&buf, "%cvlan=%d", type_sep, vlan);
        if (net->info.alias)
            virBufferAsprintf(&buf, ",name=host%s",
                              net->info.alias);
    } else {
        virBufferAsprintf(&buf, "%cid=host%s",
                          type_sep, net->info.alias);
    }

    if (is_tap) {
        if (vhostfdSize) {
            virBufferAddLit(&buf, ",vhost=on,");
            if (vhostfdSize == 1) {
                virBufferAsprintf(&buf, "vhostfd=%s", vhostfd[0]);
            } else {
                virBufferAddLit(&buf, "vhostfds=");
                for (i = 0; i < vhostfdSize; i++) {
                    if (i)
                        virBufferAddChar(&buf, ':');
                    virBufferAdd(&buf, vhostfd[i], -1);
                }
            }
        }
        if (net->tune.sndbuf_specified)
            virBufferAsprintf(&buf, ",sndbuf=%lu", net->tune.sndbuf);
    }

    virObjectUnref(cfg);

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}


char *
qemuBuildWatchdogDevStr(virDomainWatchdogDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    const char *model = virDomainWatchdogModelTypeToString(dev->model);
    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing watchdog model"));
        goto error;
    }

    virBufferAsprintf(&buf, "%s,id=%s", model, dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildMemballoonDevStr(virDomainMemballoonDefPtr dev,
                          virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    switch (dev->info.type) {
        case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
            virBufferAddLit(&buf, "virtio-balloon-pci");
            break;
        case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
            virBufferAddLit(&buf, "virtio-balloon-ccw");
            break;
        default:
            virReportError(VIR_ERR_XML_ERROR,
                           _("memballoon unsupported with address type '%s'"),
                           virDomainDeviceAddressTypeToString(dev->info.type));
            goto error;
    }

    virBufferAsprintf(&buf, ",id=%s", dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *
qemuBuildNVRAMDevStr(virDomainNVRAMDefPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (dev->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO &&
        dev->info.addr.spaprvio.has_reg) {
        virBufferAsprintf(&buf, "spapr-nvram.reg=0x%llx",
                          dev->info.addr.spaprvio.reg);
    } else {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("nvram address type must be spaprvio"));
        goto error;
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

char *
qemuBuildUSBInputDevStr(virDomainInputDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&buf, "%s,id=%s",
                      dev->type == VIR_DOMAIN_INPUT_TYPE_MOUSE ?
                      "usb-mouse" : "usb-tablet", dev->info.alias);

    if (qemuBuildDeviceAddressStr(&buf, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildSoundDevStr(virDomainSoundDefPtr sound,
                     virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *model = virDomainSoundModelTypeToString(sound->model);

    if (!model) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("invalid sound model"));
        goto error;
    }

    /* Hack for weirdly unusual devices name in QEMU */
    if (STREQ(model, "es1370"))
        model = "ES1370";
    else if (STREQ(model, "ac97"))
        model = "AC97";
    else if (STREQ(model, "ich6"))
        model = "intel-hda";

    virBufferAsprintf(&buf, "%s,id=%s", model, sound->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, &sound->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuSoundCodecTypeToCaps(int type)
{
    switch (type) {
    case VIR_DOMAIN_SOUND_CODEC_TYPE_DUPLEX:
        return QEMU_CAPS_HDA_DUPLEX;
    case VIR_DOMAIN_SOUND_CODEC_TYPE_MICRO:
        return QEMU_CAPS_HDA_MICRO;
    default:
        return -1;
    }
}


static char *
qemuBuildSoundCodecStr(virDomainSoundDefPtr sound,
                       virDomainSoundCodecDefPtr codec,
                       virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *stype;
    int type, flags;

    type = codec->type;
    stype = qemuSoundCodecTypeToString(type);
    flags = qemuSoundCodecTypeToCaps(type);

    if (flags == -1 || !virQEMUCapsGet(qemuCaps, flags)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("%s not supported in this QEMU binary"), stype);
        goto error;
    }

    virBufferAsprintf(&buf, "%s,id=%s-codec%d,bus=%s.0,cad=%d",
                      stype, sound->info.alias, codec->cad, sound->info.alias, codec->cad);

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *
qemuBuildDeviceVideoStr(virDomainVideoDefPtr video,
                        virQEMUCapsPtr qemuCaps,
                        bool primary)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *model;

    if (primary) {
        model = qemuDeviceVideoTypeToString(video->type);
        if (!model || STREQ(model, "")) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("video type %s is not supported with QEMU"),
                           virDomainVideoTypeToString(video->type));
            goto error;
        }
    } else {
        if (video->type != VIR_DOMAIN_VIDEO_TYPE_QXL) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("non-primary video device must be type of 'qxl'"));
            goto error;
        }

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QXL)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("only one video card is currently supported"));
            goto error;
        }

        model = "qxl";
    }

    virBufferAsprintf(&buf, "%s,id=%s", model, video->info.alias);

    if (video->type == VIR_DOMAIN_VIDEO_TYPE_QXL) {
        if (video->vram > (UINT_MAX / 1024)) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("value for 'vram' must be less than '%u'"),
                           UINT_MAX / 1024);
            goto error;
        }
        if (video->ram > (UINT_MAX / 1024)) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("value for 'ram' must be less than '%u'"),
                           UINT_MAX / 1024);
            goto error;
        }

        /* QEMU accepts bytes for ram_size. */
        virBufferAsprintf(&buf, ",ram_size=%u", video->ram * 1024);

        /* QEMU accepts bytes for vram_size. */
        virBufferAsprintf(&buf, ",vram_size=%u", video->vram * 1024);
    }

    if (qemuBuildDeviceAddressStr(&buf, &video->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


int
qemuOpenPCIConfig(virDomainHostdevDefPtr dev)
{
    char *path = NULL;
    int configfd = -1;

    if (virAsprintf(&path, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/config",
                    dev->source.subsys.u.pci.addr.domain,
                    dev->source.subsys.u.pci.addr.bus,
                    dev->source.subsys.u.pci.addr.slot,
                    dev->source.subsys.u.pci.addr.function) < 0)
        return -1;

    configfd = open(path, O_RDWR, 0);

    if (configfd < 0)
        virReportSystemError(errno, _("Failed opening %s"), path);

    VIR_FREE(path);

    return configfd;
}

char *
qemuBuildPCIHostdevDevStr(virDomainHostdevDefPtr dev, const char *configfd,
                          virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (dev->source.subsys.u.pci.backend
        == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
        virBufferAddLit(&buf, "vfio-pci");
    } else {
        virBufferAddLit(&buf, "pci-assign");
        if (configfd && *configfd)
            virBufferAsprintf(&buf, ",configfd=%s", configfd);
    }
    virBufferAsprintf(&buf, ",host=%.2x:%.2x.%.1x",
                      dev->source.subsys.u.pci.addr.bus,
                      dev->source.subsys.u.pci.addr.slot,
                      dev->source.subsys.u.pci.addr.function);
    virBufferAsprintf(&buf, ",id=%s", dev->info->alias);
    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%d", dev->info->bootIndex);
    if (qemuBuildDeviceAddressStr(&buf, dev->info, qemuCaps) < 0)
        goto error;
    if (qemuBuildRomStr(&buf, dev->info, qemuCaps) < 0)
       goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildPCIHostdevPCIDevStr(virDomainHostdevDefPtr dev)
{
    char *ret = NULL;

    ignore_value(virAsprintf(&ret, "host=%.2x:%.2x.%.1x",
                             dev->source.subsys.u.pci.addr.bus,
                             dev->source.subsys.u.pci.addr.slot,
                             dev->source.subsys.u.pci.addr.function));
    return ret;
}


char *
qemuBuildRedirdevDevStr(virDomainDefPtr def,
                        virDomainRedirdevDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    size_t i;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainRedirFilterDefPtr redirfilter = def->redirfilter;

    if (dev->bus != VIR_DOMAIN_REDIRDEV_BUS_USB) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Redirection bus %s is not supported by QEMU"),
                       virDomainRedirdevBusTypeToString(dev->bus));
        goto error;
    }

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_REDIR)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("USB redirection is not supported "
                         "by this version of QEMU"));
        goto error;
    }

    virBufferAsprintf(&buf, "usb-redir,chardev=char%s,id=%s",
                      dev->info.alias,
                      dev->info.alias);

    if (redirfilter && redirfilter->nusbdevs) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_REDIR_FILTER)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("USB redirection filter is not "
                             "supported by this version of QEMU"));
            goto error;
        }

        virBufferAddLit(&buf, ",filter=");

        for (i = 0; i < redirfilter->nusbdevs; i++) {
            virDomainRedirFilterUsbDevDefPtr usbdev = redirfilter->usbdevs[i];
            if (usbdev->usbClass >= 0)
                virBufferAsprintf(&buf, "0x%02X:", usbdev->usbClass);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->vendor >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->vendor);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->product >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->product);
            else
                virBufferAddLit(&buf, "-1:");

            if (usbdev->version >= 0)
                virBufferAsprintf(&buf, "0x%04X:", usbdev->version);
            else
                virBufferAddLit(&buf, "-1:");

            virBufferAsprintf(&buf, "%u", usbdev->allow);
            if (i < redirfilter->nusbdevs -1)
                virBufferAddLit(&buf, "|");
        }
    }

    if (dev->info.bootIndex) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_REDIR_BOOTINDEX)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("USB redirection booting is not "
                             "supported by this version of QEMU"));
            goto error;
        }
        virBufferAsprintf(&buf, ",bootindex=%d", dev->info.bootIndex);
    }

    if (qemuBuildDeviceAddressStr(&buf, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

char *
qemuBuildUSBHostdevDevStr(virDomainHostdevDefPtr dev,
                          virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!dev->missing &&
        !dev->source.subsys.u.usb.bus &&
        !dev->source.subsys.u.usb.device) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("USB host device is missing bus/device information"));
        return NULL;
    }

    virBufferAddLit(&buf, "usb-host");
    if (!dev->missing) {
        virBufferAsprintf(&buf, ",hostbus=%d,hostaddr=%d",
                          dev->source.subsys.u.usb.bus,
                          dev->source.subsys.u.usb.device);
    }
    virBufferAsprintf(&buf, ",id=%s", dev->info->alias);
    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%d", dev->info->bootIndex);

    if (qemuBuildDeviceAddressStr(&buf, dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildHubDevStr(virDomainHubDefPtr dev,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (dev->type != VIR_DOMAIN_HUB_TYPE_USB) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hub type %s not supported"),
                       virDomainHubTypeToString(dev->type));
        goto error;
    }

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_HUB)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("usb-hub not supported by QEMU binary"));
        goto error;
    }

    virBufferAddLit(&buf, "usb-hub");
    virBufferAsprintf(&buf, ",id=%s", dev->info.alias);
    if (qemuBuildDeviceAddressStr(&buf, &dev->info, qemuCaps) < 0)
        goto error;

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *
qemuBuildUSBHostdevUsbDevStr(virDomainHostdevDefPtr dev)
{
    char *ret = NULL;

    if (dev->missing) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("This QEMU doesn't not support missing USB devices"));
        return NULL;
    }

    if (!dev->source.subsys.u.usb.bus &&
        !dev->source.subsys.u.usb.device) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("USB host device is missing bus/device information"));
        return NULL;
    }

    ignore_value(virAsprintf(&ret, "host:%d.%d",
                             dev->source.subsys.u.usb.bus,
                             dev->source.subsys.u.usb.device));
    return ret;
}

char *
qemuBuildSCSIHostdevDrvStr(virDomainHostdevDefPtr dev,
                           virQEMUCapsPtr qemuCaps ATTRIBUTE_UNUSED,
                           qemuBuildCommandLineCallbacksPtr callbacks)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *sg = NULL;

    sg = (callbacks->qemuGetSCSIDeviceSgName)(dev->source.subsys.u.scsi.adapter,
                                              dev->source.subsys.u.scsi.bus,
                                              dev->source.subsys.u.scsi.target,
                                              dev->source.subsys.u.scsi.unit);
    if (!sg)
        goto error;

    virBufferAsprintf(&buf, "file=/dev/%s,if=none", sg);
    virBufferAsprintf(&buf, ",id=%s-%s",
                      virDomainDeviceAddressTypeToString(dev->info->type),
                      dev->info->alias);

    if (dev->readonly) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_READONLY)) {
            virBufferAddLit(&buf, ",readonly=on");
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support 'readonly' "
                             "for -drive"));
            goto error;
        }
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    VIR_FREE(sg);
    return virBufferContentAndReset(&buf);
error:
    VIR_FREE(sg);
    virBufferFreeAndReset(&buf);
    return NULL;
}

char *
qemuBuildSCSIHostdevDevStr(virDomainDefPtr def,
                           virDomainHostdevDefPtr dev,
                           virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int model = -1;

    model = virDomainDeviceFindControllerModel(def, dev->info,
                                               VIR_DOMAIN_CONTROLLER_TYPE_SCSI);

    if (qemuSetScsiControllerModel(def, qemuCaps, &model) < 0)
        goto error;

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
        if (dev->info->addr.drive.target != 0) {
           virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("target must be 0 for scsi host device "
                             "if its controller model is 'lsilogic'"));
            goto error;
        }

        if (dev->info->addr.drive.unit > 7) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("unit must be not more than 7 for scsi host "
                             "device if its controller model is 'lsilogic'"));
            goto error;
        }
    }

    virBufferAddLit(&buf, "scsi-generic");

    if (model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
        virBufferAsprintf(&buf, ",bus=scsi%d.%d,scsi-id=%d",
                          dev->info->addr.drive.controller,
                          dev->info->addr.drive.bus,
                          dev->info->addr.drive.unit);
    } else {
        virBufferAsprintf(&buf, ",bus=scsi%d.0,channel=%d,scsi-id=%d,lun=%d",
                          dev->info->addr.drive.controller,
                          dev->info->addr.drive.bus,
                          dev->info->addr.drive.target,
                          dev->info->addr.drive.unit);
    }

    virBufferAsprintf(&buf, ",drive=%s-%s,id=%s",
                      virDomainDeviceAddressTypeToString(dev->info->type),
                      dev->info->alias, dev->info->alias);

    if (dev->info->bootIndex)
        virBufferAsprintf(&buf, ",bootindex=%d", dev->info->bootIndex);

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);
error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

/* This function outputs a -chardev command line option which describes only the
 * host side of the character device */
static char *
qemuBuildChrChardevStr(virDomainChrSourceDefPtr dev, const char *alias,
                       virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    bool telnet;

    switch (dev->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
        virBufferAsprintf(&buf, "null,id=char%s", alias);
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
        virBufferAsprintf(&buf, "vc,id=char%s", alias);
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
        virBufferAsprintf(&buf, "pty,id=char%s", alias);
        break;

    case VIR_DOMAIN_CHR_TYPE_DEV:
        virBufferAsprintf(&buf, "%s,id=char%s,path=%s",
                          STRPREFIX(alias, "parallel") ? "parport" : "tty",
                          alias, dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
        virBufferAsprintf(&buf, "file,id=char%s,path=%s", alias,
                          dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        virBufferAsprintf(&buf, "pipe,id=char%s,path=%s", alias,
                          dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_STDIO:
        virBufferAsprintf(&buf, "stdio,id=char%s", alias);
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP: {
        const char *connectHost = dev->data.udp.connectHost;
        const char *bindHost = dev->data.udp.bindHost;
        const char *bindService = dev->data.udp.bindService;

        if (connectHost == NULL)
            connectHost = "";
        if (bindHost == NULL)
            bindHost = "";
        if (bindService == NULL)
            bindService = "0";

        virBufferAsprintf(&buf,
                          "udp,id=char%s,host=%s,port=%s,localaddr=%s,"
                          "localport=%s",
                          alias,
                          connectHost,
                          dev->data.udp.connectService,
                          bindHost, bindService);
        break;
    }
    case VIR_DOMAIN_CHR_TYPE_TCP:
        telnet = dev->data.tcp.protocol == VIR_DOMAIN_CHR_TCP_PROTOCOL_TELNET;
        virBufferAsprintf(&buf,
                          "socket,id=char%s,host=%s,port=%s%s%s",
                          alias,
                          dev->data.tcp.host,
                          dev->data.tcp.service,
                          telnet ? ",telnet" : "",
                          dev->data.tcp.listen ? ",server,nowait" : "");
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(&buf,
                          "socket,id=char%s,path=%s%s",
                          alias,
                          dev->data.nix.path,
                          dev->data.nix.listen ? ",server,nowait" : "");
        break;

    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spicevmc not supported in this QEMU binary"));
            goto error;
        }
        virBufferAsprintf(&buf, "spicevmc,id=char%s,name=%s", alias,
                          virDomainChrSpicevmcTypeToString(dev->data.spicevmc));
        break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported chardev '%s'"),
                       virDomainChrTypeToString(dev->type));
        goto error;
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static char *
qemuBuildChrArgStr(virDomainChrSourceDefPtr dev, const char *prefix)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (prefix)
        virBufferAdd(&buf, prefix, strlen(prefix));

    switch (dev->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
        virBufferAddLit(&buf, "null");
        break;

    case VIR_DOMAIN_CHR_TYPE_VC:
        virBufferAddLit(&buf, "vc");
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
        virBufferAddLit(&buf, "pty");
        break;

    case VIR_DOMAIN_CHR_TYPE_DEV:
        virBufferStrcat(&buf, dev->data.file.path, NULL);
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
        virBufferAsprintf(&buf, "file:%s", dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        virBufferAsprintf(&buf, "pipe:%s", dev->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_STDIO:
        virBufferAddLit(&buf, "stdio");
        break;

    case VIR_DOMAIN_CHR_TYPE_UDP: {
        const char *connectHost = dev->data.udp.connectHost;
        const char *bindHost = dev->data.udp.bindHost;
        const char *bindService  = dev->data.udp.bindService;

        if (connectHost == NULL)
            connectHost = "";
        if (bindHost == NULL)
            bindHost = "";
        if (bindService == NULL)
            bindService = "0";

        virBufferAsprintf(&buf, "udp:%s:%s@%s:%s",
                          connectHost,
                          dev->data.udp.connectService,
                          bindHost,
                          bindService);
        break;
    }
    case VIR_DOMAIN_CHR_TYPE_TCP:
        if (dev->data.tcp.protocol == VIR_DOMAIN_CHR_TCP_PROTOCOL_TELNET) {
            virBufferAsprintf(&buf, "telnet:%s:%s%s",
                              dev->data.tcp.host,
                              dev->data.tcp.service,
                              dev->data.tcp.listen ? ",server,nowait" : "");
        } else {
            virBufferAsprintf(&buf, "tcp:%s:%s%s",
                              dev->data.tcp.host,
                              dev->data.tcp.service,
                              dev->data.tcp.listen ? ",server,nowait" : "");
        }
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        virBufferAsprintf(&buf, "unix:%s%s",
                          dev->data.nix.path,
                          dev->data.nix.listen ? ",server,nowait" : "");
        break;
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static char *
qemuBuildVirtioSerialPortDevStr(virDomainChrDefPtr dev,
                                virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    switch (dev->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        virBufferAddLit(&buf, "virtconsole");
        break;
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        /* Legacy syntax  '-device spicevmc' */
        if (dev->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC)) {
            virBufferAddLit(&buf, "spicevmc");
        } else {
            virBufferAddLit(&buf, "virtserialport");
        }
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cannot use virtio serial for parallel/serial devices"));
        return NULL;
    }

    if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW &&
        dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390) {
        /* Check it's a virtio-serial address */
        if (dev->info.type !=
            VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL)
        {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("virtio serial device has invalid address type"));
            goto error;
        }

        virBufferAsprintf(&buf,
                          ",bus=" QEMU_VIRTIO_SERIAL_PREFIX "%d.%d",
                          dev->info.addr.vioserial.controller,
                          dev->info.addr.vioserial.bus);
        virBufferAsprintf(&buf,
                          ",nr=%d",
                          dev->info.addr.vioserial.port);
    }

    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
        dev->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC &&
        STRNEQ_NULLABLE(dev->target.name, "com.redhat.spice.0")) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported spicevmc target name '%s'"),
                       dev->target.name);
        goto error;
    }

    if (!(dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
          dev->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC))) {
        virBufferAsprintf(&buf, ",chardev=char%s,id=%s",
                          dev->info.alias, dev->info.alias);
        if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL) {
            virBufferAsprintf(&buf, ",name=%s", dev->target.name
                              ? dev->target.name : "com.redhat.spice.0");
        }
    } else {
        virBufferAsprintf(&buf, ",id=%s", dev->info.alias);
    }
    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *
qemuBuildSclpDevStr(virDomainChrDefPtr dev)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE) {
        switch (dev->targetType) {
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
            virBufferAddLit(&buf, "sclpconsole");
            break;
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
            virBufferAddLit(&buf, "sclplmconsole");
            break;
        }
    } else {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cannot use slcp with devices other than console"));
        goto error;
    }
    virBufferAsprintf(&buf, ",chardev=char%s,id=%s",
                      dev->info.alias, dev->info.alias);
    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildRNGBackendArgs(virCommandPtr cmd,
                        virDomainRNGDefPtr dev,
                        virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *backend = NULL;
    int ret = -1;

    switch ((enum virDomainRNGBackend) dev->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_RNG_RANDOM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the rng-random "
                             " backend"));
            goto cleanup;
        }

        virBufferAsprintf(&buf, "rng-random,id=%s", dev->info.alias);
        if (dev->source.file)
            virBufferAsprintf(&buf, ",filename=%s", dev->source.file);

        virCommandAddArg(cmd, "-object");
        virCommandAddArgBuffer(cmd, &buf);
        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_RNG_EGD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support the rng-egd "
                             "backend"));
            goto cleanup;
        }

        if (!(backend = qemuBuildChrChardevStr(dev->source.chardev,
                                               dev->info.alias, qemuCaps)))
            goto cleanup;

        virCommandAddArgList(cmd, "-chardev", backend, NULL);

        virCommandAddArg(cmd, "-object");
        virCommandAddArgFormat(cmd, "rng-egd,chardev=char%s,id=%s",
                               dev->info.alias, dev->info.alias);
        break;

    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    ret = 0;

cleanup:
    virBufferFreeAndReset(&buf);
    VIR_FREE(backend);
    return ret;
}


static int
qemuBuildRNGDeviceArgs(virCommandPtr cmd,
                       virDomainRNGDefPtr dev,
                       virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    int ret = -1;

    if (dev->model != VIR_DOMAIN_RNG_MODEL_VIRTIO ||
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VIRTIO_RNG)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("this qemu doesn't support RNG device type '%s'"),
                       virDomainRNGModelTypeToString(dev->model));
        goto cleanup;
    }

    if (dev->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
        virBufferAsprintf(&buf, "virtio-rng-ccw,rng=%s", dev->info.alias);
    else if (dev->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390)
        virBufferAsprintf(&buf, "virtio-rng-s390,rng=%s", dev->info.alias);
    else
        virBufferAsprintf(&buf, "virtio-rng-pci,rng=%s", dev->info.alias);

    if (dev->rate > 0) {
        virBufferAsprintf(&buf, ",max-bytes=%u", dev->rate);
        if (dev->period)
            virBufferAsprintf(&buf, ",period=%u", dev->period);
        else
            virBufferAddLit(&buf, ",period=1000");
    }

    if (qemuBuildDeviceAddressStr(&buf, &dev->info, qemuCaps) < 0)
        goto cleanup;

    virCommandAddArg(cmd, "-device");
    virCommandAddArgBuffer(cmd, &buf);

    ret = 0;

cleanup:
    virBufferFreeAndReset(&buf);
    return ret;
}


static char *qemuBuildTPMBackendStr(const virDomainDefPtr def,
                                    virQEMUCapsPtr qemuCaps,
                                    const char *emulator)
{
    const virDomainTPMDefPtr tpm = def->tpm;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const char *type = virDomainTPMBackendTypeToString(tpm->type);
    const char *cancel_path, *tpmdev;

    virBufferAsprintf(&buf, "%s,id=tpm-%s", type, tpm->info.alias);

    switch (tpm->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_TPM_PASSTHROUGH))
            goto no_support;

        tpmdev = tpm->data.passthrough.source.data.file.path;
        if (!(cancel_path = virTPMCreateCancelPath(tpmdev)))
            goto error;

        virBufferAddLit(&buf, ",path=");
        virBufferEscape(&buf, ',', ",", "%s", tpmdev);

        virBufferAddLit(&buf, ",cancel-path=");
        virBufferEscape(&buf, ',', ",", "%s", cancel_path);
        VIR_FREE(cancel_path);

        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        goto error;
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

 no_support:
    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                   _("The QEMU executable %s does not support TPM "
                     "backend type %s"),
                   emulator, type);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static char *qemuBuildTPMDevStr(const virDomainDefPtr def,
                                virQEMUCapsPtr qemuCaps,
                                const char *emulator)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    const virDomainTPMDefPtr tpm = def->tpm;
    const char *model = virDomainTPMModelTypeToString(tpm->model);

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_TPM_TIS)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("The QEMU executable %s does not support TPM "
                       "model %s"),
                       emulator, model);
        goto error;
    }

    virBufferAsprintf(&buf, "%s,tpmdev=tpm-%s,id=%s",
                      model, tpm->info.alias, tpm->info.alias);

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

 error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static char *qemuBuildSmbiosBiosStr(virSysinfoDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if ((def->bios_vendor == NULL) && (def->bios_version == NULL) &&
        (def->bios_date == NULL) && (def->bios_release == NULL))
        return NULL;

    virBufferAddLit(&buf, "type=0");

    /* 0:Vendor */
    if (def->bios_vendor)
        virBufferAsprintf(&buf, ",vendor=%s", def->bios_vendor);
    /* 0:BIOS Version */
    if (def->bios_version)
        virBufferAsprintf(&buf, ",version=%s", def->bios_version);
    /* 0:BIOS Release Date */
    if (def->bios_date)
        virBufferAsprintf(&buf, ",date=%s", def->bios_date);
    /* 0:System BIOS Major Release and 0:System BIOS Minor Release */
    if (def->bios_release)
        virBufferAsprintf(&buf, ",release=%s", def->bios_release);

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *qemuBuildSmbiosSystemStr(virSysinfoDefPtr def, bool skip_uuid)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if ((def->system_manufacturer == NULL) && (def->system_sku == NULL) &&
        (def->system_product == NULL) && (def->system_version == NULL) &&
        (def->system_serial == NULL) && (def->system_family == NULL) &&
        (def->system_uuid == NULL || skip_uuid))
        return NULL;

    virBufferAddLit(&buf, "type=1");

    /* 1:Manufacturer */
    if (def->system_manufacturer)
        virBufferAsprintf(&buf, ",manufacturer=%s",
                          def->system_manufacturer);
     /* 1:Product Name */
    if (def->system_product)
        virBufferAsprintf(&buf, ",product=%s", def->system_product);
    /* 1:Version */
    if (def->system_version)
        virBufferAsprintf(&buf, ",version=%s", def->system_version);
    /* 1:Serial Number */
    if (def->system_serial)
        virBufferAsprintf(&buf, ",serial=%s", def->system_serial);
    /* 1:UUID */
    if (def->system_uuid && !skip_uuid)
        virBufferAsprintf(&buf, ",uuid=%s", def->system_uuid);
    /* 1:SKU Number */
    if (def->system_sku)
        virBufferAsprintf(&buf, ",sku=%s", def->system_sku);
    /* 1:Family */
    if (def->system_family)
        virBufferAsprintf(&buf, ",family=%s", def->system_family);

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}

static char *
qemuBuildClockArgStr(virDomainClockDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    switch (def->offset) {
    case VIR_DOMAIN_CLOCK_OFFSET_UTC:
        virBufferAddLit(&buf, "base=utc");
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
    case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
        virBufferAddLit(&buf, "base=localtime");
        break;

    case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE: {
        time_t now = time(NULL);
        struct tm nowbits;

        if (def->data.variable.basis != VIR_DOMAIN_CLOCK_BASIS_UTC) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported clock basis '%s'"),
                           virDomainClockBasisTypeToString(def->data.variable.basis));
            goto error;
        }
        now += def->data.variable.adjustment;
        gmtime_r(&now, &nowbits);

        /* Store the guest's basedate */
        def->data.variable.basedate = now;

        virBufferAsprintf(&buf, "base=%d-%02d-%02dT%02d:%02d:%02d",
                          nowbits.tm_year + 1900,
                          nowbits.tm_mon + 1,
                          nowbits.tm_mday,
                          nowbits.tm_hour,
                          nowbits.tm_min,
                          nowbits.tm_sec);
    }   break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported clock offset '%s'"),
                       virDomainClockOffsetTypeToString(def->offset));
        goto error;
    }

    /* Look for an 'rtc' timer element, and add in appropriate clock= and driftfix= */
    size_t i;
    for (i = 0; i < def->ntimers; i++) {
        if (def->timers[i]->name == VIR_DOMAIN_TIMER_NAME_RTC) {
            switch (def->timers[i]->track) {
            case -1: /* unspecified - use hypervisor default */
                break;
            case VIR_DOMAIN_TIMER_TRACK_BOOT:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported rtc timer track '%s'"),
                               virDomainTimerTrackTypeToString(def->timers[i]->track));
                goto error;
            case VIR_DOMAIN_TIMER_TRACK_GUEST:
                virBufferAddLit(&buf, ",clock=vm");
                break;
            case VIR_DOMAIN_TIMER_TRACK_WALL:
                virBufferAddLit(&buf, ",clock=host");
                break;
            }

            switch (def->timers[i]->tickpolicy) {
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                /* This is the default - missed ticks delivered when
                   next scheduled, at normal rate */
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                /* deliver ticks at a faster rate until caught up */
                virBufferAddLit(&buf, ",driftfix=slew");
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported rtc timer tickpolicy '%s'"),
                               virDomainTimerTickpolicyTypeToString(def->timers[i]->tickpolicy));
                goto error;
            }
            break; /* no need to check other timers - there is only one rtc */
        }
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto error;
    }

    return virBufferContentAndReset(&buf);

error:
    virBufferFreeAndReset(&buf);
    return NULL;
}


static int
qemuBuildCpuArgStr(const virQEMUDriverPtr driver,
                   const virDomainDefPtr def,
                   const char *emulator,
                   virQEMUCapsPtr qemuCaps,
                   virArch hostarch,
                   char **opt,
                   bool *hasHwVirt,
                   bool migrating)
{
    virCPUDefPtr host = NULL;
    virCPUDefPtr guest = NULL;
    virCPUDefPtr cpu = NULL;
    size_t ncpus = 0;
    char **cpus = NULL;
    const char *default_model;
    virCPUDataPtr data = NULL;
    bool have_cpu = false;
    char *compare_msg = NULL;
    int ret = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;
    virCapsPtr caps = NULL;

    *hasHwVirt = false;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    host = caps->host.cpu;

    if (def->os.arch == VIR_ARCH_I686)
        default_model = "qemu32";
    else
        default_model = "qemu64";

    if (def->cpu &&
        (def->cpu->mode != VIR_CPU_MODE_CUSTOM || def->cpu->model)) {
        virCPUCompareResult cmp;
        const char *preferred;
        int hasSVM;

        if (!host ||
            !host->model ||
            (ncpus = virQEMUCapsGetCPUDefinitions(qemuCaps, &cpus)) == 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("CPU specification not supported by hypervisor"));
            goto cleanup;
        }

        if (!(cpu = virCPUDefCopy(def->cpu)))
            goto cleanup;

        if (cpu->mode != VIR_CPU_MODE_CUSTOM &&
            !migrating &&
            cpuUpdate(cpu, host) < 0)
            goto cleanup;

        cmp = cpuGuestData(host, cpu, &data, &compare_msg);
        switch (cmp) {
        case VIR_CPU_COMPARE_INCOMPATIBLE:
            if (compare_msg) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("guest and host CPU are not compatible: %s"),
                               compare_msg);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("guest CPU is not compatible with host CPU"));
            }
            /* fall through */
        case VIR_CPU_COMPARE_ERROR:
            goto cleanup;

        default:
            break;
        }

        /* Only 'svm' requires --enable-nesting. The nested
         * 'vmx' patches now simply hook off the CPU features
         */
        hasSVM = cpuHasFeature(data, "svm");
        if (hasSVM < 0)
            goto cleanup;
        *hasHwVirt = hasSVM > 0 ? true : false;

        if (cpu->mode == VIR_CPU_MODE_HOST_PASSTHROUGH) {
            const char *mode = virCPUModeTypeToString(cpu->mode);
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CPU_HOST)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("CPU mode '%s' is not supported by QEMU"
                                 " binary"), mode);
                goto cleanup;
            }
            if (def->virtType != VIR_DOMAIN_VIRT_KVM) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("CPU mode '%s' is only supported with kvm"),
                               mode);
                goto cleanup;
            }
            virBufferAddLit(&buf, "host");
        } else {
            if (VIR_ALLOC(guest) < 0)
                goto cleanup;
            if (VIR_STRDUP(guest->vendor_id, cpu->vendor_id) < 0)
                goto cleanup;

            guest->arch = host->arch;
            if (cpu->match == VIR_CPU_MATCH_MINIMUM)
                preferred = host->model;
            else
                preferred = cpu->model;

            guest->type = VIR_CPU_TYPE_GUEST;
            guest->fallback = cpu->fallback;
            if (cpuDecode(guest, data, (const char **)cpus, ncpus, preferred) < 0)
                goto cleanup;

            virBufferAdd(&buf, guest->model, -1);
            if (guest->vendor_id)
                virBufferAsprintf(&buf, ",vendor=%s", guest->vendor_id);
            for (i = 0; i < guest->nfeatures; i++) {
                char sign;
                if (guest->features[i].policy == VIR_CPU_FEATURE_DISABLE)
                    sign = '-';
                else
                    sign = '+';

                virBufferAsprintf(&buf, ",%c%s", sign, guest->features[i].name);
            }
        }
        have_cpu = true;
    } else {
        /*
         * Need to force a 32-bit guest CPU type if
         *
         *  1. guest OS is i686
         *  2. host OS is x86_64
         *  3. emulator is qemu-kvm or kvm
         *
         * Or
         *
         *  1. guest OS is i686
         *  2. emulator is qemu-system-x86_64
         */
        if (def->os.arch == VIR_ARCH_I686 &&
            ((hostarch == VIR_ARCH_X86_64 &&
              strstr(emulator, "kvm")) ||
             strstr(emulator, "x86_64"))) {
            virBufferAdd(&buf, default_model, -1);
            have_cpu = true;
        }
    }

    /* Now force kvmclock on/off based on the corresponding <timer> element.  */
    for (i = 0; i < def->clock.ntimers; i++) {
        if (def->clock.timers[i]->name == VIR_DOMAIN_TIMER_NAME_KVMCLOCK &&
            def->clock.timers[i]->present != -1) {
            char sign;
            if (def->clock.timers[i]->present)
                sign = '+';
            else
                sign = '-';
            virBufferAsprintf(&buf, "%s,%ckvmclock",
                              have_cpu ? "" : default_model,
                              sign);
            have_cpu = true;
            break;
        }
    }

    if (def->apic_eoi) {
        char sign;
        if (def->apic_eoi == VIR_DOMAIN_FEATURE_STATE_ON)
            sign = '+';
        else
            sign = '-';

        virBufferAsprintf(&buf, "%s,%ckvm_pv_eoi",
                          have_cpu ? "" : default_model,
                          sign);
        have_cpu = true;
    }

    if (def->features & (1 << VIR_DOMAIN_FEATURE_HYPERV)) {
        if (!have_cpu) {
            virBufferAdd(&buf, default_model, -1);
            have_cpu = true;
        }

        for (i = 0; i < VIR_DOMAIN_HYPERV_LAST; i++) {
            switch ((enum virDomainHyperv) i) {
            case VIR_DOMAIN_HYPERV_RELAXED:
            case VIR_DOMAIN_HYPERV_VAPIC:
                if (def->hyperv_features[i] == VIR_DOMAIN_FEATURE_STATE_ON)
                    virBufferAsprintf(&buf, ",hv_%s",
                                      virDomainHypervTypeToString(i));
                break;

            case VIR_DOMAIN_HYPERV_SPINLOCKS:
                if (def->hyperv_features[i] == VIR_DOMAIN_FEATURE_STATE_ON)
                    virBufferAsprintf(&buf, ",hv_spinlocks=0x%x",
                                      def->hyperv_spinlocks);
                break;

            case VIR_DOMAIN_HYPERV_LAST:
                break;
            }
        }
    }

    if (virBufferError(&buf)) {
        virReportOOMError();
        goto cleanup;
    }

    *opt = virBufferContentAndReset(&buf);

    ret = 0;

cleanup:
    VIR_FREE(compare_msg);
    cpuDataFree(data);
    virCPUDefFree(guest);
    virCPUDefFree(cpu);
    virObjectUnref(caps);
    return ret;
}

static int
qemuBuildObsoleteAccelArg(virCommandPtr cmd,
                          const virDomainDefPtr def,
                          virQEMUCapsPtr qemuCaps)
{
    bool disableKQEMU = false;
    bool enableKQEMU = false;
    bool disableKVM = false;
    bool enableKVM = false;

    switch (def->virtType) {
    case VIR_DOMAIN_VIRT_QEMU:
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KQEMU))
            disableKQEMU = true;
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM))
            disableKVM = true;
        break;

    case VIR_DOMAIN_VIRT_KQEMU:
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM))
            disableKVM = true;

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_ENABLE_KQEMU)) {
            enableKQEMU = true;
        } else if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_KQEMU)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("the QEMU binary does not support kqemu"));
            return -1;
        }
        break;

    case VIR_DOMAIN_VIRT_KVM:
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KQEMU))
            disableKQEMU = true;

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_ENABLE_KVM)) {
            enableKVM = true;
        } else if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("the QEMU binary does not support kvm"));
            return -1;
        }
        break;

    case VIR_DOMAIN_VIRT_XEN:
        /* XXX better check for xenner */
        break;

    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("the QEMU binary does not support %s"),
                       virDomainVirtTypeToString(def->virtType));
        return -1;
    }

    if (disableKQEMU)
        virCommandAddArg(cmd, "-no-kqemu");
    else if (enableKQEMU)
        virCommandAddArgList(cmd, "-enable-kqemu", "-kernel-kqemu", NULL);
    if (disableKVM)
        virCommandAddArg(cmd, "-no-kvm");
    if (enableKVM)
        virCommandAddArg(cmd, "-enable-kvm");

    return 0;
}

static int
qemuBuildMachineArgStr(virCommandPtr cmd,
                       const virDomainDefPtr def,
                       virQEMUCapsPtr qemuCaps)
{
    bool obsoleteAccel = false;

    /* This should *never* be NULL, since we always provide
     * a machine in the capabilities data for QEMU. So this
     * check is just here as a safety in case the unexpected
     * happens */
    if (!def->os.machine)
        return 0;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_OPT)) {
        /* if no parameter to the machine type is needed, we still use
         * '-M' to keep the most of the compatibility with older versions.
         */
        virCommandAddArgList(cmd, "-M", def->os.machine, NULL);
        if (def->mem.dump_core) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("dump-guest-core is not available "
                             "with this QEMU binary"));
            return -1;
        }

        if (def->mem.nosharepages) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disable shared memory is not available "
                             "with this QEMU binary"));
             return -1;
        }

        obsoleteAccel = true;
    } else {
        virBuffer buf = VIR_BUFFER_INITIALIZER;

        virCommandAddArg(cmd, "-machine");
        virBufferAdd(&buf, def->os.machine, -1);

        if (def->virtType == VIR_DOMAIN_VIRT_QEMU)
            virBufferAddLit(&buf, ",accel=tcg");
        else if (def->virtType == VIR_DOMAIN_VIRT_KVM)
            virBufferAddLit(&buf, ",accel=kvm");
        else
            obsoleteAccel = true;

        /* To avoid the collision of creating USB controllers when calling
         * machine->init in QEMU, it needs to set usb=off
         */
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MACHINE_USB_OPT))
            virBufferAddLit(&buf, ",usb=off");

        if (def->mem.dump_core) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DUMP_GUEST_CORE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("dump-guest-core is not available "
                                 "with this QEMU binary"));
                virBufferFreeAndReset(&buf);
                return -1;
            }

            virBufferAsprintf(&buf, ",dump-guest-core=%s",
                              virDomainMemDumpTypeToString(def->mem.dump_core));
        }

        if (def->mem.nosharepages) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MEM_MERGE)) {
                virBufferAddLit(&buf, ",mem-merge=off");
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("disable shared memory is not available "
                                 "with this QEMU binary"));
                virBufferFreeAndReset(&buf);
                return -1;
            }
        }

        virCommandAddArgBuffer(cmd, &buf);
    }

    if (obsoleteAccel &&
        qemuBuildObsoleteAccelArg(cmd, def, qemuCaps) < 0)
        return -1;

    return 0;
}

static char *
qemuBuildSmpArgStr(const virDomainDefPtr def,
                   virQEMUCapsPtr qemuCaps)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&buf, "%u", def->vcpus);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SMP_TOPOLOGY)) {
        if (def->vcpus != def->maxvcpus)
            virBufferAsprintf(&buf, ",maxcpus=%u", def->maxvcpus);
        /* sockets, cores, and threads are either all zero
         * or all non-zero, thus checking one of them is enough */
        if (def->cpu && def->cpu->sockets) {
            virBufferAsprintf(&buf, ",sockets=%u", def->cpu->sockets);
            virBufferAsprintf(&buf, ",cores=%u", def->cpu->cores);
            virBufferAsprintf(&buf, ",threads=%u", def->cpu->threads);
        }
        else {
            virBufferAsprintf(&buf, ",sockets=%u", def->maxvcpus);
            virBufferAsprintf(&buf, ",cores=%u", 1);
            virBufferAsprintf(&buf, ",threads=%u", 1);
        }
    } else if (def->vcpus != def->maxvcpus) {
        virBufferFreeAndReset(&buf);
        /* FIXME - consider hot-unplugging cpus after boot for older qemu */
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("setting current vcpu count less than maximum is "
                         "not supported with this QEMU binary"));
        return NULL;
    }

    if (virBufferError(&buf)) {
        virBufferFreeAndReset(&buf);
        virReportOOMError();
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}

static int
qemuBuildNumaArgStr(const virDomainDefPtr def, virCommandPtr cmd)
{
    size_t i;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *cpumask = NULL;
    int ret = -1;

    for (i = 0; i < def->cpu->ncells; i++) {
        VIR_FREE(cpumask);
        virCommandAddArg(cmd, "-numa");
        virBufferAsprintf(&buf, "node,nodeid=%d", def->cpu->cells[i].cellid);
        virBufferAddLit(&buf, ",cpus=");
        cpumask = virBitmapFormat(def->cpu->cells[i].cpumask);
        if (cpumask) {
            /* Up through qemu 1.4, -numa does not accept a cpus
             * argument any more complex than start-stop.
             *
             * XXX For qemu 1.5, the syntax has not yet been decided;
             * but when it is, we need a capability bit and
             * translation of our cpumask into the qemu syntax.  */
            if (strchr(cpumask, ',')) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("disjoint NUMA cpu ranges are not supported "
                                 "with this QEMU"));
                goto cleanup;
            }
            virBufferAdd(&buf, cpumask, -1);
        }
        def->cpu->cells[i].mem = VIR_DIV_UP(def->cpu->cells[i].mem,
                                            1024) * 1024;
        virBufferAsprintf(&buf, ",mem=%d", def->cpu->cells[i].mem / 1024);

        if (virBufferError(&buf)) {
            virReportOOMError();
            goto cleanup;
        }

        virCommandAddArgBuffer(cmd, &buf);
    }
    ret = 0;

cleanup:
    VIR_FREE(cpumask);
    virBufferFreeAndReset(&buf);
    return ret;
}


static int
qemuBuildGraphicsVNCCommandLine(virQEMUDriverConfigPtr cfg,
                                virCommandPtr cmd,
                                virDomainDefPtr def,
                                virQEMUCapsPtr qemuCaps,
                                virDomainGraphicsDefPtr graphics)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *listenNetwork;
    const char *listenAddr = NULL;
    char *netAddr = NULL;
    bool escapeAddr;
    int ret;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("vnc graphics are not supported with this QEMU"));
        goto error;
    }

    if (graphics->data.vnc.socket || cfg->vncAutoUnixSocket) {
        if (!graphics->data.vnc.socket &&
            virAsprintf(&graphics->data.vnc.socket,
                        "%s/%s.vnc", cfg->libDir, def->name) == -1)
            goto error;

        virBufferAsprintf(&opt, "unix:%s", graphics->data.vnc.socket);

    } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_COLON)) {
        switch (virDomainGraphicsListenGetType(graphics, 0)) {
        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
            listenAddr = virDomainGraphicsListenGetAddress(graphics, 0);
            break;

        case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
            listenNetwork = virDomainGraphicsListenGetNetwork(graphics, 0);
            if (!listenNetwork)
                break;
            ret = networkGetNetworkAddress(listenNetwork, &netAddr);
            if (ret <= -2) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("network-based listen not possible, "
                                       "network driver not present"));
                goto error;
            }
            if (ret < 0) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("listen network '%s' had no usable address"),
                               listenNetwork);
                goto error;
            }
            listenAddr = netAddr;
            /* store the address we found in the <graphics> element so it will
             * show up in status. */
            if (virDomainGraphicsListenSetAddress(graphics, 0,
                                                  listenAddr, -1, false) < 0)
               goto error;
            break;
        }

        if (!listenAddr)
            listenAddr = cfg->vncListen;

        escapeAddr = strchr(listenAddr, ':') != NULL;
        if (escapeAddr)
            virBufferAsprintf(&opt, "[%s]", listenAddr);
        else
            virBufferAdd(&opt, listenAddr, -1);
        virBufferAsprintf(&opt, ":%d",
                          graphics->data.vnc.port - 5900);

        VIR_FREE(netAddr);
    } else {
        virBufferAsprintf(&opt, "%d",
                          graphics->data.vnc.port - 5900);
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_COLON)) {
        if (!graphics->data.vnc.socket &&
            graphics->data.vnc.websocket) {
                if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_WEBSOCKET)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("VNC WebSockets are not supported "
                                     "with this QEMU binary"));
                    goto error;
                }
                virBufferAsprintf(&opt, ",websocket=%d", graphics->data.vnc.websocket);
            }

        if (graphics->data.vnc.sharePolicy) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VNC_SHARE_POLICY)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("vnc display sharing policy is not "
                                 "supported with this QEMU"));
                goto error;
            }

            virBufferAsprintf(&opt, ",share=%s",
                              virDomainGraphicsVNCSharePolicyTypeToString(
                              graphics->data.vnc.sharePolicy));
        }

        if (graphics->data.vnc.auth.passwd || cfg->vncPassword)
            virBufferAddLit(&opt, ",password");

        if (cfg->vncTLS) {
            virBufferAddLit(&opt, ",tls");
            if (cfg->vncTLSx509verify)
                virBufferAsprintf(&opt, ",x509verify=%s", cfg->vncTLSx509certdir);
            else
                virBufferAsprintf(&opt, ",x509=%s", cfg->vncTLSx509certdir);
        }

        if (cfg->vncSASL) {
            virBufferAddLit(&opt, ",sasl");

            if (cfg->vncSASLdir)
                virCommandAddEnvPair(cmd, "SASL_CONF_DIR", cfg->vncSASLdir);

            /* TODO: Support ACLs later */
        }
    }

    virCommandAddArg(cmd, "-vnc");
    virCommandAddArgBuffer(cmd, &opt);
    if (graphics->data.vnc.keymap)
        virCommandAddArgList(cmd, "-k", graphics->data.vnc.keymap, NULL);

    /* Unless user requested it, set the audio backend to none, to
     * prevent it opening the host OS audio devices, since that causes
     * security issues and might not work when using VNC.
     */
    if (cfg->vncAllowHostAudio)
        virCommandAddEnvPass(cmd, "QEMU_AUDIO_DRV");
    else
        virCommandAddEnvString(cmd, "QEMU_AUDIO_DRV=none");

    return 0;

error:
    VIR_FREE(netAddr);
    virBufferFreeAndReset(&opt);
    return -1;
}


static int
qemuBuildGraphicsSPICECommandLine(virQEMUDriverConfigPtr cfg,
                                  virCommandPtr cmd,
                                  virQEMUCapsPtr qemuCaps,
                                  virDomainGraphicsDefPtr graphics)
{
    virBuffer opt = VIR_BUFFER_INITIALIZER;
    const char *listenNetwork;
    const char *listenAddr = NULL;
    char *netAddr = NULL;
    int ret;
    int defaultMode = graphics->data.spice.defaultMode;
    int port = graphics->data.spice.port;
    int tlsPort = graphics->data.spice.tlsPort;
    size_t i;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPICE)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("spice graphics are not supported with this QEMU"));
        goto error;
    }

    if (port > 0 || tlsPort <= 0)
        virBufferAsprintf(&opt, "port=%u", port);

    if (tlsPort > 0) {
        if (!cfg->spiceTLS) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("spice TLS port set in XML configuration,"
                             " but TLS is disabled in qemu.conf"));
            goto error;
        }
        if (port > 0)
            virBufferAddChar(&opt, ',');
        virBufferAsprintf(&opt, "tls-port=%u", tlsPort);
    }

    switch (virDomainGraphicsListenGetType(graphics, 0)) {
    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
        listenAddr = virDomainGraphicsListenGetAddress(graphics, 0);
        break;

    case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
        listenNetwork = virDomainGraphicsListenGetNetwork(graphics, 0);
        if (!listenNetwork)
            break;
        ret = networkGetNetworkAddress(listenNetwork, &netAddr);
        if (ret <= -2) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("network-based listen not possible, "
                                   "network driver not present"));
            goto error;
        }
        if (ret < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("listen network '%s' had no usable address"),
                           listenNetwork);
            goto error;
        }
        listenAddr = netAddr;
        /* store the address we found in the <graphics> element so it will
         * show up in status. */
        if (virDomainGraphicsListenSetAddress(graphics, 0,
                                              listenAddr, -1, false) < 0)
           goto error;
        break;
    }

    if (!listenAddr)
        listenAddr = cfg->spiceListen;
    if (listenAddr)
        virBufferAsprintf(&opt, ",addr=%s", listenAddr);

    VIR_FREE(netAddr);

    if (graphics->data.spice.mousemode) {
        switch (graphics->data.spice.mousemode) {
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_SERVER:
            virBufferAddLit(&opt, ",agent-mouse=off");
            break;
        case VIR_DOMAIN_GRAPHICS_SPICE_MOUSE_MODE_CLIENT:
            virBufferAddLit(&opt, ",agent-mouse=on");
            break;
        default:
            break;
        }
    }

    /* In the password case we set it via monitor command, to avoid
     * making it visible on CLI, so there's no use of password=XXX
     * in this bit of the code */
    if (!graphics->data.spice.auth.passwd &&
        !cfg->spicePassword)
        virBufferAddLit(&opt, ",disable-ticketing");

    if (tlsPort > 0)
        virBufferAsprintf(&opt, ",x509-dir=%s", cfg->spiceTLSx509certdir);

    switch (defaultMode) {
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
        virBufferAddLit(&opt, ",tls-channel=default");
        break;
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
        virBufferAddLit(&opt, ",plaintext-channel=default");
        break;
    case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
        /* nothing */
        break;
    }

    for (i = 0; i < VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_LAST; i++) {
        switch (graphics->data.spice.channels[i]) {
        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
            if (tlsPort <= 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice secure channels set in XML configuration, "
                                 "but TLS port is not provided"));
                goto error;
            }
            virBufferAsprintf(&opt, ",tls-channel=%s",
                              virDomainGraphicsSpiceChannelNameTypeToString(i));
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
            if (port <= 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("spice insecure channels set in XML "
                                 "configuration, but plain port is not provided"));
                goto error;
            }
            virBufferAsprintf(&opt, ",plaintext-channel=%s",
                              virDomainGraphicsSpiceChannelNameTypeToString(i));
            break;

        case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
            switch (defaultMode) {
            case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_SECURE:
                if (tlsPort <= 0) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("spice defaultMode secure requested in XML "
                                     "configuration but TLS port not provided"));
                    goto error;
                }
                break;

            case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_INSECURE:
                if (port <= 0) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("spice defaultMode insecure requested in XML "
                                     "configuration but plain port not provided"));
                    goto error;
                }
                break;

            case VIR_DOMAIN_GRAPHICS_SPICE_CHANNEL_MODE_ANY:
                /* don't care */
            break;
            }
        }
    }

    if (graphics->data.spice.image)
        virBufferAsprintf(&opt, ",image-compression=%s",
                          virDomainGraphicsSpiceImageCompressionTypeToString(graphics->data.spice.image));
    if (graphics->data.spice.jpeg)
        virBufferAsprintf(&opt, ",jpeg-wan-compression=%s",
                          virDomainGraphicsSpiceJpegCompressionTypeToString(graphics->data.spice.jpeg));
    if (graphics->data.spice.zlib)
        virBufferAsprintf(&opt, ",zlib-glz-wan-compression=%s",
                          virDomainGraphicsSpiceZlibCompressionTypeToString(graphics->data.spice.zlib));
    if (graphics->data.spice.playback)
        virBufferAsprintf(&opt, ",playback-compression=%s",
                          virDomainGraphicsSpicePlaybackCompressionTypeToString(graphics->data.spice.playback));
    if (graphics->data.spice.streaming)
        virBufferAsprintf(&opt, ",streaming-video=%s",
                          virDomainGraphicsSpiceStreamingModeTypeToString(graphics->data.spice.streaming));
    if (graphics->data.spice.copypaste == VIR_DOMAIN_GRAPHICS_SPICE_CLIPBOARD_COPYPASTE_NO)
        virBufferAddLit(&opt, ",disable-copy-paste");

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SEAMLESS_MIGRATION)) {
        /* If qemu supports seamless migration turn it
         * unconditionally on. If migration destination
         * doesn't support it, it fallbacks to previous
         * migration algorithm silently. */
        virBufferAddLit(&opt, ",seamless-migration=on");
    }

    virCommandAddArg(cmd, "-spice");
    virCommandAddArgBuffer(cmd, &opt);
    if (graphics->data.spice.keymap)
        virCommandAddArgList(cmd, "-k",
                             graphics->data.spice.keymap, NULL);
    /* SPICE includes native support for tunnelling audio, so we
     * set the audio backend to point at SPICE's own driver
     */
    virCommandAddEnvString(cmd, "QEMU_AUDIO_DRV=spice");

    return 0;

error:
    VIR_FREE(netAddr);
    virBufferFreeAndReset(&opt);
    return -1;
}

static int
qemuBuildGraphicsCommandLine(virQEMUDriverConfigPtr cfg,
                             virCommandPtr cmd,
                             virDomainDefPtr def,
                             virQEMUCapsPtr qemuCaps,
                             virDomainGraphicsDefPtr graphics)
{
    switch ((enum virDomainGraphicsType) graphics->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_0_10) &&
            !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SDL)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("sdl not supported by '%s'"), def->emulator);
            return -1;
        }

        if (graphics->data.sdl.xauth)
            virCommandAddEnvPair(cmd, "XAUTHORITY", graphics->data.sdl.xauth);
        if (graphics->data.sdl.display)
            virCommandAddEnvPair(cmd, "DISPLAY", graphics->data.sdl.display);
        if (graphics->data.sdl.fullscreen)
            virCommandAddArg(cmd, "-full-screen");

        /* If using SDL for video, then we should just let it
         * use QEMU's host audio drivers, possibly SDL too
         * User can set these two before starting libvirtd
         */
        virCommandAddEnvPass(cmd, "QEMU_AUDIO_DRV");
        virCommandAddEnvPass(cmd, "SDL_AUDIODRIVER");

        /* New QEMU has this flag to let us explicitly ask for
         * SDL graphics. This is better than relying on the
         * default, since the default changes :-( */
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SDL))
            virCommandAddArg(cmd, "-sdl");

        break;

    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        return qemuBuildGraphicsVNCCommandLine(cfg, cmd, def, qemuCaps, graphics);

    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        return qemuBuildGraphicsSPICECommandLine(cfg, cmd, qemuCaps, graphics);

    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
    case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported graphics type '%s'"),
                       virDomainGraphicsTypeToString(graphics->type));
        return -1;
    }

    return 0;
}

static int
qemuBuildInterfaceCommandLine(virCommandPtr cmd,
                              virQEMUDriverPtr driver,
                              virConnectPtr conn,
                              virDomainDefPtr def,
                              virDomainNetDefPtr net,
                              virQEMUCapsPtr qemuCaps,
                              int vlan,
                              int bootindex,
                              enum virNetDevVPortProfileOp vmop)
{
    int ret = -1;
    char *nic = NULL, *host = NULL;
    int *tapfd = NULL;
    int tapfdSize = 0;
    int *vhostfd = NULL;
    int vhostfdSize = 0;
    char **tapfdName = NULL;
    char **vhostfdName = NULL;
    int actualType = virDomainNetGetActualType(net);
    size_t i;

    if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV) {
        /* NET_TYPE_HOSTDEV devices are really hostdev devices, so
         * their commandlines are constructed with other hostdevs.
         */
        return 0;
    }

    if (!bootindex)
        bootindex = net->info.bootIndex;

    /* Currently nothing besides TAP devices supports multiqueue. */
    if (net->driver.virtio.queues > 0 &&
        !(actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
          actualType == VIR_DOMAIN_NET_TYPE_BRIDGE)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Multiqueue network is not supported for: %s"),
                       virDomainNetTypeToString(actualType));
        return -1;
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
        actualType == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        tapfdSize = net->driver.virtio.queues;
        if (!tapfdSize)
            tapfdSize = 1;

        if (VIR_ALLOC_N(tapfd, tapfdSize) < 0 ||
            VIR_ALLOC_N(tapfdName, tapfdSize) < 0)
            goto cleanup;

        if (qemuNetworkIfaceConnect(def, conn, driver, net,
                                    qemuCaps, tapfd, &tapfdSize) < 0)
            goto cleanup;
    } else if (actualType == VIR_DOMAIN_NET_TYPE_DIRECT) {
        if (VIR_ALLOC(tapfd) < 0 || VIR_ALLOC(tapfdName) < 0)
            goto cleanup;
        tapfdSize = 1;
        tapfd[0] = qemuPhysIfaceConnect(def, driver, net,
                                        qemuCaps, vmop);
        if (tapfd[0] < 0)
            goto cleanup;
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK ||
        actualType == VIR_DOMAIN_NET_TYPE_BRIDGE ||
        actualType == VIR_DOMAIN_NET_TYPE_ETHERNET ||
        actualType == VIR_DOMAIN_NET_TYPE_DIRECT) {
        /* Attempt to use vhost-net mode for these types of
           network device */
        vhostfdSize = net->driver.virtio.queues;
        if (!vhostfdSize)
            vhostfdSize = 1;

        if (VIR_ALLOC_N(vhostfd, vhostfdSize) < 0 ||
            VIR_ALLOC_N(vhostfdName, vhostfdSize))
            goto cleanup;

        if (qemuOpenVhostNet(def, net, qemuCaps, vhostfd, &vhostfdSize) < 0)
            goto cleanup;
    }

    for (i = 0; i < tapfdSize; i++) {
        virCommandPassFD(cmd, tapfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        if (virAsprintf(&tapfdName[i], "%d", tapfd[i]) < 0)
            goto cleanup;
    }

    for (i = 0; i < vhostfdSize; i++) {
        virCommandPassFD(cmd, vhostfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        if (virAsprintf(&vhostfdName[i], "%d", vhostfd[i]) < 0)
            goto cleanup;
    }

    /* Possible combinations:
     *
     *  1. Old way:   -net nic,model=e1000,vlan=1 -net tap,vlan=1
     *  2. Semi-new:  -device e1000,vlan=1        -net tap,vlan=1
     *  3. Best way:  -netdev type=tap,id=netdev1 -device e1000,id=netdev1
     *
     * NB, no support for -netdev without use of -device
     */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NETDEV) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        if (!(host = qemuBuildHostNetStr(net, driver,
                                         ',', vlan,
                                         tapfdName, tapfdSize,
                                         vhostfdName, vhostfdSize)))
            goto cleanup;
        virCommandAddArgList(cmd, "-netdev", host, NULL);
    }
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        if (!(nic = qemuBuildNicDevStr(net, vlan, bootindex, qemuCaps)))
            goto cleanup;
        virCommandAddArgList(cmd, "-device", nic, NULL);
    } else {
        if (!(nic = qemuBuildNicStr(net, "nic,", vlan)))
            goto cleanup;
        virCommandAddArgList(cmd, "-net", nic, NULL);
    }
    if (!(virQEMUCapsGet(qemuCaps, QEMU_CAPS_NETDEV) &&
          virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))) {
        if (!(host = qemuBuildHostNetStr(net, driver,
                                         ',', vlan,
                                         tapfdName, tapfdSize,
                                         vhostfdName, vhostfdSize)))
            goto cleanup;
        virCommandAddArgList(cmd, "-net", host, NULL);
    }

    ret = 0;
cleanup:
    if (ret < 0)
        virDomainConfNWFilterTeardown(net);
    for (i = 0; tapfd && i < tapfdSize; i++) {
        if (ret < 0)
            VIR_FORCE_CLOSE(tapfd[i]);
        if (tapfdName)
            VIR_FREE(tapfdName[i]);
    }
    for (i = 0; vhostfd && i < vhostfdSize; i++) {
        if (ret < 0)
            VIR_FORCE_CLOSE(vhostfd[i]);
        if (vhostfdName)
            VIR_FREE(vhostfdName[i]);
    }
    VIR_FREE(tapfd);
    VIR_FREE(vhostfd);
    VIR_FREE(nic);
    VIR_FREE(host);
    VIR_FREE(tapfdName);
    VIR_FREE(vhostfdName);
    return ret;
}

static int
qemuBuildChrDeviceCommandLine(virCommandPtr cmd,
                              virDomainDefPtr def,
                              virDomainChrDefPtr chr,
                              virQEMUCapsPtr qemuCaps)
{
    char *devstr = NULL;

    if (qemuBuildChrDeviceStr(&devstr, def, chr, qemuCaps) < 0)
        return -1;

    virCommandAddArgList(cmd, "-device", devstr, NULL);
    VIR_FREE(devstr);
    return 0;
}

qemuBuildCommandLineCallbacks buildCommandLineCallbacks = {
    .qemuGetSCSIDeviceSgName = virSCSIDeviceGetSgName,
};

/*
 * Constructs a argv suitable for launching qemu with config defined
 * for a given virtual machine.
 *
 * XXX 'conn' is only required to resolve network -> bridge name
 * figure out how to remove this requirement some day
 */
virCommandPtr
qemuBuildCommandLine(virConnectPtr conn,
                     virQEMUDriverPtr driver,
                     virDomainDefPtr def,
                     virDomainChrSourceDefPtr monitor_chr,
                     bool monitor_json,
                     virQEMUCapsPtr qemuCaps,
                     const char *migrateFrom,
                     int migrateFd,
                     virDomainSnapshotObjPtr snapshot,
                     enum virNetDevVPortProfileOp vmop,
                     qemuBuildCommandLineCallbacksPtr callbacks)
{
    virErrorPtr originalError = NULL;
    size_t i, j;
    const char *emulator;
    char uuid[VIR_UUID_STRING_BUFLEN];
    char *cpu;
    char *smp;
    int last_good_net = -1;
    bool hasHwVirt = false;
    virCommandPtr cmd = NULL;
    bool allowReboot = true;
    bool emitBootindex = false;
    int sdl = 0;
    int vnc = 0;
    int spice = 0;
    int usbcontroller = 0;
    bool usblegacy = false;
    bool mlock = false;
    int contOrder[] = {
        /* We don't add an explicit IDE or FD controller because the
         * provided PIIX4 device already includes one. It isn't possible to
         * remove the PIIX4.
         *
         * We don't add PCI root controller either, because it's implicit,
         * but we do add PCI bridges. */
        VIR_DOMAIN_CONTROLLER_TYPE_PCI,
        VIR_DOMAIN_CONTROLLER_TYPE_USB,
        VIR_DOMAIN_CONTROLLER_TYPE_SCSI,
        VIR_DOMAIN_CONTROLLER_TYPE_SATA,
        VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL,
        VIR_DOMAIN_CONTROLLER_TYPE_CCID,
    };
    virArch hostarch = virArchFromHost();
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    VIR_DEBUG("conn=%p driver=%p def=%p mon=%p json=%d "
              "qemuCaps=%p migrateFrom=%s migrateFD=%d "
              "snapshot=%p vmop=%d",
              conn, driver, def, monitor_chr, monitor_json,
              qemuCaps, migrateFrom, migrateFd, snapshot, vmop);

    virUUIDFormat(def->uuid, uuid);

    emulator = def->emulator;

    /*
     * do not use boot=on for drives when not using KVM since this
     * is not supported at all in upstream QEmu.
     */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM) &&
        (def->virtType == VIR_DOMAIN_VIRT_QEMU))
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_DRIVE_BOOT);

    cmd = virCommandNew(emulator);

    virCommandAddEnvPassCommon(cmd);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NAME)) {
        virCommandAddArg(cmd, "-name");
        if (cfg->setProcessName &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_NAME_PROCESS)) {
            virCommandAddArgFormat(cmd, "%s,process=qemu:%s",
                                   def->name, def->name);
        } else {
            virCommandAddArg(cmd, def->name);
        }
    }
    virCommandAddArg(cmd, "-S"); /* freeze CPU */

    if (qemuBuildMachineArgStr(cmd, def, qemuCaps) < 0)
        goto error;

    if (qemuBuildCpuArgStr(driver, def, emulator, qemuCaps,
                           hostarch, &cpu, &hasHwVirt, !!migrateFrom) < 0)
        goto error;

    if (cpu) {
        virCommandAddArgList(cmd, "-cpu", cpu, NULL);
        VIR_FREE(cpu);

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NESTING) &&
            hasHwVirt)
            virCommandAddArg(cmd, "-enable-nesting");
    }

    if (def->os.loader) {
        virCommandAddArg(cmd, "-bios");
        virCommandAddArg(cmd, def->os.loader);
    }

    /* Set '-m MB' based on maxmem, because the lower 'memory' limit
     * is set post-startup using the balloon driver. If balloon driver
     * is not supported, then they're out of luck anyway.  Update the
     * XML to reflect our rounding.
     */
    virCommandAddArg(cmd, "-m");
    def->mem.max_balloon = VIR_DIV_UP(def->mem.max_balloon, 1024) * 1024;
    virCommandAddArgFormat(cmd, "%llu", def->mem.max_balloon / 1024);
    if (def->mem.hugepage_backed) {
        if (!cfg->hugetlbfsMount) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("hugetlbfs filesystem is not mounted"));
            goto error;
        }
        if (!cfg->hugepagePath) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("hugepages are disabled by administrator config"));
            goto error;
        }
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MEM_PATH)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("hugepage backing not supported by '%s'"),
                           def->emulator);
            goto error;
        }
        virCommandAddArgList(cmd, "-mem-prealloc", "-mem-path",
                             cfg->hugepagePath, NULL);
    }

    if (def->mem.locked && !virQEMUCapsGet(qemuCaps, QEMU_CAPS_MLOCK)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("memory locking not supported by QEMU binary"));
        goto error;
    }
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MLOCK)) {
        virCommandAddArg(cmd, "-realtime");
        virCommandAddArgFormat(cmd, "mlock=%s",
                               def->mem.locked ? "on" : "off");
    }
    mlock = def->mem.locked;

    virCommandAddArg(cmd, "-smp");
    if (!(smp = qemuBuildSmpArgStr(def, qemuCaps)))
        goto error;
    virCommandAddArg(cmd, smp);
    VIR_FREE(smp);

    if (def->cpu && def->cpu->ncells)
        if (qemuBuildNumaArgStr(def, cmd) < 0)
            goto error;

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_UUID))
        virCommandAddArgList(cmd, "-uuid", uuid, NULL);
    if (def->virtType == VIR_DOMAIN_VIRT_XEN ||
        STREQ(def->os.type, "xen") ||
        STREQ(def->os.type, "linux")) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DOMID)) {
            virCommandAddArg(cmd, "-domid");
            virCommandAddArgFormat(cmd, "%d", def->id);
        } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_XEN_DOMID)) {
            virCommandAddArg(cmd, "-xen-attach");
            virCommandAddArg(cmd, "-xen-domid");
            virCommandAddArgFormat(cmd, "%d", def->id);
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("qemu emulator '%s' does not support xen"),
                           def->emulator);
            goto error;
        }
    }

    if ((def->os.smbios_mode != VIR_DOMAIN_SMBIOS_NONE) &&
        (def->os.smbios_mode != VIR_DOMAIN_SMBIOS_EMULATE)) {
        virSysinfoDefPtr source = NULL;
        bool skip_uuid = false;

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SMBIOS_TYPE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("the QEMU binary %s does not support smbios settings"),
                           emulator);
            goto error;
        }

        /* should we really error out or just warn in those cases ? */
        if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_HOST) {
            if (driver->hostsysinfo == NULL) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Host SMBIOS information is not available"));
                goto error;
            }
            source = driver->hostsysinfo;
            /* Host and guest uuid must differ, by definition of UUID. */
            skip_uuid = true;
        } else if (def->os.smbios_mode == VIR_DOMAIN_SMBIOS_SYSINFO) {
            if (def->sysinfo == NULL) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("Domain '%s' sysinfo are not available"),
                               def->name);
                goto error;
            }
            source = def->sysinfo;
            /* domain_conf guaranteed that system_uuid matches guest uuid. */
        }
        if (source != NULL) {
            char *smbioscmd;

            smbioscmd = qemuBuildSmbiosBiosStr(source);
            if (smbioscmd != NULL) {
                virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
                VIR_FREE(smbioscmd);
            }
            smbioscmd = qemuBuildSmbiosSystemStr(source, skip_uuid);
            if (smbioscmd != NULL) {
                virCommandAddArgList(cmd, "-smbios", smbioscmd, NULL);
                VIR_FREE(smbioscmd);
            }
        }
    }

    /*
     * NB, -nographic *MUST* come before any serial, or monitor
     * or parallel port flags due to QEMU craziness, where it
     * decides to change the serial port & monitor to be on stdout
     * if you ask for nographic. So we have to make sure we override
     * these defaults ourselves...
     */
    if (!def->graphics)
        virCommandAddArg(cmd, "-nographic");

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        /* Disable global config files and default devices */
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG))
            virCommandAddArg(cmd, "-no-user-config");
        else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NODEFCONFIG))
            virCommandAddArg(cmd, "-nodefconfig");
        virCommandAddArg(cmd, "-nodefaults");
    }

    /* Serial graphics adapter */
    if (def->os.bios.useserial == VIR_DOMAIN_BIOS_USESERIAL_YES) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("qemu does not support -device"));
            goto error;
        }
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SGA)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("qemu does not support SGA"));
            goto error;
        }
        if (!def->nserials) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("need at least one serial port to use SGA"));
            goto error;
        }
        virCommandAddArgList(cmd, "-device", "sga", NULL);
    }

    if (monitor_chr) {
        char *chrdev;
        /* Use -chardev if it's available */
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV)) {

            virCommandAddArg(cmd, "-chardev");
            if (!(chrdev = qemuBuildChrChardevStr(monitor_chr, "monitor",
                                                  qemuCaps)))
                goto error;
            virCommandAddArg(cmd, chrdev);
            VIR_FREE(chrdev);

            virCommandAddArg(cmd, "-mon");
            virCommandAddArgFormat(cmd,
                                   "chardev=charmonitor,id=monitor,mode=%s",
                                   monitor_json ? "control" : "readline");
        } else {
            const char *prefix = NULL;
            if (monitor_json)
                prefix = "control,";

            virCommandAddArg(cmd, "-monitor");
            if (!(chrdev = qemuBuildChrArgStr(monitor_chr, prefix)))
                goto error;
            virCommandAddArg(cmd, chrdev);
            VIR_FREE(chrdev);
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_RTC)) {
        const char *rtcopt;
        virCommandAddArg(cmd, "-rtc");
        if (!(rtcopt = qemuBuildClockArgStr(&def->clock)))
            goto error;
        virCommandAddArg(cmd, rtcopt);
        VIR_FREE(rtcopt);
    } else {
        switch (def->clock.offset) {
        case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
        case VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE:
            virCommandAddArg(cmd, "-localtime");
            break;

        case VIR_DOMAIN_CLOCK_OFFSET_UTC:
            /* Nothing, its the default */
            break;

        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported clock offset '%s'"),
                           virDomainClockOffsetTypeToString(def->clock.offset));
            goto error;
        }
    }
    if (def->clock.offset == VIR_DOMAIN_CLOCK_OFFSET_TIMEZONE &&
        def->clock.data.timezone) {
        virCommandAddEnvPair(cmd, "TZ", def->clock.data.timezone);
    }

    for (i = 0; i < def->clock.ntimers; i++) {
        switch (def->clock.timers[i]->name) {
        default:
        case VIR_DOMAIN_TIMER_NAME_PLATFORM:
        case VIR_DOMAIN_TIMER_NAME_TSC:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported timer type (name) '%s'"),
                           virDomainTimerNameTypeToString(def->clock.timers[i]->name));
            goto error;

        case VIR_DOMAIN_TIMER_NAME_KVMCLOCK:
            /* This is handled when building -cpu.  */
            break;

        case VIR_DOMAIN_TIMER_NAME_RTC:
            /* This has already been taken care of (in qemuBuildClockArgStr)
               if QEMU_CAPS_RTC is set (mutually exclusive with
               QEMUD_FLAG_RTC_TD_HACK) */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_RTC_TD_HACK)) {
                switch (def->clock.timers[i]->tickpolicy) {
                case -1:
                case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                    /* the default - do nothing */
                    break;
                case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                    virCommandAddArg(cmd, "-rtc-td-hack");
                    break;
                case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
                case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("unsupported rtc tickpolicy '%s'"),
                                   virDomainTimerTickpolicyTypeToString(def->clock.timers[i]->tickpolicy));
                    goto error;
                }
            } else if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_RTC)
                       && (def->clock.timers[i]->tickpolicy
                           != VIR_DOMAIN_TIMER_TICKPOLICY_DELAY)
                       && (def->clock.timers[i]->tickpolicy != -1)) {
                /* a non-default rtc policy was given, but there is no
                   way to implement it in this version of qemu */
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported rtc tickpolicy '%s'"),
                               virDomainTimerTickpolicyTypeToString(def->clock.timers[i]->tickpolicy));
                goto error;
            }
            break;

        case VIR_DOMAIN_TIMER_NAME_PIT:
            switch (def->clock.timers[i]->tickpolicy) {
            case -1:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DELAY:
                /* delay is the default if we don't have kernel
                   (-no-kvm-pit), otherwise, the default is catchup. */
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_KVM_PIT))
                    virCommandAddArg(cmd, "-no-kvm-pit-reinjection");
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_CATCHUP:
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_KVM_PIT)) {
                    /* do nothing - this is default for kvm-pit */
                } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_TDF)) {
                    /* -tdf switches to 'catchup' with userspace pit. */
                    virCommandAddArg(cmd, "-tdf");
                } else {
                    /* can't catchup if we have neither pit mode */
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("unsupported pit tickpolicy '%s'"),
                                   virDomainTimerTickpolicyTypeToString(def->clock.timers[i]->tickpolicy));
                    goto error;
                }
                break;
            case VIR_DOMAIN_TIMER_TICKPOLICY_MERGE:
            case VIR_DOMAIN_TIMER_TICKPOLICY_DISCARD:
                /* no way to support these modes for pit in qemu */
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported pit tickpolicy '%s'"),
                               virDomainTimerTickpolicyTypeToString(def->clock.timers[i]->tickpolicy));
                goto error;
            }
            break;

        case VIR_DOMAIN_TIMER_NAME_HPET:
            /* the only meaningful attribute for hpet is "present". If
             * present is -1, that means it wasn't specified, and
             * should be left at the default for the
             * hypervisor. "default" when -no-hpet exists is "yes",
             * and when -no-hpet doesn't exist is "no". "confusing"?
             * "yes"! */

            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_HPET)) {
                if (def->clock.timers[i]->present == 0)
                    virCommandAddArg(cmd, "-no-hpet");
            } else {
                /* no hpet timer available. The only possible action
                   is to raise an error if present="yes" */
                if (def->clock.timers[i]->present == 1) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   "%s", _("pit timer is not supported"));
                }
            }
            break;
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_REBOOT)) {
        /* Only add -no-reboot option if each event destroys domain */
        if (def->onReboot == VIR_DOMAIN_LIFECYCLE_DESTROY &&
            def->onPoweroff == VIR_DOMAIN_LIFECYCLE_DESTROY &&
            def->onCrash == VIR_DOMAIN_LIFECYCLE_DESTROY) {
            allowReboot = false;
            virCommandAddArg(cmd, "-no-reboot");
        }
    }

    /* If JSON monitor is enabled, we can receive an event
     * when QEMU stops. If we use no-shutdown, then we can
     * watch for this event and do a soft/warm reboot.
     */
    if (monitor_json && allowReboot &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_SHUTDOWN)) {
        virCommandAddArg(cmd, "-no-shutdown");
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_ACPI)) {
        if (!(def->features & (1 << VIR_DOMAIN_FEATURE_ACPI)))
            virCommandAddArg(cmd, "-no-acpi");
    }

    if (def->pm.s3) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DISABLE_S3)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("setting ACPI S3 not supported"));
            goto error;
        }
        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "PIIX4_PM.disable_s3=%d",
                               def->pm.s3 == VIR_DOMAIN_PM_STATE_DISABLED);
    }

    if (def->pm.s4) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DISABLE_S4)) {
         virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("setting ACPI S4 not supported"));
            goto error;
        }
        virCommandAddArg(cmd, "-global");
        virCommandAddArgFormat(cmd, "PIIX4_PM.disable_s4=%d",
                               def->pm.s4 == VIR_DOMAIN_PM_STATE_DISABLED);
    }

    if (!def->os.bootloader) {
        int boot_nparams = 0;
        virBuffer boot_buf = VIR_BUFFER_INITIALIZER;
        /*
         * We prefer using explicit bootindex=N parameters for predictable
         * results even though domain XML doesn't use per device boot elements.
         * However, we can't use bootindex if boot menu was requested.
         */
        if (!def->os.nBootDevs) {
            /* def->os.nBootDevs is guaranteed to be > 0 unless per-device boot
             * configuration is used
             */
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOTINDEX)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("hypervisor lacks deviceboot feature"));
                goto error;
            }
            emitBootindex = true;
        } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOTINDEX) &&
                   (def->os.bootmenu != VIR_DOMAIN_BOOT_MENU_ENABLED ||
                    !virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOT_MENU))) {
            emitBootindex = true;
        }

        if (!emitBootindex) {
            char boot[VIR_DOMAIN_BOOT_LAST+1];

            for (i = 0; i < def->os.nBootDevs; i++) {
                switch (def->os.bootDevs[i]) {
                case VIR_DOMAIN_BOOT_CDROM:
                    boot[i] = 'd';
                    break;
                case VIR_DOMAIN_BOOT_FLOPPY:
                    boot[i] = 'a';
                    break;
                case VIR_DOMAIN_BOOT_DISK:
                    boot[i] = 'c';
                    break;
                case VIR_DOMAIN_BOOT_NET:
                    boot[i] = 'n';
                    break;
                default:
                    boot[i] = 'c';
                    break;
                }
            }
            boot[def->os.nBootDevs] = '\0';

            virBufferAsprintf(&boot_buf, "%s", boot);
            boot_nparams++;
        }

        if (def->os.bootmenu) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BOOT_MENU)) {
                if (boot_nparams++)
                    virBufferAddChar(&boot_buf, ',');

                if (def->os.bootmenu == VIR_DOMAIN_BOOT_MENU_ENABLED)
                    virBufferAddLit(&boot_buf, "menu=on");
                else
                    virBufferAddLit(&boot_buf, "menu=off");
            } else {
                /* We cannot emit an error when bootmenu is enabled but
                 * unsupported because of backward compatibility */
                VIR_WARN("bootmenu is enabled but not "
                         "supported by this QEMU binary");
            }
        }

        if (def->os.bios.rt_set) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_REBOOT_TIMEOUT)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("reboot timeout is not supported "
                                 "by this QEMU binary"));
                virBufferFreeAndReset(&boot_buf);
                goto error;
            }

            if (boot_nparams++)
                virBufferAddChar(&boot_buf, ',');

            virBufferAsprintf(&boot_buf,
                              "reboot-timeout=%d",
                              def->os.bios.rt_delay);
        }

        if (boot_nparams > 0) {
            virCommandAddArg(cmd, "-boot");

            if (boot_nparams < 2 || emitBootindex) {
                virCommandAddArgBuffer(cmd, &boot_buf);
                virBufferFreeAndReset(&boot_buf);
            } else {
                char *str = virBufferContentAndReset(&boot_buf);
                virCommandAddArgFormat(cmd,
                                       "order=%s",
                                       str);
                VIR_FREE(str);
            }
        }

        if (def->os.kernel)
            virCommandAddArgList(cmd, "-kernel", def->os.kernel, NULL);
        if (def->os.initrd)
            virCommandAddArgList(cmd, "-initrd", def->os.initrd, NULL);
        if (def->os.cmdline)
            virCommandAddArgList(cmd, "-append", def->os.cmdline, NULL);
        if (def->os.dtb) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DTB)) {
                virCommandAddArgList(cmd, "-dtb", def->os.dtb, NULL);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("dtb is not supported with this QEMU binary"));
                goto error;
            }
        }
    } else {
        virCommandAddArgList(cmd, "-bootloader", def->os.bootloader, NULL);
    }

    for (i = 0; i < def->ndisks; i++) {
        virDomainDiskDefPtr disk = def->disks[i];

        if (disk->driverName != NULL &&
            !STREQ(disk->driverName, "qemu")) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported driver name '%s' for disk '%s'"),
                           disk->driverName, disk->src);
            goto error;
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
        for (j = 0; j < ARRAY_CARDINALITY(contOrder); j++) {
            for (i = 0; i < def->ncontrollers; i++) {
                virDomainControllerDefPtr cont = def->controllers[i];

                if (cont->type != contOrder[j])
                    continue;

                /* Also, skip USB controllers with type none.*/
                if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
                    cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE) {
                    usbcontroller = -1; /* mark we don't want a controller */
                    continue;
                }

                /* Skip pci-root */
                if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI &&
                    cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT) {
                    continue;
                }

                /* Only recent QEMU implements a SATA (AHCI) controller */
                if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_SATA) {
                    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_AHCI)) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                       _("SATA is not supported with this "
                                         "QEMU binary"));
                        goto error;
                    } else {
                        char *devstr;

                        virCommandAddArg(cmd, "-device");
                        if (!(devstr = qemuBuildControllerDevStr(def, cont,
                                                                 qemuCaps, NULL)))
                            goto error;

                        virCommandAddArg(cmd, devstr);
                        VIR_FREE(devstr);
                    }
                } else if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB &&
                           cont->model == -1 &&
                           (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_PIIX3_USB_UHCI) ||
                            def->os.arch == VIR_ARCH_PPC64)) {
                    if (usblegacy) {
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                       _("Multiple legacy USB controllers are "
                                         "not supported"));
                        goto error;
                    }
                    usblegacy = true;
                } else {
                    virCommandAddArg(cmd, "-device");

                    char *devstr;
                    if (!(devstr = qemuBuildControllerDevStr(def, cont, qemuCaps,
                                                             &usbcontroller)))
                        goto error;

                    virCommandAddArg(cmd, devstr);
                    VIR_FREE(devstr);
                }
            }
        }
    }

    if (usbcontroller == 0)
        virCommandAddArg(cmd, "-usb");

    for (i = 0; i < def->nhubs; i++) {
        virDomainHubDefPtr hub = def->hubs[i];
        char *optstr;

        virCommandAddArg(cmd, "-device");
        if (!(optstr = qemuBuildHubDevStr(hub, qemuCaps)))
            goto error;
        virCommandAddArg(cmd, optstr);
        VIR_FREE(optstr);
    }

    /* If QEMU supports -drive param instead of old -hda, -hdb, -cdrom .. */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE)) {
        int bootCD = 0, bootFloppy = 0, bootDisk = 0;

        if ((virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_BOOT) || emitBootindex)) {
            /* bootDevs will get translated into either bootindex=N or boot=on
             * depending on what qemu supports */
            for (i = 0; i < def->os.nBootDevs; i++) {
                switch (def->os.bootDevs[i]) {
                case VIR_DOMAIN_BOOT_CDROM:
                    bootCD = i + 1;
                    break;
                case VIR_DOMAIN_BOOT_FLOPPY:
                    bootFloppy = i + 1;
                    break;
                case VIR_DOMAIN_BOOT_DISK:
                    bootDisk = i + 1;
                    break;
                }
            }
        }

        for (i = 0; i < def->ndisks; i++) {
            char *optstr;
            int bootindex = 0;
            virDomainDiskDefPtr disk = def->disks[i];
            bool withDeviceArg = false;
            bool deviceFlagMasked = false;

            /* Unless we have -device, then USB disks need special
               handling */
            if ((disk->bus == VIR_DOMAIN_DISK_BUS_USB) &&
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
                    virCommandAddArg(cmd, "-usbdevice");
                    virCommandAddArgFormat(cmd, "disk:%s", disk->src);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unsupported usb disk type for '%s'"),
                                   disk->src);
                    goto error;
                }
                continue;
            }

            switch (disk->device) {
            case VIR_DOMAIN_DISK_DEVICE_CDROM:
                bootindex = bootCD;
                bootCD = 0;
                break;
            case VIR_DOMAIN_DISK_DEVICE_FLOPPY:
                bootindex = bootFloppy;
                bootFloppy = 0;
                break;
            case VIR_DOMAIN_DISK_DEVICE_DISK:
            case VIR_DOMAIN_DISK_DEVICE_LUN:
                bootindex = bootDisk;
                bootDisk = 0;
                break;
            }

            virCommandAddArg(cmd, "-drive");

            /* Unfortunately it is not possible to use
               -device for floppies, or Xen paravirt
               devices. Fortunately, those don't need
               static PCI addresses, so we don't really
               care that we can't use -device */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                if (disk->bus != VIR_DOMAIN_DISK_BUS_XEN) {
                    withDeviceArg = true;
                } else {
                    virQEMUCapsClear(qemuCaps, QEMU_CAPS_DEVICE);
                    deviceFlagMasked = true;
                }
            }
            optstr = qemuBuildDriveStr(conn, disk,
                                       emitBootindex ? false : !!bootindex,
                                       qemuCaps);
            if (deviceFlagMasked)
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE);
            if (!optstr)
                goto error;
            virCommandAddArg(cmd, optstr);
            VIR_FREE(optstr);

            if (!emitBootindex)
                bootindex = 0;
            else if (disk->info.bootIndex)
                bootindex = disk->info.bootIndex;

            if (withDeviceArg) {
                if (disk->bus == VIR_DOMAIN_DISK_BUS_FDC) {
                    virCommandAddArg(cmd, "-global");
                    virCommandAddArgFormat(cmd, "isa-fdc.drive%c=drive-%s",
                                           disk->info.addr.drive.unit
                                           ? 'B' : 'A',
                                           disk->info.alias);

                    if (bootindex) {
                        virCommandAddArg(cmd, "-global");
                        virCommandAddArgFormat(cmd, "isa-fdc.bootindex%c=%d",
                                               disk->info.addr.drive.unit
                                               ? 'B' : 'A',
                                               bootindex);
                    }
                } else {
                    virCommandAddArg(cmd, "-device");

                    if (!(optstr = qemuBuildDriveDevStr(def, disk, bootindex,
                                                        qemuCaps)))
                        goto error;
                    virCommandAddArg(cmd, optstr);
                    VIR_FREE(optstr);
                }
            }
        }
    } else {
        for (i = 0; i < def->ndisks; i++) {
            char dev[NAME_MAX];
            char *file;
            const char *fmt;
            virDomainDiskDefPtr disk = def->disks[i];

            if ((disk->type == VIR_DOMAIN_DISK_TYPE_BLOCK) &&
                (disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("tray status 'open' is invalid for "
                                 "block type disk"));
                goto error;
            }

            if (disk->bus == VIR_DOMAIN_DISK_BUS_USB) {
                if (disk->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
                    virCommandAddArg(cmd, "-usbdevice");
                    virCommandAddArgFormat(cmd, "disk:%s", disk->src);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unsupported usb disk type for '%s'"),
                                   disk->src);
                    goto error;
                }
                continue;
            }

            if (STREQ(disk->dst, "hdc") &&
                disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM) {
                if (disk->src) {
                    snprintf(dev, NAME_MAX, "-%s", "cdrom");
                } else {
                    continue;
                }
            } else {
                if (STRPREFIX(disk->dst, "hd") ||
                    STRPREFIX(disk->dst, "fd")) {
                    snprintf(dev, NAME_MAX, "-%s", disk->dst);
                } else {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unsupported disk type '%s'"), disk->dst);
                    goto error;
                }
            }

            if (disk->type == VIR_DOMAIN_DISK_TYPE_DIR) {
                /* QEMU only supports magic FAT format for now */
                if (disk->format > 0 && disk->format != VIR_STORAGE_FILE_FAT) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unsupported disk driver type for '%s'"),
                                   virStorageFileFormatTypeToString(disk->format));
                    goto error;
                }
                if (!disk->readonly) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("cannot create virtual FAT disks in read-write mode"));
                    goto error;
                }
                if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
                    fmt = "fat:floppy:%s";
                else
                    fmt = "fat:%s";

                if (virAsprintf(&file, fmt, disk->src) < 0)
                    goto error;
            } else if (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("network disks are only supported with -drive"));
            } else {
                if (VIR_STRDUP(file, disk->src) < 0) {
                    goto error;
                }
            }

            /* Don't start with source if the tray is open for
             * CDROM and Floppy device.
             */
            if (!((disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY ||
                   disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM) &&
                  disk->tray_status == VIR_DOMAIN_DISK_TRAY_OPEN))
                virCommandAddArgList(cmd, dev, file, NULL);
            VIR_FREE(file);
        }
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_FSDEV)) {
        for (i = 0; i < def->nfss; i++) {
            char *optstr;
            virDomainFSDefPtr fs = def->fss[i];

            virCommandAddArg(cmd, "-fsdev");
            if (!(optstr = qemuBuildFSStr(fs, qemuCaps)))
                goto error;
            virCommandAddArg(cmd, optstr);
            VIR_FREE(optstr);

            virCommandAddArg(cmd, "-device");
            if (!(optstr = qemuBuildFSDevStr(fs, qemuCaps)))
                goto error;
            virCommandAddArg(cmd, optstr);
            VIR_FREE(optstr);
        }
    } else {
        if (def->nfss) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("filesystem passthrough not supported by this QEMU"));
            goto error;
        }
    }

    if (!def->nnets) {
        /* If we have -device, then we set -nodefault already */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
            virCommandAddArgList(cmd, "-net", "none", NULL);
    } else {
        int bootNet = 0;

        if (emitBootindex) {
            /* convert <boot dev='network'/> to bootindex since we didn't emit
             * -boot n
             */
            for (i = 0; i < def->os.nBootDevs; i++) {
                if (def->os.bootDevs[i] == VIR_DOMAIN_BOOT_NET) {
                    bootNet = i + 1;
                    break;
                }
            }
        }

        for (i = 0; i < def->nnets; i++) {
            virDomainNetDefPtr net = def->nets[i];
            int vlan;

            /* VLANs are not used with -netdev, so don't record them */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NETDEV) &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
                vlan = -1;
            else
                vlan = i;

            if (qemuBuildInterfaceCommandLine(cmd, driver, conn, def, net,
                                              qemuCaps, vlan, bootNet, vmop) < 0)
                goto error;
            last_good_net = i;
            bootNet = 0;
        }
    }

    if (def->nsmartcards) {
        /* -device usb-ccid was already emitted along with other
         * controllers.  For now, qemu handles only one smartcard.  */
        virDomainSmartcardDefPtr smartcard = def->smartcards[0];
        char *devstr;
        virBuffer opt = VIR_BUFFER_INITIALIZER;
        const char *database;

        if (def->nsmartcards > 1 ||
            smartcard->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID ||
            smartcard->info.addr.ccid.controller != 0 ||
            smartcard->info.addr.ccid.slot != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this QEMU binary lacks multiple smartcard "
                             "support"));
            virBufferFreeAndReset(&opt);
            goto error;
        }

        switch (smartcard->type) {
        case VIR_DOMAIN_SMARTCARD_TYPE_HOST:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) ||
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_CCID_EMULATED)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("this QEMU binary lacks smartcard host "
                                 "mode support"));
                goto error;
            }

            virBufferAddLit(&opt, "ccid-card-emulated,backend=nss-emulated");
            break;

        case VIR_DOMAIN_SMARTCARD_TYPE_HOST_CERTIFICATES:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) ||
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_CCID_EMULATED)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("this QEMU binary lacks smartcard host "
                                 "mode support"));
                goto error;
            }

            virBufferAddLit(&opt, "ccid-card-emulated,backend=certificates");
            for (j = 0; j < VIR_DOMAIN_SMARTCARD_NUM_CERTIFICATES; j++) {
                if (strchr(smartcard->data.cert.file[j], ',')) {
                    virBufferFreeAndReset(&opt);
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("invalid certificate name: %s"),
                                   smartcard->data.cert.file[j]);
                    goto error;
                }
                virBufferAsprintf(&opt, ",cert%zu=%s", j + 1,
                                  smartcard->data.cert.file[j]);
            }
            if (smartcard->data.cert.database) {
                if (strchr(smartcard->data.cert.database, ',')) {
                    virBufferFreeAndReset(&opt);
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("invalid database name: %s"),
                                   smartcard->data.cert.database);
                    goto error;
                }
                database = smartcard->data.cert.database;
            } else {
                database = VIR_DOMAIN_SMARTCARD_DEFAULT_DATABASE;
            }
            virBufferAsprintf(&opt, ",db=%s", database);
            break;

        case VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) ||
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_CCID_PASSTHRU)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("this QEMU binary lacks smartcard "
                                 "passthrough mode support"));
                goto error;
            }

            virCommandAddArg(cmd, "-chardev");
            if (!(devstr = qemuBuildChrChardevStr(&smartcard->data.passthru,
                                                  smartcard->info.alias,
                                                  qemuCaps))) {
                virBufferFreeAndReset(&opt);
                goto error;
            }
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            virBufferAsprintf(&opt, "ccid-card-passthru,chardev=char%s",
                              smartcard->info.alias);
            break;

        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected smartcard type %d"),
                           smartcard->type);
            virBufferFreeAndReset(&opt);
            goto error;
        }
        virCommandAddArg(cmd, "-device");
        virBufferAsprintf(&opt, ",id=%s,bus=ccid0.0", smartcard->info.alias);
        virCommandAddArgBuffer(cmd, &opt);
    }

    if (!def->nserials) {
        /* If we have -device, then we set -nodefault already */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
            virCommandAddArgList(cmd, "-serial", "none", NULL);
    } else {
        for (i = 0; i < def->nserials; i++) {
            virDomainChrDefPtr serial = def->serials[i];
            char *devstr;

            /* Use -chardev with -device if they are available */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virCommandAddArg(cmd, "-chardev");
                if (!(devstr = qemuBuildChrChardevStr(&serial->source,
                                                      serial->info.alias,
                                                      qemuCaps)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);

                if (qemuBuildChrDeviceCommandLine(cmd, def, serial, qemuCaps) < 0)
                   goto error;
            } else {
                virCommandAddArg(cmd, "-serial");
                if (!(devstr = qemuBuildChrArgStr(&serial->source, NULL)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            }
        }
    }

    if (!def->nparallels) {
        /* If we have -device, then we set -nodefault already */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
            virCommandAddArgList(cmd, "-parallel", "none", NULL);
    } else {
        for (i = 0; i < def->nparallels; i++) {
            virDomainChrDefPtr parallel = def->parallels[i];
            char *devstr;

            /* Use -chardev with -device if they are available */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virCommandAddArg(cmd, "-chardev");
                if (!(devstr = qemuBuildChrChardevStr(&parallel->source,
                                                      parallel->info.alias,
                                                      qemuCaps)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);

                if (qemuBuildChrDeviceCommandLine(cmd, def, parallel, qemuCaps) < 0)
                    goto error;
            } else {
                virCommandAddArg(cmd, "-parallel");
                if (!(devstr = qemuBuildChrArgStr(&parallel->source, NULL)))
                      goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            }
        }
    }

    for (i = 0; i < def->nchannels; i++) {
        virDomainChrDefPtr channel = def->channels[i];
        char *devstr;

        switch (channel->targetType) {
        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) ||
                !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("guestfwd requires QEMU to support -chardev & -device"));
                goto error;
            }

            virCommandAddArg(cmd, "-chardev");
            if (!(devstr = qemuBuildChrChardevStr(&channel->source,
                                                  channel->info.alias,
                                                  qemuCaps)))
                goto error;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceStr(&devstr, def, channel, qemuCaps) < 0)
                goto error;
            virCommandAddArgList(cmd, "-netdev", devstr, NULL);
            VIR_FREE(devstr);
            break;

        case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("virtio channel requires QEMU to support -device"));
                goto error;
            }

            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC) &&
                channel->source.type == VIR_DOMAIN_CHR_TYPE_SPICEVMC) {
                /* spicevmc was originally introduced via a -device
                 * with a backend internal to qemu; although we prefer
                 * the newer -chardev interface.  */
                ;
            } else {
                virCommandAddArg(cmd, "-chardev");
                if (!(devstr = qemuBuildChrChardevStr(&channel->source,
                                                      channel->info.alias,
                                                      qemuCaps)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            }

            if (qemuBuildChrDeviceCommandLine(cmd, def, channel, qemuCaps) < 0)
                goto error;
            break;
        }
    }

    /* Explicit console devices */
    for (i = 0; i < def->nconsoles; i++) {
        virDomainChrDefPtr console = def->consoles[i];
        char *devstr;

        switch (console->targetType) {
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("sclp console requires QEMU to support -device"));
                goto error;
            }
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCLP_S390)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("sclp console requires QEMU to support s390-sclp"));
                goto error;
            }

            virCommandAddArg(cmd, "-chardev");
            if (!(devstr = qemuBuildChrChardevStr(&console->source,
                                                  console->info.alias,
                                                  qemuCaps)))
                goto error;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                goto error;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO:
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("virtio channel requires QEMU to support -device"));
                goto error;
            }

            virCommandAddArg(cmd, "-chardev");
            if (!(devstr = qemuBuildChrChardevStr(&console->source,
                                                  console->info.alias,
                                                  qemuCaps)))
                goto error;
            virCommandAddArg(cmd, devstr);
            VIR_FREE(devstr);

            if (qemuBuildChrDeviceCommandLine(cmd, def, console, qemuCaps) < 0)
                goto error;
            break;

        case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL:
            break;

        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported console target type %s"),
                           NULLSTR(virDomainChrConsoleTargetTypeToString(console->targetType)));
            goto error;
        }
    }

    if (def->tpm) {
        char *optstr;

        if (!(optstr = qemuBuildTPMBackendStr(def, qemuCaps, emulator)))
            goto error;

        virCommandAddArgList(cmd, "-tpmdev", optstr, NULL);
        VIR_FREE(optstr);

        if (!(optstr = qemuBuildTPMDevStr(def, qemuCaps, emulator)))
            goto error;

        virCommandAddArgList(cmd, "-device", optstr, NULL);
        VIR_FREE(optstr);
    }

    for (i = 0; i < def->ninputs; i++) {
        virDomainInputDefPtr input = def->inputs[i];

        if (input->bus == VIR_DOMAIN_INPUT_BUS_USB) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                char *optstr;
                virCommandAddArg(cmd, "-device");
                if (!(optstr = qemuBuildUSBInputDevStr(input, qemuCaps)))
                    goto error;
                virCommandAddArg(cmd, optstr);
                VIR_FREE(optstr);
            } else {
                virCommandAddArgList(cmd, "-usbdevice",
                                     input->type == VIR_DOMAIN_INPUT_TYPE_MOUSE
                                     ? "mouse" : "tablet", NULL);
            }
        }
    }

    for (i = 0; i < def->ngraphics; ++i) {
        switch (def->graphics[i]->type) {
        case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
            ++sdl;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
            ++vnc;
            break;
        case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
            ++spice;
            break;
        }
    }
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_0_10) && sdl + vnc + spice > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only 1 graphics device is supported"));
        goto error;
    }
    if (sdl > 1 || vnc > 1 || spice > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("only 1 graphics device of each type "
                         "(sdl, vnc, spice) is supported"));
        goto error;
    }

    for (i = 0; i < def->ngraphics; ++i) {
        if (qemuBuildGraphicsCommandLine(cfg, cmd, def, qemuCaps,
                                         def->graphics[i]) < 0)
            goto error;
    }
    if (def->nvideos > 0) {
        int primaryVideoType = def->videos[0]->type;
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VIDEO_PRIMARY) &&
             ((primaryVideoType == VIR_DOMAIN_VIDEO_TYPE_VGA &&
                 virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VGA)) ||
             (primaryVideoType == VIR_DOMAIN_VIDEO_TYPE_CIRRUS &&
                 virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_CIRRUS_VGA)) ||
             (primaryVideoType == VIR_DOMAIN_VIDEO_TYPE_VMVGA &&
                 virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VMWARE_SVGA)) ||
             (primaryVideoType == VIR_DOMAIN_VIDEO_TYPE_QXL &&
                 virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QXL_VGA)))
           ) {
            for (i = 0; i < def->nvideos; i++) {
                char *str;
                virCommandAddArg(cmd, "-device");
                if (!(str = qemuBuildDeviceVideoStr(def->videos[i], qemuCaps, !i)))
                    goto error;

                virCommandAddArg(cmd, str);
                VIR_FREE(str);
            }
        } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA)) {
            if (primaryVideoType == VIR_DOMAIN_VIDEO_TYPE_XEN) {
                /* nothing - vga has no effect on Xen pvfb */
            } else {
                if ((primaryVideoType == VIR_DOMAIN_VIDEO_TYPE_QXL) &&
                    !virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA_QXL)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("This QEMU does not support QXL graphics adapters"));
                    goto error;
                }

                const char *vgastr = qemuVideoTypeToString(primaryVideoType);
                if (!vgastr || STREQ(vgastr, "")) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("video type %s is not supported with QEMU"),
                                   virDomainVideoTypeToString(primaryVideoType));
                    goto error;
                }

                virCommandAddArgList(cmd, "-vga", vgastr, NULL);

                if (def->videos[0]->type == VIR_DOMAIN_VIDEO_TYPE_QXL &&
                    (def->videos[0]->vram || def->videos[0]->ram) &&
                    virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                    const char *dev = (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QXL_VGA)
                                       ? "qxl-vga" : "qxl");
                    int ram = def->videos[0]->ram;
                    int vram = def->videos[0]->vram;

                    if (vram > (UINT_MAX / 1024)) {
                        virReportError(VIR_ERR_OVERFLOW,
                               _("value for 'vram' must be less than '%u'"),
                                       UINT_MAX / 1024);
                        goto error;
                    }
                    if (ram > (UINT_MAX / 1024)) {
                        virReportError(VIR_ERR_OVERFLOW,
                           _("value for 'ram' must be less than '%u'"),
                                       UINT_MAX / 1024);
                        goto error;
                    }

                    if (ram) {
                        virCommandAddArg(cmd, "-global");
                        virCommandAddArgFormat(cmd, "%s.ram_size=%u",
                                               dev, ram * 1024);
                    }
                    if (vram) {
                        virCommandAddArg(cmd, "-global");
                        virCommandAddArgFormat(cmd, "%s.vram_size=%u",
                                               dev, vram * 1024);
                    }
                }
            }

            if (def->nvideos > 1) {
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                    for (i = 1; i < def->nvideos; i++) {
                        char *str;
                        if (def->videos[i]->type != VIR_DOMAIN_VIDEO_TYPE_QXL) {
                            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                           _("video type %s is only valid as primary video card"),
                                           virDomainVideoTypeToString(def->videos[0]->type));
                            goto error;
                        }

                        virCommandAddArg(cmd, "-device");

                        if (!(str = qemuBuildDeviceVideoStr(def->videos[i], qemuCaps, false)))
                            goto error;

                        virCommandAddArg(cmd, str);
                        VIR_FREE(str);
                    }
                } else {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   "%s", _("only one video card is currently supported"));
                    goto error;
                }
            }
        } else {

            switch (def->videos[0]->type) {
            case VIR_DOMAIN_VIDEO_TYPE_VGA:
                virCommandAddArg(cmd, "-std-vga");
                break;

            case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
                virCommandAddArg(cmd, "-vmwarevga");
                break;

            case VIR_DOMAIN_VIDEO_TYPE_XEN:
            case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
                /* No special args - this is the default */
                break;

            default:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("video type %s is not supported with this QEMU"),
                               virDomainVideoTypeToString(def->videos[0]->type));
                goto error;
            }

            if (def->nvideos > 1) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("only one video card is currently supported"));
                goto error;
            }
        }

    } else {
        /* If we have -device, then we set -nodefault already */
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA) &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_VGA_NONE))
            virCommandAddArgList(cmd, "-vga", "none", NULL);
    }

    /* Add sound hardware */
    if (def->nsounds) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
            for (i = 0; i < def->nsounds; i++) {
                virDomainSoundDefPtr sound = def->sounds[i];
                char *str = NULL;

                /* Sadly pcspk device doesn't use -device syntax. Fortunately
                 * we don't need to set any PCI address on it, so we don't
                 * mind too much */
                if (sound->model == VIR_DOMAIN_SOUND_MODEL_PCSPK) {
                    virCommandAddArgList(cmd, "-soundhw", "pcspk", NULL);
                } else {
                    virCommandAddArg(cmd, "-device");
                    if (!(str = qemuBuildSoundDevStr(sound, qemuCaps)))
                        goto error;

                    virCommandAddArg(cmd, str);

                    if (sound->model == VIR_DOMAIN_SOUND_MODEL_ICH6) {
                        char *codecstr = NULL;

                        for (j = 0; j < sound->ncodecs; j++) {
                            virCommandAddArg(cmd, "-device");
                            if (!(codecstr = qemuBuildSoundCodecStr(sound, sound->codecs[j], qemuCaps))) {
                                goto error;

                            }
                            virCommandAddArg(cmd, codecstr);
                            VIR_FREE(codecstr);
                        }
                        if (j == 0) {
                            virDomainSoundCodecDef codec = {
                                VIR_DOMAIN_SOUND_CODEC_TYPE_DUPLEX,
                                0
                            };
                            virCommandAddArg(cmd, "-device");
                            if (!(codecstr = qemuBuildSoundCodecStr(sound, &codec, qemuCaps))) {
                                goto error;

                            }
                            virCommandAddArg(cmd, codecstr);
                            VIR_FREE(codecstr);
                        }
                    }

                    VIR_FREE(str);
                }
            }
        } else {
            int size = 100;
            char *modstr;
            if (VIR_ALLOC_N(modstr, size+1) < 0)
                goto error;

            for (i = 0; i < def->nsounds && size > 0; i++) {
                virDomainSoundDefPtr sound = def->sounds[i];
                const char *model = virDomainSoundModelTypeToString(sound->model);
                if (!model) {
                    VIR_FREE(modstr);
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("invalid sound model"));
                    goto error;
                }

                if (sound->model == VIR_DOMAIN_SOUND_MODEL_ICH6) {
                    VIR_FREE(modstr);
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("this QEMU binary lacks hda support"));
                    goto error;
                }

                strncat(modstr, model, size);
                size -= strlen(model);
                if (i < (def->nsounds - 1))
                    strncat(modstr, ",", size--);
            }
            virCommandAddArgList(cmd, "-soundhw", modstr, NULL);
            VIR_FREE(modstr);
        }
    }

    /* Add watchdog hardware */
    if (def->watchdog) {
        virDomainWatchdogDefPtr watchdog = def->watchdog;
        char *optstr;

        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
            virCommandAddArg(cmd, "-device");

            optstr = qemuBuildWatchdogDevStr(watchdog, qemuCaps);
            if (!optstr)
                goto error;
        } else {
            virCommandAddArg(cmd, "-watchdog");

            const char *model = virDomainWatchdogModelTypeToString(watchdog->model);
            if (!model) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s", _("missing watchdog model"));
                goto error;
            }

            if (VIR_STRDUP(optstr, model) < 0)
                goto error;
        }
        virCommandAddArg(cmd, optstr);
        VIR_FREE(optstr);

        int act = watchdog->action;
        if (act == VIR_DOMAIN_WATCHDOG_ACTION_DUMP)
            act = VIR_DOMAIN_WATCHDOG_ACTION_PAUSE;
        const char *action = virDomainWatchdogActionTypeToString(act);
        if (!action) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("invalid watchdog action"));
            goto error;
        }
        virCommandAddArgList(cmd, "-watchdog-action", action, NULL);
    }

    /* Add redirected devices */
    for (i = 0; i < def->nredirdevs; i++) {
        virDomainRedirdevDefPtr redirdev = def->redirdevs[i];
        char *devstr;

        virCommandAddArg(cmd, "-chardev");
        if (!(devstr = qemuBuildChrChardevStr(&redirdev->source.chr,
                                              redirdev->info.alias,
                                              qemuCaps))) {
            goto error;
        }

        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);

        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
            goto error;

        virCommandAddArg(cmd, "-device");
        if (!(devstr = qemuBuildRedirdevDevStr(def, redirdev, qemuCaps)))
            goto error;
        virCommandAddArg(cmd, devstr);
        VIR_FREE(devstr);
    }


    /* Add host passthrough hardware */
    for (i = 0; i < def->nhostdevs; i++) {
        virDomainHostdevDefPtr hostdev = def->hostdevs[i];
        char *devstr;

        if (hostdev->info->bootIndex) {
            if (hostdev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS ||
                (hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI &&
                 hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB &&
                 hostdev->source.subsys.type != VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("booting from assigned devices is only "
                                 "supported for PCI, USB and SCSI devices"));
                goto error;
            } else {
                if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
                    if (hostdev->source.subsys.u.pci.backend
                        == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VFIO_PCI_BOOTINDEX)) {
                            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                           _("booting from PCI devices assigned with VFIO "
                                             "is not supported with this version of qemu"));
                            goto error;
                        }
                    } else {
                        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_BOOTINDEX)) {
                            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                           _("booting from assigned PCI devices is not "
                                             "supported with this version of qemu"));
                            goto error;
                        }
                    }
                }
                if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB &&
                    !virQEMUCapsGet(qemuCaps, QEMU_CAPS_USB_HOST_BOOTINDEX)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("booting from assigned USB devices is not "
                                     "supported with this version of qemu"));
                    goto error;
                }
                if (hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI &&
                    !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SCSI_GENERIC_BOOTINDEX)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("booting from assigned SCSI devices is not"
                                     " supported with this version of qemu"));
                    goto error;
                }
            }
        }

        /* USB */
        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB) {

            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                virCommandAddArg(cmd, "-device");
                if (!(devstr = qemuBuildUSBHostdevDevStr(hostdev, qemuCaps)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            } else {
                virCommandAddArg(cmd, "-usbdevice");
                if (!(devstr = qemuBuildUSBHostdevUsbDevStr(hostdev)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            }
        }

        /* PCI */
        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {

            if (hostdev->source.subsys.u.pci.backend
                == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VFIO_PCI)) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("VFIO PCI device assignment is not "
                                     "supported by this version of qemu"));
                    goto error;
                }
                /* VFIO requires all of the guest's memory to be locked
                 * resident */
                mlock = true;
            }

            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
                char *configfd_name = NULL;
                if ((hostdev->source.subsys.u.pci.backend
                     != VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) &&
                    virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_CONFIGFD)) {
                    int configfd = qemuOpenPCIConfig(hostdev);

                    if (configfd >= 0) {
                        if (virAsprintf(&configfd_name, "%d", configfd) < 0) {
                            VIR_FORCE_CLOSE(configfd);
                            goto error;
                        }

                        virCommandPassFD(cmd, configfd,
                                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
                    }
                }
                virCommandAddArg(cmd, "-device");
                devstr = qemuBuildPCIHostdevDevStr(hostdev, configfd_name, qemuCaps);
                VIR_FREE(configfd_name);
                if (!devstr)
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCIDEVICE)) {
                virCommandAddArg(cmd, "-pcidevice");
                if (!(devstr = qemuBuildPCIHostdevPCIDevStr(hostdev)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("PCI device assignment is not supported by this version of qemu"));
                goto error;
            }
        }

        /* SCSI */
        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE) &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE) &&
                virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SCSI_GENERIC)) {
                char *drvstr;

                virCommandAddArg(cmd, "-drive");
                if (!(drvstr = qemuBuildSCSIHostdevDrvStr(hostdev, qemuCaps, callbacks)))
                    goto error;
                virCommandAddArg(cmd, drvstr);
                VIR_FREE(drvstr);

                virCommandAddArg(cmd, "-device");
                if (!(devstr = qemuBuildSCSIHostdevDevStr(def, hostdev, qemuCaps)))
                    goto error;
                virCommandAddArg(cmd, devstr);
                VIR_FREE(devstr);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("SCSI passthrough is not supported by this version of qemu"));
                goto error;
            }
        }
    }

    /* Migration is very annoying due to wildly varying syntax &
     * capabilities over time of KVM / QEMU codebases.
     */
    if (migrateFrom) {
        virCommandAddArg(cmd, "-incoming");
        if (STRPREFIX(migrateFrom, "tcp")) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("TCP migration is not supported with "
                                       "this QEMU binary"));
                goto error;
            }
            virCommandAddArg(cmd, migrateFrom);
        } else if (STREQ(migrateFrom, "stdio")) {
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_FD)) {
                virCommandAddArgFormat(cmd, "fd:%d", migrateFd);
                virCommandPassFD(cmd, migrateFd, 0);
            } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC)) {
                virCommandAddArg(cmd, "exec:cat");
                virCommandSetInputFD(cmd, migrateFd);
            } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_KVM_STDIO)) {
                virCommandAddArg(cmd, migrateFrom);
                virCommandSetInputFD(cmd, migrateFd);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("STDIO migration is not supported "
                                       "with this QEMU binary"));
                goto error;
            }
        } else if (STRPREFIX(migrateFrom, "exec")) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("EXEC migration is not supported "
                                       "with this QEMU binary"));
                goto error;
            }
            virCommandAddArg(cmd, migrateFrom);
        } else if (STRPREFIX(migrateFrom, "fd")) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_FD)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("FD migration is not supported "
                                       "with this QEMU binary"));
                goto error;
            }
            virCommandAddArg(cmd, migrateFrom);
            virCommandPassFD(cmd, migrateFd, 0);
        } else if (STRPREFIX(migrateFrom, "unix")) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_UNIX)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               "%s", _("UNIX migration is not supported "
                                       "with this QEMU binary"));
                goto error;
            }
            virCommandAddArg(cmd, migrateFrom);
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("unknown migration protocol"));
            goto error;
        }
    }

    /* QEMU changed its default behavior to not include the virtio balloon
     * device.  Explicitly request it to ensure it will be present.
     *
     * NB: Earlier we declared that VirtIO balloon will always be in
     * slot 0x3 on bus 0x0
     */
    if (STREQLEN(def->os.machine, "s390-virtio", 10) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_S390) && def->memballoon)
        def->memballoon->model = VIR_DOMAIN_MEMBALLOON_MODEL_NONE;

    if (def->memballoon &&
        def->memballoon->model != VIR_DOMAIN_MEMBALLOON_MODEL_NONE) {
        if (def->memballoon->model != VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Memory balloon device type '%s' is not supported by this version of qemu"),
                           virDomainMemballoonModelTypeToString(def->memballoon->model));
            goto error;
        }
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE)) {
            char *optstr;
            virCommandAddArg(cmd, "-device");

            optstr = qemuBuildMemballoonDevStr(def->memballoon, qemuCaps);
            if (!optstr)
                goto error;
            virCommandAddArg(cmd, optstr);
            VIR_FREE(optstr);
        } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BALLOON)) {
            virCommandAddArgList(cmd, "-balloon", "virtio", NULL);
        }
    }

    if (def->rng) {
        /* add the RNG source backend */
        if (qemuBuildRNGBackendArgs(cmd, def->rng, qemuCaps) < 0)
            goto error;

        /* add the device */
        if (qemuBuildRNGDeviceArgs(cmd, def->rng, qemuCaps) < 0)
            goto error;
    }

    if (def->nvram) {
        if (def->os.arch == VIR_ARCH_PPC64 &&
            STREQ(def->os.machine, "pseries")) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_NVRAM)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("nvram device is not supported by "
                                 "this QEMU binary"));
                goto error;
            }

            char *optstr;
            virCommandAddArg(cmd, "-global");
            optstr = qemuBuildNVRAMDevStr(def->nvram);
            if (!optstr)
                goto error;
            if (optstr)
                virCommandAddArg(cmd, optstr);
            VIR_FREE(optstr);
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                          _("nvram device is only supported for PPC64"));
            goto error;
        }
    }
    if (snapshot)
        virCommandAddArgList(cmd, "-loadvm", snapshot->def->name, NULL);

    if (def->namespaceData) {
        qemuDomainCmdlineDefPtr qemucmd;

        qemucmd = def->namespaceData;
        for (i = 0; i < qemucmd->num_args; i++)
            virCommandAddArg(cmd, qemucmd->args[i]);
        for (i = 0; i < qemucmd->num_env; i++)
            virCommandAddEnvPair(cmd, qemucmd->env_name[i],
                                 qemucmd->env_value[i]
                                 ? qemucmd->env_value[i] : "");
    }

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX)) {
        if (cfg->seccompSandbox == 0)
            virCommandAddArgList(cmd, "-sandbox", "off", NULL);
        else if (cfg->seccompSandbox > 0)
            virCommandAddArgList(cmd, "-sandbox", "on", NULL);
    } else if (cfg->seccompSandbox > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("QEMU does not support seccomp sandboxes"));
        goto error;
    }

    if (mlock)
        virCommandSetMaxMemLock(cmd, qemuDomainMemoryLimit(def) * 1024);

    virObjectUnref(cfg);
    return cmd;

error:
    virObjectUnref(cfg);
    /* free up any resources in the network driver
     * but don't overwrite the original error */
    originalError = virSaveLastError();
    for (i = 0; last_good_net != -1 && i <= last_good_net; i++)
        virDomainConfNWFilterTeardown(def->nets[i]);
    virSetError(originalError);
    virFreeError(originalError);
    virCommandFree(cmd);
    return NULL;
}

/* This function generates the correct '-device' string for character
 * devices of each architecture.
 */
static int
qemuBuildSerialChrDeviceStr(char **deviceStr,
                            virDomainChrDefPtr serial,
                            virQEMUCapsPtr qemuCaps,
                            virArch arch,
                            char *machine)
{
    virBuffer cmd = VIR_BUFFER_INITIALIZER;

    if ((arch == VIR_ARCH_PPC64) && STREQ(machine, "pseries")) {
        if (serial->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
            serial->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO) {
            virBufferAsprintf(&cmd, "spapr-vty,chardev=char%s",
                              serial->info.alias);
            if (qemuBuildDeviceAddressStr(&cmd, &serial->info, qemuCaps) < 0)
                goto error;
        }
    } else {
        virBufferAsprintf(&cmd, "%s,chardev=char%s,id=%s",
                          virDomainChrSerialTargetTypeToString(serial->targetType),
                          serial->info.alias, serial->info.alias);

        if (serial->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB) {
            if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_USB_SERIAL)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("usb-serial is not supported in this QEMU binary"));
                goto error;
            }

            if (serial->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
                serial->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("usb-serial requires address of usb type"));
                goto error;
            }

            if (qemuBuildDeviceAddressStr(&cmd, &serial->info, qemuCaps) < 0)
                goto error;
        }
    }

    if (virBufferError(&cmd)) {
        virReportOOMError();
        goto error;
    }

    *deviceStr = virBufferContentAndReset(&cmd);
    return 0;

error:
    virBufferFreeAndReset(&cmd);
    return -1;
}

static int
qemuBuildParallelChrDeviceStr(char **deviceStr,
                              virDomainChrDefPtr chr)
{
    if (virAsprintf(deviceStr, "isa-parallel,chardev=char%s,id=%s",
                    chr->info.alias, chr->info.alias) < 0) {
        virReportOOMError();
        return -1;
    }
    return 0;
}

static int
qemuBuildChannelChrDeviceStr(char **deviceStr,
                             virDomainChrDefPtr chr,
                             virQEMUCapsPtr qemuCaps)
{
    int ret = -1;
    char *addr = NULL;
    int port;

    switch ((enum virDomainChrChannelTargetType) chr->targetType) {
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_GUESTFWD:

        addr = virSocketAddrFormat(chr->target.addr);
        if (!addr)
            return ret;
        port = virSocketAddrGetPort(chr->target.addr);

        if (virAsprintf(deviceStr,
                        "user,guestfwd=tcp:%s:%i,chardev=char%s,id=user-%s",
                        addr, port, chr->info.alias, chr->info.alias) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO:
        if (!(*deviceStr = qemuBuildVirtioSerialPortDevStr(chr, qemuCaps)))
                goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_NONE:
    case VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_LAST:
        return ret;
    }

    ret = 0;
cleanup:
    VIR_FREE(addr);
    return ret;
}

static int
qemuBuildConsoleChrDeviceStr(char **deviceStr,
                             virDomainChrDefPtr chr,
                             virQEMUCapsPtr qemuCaps)
{
    int ret = -1;

    switch ((enum virDomainChrConsoleTargetType) chr->targetType) {
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLP:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SCLPLM:
        if (!(*deviceStr = qemuBuildSclpDevStr(chr)))
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO:
        if (!(*deviceStr = qemuBuildVirtioSerialPortDevStr(chr, qemuCaps)))
            goto cleanup;
        break;

    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_UML:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LXC:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_OPENVZ:
    case VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LAST:
        break;
    }

    ret = 0;
cleanup:
    return ret;
}

int
qemuBuildChrDeviceStr(char **deviceStr,
                      virDomainDefPtr vmdef,
                      virDomainChrDefPtr chr,
                      virQEMUCapsPtr qemuCaps)
{
    int ret = -1;

    switch ((enum virDomainChrDeviceType) chr->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        ret = qemuBuildSerialChrDeviceStr(deviceStr, chr, qemuCaps,
                                          vmdef->os.arch,
                                          vmdef->os.machine);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
        ret = qemuBuildParallelChrDeviceStr(deviceStr, chr);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        ret = qemuBuildChannelChrDeviceStr(deviceStr, chr, qemuCaps);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        ret = qemuBuildConsoleChrDeviceStr(deviceStr, chr, qemuCaps);
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        return ret;
    }

    return ret;
}


/*
 * This method takes a string representing a QEMU command line ARGV set
 * optionally prefixed by a list of environment variables. It then tries
 * to split it up into a NULL terminated list of env & argv, splitting
 * on space
 */
static int qemuStringToArgvEnv(const char *args,
                               const char ***retenv,
                               const char ***retargv)
{
    char **arglist = NULL;
    int argcount = 0;
    int argalloc = 0;
    int envend;
    size_t i;
    const char *curr = args;
    const char *start;
    const char **progenv = NULL;
    const char **progargv = NULL;

    /* Iterate over string, splitting on sequences of ' ' */
    while (curr && *curr != '\0') {
        char *arg;
        const char *next;

        start = curr;
        /* accept a space in CEPH_ARGS */
        if (STRPREFIX(curr, "CEPH_ARGS=-m ")) {
            start += strlen("CEPH_ARGS=-m ");
        }
        if (*start == '\'') {
            if (start == curr)
                curr++;
            next = strchr(start + 1, '\'');
        } else if (*start == '"') {
            if (start == curr)
                curr++;
            next = strchr(start + 1, '"');
        } else {
            next = strchr(start, ' ');
        }
        if (!next)
            next = strchr(curr, '\n');

        if (VIR_STRNDUP(arg, curr, next ? next - curr : -1) < 0)
            goto error;

        if (next && (*next == '\'' || *next == '"'))
            next++;

        if (argalloc == argcount) {
            if (VIR_REALLOC_N(arglist, argalloc+10) < 0) {
                VIR_FREE(arg);
                goto error;
            }
            argalloc+=10;
        }

        arglist[argcount++] = arg;

        while (next && c_isspace(*next))
            next++;

        curr = next;
    }

    /* Iterate over list of args, finding first arg not containing
     * the '=' character (eg, skip over env vars FOO=bar) */
    for (envend = 0; ((envend < argcount) &&
                       (strchr(arglist[envend], '=') != NULL));
         envend++)
        ; /* nada */

    /* Copy the list of env vars */
    if (envend > 0) {
        if (VIR_REALLOC_N(progenv, envend+1) < 0)
            goto error;
        for (i = 0; i < envend; i++) {
            progenv[i] = arglist[i];
            arglist[i] = NULL;
        }
        progenv[i] = NULL;
    }

    /* Copy the list of argv */
    if (VIR_REALLOC_N(progargv, argcount-envend + 1) < 0)
        goto error;
    for (i = envend; i < argcount; i++)
        progargv[i-envend] = arglist[i];
    progargv[i-envend] = NULL;

    VIR_FREE(arglist);

    *retenv = progenv;
    *retargv = progargv;

    return 0;

error:
    for (i = 0; progenv && progenv[i]; i++)
        VIR_FREE(progenv[i]);
    VIR_FREE(progenv);
    for (i = 0; i < argcount; i++)
        VIR_FREE(arglist[i]);
    VIR_FREE(arglist);
    return -1;
}


/*
 * Search for a named env variable, and return the value part
 */
static const char *qemuFindEnv(const char **progenv,
                               const char *name)
{
    size_t i;
    int len = strlen(name);

    for (i = 0; progenv && progenv[i]; i++) {
        if (STREQLEN(progenv[i], name, len) &&
            progenv[i][len] == '=')
            return progenv[i] + len + 1;
    }
    return NULL;
}

/*
 * Takes a string containing a set of key=value,key=value,key...
 * parameters and splits them up, returning two arrays with
 * the individual keys and values. If allowEmptyValue is nonzero,
 * the "=value" part is optional and if a key with no value is found,
 * NULL is be placed into corresponding place in retvalues.
 */
int
qemuParseKeywords(const char *str,
                  char ***retkeywords,
                  char ***retvalues,
                  int allowEmptyValue)
{
    int keywordCount = 0;
    int keywordAlloc = 0;
    char **keywords = NULL;
    char **values = NULL;
    const char *start = str;
    const char *end;
    size_t i;

    *retkeywords = NULL;
    *retvalues = NULL;
    end = start + strlen(str);

    while (start) {
        const char *separator;
        const char *endmark;
        char *keyword;
        char *value = NULL;

        endmark = start;
        do {
            /* Qemu accepts ',,' as an escape for a literal comma;
             * skip past those here while searching for the end of the
             * value, then strip them down below */
            endmark = strchr(endmark, ',');
        } while (endmark && endmark[1] == ',' && (endmark += 2));
        if (!endmark)
            endmark = end;
        if (!(separator = strchr(start, '=')))
            separator = end;

        if (separator >= endmark) {
            if (!allowEmptyValue) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("malformed keyword arguments in '%s'"), str);
                goto error;
            }
            separator = endmark;
        }

        if (VIR_STRNDUP(keyword, start, separator - start) < 0)
            goto error;

        if (separator < endmark) {
            separator++;
            if (VIR_STRNDUP(value, separator, endmark - separator) < 0) {
                VIR_FREE(keyword);
                goto error;
            }
            if (strchr(value, ',')) {
                char *p = strchr(value, ',') + 1;
                char *q = p + 1;
                while (*q) {
                    if (*q == ',')
                        q++;
                    *p++ = *q++;
                }
                *p = '\0';
            }
        }

        if (keywordAlloc == keywordCount) {
            if (VIR_REALLOC_N(keywords, keywordAlloc + 10) < 0 ||
                VIR_REALLOC_N(values, keywordAlloc + 10) < 0) {
                VIR_FREE(keyword);
                VIR_FREE(value);
                goto error;
            }
            keywordAlloc += 10;
        }

        keywords[keywordCount] = keyword;
        values[keywordCount] = value;
        keywordCount++;

        start = endmark < end ? endmark + 1 : NULL;
    }

    *retkeywords = keywords;
    *retvalues = values;

    return keywordCount;

error:
    for (i = 0; i < keywordCount; i++) {
        VIR_FREE(keywords[i]);
        VIR_FREE(values[i]);
    }
    VIR_FREE(keywords);
    VIR_FREE(values);
    return -1;
}

/*
 * Tries to parse new style QEMU -drive  args.
 *
 * eg -drive file=/dev/HostVG/VirtData1,if=ide,index=1
 *
 * Will fail if not using the 'index' keyword
 */
static virDomainDiskDefPtr
qemuParseCommandLineDisk(virDomainXMLOptionPtr xmlopt,
                         const char *val,
                         int nvirtiodisk,
                         bool old_style_ceph_args)
{
    virDomainDiskDefPtr def = NULL;
    char **keywords;
    char **values;
    int nkeywords;
    size_t i;
    int idx = -1;
    int busid = -1;
    int unitid = -1;
    int trans = VIR_DOMAIN_DISK_TRANS_DEFAULT;

    if ((nkeywords = qemuParseKeywords(val,
                                       &keywords,
                                       &values, 0)) < 0)
        return NULL;

    if (VIR_ALLOC(def) < 0)
        goto cleanup;

    def->bus = VIR_DOMAIN_DISK_BUS_IDE;
    def->device = VIR_DOMAIN_DISK_DEVICE_DISK;
    def->type = VIR_DOMAIN_DISK_TYPE_FILE;

    for (i = 0; i < nkeywords; i++) {
        if (STREQ(keywords[i], "file")) {
            if (values[i] && STRNEQ(values[i], "")) {
                def->src = values[i];
                values[i] = NULL;
                if (STRPREFIX(def->src, "/dev/"))
                    def->type = VIR_DOMAIN_DISK_TYPE_BLOCK;
                else if (STRPREFIX(def->src, "nbd:") ||
                         STRPREFIX(def->src, "nbd+")) {
                    def->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                    def->protocol = VIR_DOMAIN_DISK_PROTOCOL_NBD;

                    if (qemuParseNBDString(def) < 0)
                        goto error;
                } else if (STRPREFIX(def->src, "rbd:")) {
                    char *p = def->src;

                    def->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                    def->protocol = VIR_DOMAIN_DISK_PROTOCOL_RBD;
                    if (VIR_STRDUP(def->src, p + strlen("rbd:")) < 0)
                        goto error;
                    /* old-style CEPH_ARGS env variable is parsed later */
                    if (!old_style_ceph_args && qemuParseRBDString(def) < 0)
                        goto cleanup;

                    VIR_FREE(p);
                } else if (STRPREFIX(def->src, "gluster:") ||
                           STRPREFIX(def->src, "gluster+")) {
                    def->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                    def->protocol = VIR_DOMAIN_DISK_PROTOCOL_GLUSTER;

                    if (qemuParseGlusterString(def) < 0)
                        goto error;
                } else if (STRPREFIX(def->src, "iscsi:")) {
                    def->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                    def->protocol = VIR_DOMAIN_DISK_PROTOCOL_ISCSI;

                    if (qemuParseISCSIString(def) < 0)
                        goto error;
                } else if (STRPREFIX(def->src, "sheepdog:")) {
                    char *p = def->src;
                    char *port, *vdi;

                    def->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                    def->protocol = VIR_DOMAIN_DISK_PROTOCOL_SHEEPDOG;
                    if (VIR_STRDUP(def->src, p + strlen("sheepdog:")) < 0)
                        goto error;

                    /* def->src must be [vdiname] or [host]:[port]:[vdiname] */
                    port = strchr(def->src, ':');
                    if (port) {
                        *port++ = '\0';
                        vdi = strchr(port, ':');
                        if (!vdi) {
                            virReportError(VIR_ERR_INTERNAL_ERROR,
                                           _("cannot parse sheepdog filename '%s'"), p);
                            goto error;
                        }
                        *vdi++ = '\0';
                        if (VIR_ALLOC(def->hosts) < 0)
                            goto error;
                        def->nhosts = 1;
                        def->hosts->name = def->src;
                        if (VIR_STRDUP(def->hosts->port, port) < 0)
                            goto error;
                        def->hosts->transport = VIR_DOMAIN_DISK_PROTO_TRANS_TCP;
                        def->hosts->socket = NULL;
                        if (VIR_STRDUP(def->src, vdi) < 0)
                            goto error;
                    }

                    VIR_FREE(p);
                } else
                    def->type = VIR_DOMAIN_DISK_TYPE_FILE;
            } else {
                def->type = VIR_DOMAIN_DISK_TYPE_FILE;
            }
        } else if (STREQ(keywords[i], "if")) {
            if (STREQ(values[i], "ide"))
                def->bus = VIR_DOMAIN_DISK_BUS_IDE;
            else if (STREQ(values[i], "scsi"))
                def->bus = VIR_DOMAIN_DISK_BUS_SCSI;
            else if (STREQ(values[i], "virtio"))
                def->bus = VIR_DOMAIN_DISK_BUS_VIRTIO;
            else if (STREQ(values[i], "xen"))
                def->bus = VIR_DOMAIN_DISK_BUS_XEN;
        } else if (STREQ(keywords[i], "media")) {
            if (STREQ(values[i], "cdrom")) {
                def->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
                def->readonly = true;
            } else if (STREQ(values[i], "floppy"))
                def->device = VIR_DOMAIN_DISK_DEVICE_FLOPPY;
        } else if (STREQ(keywords[i], "format")) {
            if (VIR_STRDUP(def->driverName, "qemu") < 0)
                goto error;
            def->format = virStorageFileFormatTypeFromString(values[i]);
        } else if (STREQ(keywords[i], "cache")) {
            if (STREQ(values[i], "off") ||
                STREQ(values[i], "none"))
                def->cachemode = VIR_DOMAIN_DISK_CACHE_DISABLE;
            else if (STREQ(values[i], "writeback") ||
                     STREQ(values[i], "on"))
                def->cachemode = VIR_DOMAIN_DISK_CACHE_WRITEBACK;
            else if (STREQ(values[i], "writethrough"))
                def->cachemode = VIR_DOMAIN_DISK_CACHE_WRITETHRU;
            else if (STREQ(values[i], "directsync"))
                def->cachemode = VIR_DOMAIN_DISK_CACHE_DIRECTSYNC;
            else if (STREQ(values[i], "unsafe"))
                def->cachemode = VIR_DOMAIN_DISK_CACHE_UNSAFE;
        } else if (STREQ(keywords[i], "werror")) {
            if (STREQ(values[i], "stop"))
                def->error_policy = VIR_DOMAIN_DISK_ERROR_POLICY_STOP;
            else if (STREQ(values[i], "report"))
                def->error_policy = VIR_DOMAIN_DISK_ERROR_POLICY_REPORT;
            else if (STREQ(values[i], "ignore"))
                def->error_policy = VIR_DOMAIN_DISK_ERROR_POLICY_IGNORE;
            else if (STREQ(values[i], "enospc"))
                def->error_policy = VIR_DOMAIN_DISK_ERROR_POLICY_ENOSPACE;
        } else if (STREQ(keywords[i], "rerror")) {
            if (STREQ(values[i], "stop"))
                def->rerror_policy = VIR_DOMAIN_DISK_ERROR_POLICY_STOP;
            else if (STREQ(values[i], "report"))
                def->rerror_policy = VIR_DOMAIN_DISK_ERROR_POLICY_REPORT;
            else if (STREQ(values[i], "ignore"))
                def->rerror_policy = VIR_DOMAIN_DISK_ERROR_POLICY_IGNORE;
        } else if (STREQ(keywords[i], "index")) {
            if (virStrToLong_i(values[i], NULL, 10, &idx) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse drive index '%s'"), val);
                goto error;
            }
        } else if (STREQ(keywords[i], "bus")) {
            if (virStrToLong_i(values[i], NULL, 10, &busid) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse drive bus '%s'"), val);
                goto error;
            }
        } else if (STREQ(keywords[i], "unit")) {
            if (virStrToLong_i(values[i], NULL, 10, &unitid) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse drive unit '%s'"), val);
                goto error;
            }
        } else if (STREQ(keywords[i], "readonly")) {
            if ((values[i] == NULL) || STREQ(values[i], "on"))
                def->readonly = true;
        } else if (STREQ(keywords[i], "aio")) {
            if ((def->iomode = virDomainDiskIoTypeFromString(values[i])) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse io mode '%s'"), values[i]);
            }
        } else if (STREQ(keywords[i], "cyls")) {
            if (virStrToLong_ui(values[i], NULL, 10,
                                &(def->geometry.cylinders)) < 0) {
                virDomainDiskDefFree(def);
                def = NULL;
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse cylinders value'%s'"),
                               values[i]);
            }
        } else if (STREQ(keywords[i], "heads")) {
            if (virStrToLong_ui(values[i], NULL, 10,
                                &(def->geometry.heads)) < 0) {
                virDomainDiskDefFree(def);
                def = NULL;
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse heads value'%s'"),
                               values[i]);
            }
        } else if (STREQ(keywords[i], "secs")) {
            if (virStrToLong_ui(values[i], NULL, 10,
                                &(def->geometry.sectors)) < 0) {
                virDomainDiskDefFree(def);
                def = NULL;
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse sectors value'%s'"),
                               values[i]);
            }
        } else if (STREQ(keywords[i], "trans")) {
            def->geometry.trans =
                virDomainDiskGeometryTransTypeFromString(values[i]);
            if ((trans < VIR_DOMAIN_DISK_TRANS_DEFAULT) ||
                (trans >= VIR_DOMAIN_DISK_TRANS_LAST)) {
                virDomainDiskDefFree(def);
                def = NULL;
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse translation value'%s'"),
                               values[i]);
            }
        }
    }

    if (def->rerror_policy == def->error_policy)
        def->rerror_policy = 0;

    if (!def->src &&
        def->device == VIR_DOMAIN_DISK_DEVICE_DISK &&
        def->type != VIR_DOMAIN_DISK_TYPE_NETWORK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("missing file parameter in drive '%s'"), val);
        goto error;
    }
    if (idx == -1 &&
        def->bus == VIR_DOMAIN_DISK_BUS_VIRTIO)
        idx = nvirtiodisk;

    if (idx == -1 &&
        unitid == -1 &&
        busid == -1) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("missing index/unit/bus parameter in drive '%s'"),
                       val);
        goto error;
    }

    if (idx == -1) {
        if (unitid == -1)
            unitid = 0;
        if (busid == -1)
            busid = 0;
        switch (def->bus) {
        case VIR_DOMAIN_DISK_BUS_IDE:
            idx = (busid * 2) + unitid;
            break;
        case VIR_DOMAIN_DISK_BUS_SCSI:
            idx = (busid * 7) + unitid;
            break;
        default:
            idx = unitid;
            break;
        }
    }

    if (def->bus == VIR_DOMAIN_DISK_BUS_IDE) {
        ignore_value(VIR_STRDUP(def->dst, "hda"));
    } else if (def->bus == VIR_DOMAIN_DISK_BUS_SCSI) {
        ignore_value(VIR_STRDUP(def->dst, "sda"));
    } else if (def->bus == VIR_DOMAIN_DISK_BUS_VIRTIO) {
        ignore_value(VIR_STRDUP(def->dst, "vda"));
    } else if (def->bus == VIR_DOMAIN_DISK_BUS_XEN) {
        ignore_value(VIR_STRDUP(def->dst, "xvda"));
    } else {
        ignore_value(VIR_STRDUP(def->dst, "hda"));
    }

    if (!def->dst)
        goto error;
    if (STREQ(def->dst, "xvda"))
        def->dst[3] = 'a' + idx;
    else
        def->dst[2] = 'a' + idx;

    if (virDomainDiskDefAssignAddress(xmlopt, def) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid device name '%s'"), def->dst);
        virDomainDiskDefFree(def);
        def = NULL;
        /* fall through to "cleanup" */
    }

cleanup:
    for (i = 0; i < nkeywords; i++) {
        VIR_FREE(keywords[i]);
        VIR_FREE(values[i]);
    }
    VIR_FREE(keywords);
    VIR_FREE(values);
    return def;

error:
    virDomainDiskDefFree(def);
    def = NULL;
    goto cleanup;
}

/*
 * Tries to find a NIC definition matching a vlan we want
 */
static const char *
qemuFindNICForVLAN(int nnics,
                   const char **nics,
                   int wantvlan)
{
    size_t i;
    for (i = 0; i < nnics; i++) {
        int gotvlan;
        const char *tmp = strstr(nics[i], "vlan=");
        char *end;
        if (!tmp)
            continue;

        tmp += strlen("vlan=");

        if (virStrToLong_i(tmp, &end, 10, &gotvlan) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot parse NIC vlan in '%s'"), nics[i]);
            return NULL;
        }

        if (gotvlan == wantvlan)
            return nics[i];
    }

    if (wantvlan == 0 && nnics > 0)
        return nics[0];

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("cannot find NIC definition for vlan %d"), wantvlan);
    return NULL;
}


/*
 * Tries to parse a QEMU -net backend argument. Gets given
 * a list of all known -net frontend arguments to try and
 * match up against. Horribly complicated stuff
 */
static virDomainNetDefPtr
qemuParseCommandLineNet(virDomainXMLOptionPtr xmlopt,
                        const char *val,
                        int nnics,
                        const char **nics)
{
    virDomainNetDefPtr def = NULL;
    char **keywords = NULL;
    char **values = NULL;
    int nkeywords;
    const char *nic;
    int wantvlan = 0;
    const char *tmp;
    bool genmac = true;
    size_t i;

    tmp = strchr(val, ',');

    if (tmp) {
        if ((nkeywords = qemuParseKeywords(tmp+1,
                                           &keywords,
                                           &values, 0)) < 0)
            return NULL;
    } else {
        nkeywords = 0;
    }

    if (VIR_ALLOC(def) < 0)
        goto cleanup;

    /* 'tap' could turn into libvirt type=ethernet, type=bridge or
     * type=network, but we can't tell, so use the generic config */
    if (STRPREFIX(val, "tap,"))
        def->type = VIR_DOMAIN_NET_TYPE_ETHERNET;
    else if (STRPREFIX(val, "socket"))
        def->type = VIR_DOMAIN_NET_TYPE_CLIENT;
    else if (STRPREFIX(val, "user"))
        def->type = VIR_DOMAIN_NET_TYPE_USER;
    else
        def->type = VIR_DOMAIN_NET_TYPE_ETHERNET;

    for (i = 0; i < nkeywords; i++) {
        if (STREQ(keywords[i], "vlan")) {
            if (virStrToLong_i(values[i], NULL, 10, &wantvlan) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse vlan in '%s'"), val);
                virDomainNetDefFree(def);
                def = NULL;
                goto cleanup;
            }
        } else if (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET &&
                   STREQ(keywords[i], "script") && STRNEQ(values[i], "")) {
            def->script = values[i];
            values[i] = NULL;
        } else if (def->type == VIR_DOMAIN_NET_TYPE_ETHERNET &&
                   STREQ(keywords[i], "ifname")) {
            def->ifname = values[i];
            values[i] = NULL;
        }
    }


    /* Done parsing the nic backend. Now to try and find corresponding
     * frontend, based off vlan number. NB this assumes a 1-1 mapping
     */

    nic = qemuFindNICForVLAN(nnics, nics, wantvlan);
    if (!nic) {
        virDomainNetDefFree(def);
        def = NULL;
        goto cleanup;
    }

    if (!STRPREFIX(nic, "nic")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot parse NIC definition '%s'"), nic);
        virDomainNetDefFree(def);
        def = NULL;
        goto cleanup;
    }

    for (i = 0; i < nkeywords; i++) {
        VIR_FREE(keywords[i]);
        VIR_FREE(values[i]);
    }
    VIR_FREE(keywords);
    VIR_FREE(values);

    if (STRPREFIX(nic, "nic,")) {
        if ((nkeywords = qemuParseKeywords(nic + strlen("nic,"),
                                           &keywords,
                                           &values, 0)) < 0) {
            virDomainNetDefFree(def);
            def = NULL;
            goto cleanup;
        }
    } else {
        nkeywords = 0;
    }

    for (i = 0; i < nkeywords; i++) {
        if (STREQ(keywords[i], "macaddr")) {
            genmac = false;
            if (virMacAddrParse(values[i], &def->mac) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unable to parse mac address '%s'"),
                               values[i]);
                virDomainNetDefFree(def);
                def = NULL;
                goto cleanup;
            }
        } else if (STREQ(keywords[i], "model")) {
            def->model = values[i];
            values[i] = NULL;
        } else if (STREQ(keywords[i], "vhost")) {
            if ((values[i] == NULL) || STREQ(values[i], "on")) {
                def->driver.virtio.name = VIR_DOMAIN_NET_BACKEND_TYPE_VHOST;
            } else if (STREQ(keywords[i], "off")) {
                def->driver.virtio.name = VIR_DOMAIN_NET_BACKEND_TYPE_QEMU;
            }
        } else if (STREQ(keywords[i], "sndbuf") && values[i]) {
            if (virStrToLong_ul(values[i], NULL, 10, &def->tune.sndbuf) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse sndbuf size in '%s'"), val);
                virDomainNetDefFree(def);
                def = NULL;
                goto cleanup;
            }
            def->tune.sndbuf_specified = true;
        }
    }

    if (genmac)
        virDomainNetGenerateMAC(xmlopt, &def->mac);

cleanup:
    for (i = 0; i < nkeywords; i++) {
        VIR_FREE(keywords[i]);
        VIR_FREE(values[i]);
    }
    VIR_FREE(keywords);
    VIR_FREE(values);
    return def;
}


/*
 * Tries to parse a QEMU PCI device
 */
static virDomainHostdevDefPtr
qemuParseCommandLinePCI(const char *val)
{
    int bus = 0, slot = 0, func = 0;
    const char *start;
    char *end;
    virDomainHostdevDefPtr def = virDomainHostdevDefAlloc();

    if (!def)
       goto error;

    if (!STRPREFIX(val, "host=")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown PCI device syntax '%s'"), val);
        goto error;
    }

    start = val + strlen("host=");
    if (virStrToLong_i(start, &end, 16, &bus) < 0 || *end != ':') {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot extract PCI device bus '%s'"), val);
        goto error;
    }
    start = end + 1;
    if (virStrToLong_i(start, &end, 16, &slot) < 0 || *end != '.') {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot extract PCI device slot '%s'"), val);
        goto error;
    }
    start = end + 1;
    if (virStrToLong_i(start, NULL, 16, &func) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot extract PCI device function '%s'"), val);
        goto error;
    }

    def->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
    def->managed = true;
    def->source.subsys.type = VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI;
    def->source.subsys.u.pci.addr.bus = bus;
    def->source.subsys.u.pci.addr.slot = slot;
    def->source.subsys.u.pci.addr.function = func;
    return def;

 error:
    virDomainHostdevDefFree(def);
    return NULL;
}


/*
 * Tries to parse a QEMU USB device
 */
static virDomainHostdevDefPtr
qemuParseCommandLineUSB(const char *val)
{
    virDomainHostdevDefPtr def = virDomainHostdevDefAlloc();
    int first = 0, second = 0;
    const char *start;
    char *end;

    if (!def)
       goto error;

    if (!STRPREFIX(val, "host:")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown USB device syntax '%s'"), val);
        goto error;
    }

    start = val + strlen("host:");
    if (strchr(start, ':')) {
        if (virStrToLong_i(start, &end, 16, &first) < 0 || *end != ':') {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot extract USB device vendor '%s'"), val);
            goto error;
        }
        start = end + 1;
        if (virStrToLong_i(start, NULL, 16, &second) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot extract USB device product '%s'"), val);
            goto error;
        }
    } else {
        if (virStrToLong_i(start, &end, 10, &first) < 0 || *end != '.') {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot extract USB device bus '%s'"), val);
            goto error;
        }
        start = end + 1;
        if (virStrToLong_i(start, NULL, 10, &second) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot extract USB device address '%s'"), val);
            goto error;
        }
    }

    def->mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
    def->managed = false;
    def->source.subsys.type = VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB;
    if (*end == '.') {
        def->source.subsys.u.usb.bus = first;
        def->source.subsys.u.usb.device = second;
    } else {
        def->source.subsys.u.usb.vendor = first;
        def->source.subsys.u.usb.product = second;
    }
    return def;

 error:
    virDomainHostdevDefFree(def);
    return NULL;
}


/*
 * Tries to parse a QEMU serial/parallel device
 */
static int
qemuParseCommandLineChr(virDomainChrSourceDefPtr source,
                        const char *val)
{
    if (STREQ(val, "null")) {
        source->type = VIR_DOMAIN_CHR_TYPE_NULL;
    } else if (STREQ(val, "vc")) {
        source->type = VIR_DOMAIN_CHR_TYPE_VC;
    } else if (STREQ(val, "pty")) {
        source->type = VIR_DOMAIN_CHR_TYPE_PTY;
    } else if (STRPREFIX(val, "file:")) {
        source->type = VIR_DOMAIN_CHR_TYPE_FILE;
        if (VIR_STRDUP(source->data.file.path, val + strlen("file:")) < 0)
            goto error;
    } else if (STRPREFIX(val, "pipe:")) {
        source->type = VIR_DOMAIN_CHR_TYPE_PIPE;
        if (VIR_STRDUP(source->data.file.path, val + strlen("pipe:")) < 0)
            goto error;
    } else if (STREQ(val, "stdio")) {
        source->type = VIR_DOMAIN_CHR_TYPE_STDIO;
    } else if (STRPREFIX(val, "udp:")) {
        const char *svc1, *host2, *svc2;
        source->type = VIR_DOMAIN_CHR_TYPE_UDP;
        val += strlen("udp:");
        svc1 = strchr(val, ':');
        host2 = svc1 ? strchr(svc1, '@') : NULL;
        svc2 = host2 ? strchr(host2, ':') : NULL;

        if (svc1 && svc1 != val &&
            VIR_STRNDUP(source->data.udp.connectHost, val, svc1 - val) < 0)
            goto error;

        if (svc1) {
            svc1++;
            if (VIR_STRNDUP(source->data.udp.connectService, svc1,
                            host2 ? host2 - svc1 : strlen(svc1)) < 0)
                goto error;
        }

        if (host2) {
            host2++;
            if (svc2 && svc2 != host2 &&
                VIR_STRNDUP(source->data.udp.bindHost, host2, svc2 - host2) < 0)
                goto error;
        }

        if (svc2) {
            svc2++;
            if (STRNEQ(svc2, "0")) {
                if (VIR_STRDUP(source->data.udp.bindService, svc2) < 0)
                    goto error;
            }
        }
    } else if (STRPREFIX(val, "tcp:") ||
               STRPREFIX(val, "telnet:")) {
        const char *opt, *svc;
        source->type = VIR_DOMAIN_CHR_TYPE_TCP;
        if (STRPREFIX(val, "tcp:")) {
            val += strlen("tcp:");
        } else {
            val += strlen("telnet:");
            source->data.tcp.protocol = VIR_DOMAIN_CHR_TCP_PROTOCOL_TELNET;
        }
        svc = strchr(val, ':');
        if (!svc) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find port number in character device %s"), val);
            goto error;
        }
        opt = strchr(svc, ',');
        if (opt && strstr(opt, "server"))
            source->data.tcp.listen = true;

        if (VIR_STRNDUP(source->data.tcp.host, val, svc - val) < 0)
            goto error;
        svc++;
        if (VIR_STRNDUP(source->data.tcp.service, svc, opt ? opt - svc : -1) < 0)
            goto error;
    } else if (STRPREFIX(val, "unix:")) {
        const char *opt;
        val += strlen("unix:");
        opt = strchr(val, ',');
        source->type = VIR_DOMAIN_CHR_TYPE_UNIX;
        if (VIR_STRNDUP(source->data.nix.path, val, opt ? opt - val : -1) < 0)
            goto error;

    } else if (STRPREFIX(val, "/dev")) {
        source->type = VIR_DOMAIN_CHR_TYPE_DEV;
        if (VIR_STRDUP(source->data.file.path, val) < 0)
            goto error;
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown character device syntax %s"), val);
        goto error;
    }

    return 0;

error:
    return -1;
}


static virCPUDefPtr
qemuInitGuestCPU(virDomainDefPtr dom)
{
    if (!dom->cpu) {
        virCPUDefPtr cpu;

        if (VIR_ALLOC(cpu) < 0)
            return NULL;

        cpu->type = VIR_CPU_TYPE_GUEST;
        cpu->match = VIR_CPU_MATCH_EXACT;
        dom->cpu = cpu;
    }

    return dom->cpu;
}


static int
qemuParseCommandLineCPU(virDomainDefPtr dom,
                        const char *val)
{
    virCPUDefPtr cpu = NULL;
    char **tokens;
    char **hv_tokens = NULL;
    char *model = NULL;
    int ret = -1;
    size_t i;

    if (!(tokens = virStringSplit(val, ",", 0)))
        goto cleanup;

    if (tokens[0] == NULL)
        goto syntax;

    for (i = 0; tokens[i] != NULL; i++) {
        if (*tokens[i] == '\0')
            goto syntax;

        if (i == 0) {
            if (VIR_STRDUP(model, tokens[i]) < 0)
                goto cleanup;

            if (!STREQ(model, "qemu32") && !STREQ(model, "qemu64")) {
                if (!(cpu = qemuInitGuestCPU(dom)))
                    goto cleanup;

                cpu->model = model;
                model = NULL;
            }
        } else if (*tokens[i] == '+' || *tokens[i] == '-') {
            const char *feature = tokens[i] + 1; /* '+' or '-' */
            int policy;

            if (*tokens[i] == '+')
                policy = VIR_CPU_FEATURE_REQUIRE;
            else
                policy = VIR_CPU_FEATURE_DISABLE;

            if (*feature == '\0')
                goto syntax;

            if (STREQ(feature, "kvmclock")) {
                bool present = (policy == VIR_CPU_FEATURE_REQUIRE);
                size_t j;

                for (j = 0; j < dom->clock.ntimers; j++) {
                    if (dom->clock.timers[j]->name == VIR_DOMAIN_TIMER_NAME_KVMCLOCK)
                        break;
                }

                if (j == dom->clock.ntimers) {
                    if (VIR_REALLOC_N(dom->clock.timers, j + 1) < 0 ||
                        VIR_ALLOC(dom->clock.timers[j]) < 0)
                        goto cleanup;
                    dom->clock.timers[j]->name = VIR_DOMAIN_TIMER_NAME_KVMCLOCK;
                    dom->clock.timers[j]->present = present;
                    dom->clock.timers[j]->tickpolicy = -1;
                    dom->clock.timers[j]->track = -1;
                    dom->clock.ntimers++;
                } else if (dom->clock.timers[j]->present != -1 &&
                    dom->clock.timers[j]->present != present) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("conflicting occurrences of kvmclock feature"));
                    goto cleanup;
                }
            } else if (STREQ(feature, "kvm_pv_eoi")) {
                if (policy == VIR_CPU_FEATURE_REQUIRE)
                    dom->apic_eoi = VIR_DOMAIN_FEATURE_STATE_ON;
                else
                    dom->apic_eoi = VIR_DOMAIN_FEATURE_STATE_OFF;
            } else {
                if (!cpu) {
                    if (!(cpu = qemuInitGuestCPU(dom)))
                        goto cleanup;

                    cpu->model = model;
                    model = NULL;
                }

                if (virCPUDefAddFeature(cpu, feature, policy) < 0)
                    goto cleanup;
            }
        } else if (STRPREFIX(tokens[i], "hv_")) {
            const char *token = tokens[i] + 3; /* "hv_" */
            const char *feature, *value;
            int f;

            if (*token == '\0')
                goto syntax;

            if (!(hv_tokens = virStringSplit(token, "=", 2)))
                goto cleanup;

            feature = hv_tokens[0];
            value = hv_tokens[1];

            if (*feature == '\0')
                goto syntax;

            dom->features |= (1 << VIR_DOMAIN_FEATURE_HYPERV);

            if ((f = virDomainHypervTypeFromString(feature)) < 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported HyperV Enlightenment feature "
                                 "'%s'"), feature);
                goto cleanup;
            }

            switch ((enum virDomainHyperv) f) {
            case VIR_DOMAIN_HYPERV_RELAXED:
            case VIR_DOMAIN_HYPERV_VAPIC:
                if (value) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("HyperV feature '%s' should not "
                                     "have a value"), feature);
                    goto cleanup;
                }
                dom->hyperv_features[f] = VIR_DOMAIN_FEATURE_STATE_ON;
                break;

            case VIR_DOMAIN_HYPERV_SPINLOCKS:
                dom->hyperv_features[f] = VIR_DOMAIN_FEATURE_STATE_ON;
                if (!value) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("missing HyperV spinlock retry count"));
                    goto cleanup;
                }

                if (virStrToLong_ui(value, NULL, 0, &dom->hyperv_spinlocks) < 0) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("cannot parse HyperV spinlock retry count"));
                    goto cleanup;
                }

                if (dom->hyperv_spinlocks < 0xFFF)
                    dom->hyperv_spinlocks = 0xFFF;
                break;

            case VIR_DOMAIN_HYPERV_LAST:
                break;
            }
            virStringFreeList(hv_tokens);
            hv_tokens = NULL;
        }
    }

    if (dom->os.arch == VIR_ARCH_X86_64) {
        bool is_32bit = false;
        if (cpu) {
            virCPUDataPtr cpuData = NULL;

            if (cpuEncode(VIR_ARCH_X86_64, cpu, NULL, &cpuData,
                          NULL, NULL, NULL, NULL) < 0)
                goto cleanup;

            is_32bit = (cpuHasFeature(cpuData, "lm") != 1);
            cpuDataFree(cpuData);
        } else if (model) {
            is_32bit = STREQ(model, "qemu32");
        }

        if (is_32bit)
            dom->os.arch = VIR_ARCH_I686;
    }

    ret = 0;

cleanup:
    VIR_FREE(model);
    virStringFreeList(tokens);
    virStringFreeList(hv_tokens);
    return ret;

syntax:
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("unknown CPU syntax '%s'"), val);
    goto cleanup;
}


static int
qemuParseCommandLineSmp(virDomainDefPtr dom,
                        const char *val)
{
    unsigned int sockets = 0;
    unsigned int cores = 0;
    unsigned int threads = 0;
    unsigned int maxcpus = 0;
    size_t i;
    int nkws;
    char **kws;
    char **vals;
    int n;
    char *end;
    int ret;

    nkws = qemuParseKeywords(val, &kws, &vals, 1);
    if (nkws < 0)
        return -1;

    for (i = 0; i < nkws; i++) {
        if (vals[i] == NULL) {
            if (i > 0 ||
                virStrToLong_i(kws[i], &end, 10, &n) < 0 || *end != '\0')
                goto syntax;
            dom->vcpus = n;
        } else {
            if (virStrToLong_i(vals[i], &end, 10, &n) < 0 || *end != '\0')
                goto syntax;
            if (STREQ(kws[i], "sockets"))
                sockets = n;
            else if (STREQ(kws[i], "cores"))
                cores = n;
            else if (STREQ(kws[i], "threads"))
                threads = n;
            else if (STREQ(kws[i], "maxcpus"))
                maxcpus = n;
            else
                goto syntax;
        }
    }

    dom->maxvcpus = maxcpus ? maxcpus : dom->vcpus;

    if (sockets && cores && threads) {
        virCPUDefPtr cpu;

        if (!(cpu = qemuInitGuestCPU(dom)))
            goto error;
        cpu->sockets = sockets;
        cpu->cores = cores;
        cpu->threads = threads;
    } else if (sockets || cores || threads)
        goto syntax;

    ret = 0;

cleanup:
    for (i = 0; i < nkws; i++) {
        VIR_FREE(kws[i]);
        VIR_FREE(vals[i]);
    }
    VIR_FREE(kws);
    VIR_FREE(vals);

    return ret;

syntax:
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("cannot parse CPU topology '%s'"), val);
error:
    ret = -1;
    goto cleanup;
}


static void
qemuParseCommandLineBootDevs(virDomainDefPtr def, const char *str) {
    int n, b = 0;

    for (n = 0; str[n] && b < VIR_DOMAIN_BOOT_LAST; n++) {
        if (str[n] == 'a')
            def->os.bootDevs[b++] = VIR_DOMAIN_BOOT_FLOPPY;
        else if (str[n] == 'c')
            def->os.bootDevs[b++] = VIR_DOMAIN_BOOT_DISK;
        else if (str[n] == 'd')
            def->os.bootDevs[b++] = VIR_DOMAIN_BOOT_CDROM;
        else if (str[n] == 'n')
            def->os.bootDevs[b++] = VIR_DOMAIN_BOOT_NET;
        else if (str[n] == ',')
            break;
    }
    def->os.nBootDevs = b;
}


/*
 * Analyse the env and argv settings and reconstruct a
 * virDomainDefPtr representing these settings as closely
 * as is practical. This is not an exact science....
 */
virDomainDefPtr qemuParseCommandLine(virCapsPtr qemuCaps,
                                     virDomainXMLOptionPtr xmlopt,
                                     const char **progenv,
                                     const char **progargv,
                                     char **pidfile,
                                     virDomainChrSourceDefPtr *monConfig,
                                     bool *monJSON)
{
    virDomainDefPtr def;
    size_t i;
    bool nographics = false;
    bool fullscreen = false;
    char *path;
    int nnics = 0;
    const char **nics = NULL;
    int video = VIR_DOMAIN_VIDEO_TYPE_CIRRUS;
    int nvirtiodisk = 0;
    qemuDomainCmdlineDefPtr cmd = NULL;
    virDomainDiskDefPtr disk = NULL;
    const char *ceph_args = qemuFindEnv(progenv, "CEPH_ARGS");

    if (pidfile)
        *pidfile = NULL;
    if (monConfig)
        *monConfig = NULL;
    if (monJSON)
        *monJSON = false;

    if (!progargv[0]) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("no emulator path found"));
        return NULL;
    }

    if (VIR_ALLOC(def) < 0)
        goto error;

    /* allocate the cmdlinedef up-front; if it's unused, we'll free it later */
    if (VIR_ALLOC(cmd) < 0)
        goto error;

    if (virUUIDGenerate(def->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to generate uuid"));
        goto error;
    }

    def->id = -1;
    def->mem.cur_balloon = def->mem.max_balloon = 64 * 1024;
    def->maxvcpus = 1;
    def->vcpus = 1;
    def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_UTC;

    def->onReboot = VIR_DOMAIN_LIFECYCLE_RESTART;
    def->onCrash = VIR_DOMAIN_LIFECYCLE_DESTROY;
    def->onPoweroff = VIR_DOMAIN_LIFECYCLE_DESTROY;
    def->virtType = VIR_DOMAIN_VIRT_QEMU;
    if (VIR_STRDUP(def->emulator, progargv[0]) < 0)
        goto error;

    if (strstr(def->emulator, "kvm")) {
        def->virtType = VIR_DOMAIN_VIRT_KVM;
        def->features |= (1 << VIR_DOMAIN_FEATURE_PAE);
    }


    if (strstr(def->emulator, "xenner")) {
        def->virtType = VIR_DOMAIN_VIRT_KVM;
        if (VIR_STRDUP(def->os.type, "xen") < 0)
            goto error;
    } else {
        if (VIR_STRDUP(def->os.type, "hvm") < 0)
            goto error;
    }

    if (STRPREFIX(def->emulator, "qemu"))
        path = def->emulator;
    else
        path = strstr(def->emulator, "qemu");
    if (def->virtType == VIR_DOMAIN_VIRT_KVM)
        def->os.arch = qemuCaps->host.arch;
    else if (path &&
             STRPREFIX(path, "qemu-system-"))
        def->os.arch = virArchFromString(path + strlen("qemu-system-"));
    else
        def->os.arch = VIR_ARCH_I686;

    if ((def->os.arch == VIR_ARCH_I686) ||
        (def->os.arch == VIR_ARCH_X86_64))
        def->features |= (1 << VIR_DOMAIN_FEATURE_ACPI)
        /*| (1 << VIR_DOMAIN_FEATURE_APIC)*/;
#define WANT_VALUE()                                                   \
    const char *val = progargv[++i];                                   \
    if (!val) {                                                        \
        virReportError(VIR_ERR_INTERNAL_ERROR,                        \
                       _("missing value for %s argument"), arg);       \
        goto error;                                                    \
    }

    /* One initial loop to get list of NICs, so we
     * can correlate them later */
    for (i = 1; progargv[i]; i++) {
        const char *arg = progargv[i];
        /* Make sure we have a single - for all options to
           simplify next logic */
        if (STRPREFIX(arg, "--"))
            arg++;

        if (STREQ(arg, "-net")) {
            WANT_VALUE();
            if (STRPREFIX(val, "nic")) {
                if (VIR_REALLOC_N(nics, nnics+1) < 0)
                    goto error;
                nics[nnics++] = val;
            }
        }
    }

    /* Now the real processing loop */
    for (i = 1; progargv[i]; i++) {
        const char *arg = progargv[i];
        /* Make sure we have a single - for all options to
           simplify next logic */
        if (STRPREFIX(arg, "--"))
            arg++;

        if (STREQ(arg, "-vnc")) {
            virDomainGraphicsDefPtr vnc;
            char *tmp;
            WANT_VALUE();
            if (VIR_ALLOC(vnc) < 0)
                goto error;
            vnc->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;

            if (STRPREFIX(val, "unix:")) {
                /* -vnc unix:/some/big/path */
                if (VIR_STRDUP(vnc->data.vnc.socket, val + 5) < 0) {
                    virDomainGraphicsDefFree(vnc);
                    goto error;
                }
            } else {
                /*
                 * -vnc 127.0.0.1:4
                 * -vnc [2001:1:2:3:4:5:1234:1234]:4
                 * -vnc some.host.name:4
                 */
                char *opts;
                char *port;
                const char *sep = ":";
                if (val[0] == '[')
                    sep = "]:";
                tmp = strstr(val, sep);
                if (!tmp) {
                    virDomainGraphicsDefFree(vnc);
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("missing VNC port number in '%s'"), val);
                    goto error;
                }
                port = tmp + strlen(sep);
                if (virStrToLong_i(port, &opts, 10,
                                   &vnc->data.vnc.port) < 0) {
                    virDomainGraphicsDefFree(vnc);
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("cannot parse VNC port '%s'"), port);
                    goto error;
                }
                if (val[0] == '[')
                    virDomainGraphicsListenSetAddress(vnc, 0,
                                                      val+1, tmp-(val+1), true);
                else
                    virDomainGraphicsListenSetAddress(vnc, 0,
                                                      val, tmp-val, true);
                if (!virDomainGraphicsListenGetAddress(vnc, 0)) {
                    virDomainGraphicsDefFree(vnc);
                    goto error;
                }

                if (*opts == ',') {
                    char *orig_opts;

                    if (VIR_STRDUP(orig_opts, opts + 1) < 0) {
                        virDomainGraphicsDefFree(vnc);
                        goto error;
                    }
                    opts = orig_opts;

                    while (opts && *opts) {
                        char *nextopt = strchr(opts, ',');
                        if (nextopt)
                            *(nextopt++) = '\0';

                        if (STRPREFIX(opts, "websocket")) {
                            char *websocket = opts + strlen("websocket");
                            if (*(websocket++) == '=' &&
                                *websocket) {
                                /* If the websocket continues with
                                 * '=<something>', we'll parse it */
                                if (virStrToLong_i(websocket,
                                                   NULL, 0,
                                                   &vnc->data.vnc.websocket) < 0) {
                                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                                   _("cannot parse VNC "
                                                     "WebSocket port '%s'"),
                                                   websocket);
                                    virDomainGraphicsDefFree(vnc);
                                    VIR_FREE(orig_opts);
                                    goto error;
                                }
                            } else {
                                /* Otherwise, we'll compute the port the same
                                 * way QEMU does, by adding a 5700 to the
                                 * display value. */
                                vnc->data.vnc.websocket =
                                    vnc->data.vnc.port + 5700;
                            }
                        } else if (STRPREFIX(opts, "share=")) {
                            char *sharePolicy = opts + strlen("share=");
                            if (sharePolicy && *sharePolicy) {
                                int policy =
                                    virDomainGraphicsVNCSharePolicyTypeFromString(sharePolicy);

                                if (policy < 0) {
                                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                                   _("unknown vnc display sharing policy '%s'"),
                                                     sharePolicy);
                                    virDomainGraphicsDefFree(vnc);
                                    VIR_FREE(orig_opts);
                                    goto error;
                                } else {
                                    vnc->data.vnc.sharePolicy = policy;
                                }
                            } else {
                                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                               _("missing vnc sharing policy"));
                                virDomainGraphicsDefFree(vnc);
                                VIR_FREE(orig_opts);
                                goto error;
                            }
                        }

                        opts = nextopt;
                    }
                    VIR_FREE(orig_opts);
                }
                vnc->data.vnc.port += 5900;
                vnc->data.vnc.autoport = false;
            }

            if (VIR_REALLOC_N(def->graphics, def->ngraphics+1) < 0) {
                virDomainGraphicsDefFree(vnc);
                goto error;
            }
            def->graphics[def->ngraphics++] = vnc;
        } else if (STREQ(arg, "-m")) {
            int mem;
            WANT_VALUE();
            if (virStrToLong_i(val, NULL, 10, &mem) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, \
                               _("cannot parse memory level '%s'"), val);
                goto error;
            }
            def->mem.cur_balloon = def->mem.max_balloon = mem * 1024;
        } else if (STREQ(arg, "-smp")) {
            WANT_VALUE();
            if (qemuParseCommandLineSmp(def, val) < 0)
                goto error;
        } else if (STREQ(arg, "-uuid")) {
            WANT_VALUE();
            if (virUUIDParse(val, def->uuid) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, \
                               _("cannot parse UUID '%s'"), val);
                goto error;
            }
        } else if (STRPREFIX(arg, "-hd") ||
                   STRPREFIX(arg, "-sd") ||
                   STRPREFIX(arg, "-fd") ||
                   STREQ(arg, "-cdrom")) {
            WANT_VALUE();
            if (VIR_ALLOC(disk) < 0)
                goto error;

            if (STRPREFIX(val, "/dev/"))
                disk->type = VIR_DOMAIN_DISK_TYPE_BLOCK;
            else if (STRPREFIX(val, "nbd:")) {
                disk->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                disk->protocol = VIR_DOMAIN_DISK_PROTOCOL_NBD;
            } else if (STRPREFIX(val, "rbd:")) {
                disk->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                disk->protocol = VIR_DOMAIN_DISK_PROTOCOL_RBD;
                val += strlen("rbd:");
            } else if (STRPREFIX(val, "gluster")) {
                disk->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                disk->protocol = VIR_DOMAIN_DISK_PROTOCOL_GLUSTER;
            } else if (STRPREFIX(val, "sheepdog:")) {
                disk->type = VIR_DOMAIN_DISK_TYPE_NETWORK;
                disk->protocol = VIR_DOMAIN_DISK_PROTOCOL_SHEEPDOG;
                val += strlen("sheepdog:");
            } else
                disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
            if (STREQ(arg, "-cdrom")) {
                disk->device = VIR_DOMAIN_DISK_DEVICE_CDROM;
                if (VIR_STRDUP(disk->dst, "hdc") < 0)
                    goto error;
                disk->readonly = true;
            } else {
                if (STRPREFIX(arg, "-fd")) {
                    disk->device = VIR_DOMAIN_DISK_DEVICE_FLOPPY;
                    disk->bus = VIR_DOMAIN_DISK_BUS_FDC;
                } else {
                    disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
                    if (STRPREFIX(arg, "-hd"))
                        disk->bus = VIR_DOMAIN_DISK_BUS_IDE;
                    else
                        disk->bus = VIR_DOMAIN_DISK_BUS_SCSI;
                }
                if (VIR_STRDUP(disk->dst, arg + 1) < 0)
                    goto error;
            }
            if (VIR_STRDUP(disk->src, val) < 0)
                goto error;

            if (disk->type == VIR_DOMAIN_DISK_TYPE_NETWORK) {
                char *port;

                switch (disk->protocol) {
                case VIR_DOMAIN_DISK_PROTOCOL_NBD:
                    if (qemuParseNBDString(disk) < 0)
                        goto error;
                    break;
                case VIR_DOMAIN_DISK_PROTOCOL_RBD:
                    /* old-style CEPH_ARGS env variable is parsed later */
                    if (!ceph_args && qemuParseRBDString(disk) < 0)
                        goto error;
                    break;
                case VIR_DOMAIN_DISK_PROTOCOL_SHEEPDOG:
                    /* disk->src must be [vdiname] or [host]:[port]:[vdiname] */
                    port = strchr(disk->src, ':');
                    if (port) {
                        char *vdi;

                        *port++ = '\0';
                        vdi = strchr(port, ':');
                        if (!vdi) {
                            virReportError(VIR_ERR_INTERNAL_ERROR,
                                           _("cannot parse sheepdog filename '%s'"), val);
                            goto error;
                        }
                        *vdi++ = '\0';
                        if (VIR_ALLOC(disk->hosts) < 0)
                            goto error;
                        disk->nhosts = 1;
                        disk->hosts->name = disk->src;
                        if (VIR_STRDUP(disk->hosts->port, port) < 0)
                            goto error;
                        if (VIR_STRDUP(disk->src, vdi) < 0)
                            goto error;
                    }
                    break;
                case VIR_DOMAIN_DISK_PROTOCOL_GLUSTER:
                    if (qemuParseGlusterString(disk) < 0)
                        goto error;

                    break;
                case VIR_DOMAIN_DISK_PROTOCOL_ISCSI:
                    if (qemuParseISCSIString(disk) < 0)
                        goto error;

                    break;
                }
            }

            if (virDomainDiskDefAssignAddress(xmlopt, disk) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Cannot assign address for device name '%s'"),
                               disk->dst);
                goto error;
            }

            if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0)
                goto error;
            def->disks[def->ndisks++] = disk;
            disk = NULL;
        } else if (STREQ(arg, "-no-acpi")) {
            def->features &= ~(1 << VIR_DOMAIN_FEATURE_ACPI);
        } else if (STREQ(arg, "-no-reboot")) {
            def->onReboot = VIR_DOMAIN_LIFECYCLE_DESTROY;
        } else if (STREQ(arg, "-no-kvm")) {
            def->virtType = VIR_DOMAIN_VIRT_QEMU;
        } else if (STREQ(arg, "-enable-kvm")) {
            def->virtType = VIR_DOMAIN_VIRT_KVM;
        } else if (STREQ(arg, "-nographic")) {
            nographics = true;
        } else if (STREQ(arg, "-full-screen")) {
            fullscreen = true;
        } else if (STREQ(arg, "-localtime")) {
            def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME;
        } else if (STREQ(arg, "-kernel")) {
            WANT_VALUE();
            if (VIR_STRDUP(def->os.kernel, val) < 0)
                goto error;
        } else if (STREQ(arg, "-bios")) {
            WANT_VALUE();
            if (VIR_STRDUP(def->os.loader, val) < 0)
                goto error;
        } else if (STREQ(arg, "-initrd")) {
            WANT_VALUE();
            if (VIR_STRDUP(def->os.initrd, val) < 0)
                goto error;
        } else if (STREQ(arg, "-append")) {
            WANT_VALUE();
            if (VIR_STRDUP(def->os.cmdline, val) < 0)
                goto error;
        } else if (STREQ(arg, "-dtb")) {
            WANT_VALUE();
            if (VIR_STRDUP(def->os.dtb, val) < 0)
                goto error;
        } else if (STREQ(arg, "-boot")) {
            const char *token = NULL;
            WANT_VALUE();

            if (!strchr(val, ','))
                qemuParseCommandLineBootDevs(def, val);
            else {
                token = val;
                while (token && *token) {
                    if (STRPREFIX(token, "order=")) {
                        token += strlen("order=");
                        qemuParseCommandLineBootDevs(def, token);
                    } else if (STRPREFIX(token, "menu=on")) {
                        def->os.bootmenu = 1;
                    } else if (STRPREFIX(token, "reboot-timeout=")) {
                        int num;
                        char *endptr;
                        if (virStrToLong_i(token + strlen("reboot-timeout="),
                                           &endptr, 10, &num) < 0 ||
                            (*endptr != '\0' && endptr != strchr(token, ','))) {
                            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                           _("cannot parse reboot-timeout value"));
                            goto error;
                        }
                        if (num > 65535)
                            num = 65535;
                        else if (num < -1)
                            num = -1;
                        def->os.bios.rt_delay = num;
                        def->os.bios.rt_set = true;
                    }
                    token = strchr(token, ',');
                    /* This incrementation has to be done here in order to make it
                     * possible to pass the token pointer properly into the loop */
                    if (token)
                        token++;
                }
            }
        } else if (STREQ(arg, "-name")) {
            char *process;
            WANT_VALUE();
            process = strstr(val, ",process=");
            if (process == NULL) {
                if (VIR_STRDUP(def->name, val) < 0)
                    goto error;
            } else {
                if (VIR_STRNDUP(def->name, val, process - val) < 0)
                    goto error;
            }
            if (STREQ(def->name, ""))
                VIR_FREE(def->name);
        } else if (STREQ(arg, "-M") ||
                   STREQ(arg, "-machine")) {
            char *params;
            WANT_VALUE();
            params = strchr(val, ',');
            if (params == NULL) {
                if (VIR_STRDUP(def->os.machine, val) < 0)
                    goto error;
            } else {
                if (VIR_STRNDUP(def->os.machine, val, params - val) < 0)
                    goto error;

                while (params++) {
                    /* prepared for more "-machine" parameters */
                    char *tmp = params;
                    params = strchr(params, ',');

                    if (STRPREFIX(tmp, "dump-guest-core=")) {
                        tmp += strlen("dump-guest-core=");
                        if (params && VIR_STRNDUP(tmp, tmp, params - tmp) < 0)
                            goto error;
                        def->mem.dump_core = virDomainMemDumpTypeFromString(tmp);
                        if (def->mem.dump_core <= 0)
                            def->mem.dump_core = VIR_DOMAIN_MEM_DUMP_DEFAULT;
                        if (params)
                            VIR_FREE(tmp);
                    } else if (STRPREFIX(tmp, "mem-merge=off")) {
                        def->mem.nosharepages = true;
                    }
                }
            }
        } else if (STREQ(arg, "-serial")) {
            WANT_VALUE();
            if (STRNEQ(val, "none")) {
                virDomainChrDefPtr chr;

                if (!(chr = virDomainChrDefNew()))
                    goto error;

                if (qemuParseCommandLineChr(&chr->source, val) < 0) {
                    virDomainChrDefFree(chr);
                    goto error;
                }
                if (VIR_REALLOC_N(def->serials, def->nserials+1) < 0) {
                    virDomainChrDefFree(chr);
                    goto error;
                }
                chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
                chr->target.port = def->nserials;
                def->serials[def->nserials++] = chr;
            }
        } else if (STREQ(arg, "-parallel")) {
            WANT_VALUE();
            if (STRNEQ(val, "none")) {
                virDomainChrDefPtr chr;

                if (!(chr = virDomainChrDefNew()))
                    goto error;

                if (qemuParseCommandLineChr(&chr->source, val) < 0) {
                    virDomainChrDefFree(chr);
                    goto error;
                }
                if (VIR_REALLOC_N(def->parallels, def->nparallels+1) < 0) {
                    virDomainChrDefFree(chr);
                    goto error;
                }
                chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL;
                chr->target.port = def->nparallels;
                def->parallels[def->nparallels++] = chr;
            }
        } else if (STREQ(arg, "-usbdevice")) {
            WANT_VALUE();
            if (STREQ(val, "tablet") ||
                STREQ(val, "mouse")) {
                virDomainInputDefPtr input;
                if (VIR_ALLOC(input) < 0)
                    goto error;
                input->bus = VIR_DOMAIN_INPUT_BUS_USB;
                if (STREQ(val, "tablet"))
                    input->type = VIR_DOMAIN_INPUT_TYPE_TABLET;
                else
                    input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;
                if (VIR_REALLOC_N(def->inputs, def->ninputs+1) < 0) {
                    virDomainInputDefFree(input);
                    goto error;
                }
                def->inputs[def->ninputs++] = input;
            } else if (STRPREFIX(val, "disk:")) {
                if (VIR_ALLOC(disk) < 0)
                    goto error;
                if (VIR_STRDUP(disk->src, val + strlen("disk:")) < 0)
                    goto error;
                if (STRPREFIX(disk->src, "/dev/"))
                    disk->type = VIR_DOMAIN_DISK_TYPE_BLOCK;
                else
                    disk->type = VIR_DOMAIN_DISK_TYPE_FILE;
                disk->device = VIR_DOMAIN_DISK_DEVICE_DISK;
                disk->bus = VIR_DOMAIN_DISK_BUS_USB;
                if (VIR_STRDUP(disk->dst, "sda") < 0)
                    goto error;
                if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0)
                    goto error;
                def->disks[def->ndisks++] = disk;
                disk = NULL;
            } else {
                virDomainHostdevDefPtr hostdev;
                if (!(hostdev = qemuParseCommandLineUSB(val)))
                    goto error;
                if (VIR_REALLOC_N(def->hostdevs, def->nhostdevs+1) < 0) {
                    virDomainHostdevDefFree(hostdev);
                    goto error;
                }
                def->hostdevs[def->nhostdevs++] = hostdev;
            }
        } else if (STREQ(arg, "-net")) {
            WANT_VALUE();
            if (!STRPREFIX(val, "nic") && STRNEQ(val, "none")) {
                virDomainNetDefPtr net;
                if (!(net = qemuParseCommandLineNet(xmlopt, val, nnics, nics)))
                    goto error;
                if (VIR_REALLOC_N(def->nets, def->nnets+1) < 0) {
                    virDomainNetDefFree(net);
                    goto error;
                }
                def->nets[def->nnets++] = net;
            }
        } else if (STREQ(arg, "-drive")) {
            WANT_VALUE();
            if (!(disk = qemuParseCommandLineDisk(xmlopt, val,
                                                  nvirtiodisk,
                                                  ceph_args != NULL)))
                goto error;
            if (VIR_REALLOC_N(def->disks, def->ndisks+1) < 0)
                goto error;
            if (disk->bus == VIR_DOMAIN_DISK_BUS_VIRTIO)
                nvirtiodisk++;

            def->disks[def->ndisks++] = disk;
            disk = NULL;
        } else if (STREQ(arg, "-pcidevice")) {
            virDomainHostdevDefPtr hostdev;
            WANT_VALUE();
            if (!(hostdev = qemuParseCommandLinePCI(val)))
                goto error;
            if (VIR_REALLOC_N(def->hostdevs, def->nhostdevs+1) < 0) {
                virDomainHostdevDefFree(hostdev);
                goto error;
            }
            def->hostdevs[def->nhostdevs++] = hostdev;
        } else if (STREQ(arg, "-soundhw")) {
            const char *start;
            WANT_VALUE();
            start = val;
            while (start) {
                const char *tmp = strchr(start, ',');
                int type = -1;
                if (STRPREFIX(start, "pcspk")) {
                    type = VIR_DOMAIN_SOUND_MODEL_PCSPK;
                } else if (STRPREFIX(start, "sb16")) {
                    type = VIR_DOMAIN_SOUND_MODEL_SB16;
                } else if (STRPREFIX(start, "es1370")) {
                    type = VIR_DOMAIN_SOUND_MODEL_ES1370;
                } else if (STRPREFIX(start, "ac97")) {
                    type = VIR_DOMAIN_SOUND_MODEL_AC97;
                } else if (STRPREFIX(start, "hda")) {
                    type = VIR_DOMAIN_SOUND_MODEL_ICH6;
                }

                if (type != -1) {
                    virDomainSoundDefPtr snd;
                    if (VIR_ALLOC(snd) < 0)
                        goto error;
                    snd->model = type;
                    if (VIR_REALLOC_N(def->sounds, def->nsounds+1) < 0) {
                        VIR_FREE(snd);
                        goto error;
                    }
                    def->sounds[def->nsounds++] = snd;
                }

                start = tmp ? tmp + 1 : NULL;
            }
        } else if (STREQ(arg, "-watchdog")) {
            WANT_VALUE();
            int model = virDomainWatchdogModelTypeFromString(val);

            if (model != -1) {
                virDomainWatchdogDefPtr wd;
                if (VIR_ALLOC(wd) < 0)
                    goto error;
                wd->model = model;
                wd->action = VIR_DOMAIN_WATCHDOG_ACTION_RESET;
                def->watchdog = wd;
            }
        } else if (STREQ(arg, "-watchdog-action") && def->watchdog) {
            WANT_VALUE();
            int action = virDomainWatchdogActionTypeFromString(val);

            if (action != -1)
                def->watchdog->action = action;
        } else if (STREQ(arg, "-bootloader")) {
            WANT_VALUE();
            if (VIR_STRDUP(def->os.bootloader, val) < 0)
                goto error;
        } else if (STREQ(arg, "-vmwarevga")) {
            video = VIR_DOMAIN_VIDEO_TYPE_VMVGA;
        } else if (STREQ(arg, "-std-vga")) {
            video = VIR_DOMAIN_VIDEO_TYPE_VGA;
        } else if (STREQ(arg, "-vga")) {
            WANT_VALUE();
            if (STRNEQ(val, "none")) {
                video = qemuVideoTypeFromString(val);
                if (video < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("unknown video adapter type '%s'"), val);
                    goto error;
                }
            }
        } else if (STREQ(arg, "-cpu")) {
            WANT_VALUE();
            if (qemuParseCommandLineCPU(def, val) < 0)
                goto error;
        } else if (STREQ(arg, "-domid")) {
            WANT_VALUE();
            /* ignore, generted on the fly */
        } else if (STREQ(arg, "-usb")) {
            virDomainControllerDefPtr ctldef;
            if (VIR_ALLOC(ctldef) < 0)
                goto error;
            ctldef->type = VIR_DOMAIN_CONTROLLER_TYPE_USB;
            ctldef->idx = 0;
            ctldef->model = -1;
            virDomainControllerInsert(def, ctldef);
        } else if (STREQ(arg, "-pidfile")) {
            WANT_VALUE();
            if (pidfile)
                if (VIR_STRDUP(*pidfile, val) < 0)
                    goto error;
        } else if (STREQ(arg, "-incoming")) {
            WANT_VALUE();
            /* ignore, used via restore/migrate APIs */
        } else if (STREQ(arg, "-monitor")) {
            WANT_VALUE();
            if (monConfig) {
                virDomainChrSourceDefPtr chr;

                if (VIR_ALLOC(chr) < 0)
                    goto error;

                if (qemuParseCommandLineChr(chr, val) < 0) {
                    virDomainChrSourceDefFree(chr);
                    goto error;
                }

                *monConfig = chr;
            }
        } else if (STREQ(arg, "-global") &&
                   STRPREFIX(progargv[i + 1], "PIIX4_PM.disable_s3=")) {
            /* We want to parse only the known "-global" parameters,
             * so the ones that we don't know are still added to the
             * namespace */
            WANT_VALUE();

            val += strlen("PIIX4_PM.disable_s3=");
            if (STREQ(val, "0"))
                def->pm.s3 = VIR_DOMAIN_PM_STATE_ENABLED;
            else if (STREQ(val, "1"))
                def->pm.s3 = VIR_DOMAIN_PM_STATE_DISABLED;
            else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("invalid value for disable_s3 parameter: "
                                 "'%s'"), val);
                goto error;
            }

        } else if (STREQ(arg, "-global") &&
                   STRPREFIX(progargv[i + 1], "PIIX4_PM.disable_s4=")) {

            WANT_VALUE();

            val += strlen("PIIX4_PM.disable_s4=");
            if (STREQ(val, "0"))
                def->pm.s4 = VIR_DOMAIN_PM_STATE_ENABLED;
            else if (STREQ(val, "1"))
                def->pm.s4 = VIR_DOMAIN_PM_STATE_DISABLED;
            else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("invalid value for disable_s4 parameter: "
                                 "'%s'"), val);
                goto error;
            }

        } else if (STREQ(arg, "-global") &&
                   STRPREFIX(progargv[i + 1], "spapr-nvram.reg=")) {
            WANT_VALUE();

            if (VIR_ALLOC(def->nvram) < 0)
                goto error;

            def->nvram->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO;
            def->nvram->info.addr.spaprvio.has_reg = true;

            val += strlen("spapr-nvram.reg=");
            if (virStrToLong_ull(val, NULL, 16,
                                 &def->nvram->info.addr.spaprvio.reg) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot parse nvram's address '%s'"), val);
                goto error;
            }
        } else if (STREQ(arg, "-S")) {
            /* ignore, always added by libvirt */
        } else {
            /* something we can't yet parse.  Add it to the qemu namespace
             * cmdline/environment advanced options and hope for the best
             */
            VIR_WARN("unknown QEMU argument '%s', adding to the qemu namespace",
                     arg);
            if (VIR_REALLOC_N(cmd->args, cmd->num_args+1) < 0)
                goto error;
            if (VIR_STRDUP(cmd->args[cmd->num_args], arg) < 0)
                goto error;
            cmd->num_args++;
        }
    }

#undef WANT_VALUE
    if (def->ndisks > 0 && ceph_args) {
        char *hosts, *port, *saveptr = NULL, *token;
        virDomainDiskDefPtr first_rbd_disk = NULL;
        for (i = 0; i < def->ndisks; i++) {
            if (def->disks[i]->type == VIR_DOMAIN_DISK_TYPE_NETWORK &&
                def->disks[i]->protocol == VIR_DOMAIN_DISK_PROTOCOL_RBD) {
                first_rbd_disk = def->disks[i];
                break;
            }
        }

        if (!first_rbd_disk) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("CEPH_ARGS was set without an rbd disk"));
            goto error;
        }

        /* CEPH_ARGS should be: -m host1[:port1][,host2[:port2]]... */
        if (!STRPREFIX(ceph_args, "-m ")) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("could not parse CEPH_ARGS '%s'"), ceph_args);
            goto error;
        }
        if (VIR_STRDUP(hosts, strchr(ceph_args, ' ') + 1) < 0)
            goto error;
        first_rbd_disk->nhosts = 0;
        token = strtok_r(hosts, ",", &saveptr);
        while (token != NULL) {
            if (VIR_REALLOC_N(first_rbd_disk->hosts, first_rbd_disk->nhosts + 1) < 0) {
                VIR_FREE(hosts);
                goto error;
            }
            port = strchr(token, ':');
            if (port) {
                *port++ = '\0';
                if (VIR_STRDUP(port, port) < 0) {
                    VIR_FREE(hosts);
                    goto error;
                }
            }
            first_rbd_disk->hosts[first_rbd_disk->nhosts].port = port;
            if (VIR_STRDUP(first_rbd_disk->hosts[first_rbd_disk->nhosts].name,
                           token) < 0) {
                VIR_FREE(hosts);
                goto error;
            }
            first_rbd_disk->hosts[first_rbd_disk->nhosts].transport = VIR_DOMAIN_DISK_PROTO_TRANS_TCP;
            first_rbd_disk->hosts[first_rbd_disk->nhosts].socket = NULL;

            first_rbd_disk->nhosts++;
            token = strtok_r(NULL, ",", &saveptr);
        }
        VIR_FREE(hosts);

        if (first_rbd_disk->nhosts == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("found no rbd hosts in CEPH_ARGS '%s'"), ceph_args);
            goto error;
        }
    }

    if (!def->os.machine) {
        const char *defaultMachine =
                        virCapabilitiesDefaultGuestMachine(qemuCaps,
                                                           def->os.type,
                                                           def->os.arch,
                                                           virDomainVirtTypeToString(def->virtType));
        if (defaultMachine != NULL)
            if (VIR_STRDUP(def->os.machine, defaultMachine) < 0)
                goto error;
    }

    if (!nographics && def->ngraphics == 0) {
        virDomainGraphicsDefPtr sdl;
        const char *display = qemuFindEnv(progenv, "DISPLAY");
        const char *xauth = qemuFindEnv(progenv, "XAUTHORITY");
        if (VIR_ALLOC(sdl) < 0)
            goto error;
        sdl->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
        sdl->data.sdl.fullscreen = fullscreen;
        if (VIR_STRDUP(sdl->data.sdl.display, display) < 0) {
            VIR_FREE(sdl);
            goto error;
        }
        if (VIR_STRDUP(sdl->data.sdl.xauth, xauth) < 0) {
            VIR_FREE(sdl);
            goto error;
        }

        if (VIR_REALLOC_N(def->graphics, def->ngraphics+1) < 0) {
            virDomainGraphicsDefFree(sdl);
            goto error;
        }
        def->graphics[def->ngraphics++] = sdl;
    }

    if (def->ngraphics) {
        virDomainVideoDefPtr vid;
        if (VIR_ALLOC(vid) < 0)
            goto error;
        if (def->virtType == VIR_DOMAIN_VIRT_XEN)
            vid->type = VIR_DOMAIN_VIDEO_TYPE_XEN;
        else
            vid->type = video;
        vid->vram = virDomainVideoDefaultRAM(def, vid->type);
        vid->ram = vid->type == VIR_DOMAIN_VIDEO_TYPE_QXL ?
                       virDomainVideoDefaultRAM(def, vid->type) : 0;
        vid->heads = 1;

        if (VIR_REALLOC_N(def->videos, def->nvideos+1) < 0) {
            virDomainVideoDefFree(vid);
            goto error;
        }
        def->videos[def->nvideos++] = vid;
    }

    /*
     * having a balloon is the default, define one with type="none" to avoid it
     */
    if (!def->memballoon) {
        virDomainMemballoonDefPtr memballoon;
        if (VIR_ALLOC(memballoon) < 0)
            goto error;
        memballoon->model = VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO;

        def->memballoon = memballoon;
    }

    VIR_FREE(nics);

    if (virDomainDefAddImplicitControllers(def) < 0)
        goto error;

    if (virDomainDefPostParse(def, qemuCaps, xmlopt) < 0)
        goto error;

    if (cmd->num_args || cmd->num_env) {
        def->ns = *virDomainXMLOptionGetNamespace(xmlopt);
        def->namespaceData = cmd;
    }
    else
        VIR_FREE(cmd);

    return def;

error:
    virDomainDiskDefFree(disk);
    VIR_FREE(cmd);
    virDomainDefFree(def);
    VIR_FREE(nics);
    if (monConfig) {
        virDomainChrSourceDefFree(*monConfig);
        *monConfig = NULL;
    }
    if (pidfile)
        VIR_FREE(*pidfile);
    return NULL;
}


virDomainDefPtr qemuParseCommandLineString(virCapsPtr qemuCaps,
                                           virDomainXMLOptionPtr xmlopt,
                                           const char *args,
                                           char **pidfile,
                                           virDomainChrSourceDefPtr *monConfig,
                                           bool *monJSON)
{
    const char **progenv = NULL;
    const char **progargv = NULL;
    virDomainDefPtr def = NULL;
    size_t i;

    if (qemuStringToArgvEnv(args, &progenv, &progargv) < 0)
        goto cleanup;

    def = qemuParseCommandLine(qemuCaps, xmlopt, progenv, progargv,
                               pidfile, monConfig, monJSON);

cleanup:
    for (i = 0; progargv && progargv[i]; i++)
        VIR_FREE(progargv[i]);
    VIR_FREE(progargv);

    for (i = 0; progenv && progenv[i]; i++)
        VIR_FREE(progenv[i]);
    VIR_FREE(progenv);

    return def;
}


static int qemuParseProcFileStrings(int pid_value,
                                    const char *name,
                                    const char ***list)
{
    char *path = NULL;
    int ret = -1;
    char *data = NULL;
    ssize_t len;
    char *tmp;
    size_t nstr = 0;
    char **str = NULL;
    size_t i;

    if (virAsprintf(&path, "/proc/%d/%s", pid_value, name) < 0)
        goto cleanup;

    if ((len = virFileReadAll(path, 1024*128, &data)) < 0)
        goto cleanup;

    tmp = data;
    while (tmp < (data + len)) {
        if (VIR_EXPAND_N(str, nstr, 1) < 0)
            goto cleanup;

        if (VIR_STRDUP(str[nstr-1], tmp) < 0)
            goto cleanup;
        /* Skip arg */
        tmp += strlen(tmp);
        /* Skip \0 separator */
        tmp++;
    }

    if (VIR_EXPAND_N(str, nstr, 1) < 0)
        goto cleanup;

    str[nstr-1] = NULL;

    ret = nstr-1;
    *list = (const char **) str;

cleanup:
    if (ret < 0) {
        for (i = 0; str && str[i]; i++)
            VIR_FREE(str[i]);
        VIR_FREE(str);
    }
    VIR_FREE(data);
    VIR_FREE(path);
    return ret;
}

virDomainDefPtr qemuParseCommandLinePid(virCapsPtr qemuCaps,
                                        virDomainXMLOptionPtr xmlopt,
                                        pid_t pid,
                                        char **pidfile,
                                        virDomainChrSourceDefPtr *monConfig,
                                        bool *monJSON)
{
    virDomainDefPtr def = NULL;
    const char **progargv = NULL;
    const char **progenv = NULL;
    char *exepath = NULL;
    char *emulator;
    size_t i;

    /* The parser requires /proc/pid, which only exists on platforms
     * like Linux where pid_t fits in int.  */
    if ((int) pid != pid ||
        qemuParseProcFileStrings(pid, "cmdline", &progargv) < 0 ||
        qemuParseProcFileStrings(pid, "environ", &progenv) < 0)
        goto cleanup;

    if (!(def = qemuParseCommandLine(qemuCaps, xmlopt, progenv, progargv,
                                     pidfile, monConfig, monJSON)))
        goto cleanup;

    if (virAsprintf(&exepath, "/proc/%d/exe", (int) pid) < 0)
        goto cleanup;

    if (virFileResolveLink(exepath, &emulator) < 0) {
        virReportSystemError(errno,
                             _("Unable to resolve %s for pid %u"),
                             exepath, (int) pid);
        goto cleanup;
    }
    VIR_FREE(def->emulator);
    def->emulator = emulator;

cleanup:
    VIR_FREE(exepath);
    for (i = 0; progargv && progargv[i]; i++)
        VIR_FREE(progargv[i]);
    VIR_FREE(progargv);
    for (i = 0; progenv && progenv[i]; i++)
        VIR_FREE(progenv[i]);
    VIR_FREE(progenv);
    return def;
}
