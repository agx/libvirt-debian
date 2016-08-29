/*
 * domain_addr.c: helper APIs for managing domain device addresses
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
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

#include "viralloc.h"
#include "virlog.h"
#include "virstring.h"
#include "domain_addr.h"

#define VIR_FROM_THIS VIR_FROM_DOMAIN

VIR_LOG_INIT("conf.domain_addr");

virDomainPCIConnectFlags
virDomainPCIControllerModelToConnectType(virDomainControllerModelPCI model)
{
    /* given a VIR_DOMAIN_CONTROLLER_MODEL_PCI*, set connectType to
     * the equivalent VIR_PCI_CONNECT_TYPE_*. return 0 on success, -1
     * if the model wasn't recognized.
     */
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        /* pci-root and pcie-root are implicit in the machine,
         * and have no upstream connection, "last" will never actually
         * happen, it's just there so that all possible cases are
         * covered in the switch (keeps the compiler happy).
         */
        return 0;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
        /* pci-bridge is treated like a standard
         * PCI endpoint device, because it can plug into any
         * standard PCI slot (it just can't be hotplugged).
         */
        return VIR_PCI_CONNECT_TYPE_PCI_DEVICE;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
        return VIR_PCI_CONNECT_TYPE_PCI_EXPANDER_BUS;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
        return VIR_PCI_CONNECT_TYPE_PCIE_EXPANDER_BUS;

    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
        return VIR_PCI_CONNECT_TYPE_DMI_TO_PCI_BRIDGE;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
        return VIR_PCI_CONNECT_TYPE_PCIE_ROOT_PORT;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
        return VIR_PCI_CONNECT_TYPE_PCIE_SWITCH_UPSTREAM_PORT;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
        return VIR_PCI_CONNECT_TYPE_PCIE_SWITCH_DOWNSTREAM_PORT;

        /* if this happens, there is an error in the code. A
         * PCI controller should always have a proper model
         * set
         */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("PCI controller model incorrectly set to 'last'"));
        return -1;
    }
    return 0;
}

bool
virDomainPCIAddressFlagsCompatible(virPCIDeviceAddressPtr addr,
                                   const char *addrStr,
                                   virDomainPCIConnectFlags busFlags,
                                   virDomainPCIConnectFlags devFlags,
                                   bool reportError,
                                   bool fromConfig)
{
    virErrorNumber errType = (fromConfig
                              ? VIR_ERR_XML_ERROR : VIR_ERR_INTERNAL_ERROR);

    if (fromConfig) {
        /* If the requested connection was manually specified in
         * config, allow a PCI device to connect to a PCIe slot, or
         * vice versa.
         */
        if (busFlags & VIR_PCI_CONNECT_TYPES_ENDPOINT)
            busFlags |= VIR_PCI_CONNECT_TYPES_ENDPOINT;
        /* Also allow manual specification of bus to override
         * libvirt's assumptions about whether or not hotplug
         * capability will be required.
         */
        if (devFlags & VIR_PCI_CONNECT_HOTPLUGGABLE)
            busFlags |= VIR_PCI_CONNECT_HOTPLUGGABLE;
    }

    /* If this bus doesn't allow the type of connection (PCI
     * vs. PCIe) required by the device, or if the device requires
     * hot-plug and this bus doesn't have it, return false.
     */
    if (!(devFlags & busFlags & VIR_PCI_CONNECT_TYPES_MASK)) {
        const char *connectStr;

        if (!reportError)
            return false;

        if (devFlags & VIR_PCI_CONNECT_TYPE_PCI_DEVICE) {
            connectStr = "standard PCI device";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCIE_DEVICE) {
            connectStr = "PCI Express device";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCIE_ROOT_PORT) {
            connectStr = "pcie-root-port";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCIE_SWITCH_UPSTREAM_PORT) {
            connectStr = "pci-switch-upstream-port";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCIE_SWITCH_DOWNSTREAM_PORT) {
            connectStr = "pci-switch-downstream-port";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_DMI_TO_PCI_BRIDGE) {
            connectStr = "dmi-to-pci-bridge";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCI_EXPANDER_BUS) {
            connectStr = "pci-expander-bus";
        } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCIE_EXPANDER_BUS) {
            connectStr = "pcie-expander-bus";
        } else {
            /* this should never happen. If it does, there is a
             * bug in the code that sets the flag bits for devices.
             */
            virReportError(errType,
                           _("The device at PCI address %s has "
                             "unrecognized connection type flags 0x%.2x"),
                           addrStr, devFlags & VIR_PCI_CONNECT_TYPES_MASK);
            return false;
        }
        virReportError(errType,
                       _("The device at PCI address %s cannot be "
                         "plugged into the PCI controller with index='%d'. "
                         "It requires a controller that accepts a %s."),
                       addrStr, addr->bus, connectStr);
        return false;
    }
    if ((devFlags & VIR_PCI_CONNECT_HOTPLUGGABLE) &&
        !(busFlags & VIR_PCI_CONNECT_HOTPLUGGABLE)) {
        if (reportError) {
            virReportError(errType,
                           _("The device at PCI address %s requires "
                             "hotplug capability, but the PCI controller "
                             "with index='%d' doesn't support hotplug"),
                           addrStr, addr->bus);
        }
        return false;
    }
    return true;
}


/* Verify that the address is in bounds for the chosen bus, and
 * that the bus is of the correct type for the device (via
 * comparing the flags).
 */
bool
virDomainPCIAddressValidate(virDomainPCIAddressSetPtr addrs,
                            virPCIDeviceAddressPtr addr,
                            const char *addrStr,
                            virDomainPCIConnectFlags flags,
                            bool fromConfig)
{
    virDomainPCIAddressBusPtr bus;
    virErrorNumber errType = (fromConfig
                              ? VIR_ERR_XML_ERROR : VIR_ERR_INTERNAL_ERROR);

    if (addrs->nbuses == 0) {
        virReportError(errType, "%s", _("No PCI buses available"));
        return false;
    }
    if (addr->domain != 0) {
        virReportError(errType,
                       _("Invalid PCI address %s. "
                         "Only PCI domain 0 is available"),
                       addrStr);
        return false;
    }
    if (addr->bus >= addrs->nbuses) {
        virReportError(errType,
                       _("Invalid PCI address %s. "
                         "Only PCI buses up to %zu are available"),
                       addrStr, addrs->nbuses - 1);
        return false;
    }

    bus = &addrs->buses[addr->bus];

    /* assure that at least one of the requested connection types is
     * provided by this bus
     */
    if (!virDomainPCIAddressFlagsCompatible(addr, addrStr, bus->flags,
                                            flags, true, fromConfig))
        return false;

    /* some "buses" are really just a single port */
    if (bus->minSlot && addr->slot < bus->minSlot) {
        virReportError(errType,
                       _("Invalid PCI address %s. slot must be >= %zu"),
                       addrStr, bus->minSlot);
        return false;
    }
    if (addr->slot > bus->maxSlot) {
        virReportError(errType,
                       _("Invalid PCI address %s. slot must be <= %zu"),
                       addrStr, bus->maxSlot);
        return false;
    }
    if (addr->function > VIR_PCI_ADDRESS_FUNCTION_LAST) {
        virReportError(errType,
                       _("Invalid PCI address %s. function must be <= %u"),
                       addrStr, VIR_PCI_ADDRESS_FUNCTION_LAST);
        return false;
    }
    return true;
}


int
virDomainPCIAddressBusSetModel(virDomainPCIAddressBusPtr bus,
                               virDomainControllerModelPCI model)
{
    /* set flags for what can be connected *downstream* from each
     * bus.
     */
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        bus->flags = (VIR_PCI_CONNECT_HOTPLUGGABLE |
                      VIR_PCI_CONNECT_TYPE_PCI_DEVICE |
                      VIR_PCI_CONNECT_TYPE_PCI_EXPANDER_BUS);
        bus->minSlot = 1;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
        bus->flags = (VIR_PCI_CONNECT_HOTPLUGGABLE |
                      VIR_PCI_CONNECT_TYPE_PCI_DEVICE);
        bus->minSlot = 1;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
        bus->flags = (VIR_PCI_CONNECT_HOTPLUGGABLE |
                      VIR_PCI_CONNECT_TYPE_PCI_DEVICE);
        bus->minSlot = 0;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        /* slots 1 - 31, no hotplug, PCIe endpoint device or
         * pcie-root-port only, unless the address was specified in
         * user config *and* the particular device being attached also
         * allows it.
         */
        bus->flags = (VIR_PCI_CONNECT_TYPE_PCIE_DEVICE |
                      VIR_PCI_CONNECT_TYPE_PCIE_ROOT_PORT |
                      VIR_PCI_CONNECT_TYPE_DMI_TO_PCI_BRIDGE |
                      VIR_PCI_CONNECT_TYPE_PCIE_EXPANDER_BUS);
        bus->minSlot = 1;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
        /* slots 0 - 31, standard PCI slots,
         * but *not* hot-pluggable */
        bus->flags = VIR_PCI_CONNECT_TYPE_PCI_DEVICE;
        bus->minSlot = 0;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
        /* provides one slot which is pcie, can be used by endpoint
         * devices and pcie-switch-upstream-ports, and is hotpluggable
         */
        bus->flags = VIR_PCI_CONNECT_TYPE_PCIE_DEVICE
           | VIR_PCI_CONNECT_TYPE_PCIE_SWITCH_UPSTREAM_PORT
           | VIR_PCI_CONNECT_HOTPLUGGABLE;
        bus->minSlot = 0;
        bus->maxSlot = 0;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
        /* 32 slots, can only accept pcie-switch-downstrean-ports,
         * no hotplug
         */
        bus->flags = VIR_PCI_CONNECT_TYPE_PCIE_SWITCH_DOWNSTREAM_PORT;
        bus->minSlot = 0;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
        /* single slot, no hotplug, only accepts pcie-root-port or
         * dmi-to-pci-bridge
         */
        bus->flags = (VIR_PCI_CONNECT_TYPE_PCIE_ROOT_PORT |
                      VIR_PCI_CONNECT_TYPE_DMI_TO_PCI_BRIDGE);
        bus->minSlot = 0;
        bus->maxSlot = 0;
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
int
virDomainPCIAddressSetGrow(virDomainPCIAddressSetPtr addrs,
                           virPCIDeviceAddressPtr addr,
                           virDomainPCIConnectFlags flags)
{
    int add;
    size_t i;

    add = addr->bus - addrs->nbuses + 1;
    i = addrs->nbuses;
    if (add <= 0)
        return 0;

    /* auto-grow only works when we're adding plain PCI devices */
    if (!(flags & VIR_PCI_CONNECT_TYPE_PCI_DEVICE)) {
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
        virDomainPCIAddressBusSetModel(&addrs->buses[i],
                                       VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE);
    }
    return add;
}


char *
virDomainPCIAddressAsString(virPCIDeviceAddressPtr addr)
{
    char *str;

    ignore_value(virAsprintf(&str, "%.4x:%.2x:%.2x.%.1x",
                             addr->domain,
                             addr->bus,
                             addr->slot,
                             addr->function));
    return str;
}


/*
 * Check if the PCI slot is used by another device.
 */
bool
virDomainPCIAddressSlotInUse(virDomainPCIAddressSetPtr addrs,
                             virPCIDeviceAddressPtr addr)
{
    return !!addrs->buses[addr->bus].slots[addr->slot];
}


/*
 * Reserve a slot (or just one function) for a device. If
 * reserveEntireSlot is true, all functions for the slot are reserved,
 * otherwise only one. If fromConfig is true, the address being
 * requested came directly from the config and errors should be worded
 * appropriately. If fromConfig is false, the address was
 * automatically created by libvirt, so it is an internal error (not
 * XML).
 */
int
virDomainPCIAddressReserveAddr(virDomainPCIAddressSetPtr addrs,
                               virPCIDeviceAddressPtr addr,
                               virDomainPCIConnectFlags flags,
                               bool reserveEntireSlot,
                               bool fromConfig)
{
    int ret = -1;
    char *addrStr = NULL;
    virDomainPCIAddressBusPtr bus;
    virErrorNumber errType = (fromConfig
                              ? VIR_ERR_XML_ERROR : VIR_ERR_INTERNAL_ERROR);

    if (!(addrStr = virDomainPCIAddressAsString(addr)))
        goto cleanup;

    /* Add an extra bus if necessary */
    if (addrs->dryRun && virDomainPCIAddressSetGrow(addrs, addr, flags) < 0)
        goto cleanup;
    /* Check that the requested bus exists, is the correct type, and we
     * are asking for a valid slot
     */
    if (!virDomainPCIAddressValidate(addrs, addr, addrStr, flags, fromConfig))
        goto cleanup;

    bus = &addrs->buses[addr->bus];

    if (reserveEntireSlot) {
        if (bus->slots[addr->slot]) {
            virReportError(errType,
                           _("Attempted double use of PCI slot %s "
                             "(may need \"multifunction='on'\" for "
                             "device on function 0)"), addrStr);
            goto cleanup;
        }
        bus->slots[addr->slot] = 0xFF; /* reserve all functions of slot */
        VIR_DEBUG("Reserving PCI slot %s (multifunction='off')", addrStr);
    } else {
        if (bus->slots[addr->slot] & (1 << addr->function)) {
            if (addr->function == 0) {
                virReportError(errType,
                               _("Attempted double use of PCI Address %s"),
                               addrStr);
            } else {
                virReportError(errType,
                               _("Attempted double use of PCI Address %s "
                                 "(may need \"multifunction='on'\" "
                                 "for device on function 0)"), addrStr);
            }
            goto cleanup;
        }
        bus->slots[addr->slot] |= (1 << addr->function);
        VIR_DEBUG("Reserving PCI address %s", addrStr);
    }

    ret = 0;
 cleanup:
    VIR_FREE(addrStr);
    return ret;
}


int
virDomainPCIAddressReserveSlot(virDomainPCIAddressSetPtr addrs,
                               virPCIDeviceAddressPtr addr,
                               virDomainPCIConnectFlags flags)
{
    return virDomainPCIAddressReserveAddr(addrs, addr, flags, true, false);
}

int
virDomainPCIAddressEnsureAddr(virDomainPCIAddressSetPtr addrs,
                              virDomainDeviceInfoPtr dev)
{
    int ret = -1;
    char *addrStr = NULL;
    /* Flags should be set according to the particular device,
     * but only the caller knows the type of device. Currently this
     * function is only used for hot-plug, though, and hot-plug is
     * only supported for standard PCI devices, so we can safely use
     * the setting below */
    virDomainPCIConnectFlags flags = (VIR_PCI_CONNECT_HOTPLUGGABLE |
                                      VIR_PCI_CONNECT_TYPE_PCI_DEVICE);

    if (!(addrStr = virDomainPCIAddressAsString(&dev->addr.pci)))
        goto cleanup;

    if (virDeviceInfoPCIAddressPresent(dev)) {
        /* We do not support hotplug multi-function PCI device now, so we should
         * reserve the whole slot. The function of the PCI device must be 0.
         */
        if (dev->addr.pci.function != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only PCI device addresses with function=0"
                             " are supported"));
            goto cleanup;
        }

        if (!virDomainPCIAddressValidate(addrs, &dev->addr.pci,
                                         addrStr, flags, true))
            goto cleanup;

        ret = virDomainPCIAddressReserveSlot(addrs, &dev->addr.pci, flags);
    } else {
        ret = virDomainPCIAddressReserveNextSlot(addrs, dev, flags);
    }

 cleanup:
    VIR_FREE(addrStr);
    return ret;
}


int
virDomainPCIAddressReleaseAddr(virDomainPCIAddressSetPtr addrs,
                               virPCIDeviceAddressPtr addr)
{
    addrs->buses[addr->bus].slots[addr->slot] &= ~(1 << addr->function);
    return 0;
}

int
virDomainPCIAddressReleaseSlot(virDomainPCIAddressSetPtr addrs,
                               virPCIDeviceAddressPtr addr)
{
    /* permit any kind of connection type in validation, since we
     * already had it, and are giving it back.
     */
    virDomainPCIConnectFlags flags = VIR_PCI_CONNECT_TYPES_MASK;
    int ret = -1;
    char *addrStr = NULL;

    if (!(addrStr = virDomainPCIAddressAsString(addr)))
        goto cleanup;

    if (!virDomainPCIAddressValidate(addrs, addr, addrStr, flags, false))
        goto cleanup;

    addrs->buses[addr->bus].slots[addr->slot] = 0;
    ret = 0;
 cleanup:
    VIR_FREE(addrStr);
    return ret;
}


virDomainPCIAddressSetPtr
virDomainPCIAddressSetAlloc(unsigned int nbuses)
{
    virDomainPCIAddressSetPtr addrs;

    if (VIR_ALLOC(addrs) < 0)
        goto error;

    if (VIR_ALLOC_N(addrs->buses, nbuses) < 0)
        goto error;

    addrs->nbuses = nbuses;
    return addrs;

 error:
    virDomainPCIAddressSetFree(addrs);
    return NULL;
}


void
virDomainPCIAddressSetFree(virDomainPCIAddressSetPtr addrs)
{
    if (!addrs)
        return;

    VIR_FREE(addrs->buses);
    VIR_FREE(addrs);
}


int
virDomainPCIAddressGetNextSlot(virDomainPCIAddressSetPtr addrs,
                               virPCIDeviceAddressPtr next_addr,
                               virDomainPCIConnectFlags flags)
{
    /* default to starting the search for a free slot from
     * the first slot of domain 0 bus 0...
     */
    virPCIDeviceAddress a = { 0, 0, 0, 0, false };
    char *addrStr = NULL;

    if (addrs->nbuses == 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s", _("No PCI buses available"));
        goto error;
    }

    /* ...unless this search is for the exact same type of device as
     * last time, then continue the search from the next slot after
     * the previous match (the "next slot" may possibly be the first
     * slot of the next bus).
     */
    if (flags == addrs->lastFlags) {
        a = addrs->lastaddr;
        if (++a.slot > addrs->buses[a.bus].maxSlot &&
            ++a.bus < addrs->nbuses)
            a.slot = addrs->buses[a.bus].minSlot;
    } else {
        a.slot = addrs->buses[0].minSlot;
    }

    while (a.bus < addrs->nbuses) {
        VIR_FREE(addrStr);
        if (!(addrStr = virDomainPCIAddressAsString(&a)))
            goto error;
        if (!virDomainPCIAddressFlagsCompatible(&a, addrStr,
                                                addrs->buses[a.bus].flags,
                                                flags, false, false)) {
            VIR_DEBUG("PCI bus %.4x:%.2x is not compatible with the device",
                      a.domain, a.bus);
        } else {
            while (a.slot <= addrs->buses[a.bus].maxSlot) {
                if (!virDomainPCIAddressSlotInUse(addrs, &a))
                    goto success;

                VIR_DEBUG("PCI slot %.4x:%.2x:%.2x already in use",
                          a.domain, a.bus, a.slot);
                a.slot++;
            }
        }
        if (++a.bus < addrs->nbuses)
            a.slot = addrs->buses[a.bus].minSlot;
    }

    /* There were no free slots after the last used one */
    if (addrs->dryRun) {
        /* a is already set to the first new bus */
        if (virDomainPCIAddressSetGrow(addrs, &a, flags) < 0)
            goto error;
        /* this device will use the first slot of the new bus */
        a.slot = addrs->buses[a.bus].minSlot;
        goto success;
    } else if (flags == addrs->lastFlags) {
        /* Check the buses from 0 up to the last used one */
        for (a.bus = 0; a.bus <= addrs->lastaddr.bus; a.bus++) {
            a.slot = addrs->buses[a.bus].minSlot;
            VIR_FREE(addrStr);
            if (!(addrStr = virDomainPCIAddressAsString(&a)))
                goto error;
            if (!virDomainPCIAddressFlagsCompatible(&a, addrStr,
                                                    addrs->buses[a.bus].flags,
                                                    flags, false, false)) {
                VIR_DEBUG("PCI bus %.4x:%.2x is not compatible with the device",
                          a.domain, a.bus);
            } else {
                while (a.slot <= addrs->buses[a.bus].maxSlot) {
                    if (!virDomainPCIAddressSlotInUse(addrs, &a))
                        goto success;

                    VIR_DEBUG("PCI slot %.4x:%.2x:%.2x already in use",
                              a.domain, a.bus, a.slot);
                    a.slot++;
                }
            }
        }
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("No more available PCI slots"));
 error:
    VIR_FREE(addrStr);
    return -1;

 success:
    VIR_DEBUG("Found free PCI slot %.4x:%.2x:%.2x",
              a.domain, a.bus, a.slot);
    *next_addr = a;
    VIR_FREE(addrStr);
    return 0;
}

int
virDomainPCIAddressReserveNextSlot(virDomainPCIAddressSetPtr addrs,
                                   virDomainDeviceInfoPtr dev,
                                   virDomainPCIConnectFlags flags)
{
    virPCIDeviceAddress addr;
    if (virDomainPCIAddressGetNextSlot(addrs, &addr, flags) < 0)
        return -1;

    if (virDomainPCIAddressReserveSlot(addrs, &addr, flags) < 0)
        return -1;

    if (!addrs->dryRun) {
        dev->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        dev->addr.pci = addr;
    }

    addrs->lastaddr = addr;
    addrs->lastFlags = flags;
    return 0;
}


static char*
virDomainCCWAddressAsString(virDomainDeviceCCWAddressPtr addr)
{
    char *addrstr = NULL;

    ignore_value(virAsprintf(&addrstr, "%x.%x.%04x",
                             addr->cssid,
                             addr->ssid,
                             addr->devno));
    return addrstr;
}

static int
virDomainCCWAddressIncrement(virDomainDeviceCCWAddressPtr addr)
{
    virDomainDeviceCCWAddress ccwaddr = *addr;

    /* We are not touching subchannel sets and channel subsystems */
    if (++ccwaddr.devno > VIR_DOMAIN_DEVICE_CCW_MAX_DEVNO)
        return -1;

    *addr = ccwaddr;
    return 0;
}


int
virDomainCCWAddressAssign(virDomainDeviceInfoPtr dev,
                          virDomainCCWAddressSetPtr addrs,
                          bool autoassign)
{
    int ret = -1;
    char *addr = NULL;

    if (dev->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
        return 0;

    if (!autoassign && dev->addr.ccw.assigned) {
        if (!(addr = virDomainCCWAddressAsString(&dev->addr.ccw)))
            goto cleanup;

        if (virHashLookup(addrs->defined, addr)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("The CCW devno '%s' is in use already "),
                           addr);
            goto cleanup;
        }
    } else if (autoassign && !dev->addr.ccw.assigned) {
        if (!(addr = virDomainCCWAddressAsString(&addrs->next)))
            goto cleanup;

        while (virHashLookup(addrs->defined, addr)) {
            if (virDomainCCWAddressIncrement(&addrs->next) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("There are no more free CCW devnos."));
                goto cleanup;
            }
            VIR_FREE(addr);
            if (!(addr = virDomainCCWAddressAsString(&addrs->next)))
                goto cleanup;
        }
        dev->addr.ccw = addrs->next;
        dev->addr.ccw.assigned = true;
    } else {
        return 0;
    }

    if (virHashAddEntry(addrs->defined, addr, addr) < 0)
        goto cleanup;
    else
        addr = NULL; /* memory will be freed by hash table */

    ret = 0;

 cleanup:
    VIR_FREE(addr);
    return ret;
}

int
virDomainCCWAddressAllocate(virDomainDefPtr def ATTRIBUTE_UNUSED,
                            virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                            virDomainDeviceInfoPtr info,
                            void *data)
{
    return virDomainCCWAddressAssign(info, data, true);
}

int
virDomainCCWAddressValidate(virDomainDefPtr def ATTRIBUTE_UNUSED,
                            virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                            virDomainDeviceInfoPtr info,
                            void *data)
{
    return virDomainCCWAddressAssign(info, data, false);
}

int
virDomainCCWAddressReleaseAddr(virDomainCCWAddressSetPtr addrs,
                               virDomainDeviceInfoPtr dev)
{
    char *addr;
    int ret;

    addr = virDomainCCWAddressAsString(&(dev->addr.ccw));
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

void virDomainCCWAddressSetFree(virDomainCCWAddressSetPtr addrs)
{
    if (!addrs)
        return;

    virHashFree(addrs->defined);
    VIR_FREE(addrs);
}

virDomainCCWAddressSetPtr
virDomainCCWAddressSetCreate(void)
{
    virDomainCCWAddressSetPtr addrs = NULL;

    if (VIR_ALLOC(addrs) < 0)
        goto error;

    if (!(addrs->defined = virHashCreate(10, virHashValueFree)))
        goto error;

    /* must use cssid = 0xfe (254) for virtio-ccw devices */
    addrs->next.cssid = 254;
    addrs->next.ssid = 0;
    addrs->next.devno = 0;
    addrs->next.assigned = 0;
    return addrs;

 error:
    virDomainCCWAddressSetFree(addrs);
    return NULL;
}


#define VIR_DOMAIN_DEFAULT_VIRTIO_SERIAL_PORTS 31


/* virDomainVirtioSerialAddrSetCreate
 *
 * Allocates an address set for virtio serial addresses
 */
virDomainVirtioSerialAddrSetPtr
virDomainVirtioSerialAddrSetCreate(void)
{
    virDomainVirtioSerialAddrSetPtr ret = NULL;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    return ret;
}

static void
virDomainVirtioSerialControllerFree(virDomainVirtioSerialControllerPtr cont)
{
    if (cont) {
        virBitmapFree(cont->ports);
        VIR_FREE(cont);
    }
}

static ssize_t
virDomainVirtioSerialAddrPlaceController(virDomainVirtioSerialAddrSetPtr addrs,
                                         virDomainVirtioSerialControllerPtr cont)
{
    size_t i;

    for (i = 0; i < addrs->ncontrollers; i++) {
        if (addrs->controllers[i]->idx == cont->idx) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("virtio serial controller with index %u already exists"
                             " in the address set"),
                           cont->idx);
            return -2;
        }
        if (addrs->controllers[i]->idx > cont->idx)
            return i;
    }
    return -1;
}

static ssize_t
virDomainVirtioSerialAddrFindController(virDomainVirtioSerialAddrSetPtr addrs,
                                        unsigned int idx)
{
    size_t i;

    for (i = 0; i < addrs->ncontrollers; i++) {
        if (addrs->controllers[i]->idx == idx)
            return i;
    }
    return -1;
}

/* virDomainVirtioSerialAddrSetAddController
 *
 * Adds virtio serial ports of the existing controller
 * to the address set.
 */
static int
virDomainVirtioSerialAddrSetAddController(virDomainVirtioSerialAddrSetPtr addrs,
                                          virDomainControllerDefPtr cont)
{
    int ret = -1;
    int ports;
    virDomainVirtioSerialControllerPtr cnt = NULL;
    ssize_t insertAt;

    if (cont->type != VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL)
        return 0;

    ports = cont->opts.vioserial.ports;
    if (ports == -1)
        ports = VIR_DOMAIN_DEFAULT_VIRTIO_SERIAL_PORTS;

    VIR_DEBUG("Adding virtio serial controller index %u with %d"
              " ports to the address set", cont->idx, ports);

    if (VIR_ALLOC(cnt) < 0)
        goto cleanup;

    if (!(cnt->ports = virBitmapNew(ports)))
        goto cleanup;
    cnt->idx = cont->idx;

    if ((insertAt = virDomainVirtioSerialAddrPlaceController(addrs, cnt)) < -1)
        goto cleanup;
    if (VIR_INSERT_ELEMENT(addrs->controllers, insertAt,
                           addrs->ncontrollers, cnt) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virDomainVirtioSerialControllerFree(cnt);
    return ret;
}

/* virDomainVirtioSerialAddrSetAddControllers
 *
 * Adds virtio serial ports of controllers present in the domain definition
 * to the address set.
 */
int
virDomainVirtioSerialAddrSetAddControllers(virDomainVirtioSerialAddrSetPtr addrs,
                                           virDomainDefPtr def)
{
    size_t i;

    for (i = 0; i < def->ncontrollers; i++) {
        if (virDomainVirtioSerialAddrSetAddController(addrs,
                                                      def->controllers[i]) < 0)
            return -1;
    }

    return 0;
}


void
virDomainVirtioSerialAddrSetFree(virDomainVirtioSerialAddrSetPtr addrs)
{
    size_t i;
    if (addrs) {
        for (i = 0; i < addrs->ncontrollers; i++)
            virDomainVirtioSerialControllerFree(addrs->controllers[i]);
        VIR_FREE(addrs->controllers);
        VIR_FREE(addrs);
    }
}


/* virDomainVirtioSerialAddrSetCreateFromDomain
+ *
+ * @def: Domain def to introspect
+ *
+ * Inspect the domain definition and return an address set containing
+ * every virtio serial address we find
+ */
virDomainVirtioSerialAddrSetPtr
virDomainVirtioSerialAddrSetCreateFromDomain(virDomainDefPtr def)
{
    virDomainVirtioSerialAddrSetPtr addrs = NULL;
    virDomainVirtioSerialAddrSetPtr ret = NULL;

    if (!(addrs = virDomainVirtioSerialAddrSetCreate()))
        goto cleanup;

    if (virDomainVirtioSerialAddrSetAddControllers(addrs, def) < 0)
        goto cleanup;

    if (virDomainDeviceInfoIterate(def, virDomainVirtioSerialAddrReserve,
                                   addrs) < 0)
        goto cleanup;

    ret = addrs;
    addrs = NULL;
 cleanup:
    virDomainVirtioSerialAddrSetFree(addrs);
    return ret;
}

static int
virDomainVirtioSerialAddrSetAutoaddController(virDomainDefPtr def,
                                              virDomainVirtioSerialAddrSetPtr addrs,
                                              unsigned int idx)
{
    int contidx;

    if (virDomainDefMaybeAddController(def,
                                       VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL,
                                       idx, -1) < 0)
        return -1;

    contidx = virDomainControllerFind(def, VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL, idx);

    if (virDomainVirtioSerialAddrSetAddController(addrs, def->controllers[contidx]) < 0)
        return -1;

    return 0;
}

static int
virDomainVirtioSerialAddrNext(virDomainDefPtr def,
                              virDomainVirtioSerialAddrSetPtr addrs,
                              virDomainDeviceVirtioSerialAddress *addr,
                              bool allowZero)
{
    int ret = -1;
    ssize_t port, startPort = 0;
    ssize_t i;
    unsigned int controller;

    /* port number 0 is reserved for virtconsoles */
    if (allowZero)
        startPort = -1;

    if (addrs->ncontrollers == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("no virtio-serial controllers are available"));
        goto cleanup;
    }

    for (i = 0; i < addrs->ncontrollers; i++) {
        virBitmapPtr map = addrs->controllers[i]->ports;
        if ((port = virBitmapNextClearBit(map, startPort)) >= 0) {
            controller = addrs->controllers[i]->idx;
            goto success;
        }
    }

    if (def) {
        for (i = 0; i < INT_MAX; i++) {
            int idx = virDomainControllerFind(def, VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL, i);

            if (idx == -1) {
                if (virDomainVirtioSerialAddrSetAutoaddController(def, addrs, i) < 0)
                    goto cleanup;
                controller = i;
                port = startPort + 1;
                goto success;
            }
        }
    }

    virReportError(VIR_ERR_XML_ERROR, "%s",
                   _("Unable to find a free virtio-serial port"));

 cleanup:
    return ret;

 success:
    addr->bus = 0;
    addr->port = port;
    addr->controller = controller;
    VIR_DEBUG("Found free virtio serial controller %u port %u", addr->controller,
              addr->port);
    ret = 0;
    goto cleanup;
}

static int
virDomainVirtioSerialAddrNextFromController(virDomainVirtioSerialAddrSetPtr addrs,
                                            virDomainDeviceVirtioSerialAddress *addr)
{
    ssize_t port;
    ssize_t i;
    virBitmapPtr map;

    i = virDomainVirtioSerialAddrFindController(addrs, addr->controller);
    if (i < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("virtio-serial controller %u not available"),
                       addr->controller);
        return -1;
    }

    map = addrs->controllers[i]->ports;
    if ((port = virBitmapNextClearBit(map, 0)) <= 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Unable to find a free port on virtio-serial controller %u"),
                       addr->controller);
        return -1;
    }

    addr->bus = 0;
    addr->port = port;
    VIR_DEBUG("Found free virtio serial controller %u port %u", addr->controller,
              addr->port);
    return 0;
}

/* virDomainVirtioSerialAddrAutoAssign
 *
 * reserve a virtio serial address of the device (if it has one)
 * or assign a virtio serial address to the device
 */
int
virDomainVirtioSerialAddrAutoAssign(virDomainDefPtr def,
                                    virDomainVirtioSerialAddrSetPtr addrs,
                                    virDomainDeviceInfoPtr info,
                                    bool allowZero)
{
    bool portOnly = info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL;
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
        info->addr.vioserial.port)
        return virDomainVirtioSerialAddrReserve(NULL, NULL, info, addrs);
    else
        return virDomainVirtioSerialAddrAssign(def, addrs, info, allowZero, portOnly);
}


int
virDomainVirtioSerialAddrAssign(virDomainDefPtr def,
                                virDomainVirtioSerialAddrSetPtr addrs,
                                virDomainDeviceInfoPtr info,
                                bool allowZero,
                                bool portOnly)
{
    int ret = -1;
    virDomainDeviceInfo nfo = { NULL };
    virDomainDeviceInfoPtr ptr = allowZero ? &nfo : info;

    ptr->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL;

    if (portOnly) {
        if (virDomainVirtioSerialAddrNextFromController(addrs,
                                                        &ptr->addr.vioserial) < 0)
            goto cleanup;
    } else {
        if (virDomainVirtioSerialAddrNext(def, addrs, &ptr->addr.vioserial,
                                          allowZero) < 0)
            goto cleanup;
    }

    if (virDomainVirtioSerialAddrReserve(NULL, NULL, ptr, addrs) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    return ret;
}

/* virDomainVirtioSerialAddrIsComplete
 *
 * Check if the address is complete, or it needs auto-assignment
 */
bool
virDomainVirtioSerialAddrIsComplete(virDomainDeviceInfoPtr info)
{
    return info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL &&
        info->addr.vioserial.port != 0;
}

/* virDomainVirtioSerialAddrReserve
 *
 * Reserve the virtio serial address of the device
 *
 * For use with virDomainDeviceInfoIterate,
 * opaque should be the address set
 */
int
virDomainVirtioSerialAddrReserve(virDomainDefPtr def ATTRIBUTE_UNUSED,
                                 virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                                 virDomainDeviceInfoPtr info,
                                 void *data)
{
    virDomainVirtioSerialAddrSetPtr addrs = data;
    char *str = NULL;
    int ret = -1;
    virBitmapPtr map = NULL;
    bool b;
    ssize_t i;

    if (!virDomainVirtioSerialAddrIsComplete(info))
        return 0;

    VIR_DEBUG("Reserving virtio serial %u %u", info->addr.vioserial.controller,
              info->addr.vioserial.port);

    i = virDomainVirtioSerialAddrFindController(addrs, info->addr.vioserial.controller);
    if (i < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("virtio serial controller %u is missing"),
                       info->addr.vioserial.controller);
        goto cleanup;
    }

    map = addrs->controllers[i]->ports;
    if (virBitmapGetBit(map, info->addr.vioserial.port, &b) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("virtio serial controller %u does not have port %u"),
                       info->addr.vioserial.controller,
                       info->addr.vioserial.port);
        goto cleanup;
    }

    if (b) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("virtio serial port %u on controller %u is already occupied"),
                       info->addr.vioserial.port,
                       info->addr.vioserial.controller);
        goto cleanup;
    }

    ignore_value(virBitmapSetBit(map, info->addr.vioserial.port));

    ret = 0;

 cleanup:
    VIR_FREE(str);
    return ret;
}

/* virDomainVirtioSerialAddrRelease
 *
 * Release the virtio serial address of the device
 */
int
virDomainVirtioSerialAddrRelease(virDomainVirtioSerialAddrSetPtr addrs,
                                 virDomainDeviceInfoPtr info)
{
    virBitmapPtr map;
    char *str = NULL;
    int ret = -1;
    ssize_t i;

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL ||
        info->addr.vioserial.port == 0)
        return 0;

    VIR_DEBUG("Releasing virtio serial %u %u", info->addr.vioserial.controller,
              info->addr.vioserial.port);

    i = virDomainVirtioSerialAddrFindController(addrs, info->addr.vioserial.controller);
    if (i < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("virtio serial controller %u is missing"),
                       info->addr.vioserial.controller);
        goto cleanup;
    }

    map = addrs->controllers[i]->ports;
    if (virBitmapClearBit(map, info->addr.vioserial.port) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("virtio serial controller %u does not have port %u"),
                       info->addr.vioserial.controller,
                       info->addr.vioserial.port);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(str);
    return ret;
}


bool
virDomainUSBAddressPortIsValid(unsigned int *port)
{
    return port[0] != 0;
}


void
virDomainUSBAddressPortFormatBuf(virBufferPtr buf,
                                 unsigned int *port)
{
    size_t i;

    for (i = 0; i < VIR_DOMAIN_DEVICE_USB_MAX_PORT_DEPTH; i++) {
        if (port[i] == 0)
            break;
        virBufferAsprintf(buf, "%u.", port[i]);
    }
    virBufferTrim(buf, ".", -1);
}


char *
virDomainUSBAddressPortFormat(unsigned int *port)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainUSBAddressPortFormatBuf(&buf, port);
    if (virBufferCheckError(&buf) < 0)
        return NULL;
    return virBufferContentAndReset(&buf);
}


virDomainUSBAddressSetPtr
virDomainUSBAddressSetCreate(void)
{
    virDomainUSBAddressSetPtr addrs;

    if (VIR_ALLOC(addrs) < 0)
        return NULL;

    return addrs;
}


static void
virDomainUSBAddressHubFree(virDomainUSBAddressHubPtr hub)
{
    size_t i;

    if (!hub)
        return;

    for (i = 0; i < hub->nports; i++)
        virDomainUSBAddressHubFree(hub->ports[i]);
    VIR_FREE(hub->ports);
    virBitmapFree(hub->portmap);
    VIR_FREE(hub);
}


void
virDomainUSBAddressSetFree(virDomainUSBAddressSetPtr addrs)
{
    size_t i;

    if (!addrs)
        return;

    for (i = 0; i < addrs->nbuses; i++)
        virDomainUSBAddressHubFree(addrs->buses[i]);
    VIR_FREE(addrs->buses);
    VIR_FREE(addrs);
}


static size_t
virDomainUSBAddressControllerModelToPorts(virDomainControllerDefPtr cont)
{
    int model = cont->model;

    if (model == -1)
        model = VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI;

    switch ((virDomainControllerModelUSB) model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX4_UHCI:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_VT82C686B_UHCI:
        return 2;

    case VIR_DOMAIN_CONTROLLER_MODEL_USB_EHCI:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1:
        return 6;

    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI2:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_UHCI3:
        /* These have two ports each and are used to provide USB1.1
         * ports while ICH9_EHCI1 provides 6 USB2.0 ports.
         * Ignore these since we will add the EHCI1 too. */
        return 0;

    case VIR_DOMAIN_CONTROLLER_MODEL_USB_PCI_OHCI:
        return 3;

    case VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI:
        if (cont->opts.usbopts.ports != -1)
            return cont->opts.usbopts.ports;
        return 4;

    case VIR_DOMAIN_CONTROLLER_MODEL_USB_QUSB1:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_QUSB2:
        if (cont->opts.usbopts.ports != -1)
            return cont->opts.usbopts.ports;
        return 8;

    case VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE:
    case VIR_DOMAIN_CONTROLLER_MODEL_USB_LAST:
        break;
    }
    return 0;
}


static virDomainUSBAddressHubPtr
virDomainUSBAddressHubNew(size_t nports)
{
    virDomainUSBAddressHubPtr hub = NULL, ret = NULL;

    if (VIR_ALLOC(hub) < 0)
        goto cleanup;

    if (!(hub->portmap = virBitmapNew(nports)))
        goto cleanup;

    if (VIR_ALLOC_N(hub->ports, nports) < 0)
        goto cleanup;
    hub->nports = nports;

    ret = hub;
    hub = NULL;
 cleanup:
    virDomainUSBAddressHubFree(hub);
    return ret;
}


static int
virDomainUSBAddressSetAddController(virDomainUSBAddressSetPtr addrs,
                                    virDomainControllerDefPtr cont)
{
    size_t nports = virDomainUSBAddressControllerModelToPorts(cont);
    virDomainUSBAddressHubPtr hub = NULL;
    int ret = -1;

    VIR_DEBUG("Adding a USB controller model=%s with %zu ports",
              virDomainControllerModelUSBTypeToString(cont->model),
              nports);

    /* Skip UHCI{1,2,3} companions; only add the EHCI1 */
    if (nports == 0)
        return 0;

    if (addrs->nbuses <= cont->idx) {
        if (VIR_EXPAND_N(addrs->buses, addrs->nbuses, cont->idx - addrs->nbuses + 1) < 0)
            goto cleanup;
    } else if (addrs->buses[cont->idx]) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Duplicate USB controllers with index %u"),
                       cont->idx);
        goto cleanup;
    }

    if (!(hub = virDomainUSBAddressHubNew(nports)))
        goto cleanup;

    addrs->buses[cont->idx] = hub;
    hub = NULL;

    ret = 0;
 cleanup:
    virDomainUSBAddressHubFree(hub);
    return ret;
}


static ssize_t
virDomainUSBAddressGetLastIdx(virDomainDeviceInfoPtr info)
{
    ssize_t i;
    for (i = VIR_DOMAIN_DEVICE_USB_MAX_PORT_DEPTH - 1; i > 0; i--) {
        if (info->addr.usb.port[i] != 0)
            break;
    }
    return i;
}


/* Find the USBAddressHub structure representing the hub/controller
 * that corresponds to the bus/port path specified by info.
 * Returns the index of the requested port in targetIdx.
 */
static virDomainUSBAddressHubPtr
virDomainUSBAddressFindPort(virDomainUSBAddressSetPtr addrs,
                            virDomainDeviceInfoPtr info,
                            int *targetIdx,
                            const char *portStr)
{
    virDomainUSBAddressHubPtr hub = NULL;
    ssize_t i, lastIdx;

    if (info->addr.usb.bus >= addrs->nbuses ||
        !addrs->buses[info->addr.usb.bus]) {
        virReportError(VIR_ERR_XML_ERROR, _("Missing USB bus %u"),
                       info->addr.usb.bus);
        return NULL;
    }
    hub = addrs->buses[info->addr.usb.bus];

    lastIdx = virDomainUSBAddressGetLastIdx(info);

    for (i = 0; i < lastIdx; i++) {
        /* ports are numbered from 1 */
        int portIdx = info->addr.usb.port[i] - 1;

        if (hub->nports <= portIdx) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("port %u out of range in USB address bus: %u port: %s"),
                           info->addr.usb.port[i],
                           info->addr.usb.bus,
                           portStr);
            return NULL;
        }
        hub = hub->ports[portIdx];
        if (!hub) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("there is no hub at port %u in USB address bus: %u port: %s"),
                           info->addr.usb.port[i],
                           info->addr.usb.bus,
                           portStr);
            return NULL;
        }
    }

    *targetIdx = info->addr.usb.port[lastIdx] - 1;
    return hub;
}


#define VIR_DOMAIN_USB_HUB_PORTS 8

int
virDomainUSBAddressSetAddHub(virDomainUSBAddressSetPtr addrs,
                             virDomainHubDefPtr hub)
{
    virDomainUSBAddressHubPtr targetHub = NULL, newHub = NULL;
    int ret = -1;
    int targetPort;
    char *portStr = NULL;

    if (hub->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Wrong address type for USB hub"));
        goto cleanup;
    }

    if (!(portStr = virDomainUSBAddressPortFormat(hub->info.addr.usb.port)))
        goto cleanup;

    VIR_DEBUG("Adding a USB hub with 8 ports on bus=%u port=%s",
              hub->info.addr.usb.bus, portStr);

    if (!(newHub = virDomainUSBAddressHubNew(VIR_DOMAIN_USB_HUB_PORTS)))
        goto cleanup;

    if (!(targetHub = virDomainUSBAddressFindPort(addrs, &(hub->info), &targetPort,
                                                  portStr)))
        goto cleanup;

    if (targetHub->ports[targetPort]) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Duplicate USB hub on bus %u port %s"),
                       hub->info.addr.usb.bus, portStr);
        goto cleanup;
    }
    ignore_value(virBitmapSetBit(targetHub->portmap, targetPort));
    targetHub->ports[targetPort] = newHub;
    newHub = NULL;

    ret = 0;
 cleanup:
    virDomainUSBAddressHubFree(newHub);
    VIR_FREE(portStr);
    return ret;
}


int
virDomainUSBAddressSetAddControllers(virDomainUSBAddressSetPtr addrs,
                                     virDomainDefPtr def)
{
    size_t i;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDefPtr cont = def->controllers[i];
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB) {
            if (virDomainUSBAddressSetAddController(addrs, cont) < 0)
                return -1;
        }
    }

    for (i = 0; i < def->nhubs; i++) {
        virDomainHubDefPtr hub = def->hubs[i];
        if (hub->type == VIR_DOMAIN_HUB_TYPE_USB &&
            hub->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB &&
            virDomainUSBAddressPortIsValid(hub->info.addr.usb.port)) {
            /* USB hubs that do not yet have an USB address have to be
             * dealt with later */
            if (virDomainUSBAddressSetAddHub(addrs, hub) < 0)
                return -1;
        }
    }
    return 0;
}


static int
virDomainUSBAddressFindFreePort(virDomainUSBAddressHubPtr hub,
                                unsigned int *portpath,
                                unsigned int level)
{
    unsigned int port;
    ssize_t portIdx;
    size_t i;

    /* Look for free ports on the current hub */
    if ((portIdx = virBitmapNextClearBit(hub->portmap, -1)) >= 0) {
        port = portIdx + 1;
        VIR_DEBUG("Found a free port %u at level %u", port, level);
        portpath[level] = port;
        return 0;
    }

    VIR_DEBUG("No ports found on hub %p, trying the hubs on it", hub);

    if (level >= VIR_DOMAIN_DEVICE_USB_MAX_PORT_DEPTH - 1)
        return -1;

    /* Recursively search through the ports that contain another hub */
    for (i = 0; i < hub->nports; i++) {
        if (!hub->ports[i])
            continue;

        port = i + 1;
        VIR_DEBUG("Looking at USB hub at level: %u port: %u", level, port);
        if (virDomainUSBAddressFindFreePort(hub->ports[i], portpath,
                                            level + 1) < 0)
            continue;

        portpath[level] = port;
        return 0;
    }
    return -1;
}


size_t
virDomainUSBAddressCountAllPorts(virDomainDefPtr def)
{
    size_t i, ret = 0;

    for (i = 0; i < def->ncontrollers; i++) {
        virDomainControllerDefPtr cont = def->controllers[i];
        if (cont->type == VIR_DOMAIN_CONTROLLER_TYPE_USB)
            ret += virDomainUSBAddressControllerModelToPorts(cont);
    }

    for (i = 0; i < def->nhubs; i++) {
        virDomainHubDefPtr hub = def->hubs[i];
        if (hub->type == VIR_DOMAIN_HUB_TYPE_USB)
            ret += VIR_DOMAIN_USB_HUB_PORTS;
    }
    return ret;
}


/* Try to find a free port on bus @bus.
 *
 * Returns  0 on success
 *         -1 on fatal error (OOM)
 *         -2 if there is no bus at @bus or no free port on this bus
 */
static int
virDomainUSBAddressAssignFromBus(virDomainUSBAddressSetPtr addrs,
                                 virDomainDeviceInfoPtr info,
                                 size_t bus)
{
    unsigned int portpath[VIR_DOMAIN_DEVICE_USB_MAX_PORT_DEPTH] = { 0 };
    virDomainUSBAddressHubPtr hub = addrs->buses[bus];
    char *portStr = NULL;
    int ret = -1;

    if (!hub)
        return -2;

    if (virDomainUSBAddressFindFreePort(hub, portpath, 0) < 0)
        return -2;

    /* we found a free port */
    if (!(portStr = virDomainUSBAddressPortFormat(portpath)))
        goto cleanup;

    info->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB;
    info->addr.usb.bus = bus;
    memcpy(info->addr.usb.port, portpath, sizeof(portpath));
    VIR_DEBUG("Assigning USB addr bus=%u port=%s",
              info->addr.usb.bus, portStr);
    if (virDomainUSBAddressReserve(info, addrs) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(portStr);
    return ret;
}


int
virDomainUSBAddressAssign(virDomainUSBAddressSetPtr addrs,
                          virDomainDeviceInfoPtr info)
{
    size_t i;
    int rc;

    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        VIR_DEBUG("A USB port on bus %u was requested", info->addr.usb.bus);
        if (!addrs->buses[info->addr.usb.bus]) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("USB bus %u requested but no controller "
                             "with that index is present"), info->addr.usb.bus);
            return -1;
        }
        rc = virDomainUSBAddressAssignFromBus(addrs, info, info->addr.usb.bus);
        if (rc >= -1)
            return rc;
    } else {
        VIR_DEBUG("Looking for a free USB port on all the buses");
        for (i = 0; i < addrs->nbuses; i++) {
            rc = virDomainUSBAddressAssignFromBus(addrs, info, i);
            if (rc >= -1)
                return rc;
        }
    }

    virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("No free USB ports"));
    return -1;
}


int
virDomainUSBAddressReserve(virDomainDeviceInfoPtr info,
                           void *data)
{
    virDomainUSBAddressSetPtr addrs = data;
    virDomainUSBAddressHubPtr targetHub = NULL;
    char *portStr = NULL;
    int ret = -1;
    int targetPort;

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB)
        return 0;

    if (!virDomainUSBAddressPortIsValid(info->addr.usb.port))
        return 0;

    portStr = virDomainUSBAddressPortFormat(info->addr.usb.port);
    if (!portStr)
        goto cleanup;
    VIR_DEBUG("Reserving USB address bus=%u port=%s", info->addr.usb.bus, portStr);

    if (!(targetHub = virDomainUSBAddressFindPort(addrs, info, &targetPort,
                                                  portStr)))
        goto cleanup;

    if (virBitmapIsBitSet(targetHub->portmap, targetPort)) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Duplicate USB address bus %u port %s"),
                       info->addr.usb.bus, portStr);
        goto cleanup;
    }

    ignore_value(virBitmapSetBit(targetHub->portmap, targetPort));

    ret = 0;

 cleanup:
    VIR_FREE(portStr);
    return ret;
}


int
virDomainUSBAddressEnsure(virDomainUSBAddressSetPtr addrs,
                          virDomainDeviceInfoPtr info)
{
    if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE ||
        (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB &&
         !virDomainUSBAddressPortIsValid(info->addr.usb.port))) {
        if (virDomainUSBAddressAssign(addrs, info) < 0)
            return -1;
    } else if (info->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB) {
        if (virDomainUSBAddressReserve(info, addrs) < 0)
            return -1;
    }

    return 0;
}


int
virDomainUSBAddressRelease(virDomainUSBAddressSetPtr addrs,
                           virDomainDeviceInfoPtr info)
{
    virDomainUSBAddressHubPtr targetHub = NULL;
    char *portStr = NULL;
    int targetPort;
    int ret = -1;

    if (info->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB ||
        !virDomainUSBAddressPortIsValid(info->addr.usb.port))
        return 0;

    portStr = virDomainUSBAddressPortFormat(info->addr.usb.port);
    VIR_DEBUG("Releasing USB addr bus=%u port=%s", info->addr.usb.bus, portStr);

    if (!(targetHub = virDomainUSBAddressFindPort(addrs, info, &targetPort,
                                                  portStr)))
        goto cleanup;

    ignore_value(virBitmapClearBit(targetHub->portmap, targetPort));

    ret = 0;

 cleanup:
    VIR_FREE(portStr);
    return ret;
}
