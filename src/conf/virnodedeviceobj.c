/*
 * virnodedeviceobj.c: node device object handling
 *                     (derived from node_device_conf.c)
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

#include <config.h>

#include "datatypes.h"
#include "node_device_conf.h"

#include "viralloc.h"
#include "virnodedeviceobj.h"
#include "virerror.h"
#include "virlog.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NODEDEV

VIR_LOG_INIT("conf.virnodedeviceobj");


virNodeDeviceDefPtr
virNodeDeviceObjGetDef(virNodeDeviceObjPtr obj)
{
    return obj->def;
}


static int
virNodeDeviceObjHasCap(const virNodeDeviceObj *dev,
                       const char *cap)
{
    virNodeDevCapsDefPtr caps = dev->def->caps;
    const char *fc_host_cap =
        virNodeDevCapTypeToString(VIR_NODE_DEV_CAP_FC_HOST);
    const char *vports_cap =
        virNodeDevCapTypeToString(VIR_NODE_DEV_CAP_VPORTS);
    const char *mdev_types =
        virNodeDevCapTypeToString(VIR_NODE_DEV_CAP_MDEV_TYPES);

    while (caps) {
        if (STREQ(cap, virNodeDevCapTypeToString(caps->data.type))) {
            return 1;
        } else {
            switch (caps->data.type) {
            case VIR_NODE_DEV_CAP_PCI_DEV:
                if ((STREQ(cap, mdev_types)) &&
                    (caps->data.pci_dev.flags & VIR_NODE_DEV_CAP_FLAG_PCI_MDEV))
                    return 1;
                break;

            case VIR_NODE_DEV_CAP_SCSI_HOST:
                if ((STREQ(cap, fc_host_cap) &&
                    (caps->data.scsi_host.flags & VIR_NODE_DEV_CAP_FLAG_HBA_FC_HOST)) ||
                    (STREQ(cap, vports_cap) &&
                    (caps->data.scsi_host.flags & VIR_NODE_DEV_CAP_FLAG_HBA_VPORT_OPS)))
                    return 1;
                break;

            case VIR_NODE_DEV_CAP_SYSTEM:
            case VIR_NODE_DEV_CAP_USB_DEV:
            case VIR_NODE_DEV_CAP_USB_INTERFACE:
            case VIR_NODE_DEV_CAP_NET:
            case VIR_NODE_DEV_CAP_SCSI_TARGET:
            case VIR_NODE_DEV_CAP_SCSI:
            case VIR_NODE_DEV_CAP_STORAGE:
            case VIR_NODE_DEV_CAP_FC_HOST:
            case VIR_NODE_DEV_CAP_VPORTS:
            case VIR_NODE_DEV_CAP_SCSI_GENERIC:
            case VIR_NODE_DEV_CAP_DRM:
            case VIR_NODE_DEV_CAP_MDEV_TYPES:
            case VIR_NODE_DEV_CAP_MDEV:
            case VIR_NODE_DEV_CAP_CCW_DEV:
            case VIR_NODE_DEV_CAP_LAST:
                break;
            }
        }

        caps = caps->next;
    }
    return 0;
}


/* virNodeDeviceFindFCCapDef:
 * @dev: Pointer to current device
 *
 * Search the device object 'caps' array for fc_host capability.
 *
 * Returns:
 * Pointer to the caps or NULL if not found
 */
static virNodeDevCapsDefPtr
virNodeDeviceFindFCCapDef(const virNodeDeviceObj *dev)
{
    virNodeDevCapsDefPtr caps = dev->def->caps;

    while (caps) {
        if (caps->data.type == VIR_NODE_DEV_CAP_SCSI_HOST &&
            (caps->data.scsi_host.flags & VIR_NODE_DEV_CAP_FLAG_HBA_FC_HOST))
            break;

        caps = caps->next;
    }
    return caps;
}


/* virNodeDeviceFindVPORTCapDef:
 * @dev: Pointer to current device
 *
 * Search the device object 'caps' array for vport_ops capability.
 *
 * Returns:
 * Pointer to the caps or NULL if not found
 */
static virNodeDevCapsDefPtr
virNodeDeviceFindVPORTCapDef(const virNodeDeviceObj *dev)
{
    virNodeDevCapsDefPtr caps = dev->def->caps;

    while (caps) {
        if (caps->data.type == VIR_NODE_DEV_CAP_SCSI_HOST &&
            (caps->data.scsi_host.flags & VIR_NODE_DEV_CAP_FLAG_HBA_VPORT_OPS))
            break;

        caps = caps->next;
    }
    return caps;
}


virNodeDeviceObjPtr
virNodeDeviceObjFindBySysfsPath(virNodeDeviceObjListPtr devs,
                                const char *sysfs_path)
{
    size_t i;

    for (i = 0; i < devs->count; i++) {
        virNodeDeviceObjLock(devs->objs[i]);
        if ((devs->objs[i]->def->sysfs_path != NULL) &&
            (STREQ(devs->objs[i]->def->sysfs_path, sysfs_path))) {
            return devs->objs[i];
        }
        virNodeDeviceObjUnlock(devs->objs[i]);
    }

    return NULL;
}


virNodeDeviceObjPtr
virNodeDeviceObjFindByName(virNodeDeviceObjListPtr devs,
                           const char *name)
{
    size_t i;

    for (i = 0; i < devs->count; i++) {
        virNodeDeviceObjLock(devs->objs[i]);
        if (STREQ(devs->objs[i]->def->name, name))
            return devs->objs[i];
        virNodeDeviceObjUnlock(devs->objs[i]);
    }

    return NULL;
}


static virNodeDeviceObjPtr
virNodeDeviceFindByWWNs(virNodeDeviceObjListPtr devs,
                        const char *parent_wwnn,
                        const char *parent_wwpn)
{
    size_t i;

    for (i = 0; i < devs->count; i++) {
        virNodeDevCapsDefPtr cap;
        virNodeDeviceObjLock(devs->objs[i]);
        if ((cap = virNodeDeviceFindFCCapDef(devs->objs[i])) &&
            STREQ_NULLABLE(cap->data.scsi_host.wwnn, parent_wwnn) &&
            STREQ_NULLABLE(cap->data.scsi_host.wwpn, parent_wwpn) &&
            virNodeDeviceFindVPORTCapDef(devs->objs[i]))
            return devs->objs[i];
        virNodeDeviceObjUnlock(devs->objs[i]);
    }

    return NULL;
}


static virNodeDeviceObjPtr
virNodeDeviceFindByFabricWWN(virNodeDeviceObjListPtr devs,
                             const char *parent_fabric_wwn)
{
    size_t i;

    for (i = 0; i < devs->count; i++) {
        virNodeDevCapsDefPtr cap;
        virNodeDeviceObjLock(devs->objs[i]);
        if ((cap = virNodeDeviceFindFCCapDef(devs->objs[i])) &&
            STREQ_NULLABLE(cap->data.scsi_host.fabric_wwn, parent_fabric_wwn) &&
            virNodeDeviceFindVPORTCapDef(devs->objs[i]))
            return devs->objs[i];
        virNodeDeviceObjUnlock(devs->objs[i]);
    }

    return NULL;
}


static virNodeDeviceObjPtr
virNodeDeviceFindByCap(virNodeDeviceObjListPtr devs,
                       const char *cap)
{
    size_t i;

    for (i = 0; i < devs->count; i++) {
        virNodeDeviceObjLock(devs->objs[i]);
        if (virNodeDeviceObjHasCap(devs->objs[i], cap))
            return devs->objs[i];
        virNodeDeviceObjUnlock(devs->objs[i]);
    }

    return NULL;
}


void
virNodeDeviceObjFree(virNodeDeviceObjPtr dev)
{
    if (!dev)
        return;

    virNodeDeviceDefFree(dev->def);

    virMutexDestroy(&dev->lock);

    VIR_FREE(dev);
}


void
virNodeDeviceObjListFree(virNodeDeviceObjListPtr devs)
{
    size_t i;
    for (i = 0; i < devs->count; i++)
        virNodeDeviceObjFree(devs->objs[i]);
    VIR_FREE(devs->objs);
    devs->count = 0;
}


virNodeDeviceObjPtr
virNodeDeviceObjAssignDef(virNodeDeviceObjListPtr devs,
                          virNodeDeviceDefPtr def)
{
    virNodeDeviceObjPtr device;

    if ((device = virNodeDeviceObjFindByName(devs, def->name))) {
        virNodeDeviceDefFree(device->def);
        device->def = def;
        return device;
    }

    if (VIR_ALLOC(device) < 0)
        return NULL;

    if (virMutexInit(&device->lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot initialize mutex"));
        VIR_FREE(device);
        return NULL;
    }
    virNodeDeviceObjLock(device);

    if (VIR_APPEND_ELEMENT_COPY(devs->objs, devs->count, device) < 0) {
        virNodeDeviceObjUnlock(device);
        virNodeDeviceObjFree(device);
        return NULL;
    }
    device->def = def;

    return device;

}


void
virNodeDeviceObjRemove(virNodeDeviceObjListPtr devs,
                       virNodeDeviceObjPtr *dev)
{
    size_t i;

    virNodeDeviceObjUnlock(*dev);

    for (i = 0; i < devs->count; i++) {
        virNodeDeviceObjLock(*dev);
        if (devs->objs[i] == *dev) {
            virNodeDeviceObjUnlock(*dev);
            virNodeDeviceObjFree(devs->objs[i]);
            *dev = NULL;

            VIR_DELETE_ELEMENT(devs->objs, i, devs->count);
            break;
        }
        virNodeDeviceObjUnlock(*dev);
    }
}


/*
 * Return the NPIV dev's parent device name
 */
/* virNodeDeviceFindFCParentHost:
 * @parent: Pointer to node device object
 *
 * Search the capabilities for the device to find the FC capabilities
 * in order to set the parent_host value.
 *
 * Returns:
 *   parent_host value on success (>= 0), -1 otherwise.
 */
static int
virNodeDeviceFindFCParentHost(virNodeDeviceObjPtr parent)
{
    virNodeDevCapsDefPtr cap = virNodeDeviceFindVPORTCapDef(parent);

    if (!cap) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Parent device %s is not capable "
                         "of vport operations"),
                       parent->def->name);
        return -1;
    }

    return cap->data.scsi_host.host;
}


static int
virNodeDeviceGetParentHostByParent(virNodeDeviceObjListPtr devs,
                                   const char *dev_name,
                                   const char *parent_name)
{
    virNodeDeviceObjPtr parent = NULL;
    int ret;

    if (!(parent = virNodeDeviceObjFindByName(devs, parent_name))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not find parent device for '%s'"),
                       dev_name);
        return -1;
    }

    ret = virNodeDeviceFindFCParentHost(parent);

    virNodeDeviceObjUnlock(parent);

    return ret;
}


static int
virNodeDeviceGetParentHostByWWNs(virNodeDeviceObjListPtr devs,
                                 const char *dev_name,
                                 const char *parent_wwnn,
                                 const char *parent_wwpn)
{
    virNodeDeviceObjPtr parent = NULL;
    int ret;

    if (!(parent = virNodeDeviceFindByWWNs(devs, parent_wwnn, parent_wwpn))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not find parent device for '%s'"),
                       dev_name);
        return -1;
    }

    ret = virNodeDeviceFindFCParentHost(parent);

    virNodeDeviceObjUnlock(parent);

    return ret;
}


static int
virNodeDeviceGetParentHostByFabricWWN(virNodeDeviceObjListPtr devs,
                                      const char *dev_name,
                                      const char *parent_fabric_wwn)
{
    virNodeDeviceObjPtr parent = NULL;
    int ret;

    if (!(parent = virNodeDeviceFindByFabricWWN(devs, parent_fabric_wwn))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not find parent device for '%s'"),
                       dev_name);
        return -1;
    }

    ret = virNodeDeviceFindFCParentHost(parent);

    virNodeDeviceObjUnlock(parent);

    return ret;
}


static int
virNodeDeviceFindVportParentHost(virNodeDeviceObjListPtr devs)
{
    virNodeDeviceObjPtr parent = NULL;
    const char *cap = virNodeDevCapTypeToString(VIR_NODE_DEV_CAP_VPORTS);
    int ret;

    if (!(parent = virNodeDeviceFindByCap(devs, cap))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not find any vport capable device"));
        return -1;
    }

    ret = virNodeDeviceFindFCParentHost(parent);

    virNodeDeviceObjUnlock(parent);

    return ret;
}


int
virNodeDeviceObjGetParentHost(virNodeDeviceObjListPtr devs,
                              virNodeDeviceDefPtr def,
                              int create)
{
    int parent_host = -1;

    if (def->parent) {
        parent_host = virNodeDeviceGetParentHostByParent(devs, def->name,
                                                         def->parent);
    } else if (def->parent_wwnn && def->parent_wwpn) {
        parent_host = virNodeDeviceGetParentHostByWWNs(devs, def->name,
                                                       def->parent_wwnn,
                                                       def->parent_wwpn);
    } else if (def->parent_fabric_wwn) {
        parent_host =
            virNodeDeviceGetParentHostByFabricWWN(devs, def->name,
                                                  def->parent_fabric_wwn);
    } else if (create == CREATE_DEVICE) {
        /* Try to find a vport capable scsi_host when no parent supplied */
        parent_host = virNodeDeviceFindVportParentHost(devs);
    }

    return parent_host;
}


void
virNodeDeviceObjLock(virNodeDeviceObjPtr obj)
{
    virMutexLock(&obj->lock);
}


void
virNodeDeviceObjUnlock(virNodeDeviceObjPtr obj)
{
    virMutexUnlock(&obj->lock);
}


static bool
virNodeDeviceCapMatch(virNodeDeviceObjPtr devobj,
                      int type)
{
    virNodeDevCapsDefPtr cap = NULL;

    for (cap = devobj->def->caps; cap; cap = cap->next) {
        if (type == cap->data.type)
            return true;

        switch (cap->data.type) {
        case VIR_NODE_DEV_CAP_PCI_DEV:
            if (type == VIR_NODE_DEV_CAP_MDEV_TYPES &&
                (cap->data.pci_dev.flags & VIR_NODE_DEV_CAP_FLAG_PCI_MDEV))
                return true;
            break;

        case VIR_NODE_DEV_CAP_SCSI_HOST:
            if (type == VIR_NODE_DEV_CAP_FC_HOST &&
                (cap->data.scsi_host.flags & VIR_NODE_DEV_CAP_FLAG_HBA_FC_HOST))
                return true;

            if (type == VIR_NODE_DEV_CAP_VPORTS &&
                (cap->data.scsi_host.flags & VIR_NODE_DEV_CAP_FLAG_HBA_VPORT_OPS))
                return true;
            break;

        case VIR_NODE_DEV_CAP_SYSTEM:
        case VIR_NODE_DEV_CAP_USB_DEV:
        case VIR_NODE_DEV_CAP_USB_INTERFACE:
        case VIR_NODE_DEV_CAP_NET:
        case VIR_NODE_DEV_CAP_SCSI_TARGET:
        case VIR_NODE_DEV_CAP_SCSI:
        case VIR_NODE_DEV_CAP_STORAGE:
        case VIR_NODE_DEV_CAP_FC_HOST:
        case VIR_NODE_DEV_CAP_VPORTS:
        case VIR_NODE_DEV_CAP_SCSI_GENERIC:
        case VIR_NODE_DEV_CAP_DRM:
        case VIR_NODE_DEV_CAP_MDEV_TYPES:
        case VIR_NODE_DEV_CAP_MDEV:
        case VIR_NODE_DEV_CAP_CCW_DEV:
        case VIR_NODE_DEV_CAP_LAST:
            break;
        }
    }

    return false;
}


int
virNodeDeviceObjNumOfDevices(virNodeDeviceObjListPtr devs,
                             virConnectPtr conn,
                             const char *cap,
                             virNodeDeviceObjListFilter aclfilter)
{
    size_t i;
    int ndevs = 0;

    for (i = 0; i < devs->count; i++) {
        virNodeDeviceObjPtr obj = devs->objs[i];
        virNodeDeviceObjLock(obj);
        if ((!aclfilter || aclfilter(conn, obj->def)) &&
            (!cap || virNodeDeviceObjHasCap(obj, cap)))
            ++ndevs;
        virNodeDeviceObjUnlock(obj);
    }

    return ndevs;
}


int
virNodeDeviceObjGetNames(virNodeDeviceObjListPtr devs,
                         virConnectPtr conn,
                         virNodeDeviceObjListFilter aclfilter,
                         const char *cap,
                         char **const names,
                         int maxnames)
{
    int nnames = 0;
    size_t i;

    for (i = 0; i < devs->count && nnames < maxnames; i++) {
        virNodeDeviceObjPtr obj = devs->objs[i];
        virNodeDeviceObjLock(obj);
        if ((!aclfilter || aclfilter(conn, obj->def)) &&
            (!cap || virNodeDeviceObjHasCap(obj, cap))) {
            if (VIR_STRDUP(names[nnames], obj->def->name) < 0) {
                virNodeDeviceObjUnlock(obj);
                goto failure;
            }
            nnames++;
        }
        virNodeDeviceObjUnlock(obj);
    }

    return nnames;

 failure:
    while (--nnames >= 0)
        VIR_FREE(names[nnames]);
    return -1;
}


#define MATCH(FLAG) ((flags & (VIR_CONNECT_LIST_NODE_DEVICES_CAP_ ## FLAG)) && \
                     virNodeDeviceCapMatch(devobj, VIR_NODE_DEV_CAP_ ## FLAG))
static bool
virNodeDeviceMatch(virNodeDeviceObjPtr devobj,
                   unsigned int flags)
{
    /* filter by cap type */
    if (flags & VIR_CONNECT_LIST_NODE_DEVICES_FILTERS_CAP) {
        if (!(MATCH(SYSTEM)        ||
              MATCH(PCI_DEV)       ||
              MATCH(USB_DEV)       ||
              MATCH(USB_INTERFACE) ||
              MATCH(NET)           ||
              MATCH(SCSI_HOST)     ||
              MATCH(SCSI_TARGET)   ||
              MATCH(SCSI)          ||
              MATCH(STORAGE)       ||
              MATCH(FC_HOST)       ||
              MATCH(VPORTS)        ||
              MATCH(SCSI_GENERIC)  ||
              MATCH(DRM)           ||
              MATCH(MDEV_TYPES)    ||
              MATCH(MDEV)          ||
              MATCH(CCW_DEV)))
            return false;
    }

    return true;
}
#undef MATCH


int
virNodeDeviceObjListExport(virConnectPtr conn,
                           virNodeDeviceObjListPtr devobjs,
                           virNodeDevicePtr **devices,
                           virNodeDeviceObjListFilter filter,
                           unsigned int flags)
{
    virNodeDevicePtr *tmp_devices = NULL;
    virNodeDevicePtr device = NULL;
    int ndevices = 0;
    int ret = -1;
    size_t i;

    if (devices && VIR_ALLOC_N(tmp_devices, devobjs->count + 1) < 0)
        goto cleanup;

    for (i = 0; i < devobjs->count; i++) {
        virNodeDeviceObjPtr devobj = devobjs->objs[i];
        virNodeDeviceObjLock(devobj);
        if ((!filter || filter(conn, devobj->def)) &&
            virNodeDeviceMatch(devobj, flags)) {
            if (devices) {
                if (!(device = virGetNodeDevice(conn, devobj->def->name)) ||
                    VIR_STRDUP(device->parent, devobj->def->parent) < 0) {
                    virObjectUnref(device);
                    virNodeDeviceObjUnlock(devobj);
                    goto cleanup;
                }
                tmp_devices[ndevices] = device;
            }
            ndevices++;
        }
        virNodeDeviceObjUnlock(devobj);
    }

    if (tmp_devices) {
        /* trim the array to the final size */
        ignore_value(VIR_REALLOC_N(tmp_devices, ndevices + 1));
        *devices = tmp_devices;
        tmp_devices = NULL;
    }

    ret = ndevices;

 cleanup:
    if (tmp_devices) {
        for (i = 0; i < ndevices; i++)
            virObjectUnref(tmp_devices[i]);
    }

    VIR_FREE(tmp_devices);
    return ret;
}
