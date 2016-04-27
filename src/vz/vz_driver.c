/*
 * vz_driver.c: core driver functions for managing
 * Parallels Cloud Server hosts
 *
 * Copyright (C) 2014-2015 Red Hat, Inc.
 * Copyright (C) 2012 Parallels, Inc.
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
 */

#include <config.h>

#include <sys/types.h>
#include <sys/poll.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/statvfs.h>

#include "datatypes.h"
#include "virerror.h"
#include "viralloc.h"
#include "virlog.h"
#include "vircommand.h"
#include "configmake.h"
#include "virfile.h"
#include "virstoragefile.h"
#include "nodeinfo.h"
#include "virstring.h"
#include "cpu/cpu.h"
#include "virtypedparam.h"

#include "vz_driver.h"
#include "vz_utils.h"
#include "vz_sdk.h"

#define VIR_FROM_THIS VIR_FROM_PARALLELS

VIR_LOG_INIT("parallels.parallels_driver");

#define PRLCTL                      "prlctl"

static virClassPtr vzDriverClass;

static virMutex vz_driver_lock;
static vzDriverPtr vz_driver;
static vzConnPtr vz_conn_list;

static vzDriverPtr
vzDriverObjNew(void);

static int
vzCapsAddGuestDomain(virCapsPtr caps,
                     virDomainOSType ostype,
                     virArch arch,
                     const char * emulator,
                     virDomainVirtType virt_type)
{
    virCapsGuestPtr guest;

    if ((guest = virCapabilitiesAddGuest(caps, ostype, arch, emulator,
                                         NULL, 0, NULL)) == NULL)
        return -1;


    if (virCapabilitiesAddGuestDomain(guest, virt_type,
                                      NULL, NULL, 0, NULL) == NULL)
        return -1;

    return 0;
}

static virCapsPtr
vzBuildCapabilities(void)
{
    virCapsPtr caps = NULL;
    virCPUDefPtr cpu = NULL;
    virCPUDataPtr data = NULL;
    virNodeInfo nodeinfo;
    virDomainOSType ostypes[] = {
        VIR_DOMAIN_OSTYPE_HVM,
        VIR_DOMAIN_OSTYPE_EXE
    };
    virArch archs[] = { VIR_ARCH_I686, VIR_ARCH_X86_64 };
    const char *const emulators[] = { "vz", "parallels"};
    virDomainVirtType virt_types[] = {
        VIR_DOMAIN_VIRT_VZ,
        VIR_DOMAIN_VIRT_PARALLELS
    };
    size_t i, j, k;

    if ((caps = virCapabilitiesNew(virArchFromHost(),
                                   false, false)) == NULL)
        return NULL;

    if (nodeCapsInitNUMA(NULL, caps) < 0)
        goto error;

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            for (k = 0; k < 2; k++)
                if (vzCapsAddGuestDomain(caps, ostypes[i], archs[j],
                                         emulators[k], virt_types[k]) < 0)
                    goto error;

    if (nodeGetInfo(NULL, &nodeinfo))
        goto error;

    if (VIR_ALLOC(cpu) < 0)
        goto error;

    cpu->arch = caps->host.arch;
    cpu->type = VIR_CPU_TYPE_HOST;
    cpu->sockets = nodeinfo.sockets;
    cpu->cores = nodeinfo.cores;
    cpu->threads = nodeinfo.threads;

    caps->host.cpu = cpu;

    if (!(data = cpuNodeData(cpu->arch))
        || cpuDecode(cpu, data, NULL, 0, NULL) < 0) {
        goto cleanup;
    }

 cleanup:
    cpuDataFree(data);
    return caps;

 error:
    virObjectUnref(caps);
    goto cleanup;
}

static void vzDriverDispose(void * obj)
{
    vzDriverPtr driver = obj;

    if (driver->server) {
        prlsdkUnsubscribeFromPCSEvents(driver);
        prlsdkDisconnect(driver);
    }

    virObjectUnref(driver->domains);
    virObjectUnref(driver->caps);
    virObjectUnref(driver->xmlopt);
    virObjectEventStateFree(driver->domainEventState);
    virSysinfoDefFree(driver->hostsysinfo);
}

static int vzDriverOnceInit(void)
{
    if (!(vzDriverClass = virClassNew(virClassForObjectLockable(),
                                      "vzDriver",
                                      sizeof(vzDriver),
                                      vzDriverDispose)))
        return -1;

    return 0;
}
VIR_ONCE_GLOBAL_INIT(vzDriver)

vzDriverPtr
vzGetDriverConnection(void)
{
    virMutexLock(&vz_driver_lock);
    if (!vz_driver)
        vz_driver = vzDriverObjNew();
    virObjectRef(vz_driver);
    virMutexUnlock(&vz_driver_lock);

    return vz_driver;
}

void
vzDestroyDriverConnection(void)
{

    vzDriverPtr driver;
    vzConnPtr privconn_list;

    virMutexLock(&vz_driver_lock);
    driver = vz_driver;
    vz_driver = NULL;

    privconn_list = vz_conn_list;
    vz_conn_list = NULL;

    virMutexUnlock(&vz_driver_lock);

    while (privconn_list) {
        vzConnPtr privconn = privconn_list;
        privconn_list = privconn->next;
        virConnectCloseCallbackDataCall(privconn->closeCallback,
                                        VIR_CONNECT_CLOSE_REASON_EOF);
    }
    virObjectUnref(driver);
}

static char *
vzConnectGetCapabilities(virConnectPtr conn)
{
    vzConnPtr privconn = conn->privateData;
    char *xml;

    xml = virCapabilitiesFormatXML(privconn->driver->caps);
    return xml;
}
static int
vzDomainDefAddDefaultInputDevices(virDomainDefPtr def)
{
    if (def->ngraphics == 0)
        return 0;

    int bus = IS_CT(def) ? VIR_DOMAIN_INPUT_BUS_PARALLELS :
                           VIR_DOMAIN_INPUT_BUS_PS2;

    if (virDomainDefMaybeAddInput(def,
                                  VIR_DOMAIN_INPUT_TYPE_MOUSE,
                                  bus) < 0)
        return -1;

    if (virDomainDefMaybeAddInput(def,
                                  VIR_DOMAIN_INPUT_TYPE_KBD,
                                  bus) < 0)
        return -1;

    return 0;
}

static int
vzDomainDefPostParse(virDomainDefPtr def,
                     virCapsPtr caps ATTRIBUTE_UNUSED,
                     unsigned int parseFlags ATTRIBUTE_UNUSED,
                     void *opaque)
{
    if (vzDomainDefAddDefaultInputDevices(def) < 0)
        return -1;

    if (vzCheckUnsupportedDisks(def, opaque) < 0)
        return -1;

    if (vzCheckUnsupportedControllers(def, opaque) < 0)
        return -1;

    return 0;
}

static int
vzDomainDeviceDefPostParse(virDomainDeviceDefPtr dev,
                           const virDomainDef *def,
                           virCapsPtr caps ATTRIBUTE_UNUSED,
                           unsigned int parseFlags ATTRIBUTE_UNUSED,
                           void *opaque ATTRIBUTE_UNUSED)
{
    int ret = -1;

    if (dev->type == VIR_DOMAIN_DEVICE_NET &&
        (dev->data.net->type == VIR_DOMAIN_NET_TYPE_NETWORK ||
         dev->data.net->type == VIR_DOMAIN_NET_TYPE_BRIDGE) &&
        !dev->data.net->model &&
        def->os.type == VIR_DOMAIN_OSTYPE_HVM &&
        VIR_STRDUP(dev->data.net->model, "e1000") < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    return ret;
}


static virDomainDefParserConfig vzDomainDefParserConfig = {
    .macPrefix = {0x42, 0x1C, 0x00},
    .devicesPostParseCallback = vzDomainDeviceDefPostParse,
    .domainPostParseCallback = vzDomainDefPostParse,
};

static vzDriverPtr
vzDriverObjNew(void)
{
    vzDriverPtr driver;

    if (vzDriverInitialize() < 0)
        return NULL;

    if (!(driver = virObjectLockableNew(vzDriverClass)))
        return NULL;

    vzDomainDefParserConfig.priv = &driver->vzCaps;

    if (!(driver->caps = vzBuildCapabilities()) ||
        !(driver->xmlopt = virDomainXMLOptionNew(&vzDomainDefParserConfig,
                                                   NULL, NULL)) ||
        !(driver->domains = virDomainObjListNew()) ||
        !(driver->domainEventState = virObjectEventStateNew()) ||
        (vzInitVersion(driver) < 0) ||
        (prlsdkConnect(driver) < 0) ||
        (prlsdkSubscribeToPCSEvents(driver) < 0)
        ) {
        virObjectUnref(driver);
        return NULL;
    }

    driver->hostsysinfo = virSysinfoRead();
    ignore_value(prlsdkLoadDomains(driver));
    return driver;
}

static virDrvOpenStatus
vzConnectOpen(virConnectPtr conn,
              virConnectAuthPtr auth ATTRIBUTE_UNUSED,
              unsigned int flags)
{
    vzDriverPtr driver = NULL;
    vzConnPtr privconn = NULL;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (!conn->uri)
        return VIR_DRV_OPEN_DECLINED;

    if (!conn->uri->scheme)
        return VIR_DRV_OPEN_DECLINED;

    if (STRNEQ(conn->uri->scheme, "vz") &&
        STRNEQ(conn->uri->scheme, "parallels"))
        return VIR_DRV_OPEN_DECLINED;

    if (STREQ(conn->uri->scheme, "vz") && STRNEQ(conn->driver->name, "vz"))
        return VIR_DRV_OPEN_DECLINED;

    if (STREQ(conn->uri->scheme, "parallels") && STRNEQ(conn->driver->name, "Parallels"))
        return VIR_DRV_OPEN_DECLINED;

    /* Remote driver should handle these. */
    if (conn->uri->server)
        return VIR_DRV_OPEN_DECLINED;

    /* From this point on, the connection is for us. */
    if (STRNEQ_NULLABLE(conn->uri->path, "/system")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected Virtuozzo URI path '%s', try vz:///system"),
                       conn->uri->path);
        return VIR_DRV_OPEN_ERROR;
    }

    if (!(driver = vzGetDriverConnection()))
        return VIR_DRV_OPEN_ERROR;

    if (VIR_ALLOC(privconn) < 0)
        goto error;

    conn->privateData = privconn;
    privconn->driver = driver;

    if (!(privconn->closeCallback = virNewConnectCloseCallbackData()))
        goto error;

    virMutexLock(&vz_driver_lock);
    privconn->next = vz_conn_list;
    vz_conn_list = privconn;
    virMutexUnlock(&vz_driver_lock);

    return VIR_DRV_OPEN_SUCCESS;

 error:

    conn->privateData = NULL;
    virObjectUnref(driver);
    VIR_FREE(privconn);
    return VIR_DRV_OPEN_ERROR;
}

static int
vzConnectClose(virConnectPtr conn)
{
    vzConnPtr curr, *prev = &vz_conn_list;
    vzConnPtr privconn = conn->privateData;

    if (!privconn)
        return 0;

    virMutexLock(&vz_driver_lock);
    for (curr = vz_conn_list; curr; prev = &curr->next, curr = curr->next) {
        if (curr == privconn) {
            *prev = curr->next;
            break;
        }
    }

    virMutexUnlock(&vz_driver_lock);

    virObjectUnref(privconn->closeCallback);
    virObjectUnref(privconn->driver);
    VIR_FREE(privconn);
    conn->privateData = NULL;
    return 0;
}

static int
vzConnectGetVersion(virConnectPtr conn, unsigned long *hvVer)
{
    vzConnPtr privconn = conn->privateData;
    *hvVer = privconn->driver->vzVersion;
    return 0;
}


static char *vzConnectGetHostname(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return virGetHostname();
}

static char *
vzConnectGetSysinfo(virConnectPtr conn, unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    vzDriverPtr driver = privconn->driver;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, NULL);

    if (!driver->hostsysinfo) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Host SMBIOS information is not available"));
        return NULL;
    }

    if (virSysinfoFormat(&buf, driver->hostsysinfo) < 0)
        return NULL;
    if (virBufferCheckError(&buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}

static int
vzConnectListDomains(virConnectPtr conn, int *ids, int maxids)
{
    vzConnPtr privconn = conn->privateData;
    int n;

    n = virDomainObjListGetActiveIDs(privconn->driver->domains, ids, maxids,
                                     NULL, NULL);

    return n;
}

static int
vzConnectNumOfDomains(virConnectPtr conn)
{
    vzConnPtr privconn = conn->privateData;
    int count;

    count = virDomainObjListNumOfDomains(privconn->driver->domains, true,
                                         NULL, NULL);

    return count;
}

static int
vzConnectListDefinedDomains(virConnectPtr conn, char **const names, int maxnames)
{
    vzConnPtr privconn = conn->privateData;
    int n;

    memset(names, 0, sizeof(*names) * maxnames);
    n = virDomainObjListGetInactiveNames(privconn->driver->domains, names,
                                         maxnames, NULL, NULL);

    return n;
}

static int
vzConnectNumOfDefinedDomains(virConnectPtr conn)
{
    vzConnPtr privconn = conn->privateData;
    int count;

    count = virDomainObjListNumOfDomains(privconn->driver->domains, false,
                                         NULL, NULL);
    return count;
}

static int
vzConnectListAllDomains(virConnectPtr conn,
                        virDomainPtr **domains,
                        unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);
    ret = virDomainObjListExport(privconn->driver->domains, conn, domains,
                                 NULL, flags);

    return ret;
}

static virDomainPtr
vzDomainLookupByID(virConnectPtr conn, int id)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr ret = NULL;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByID(privconn->driver->domains, id);

    if (dom == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    ret = virGetDomain(conn, dom->def->name, dom->def->uuid);
    if (ret)
        ret->id = dom->def->id;

 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return ret;
}

static virDomainPtr
vzDomainLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr ret = NULL;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByUUID(privconn->driver->domains, uuid);

    if (dom == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    ret = virGetDomain(conn, dom->def->name, dom->def->uuid);
    if (ret)
        ret->id = dom->def->id;

 cleanup:
    if (dom)
        virObjectUnlock(dom);
    return ret;
}

static virDomainPtr
vzDomainLookupByName(virConnectPtr conn, const char *name)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr ret = NULL;
    virDomainObjPtr dom;

    dom = virDomainObjListFindByName(privconn->driver->domains, name);

    if (dom == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), name);
        goto cleanup;
    }

    ret = virGetDomain(conn, dom->def->name, dom->def->uuid);
    if (ret)
        ret->id = dom->def->id;

 cleanup:
    virDomainObjEndAPI(&dom);
    return ret;
}

static int
vzDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
    virDomainObjPtr privdom;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomainRef(domain)))
        goto cleanup;

    info->state = virDomainObjGetState(privdom, NULL);
    info->memory = privdom->def->mem.cur_balloon;
    info->maxMem = virDomainDefGetMemoryActual(privdom->def);
    info->nrVirtCpu = virDomainDefGetVcpus(privdom->def);
    info->cpuTime = 0;

    if (virDomainObjIsActive(privdom)) {
        unsigned long long vtime;
        size_t i;

        for (i = 0; i < virDomainDefGetVcpus(privdom->def); ++i) {
            if (prlsdkGetVcpuStats(privdom, i, &vtime) < 0) {
                virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                               _("cannot read cputime for domain"));
                goto cleanup;
            }
            info->cpuTime += vtime;
        }
    }
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&privdom);
    return ret;
}

static char *
vzDomainGetOSType(virDomainPtr domain)
{
    virDomainObjPtr privdom;

    char *ret = NULL;

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    ignore_value(VIR_STRDUP(ret, virDomainOSTypeToString(privdom->def->os.type)));

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int
vzDomainIsPersistent(virDomainPtr domain)
{
    virDomainObjPtr privdom;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    ret = 1;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int
vzDomainGetState(virDomainPtr domain,
                 int *state, int *reason, unsigned int flags)
{
    virDomainObjPtr privdom;
    int ret = -1;
    virCheckFlags(0, -1);

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    *state = virDomainObjGetState(privdom, reason);
    ret = 0;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static char *
vzDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    vzConnPtr privconn = domain->conn->privateData;
    virDomainDefPtr def;
    virDomainObjPtr privdom;
    char *ret = NULL;

    /* Flags checked by virDomainDefFormat */

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    def = (flags & VIR_DOMAIN_XML_INACTIVE) &&
        privdom->newDef ? privdom->newDef : privdom->def;

    ret = virDomainDefFormat(def, privconn->driver->caps, flags);

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int
vzDomainGetAutostart(virDomainPtr domain, int *autostart)
{
    virDomainObjPtr privdom;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomain(domain)))
        goto cleanup;

    *autostart = privdom->autostart;
    ret = 0;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static virDomainPtr
vzDomainDefineXMLFlags(virConnectPtr conn, const char *xml, unsigned int flags)
{
    vzConnPtr privconn = conn->privateData;
    virDomainPtr retdom = NULL;
    virDomainDefPtr def;
    virDomainObjPtr olddom = NULL;
    virDomainObjPtr newdom = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE;
    vzDriverPtr driver = privconn->driver;

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_DEFINE_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE;

    virObjectLock(driver);
    if ((def = virDomainDefParseString(xml, driver->caps, driver->xmlopt,
                                       parse_flags)) == NULL)
        goto cleanup;

    olddom = virDomainObjListFindByUUID(driver->domains, def->uuid);
    if (olddom == NULL) {
        virResetLastError();
        newdom = vzNewDomain(driver, def->name, def->uuid);
        if (!newdom)
            goto cleanup;
        if (def->os.type == VIR_DOMAIN_OSTYPE_HVM) {
            if (prlsdkCreateVm(driver, def))
                goto cleanup;
        } else if (def->os.type == VIR_DOMAIN_OSTYPE_EXE) {
            if (prlsdkCreateCt(driver, def))
                goto cleanup;
        } else {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("Unsupported OS type: %s"),
                           virDomainOSTypeToString(def->os.type));
            goto cleanup;
        }

        if (prlsdkLoadDomain(driver, newdom))
            goto cleanup;
    } else {
        int state, reason;

        state = virDomainObjGetState(olddom, &reason);

        if (state == VIR_DOMAIN_SHUTOFF &&
            reason == VIR_DOMAIN_SHUTOFF_SAVED) {

            /* PCS doesn't store domain config in managed save state file.
             * It's forbidden to change config for VMs in this state.
             * It's possible to change config for containers, but after
             * restoring domain will have that new config, not a config,
             * which domain had at the moment of virDomainManagedSave.
             *
             * So forbid this operation, if config is changed. If it's
             * not changed - just do nothing. */

            if (!virDomainDefCheckABIStability(olddom->def, def)) {
                virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                               _("Can't change domain configuration "
                                 "in managed save state"));
                goto cleanup;
            }
        } else {
            if (prlsdkApplyConfig(driver, olddom, def))
                goto cleanup;

            if (prlsdkUpdateDomain(driver, olddom))
                goto cleanup;
        }
    }

    retdom = virGetDomain(conn, def->name, def->uuid);
    if (retdom)
        retdom->id = def->id;

 cleanup:
    if (olddom)
        virObjectUnlock(olddom);
    if (newdom) {
        if (!retdom)
             virDomainObjListRemove(driver->domains, newdom);
        else
             virObjectUnlock(newdom);
    }
    virDomainDefFree(def);
    virObjectUnlock(driver);
    return retdom;
}

static virDomainPtr
vzDomainDefineXML(virConnectPtr conn, const char *xml)
{
    return vzDomainDefineXMLFlags(conn, xml, 0);
}


static int
vzNodeGetInfo(virConnectPtr conn ATTRIBUTE_UNUSED,
              virNodeInfoPtr nodeinfo)
{
    return nodeGetInfo(NULL, nodeinfo);
}

static int vzConnectIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Encryption is not relevant / applicable to way we talk to PCS */
    return 0;
}

static int vzConnectIsSecure(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* We run CLI tools directly so this is secure */
    return 1;
}

static int vzConnectIsAlive(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 1;
}


static char *
vzConnectBaselineCPU(virConnectPtr conn ATTRIBUTE_UNUSED,
                     const char **xmlCPUs,
                     unsigned int ncpus,
                     unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES, NULL);

    return cpuBaselineXML(xmlCPUs, ncpus, NULL, 0, flags);
}


static int
vzDomainGetVcpus(virDomainPtr domain,
                 virVcpuInfoPtr info,
                 int maxinfo,
                 unsigned char *cpumaps,
                 int maplen)
{
    virDomainObjPtr privdom = NULL;
    size_t i;
    int ret = -1;

    if (!(privdom = vzDomObjFromDomainRef(domain)))
        goto cleanup;

    if (!virDomainObjIsActive(privdom)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s",
                       _("cannot list vcpu pinning for an inactive domain"));
        goto cleanup;
    }

    if (maxinfo >= 1) {
        if (info != NULL) {
            memset(info, 0, sizeof(*info) * maxinfo);
            for (i = 0; i < maxinfo; i++) {
                info[i].number = i;
                info[i].state = VIR_VCPU_RUNNING;
                if (prlsdkGetVcpuStats(privdom, i, &info[i].cpuTime) < 0)
                    goto cleanup;
            }
        }
        if (cpumaps != NULL) {
            memset(cpumaps, 0, maplen * maxinfo);
            for (i = 0; i < maxinfo; i++)
                virBitmapToDataBuf(privdom->def->cpumask,
                                   VIR_GET_CPUMAP(cpumaps, maplen, i),
                                   maplen);
        }
    }
    ret = maxinfo;

 cleanup:
    if (privdom)
        virDomainObjEndAPI(&privdom);
    return ret;
}


static int
vzNodeGetCPUMap(virConnectPtr conn ATTRIBUTE_UNUSED,
                unsigned char **cpumap,
                unsigned int *online,
                unsigned int flags)
{
    return nodeGetCPUMap(NULL, cpumap, online, flags);
}

static int
vzConnectDomainEventRegisterAny(virConnectPtr conn,
                                virDomainPtr domain,
                                int eventID,
                                virConnectDomainEventGenericCallback callback,
                                void *opaque,
                                virFreeCallback freecb)
{
    int ret = -1;
    vzConnPtr privconn = conn->privateData;
    if (virDomainEventStateRegisterID(conn,
                                      privconn->driver->domainEventState,
                                      domain, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;
    return ret;
}

static int
vzConnectDomainEventDeregisterAny(virConnectPtr conn,
                                  int callbackID)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;

    if (virObjectEventStateDeregisterID(conn,
                                        privconn->driver->domainEventState,
                                        callbackID) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    return ret;
}

static int vzDomainSuspend(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkPause);
}

static int vzDomainResume(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkResume);
}

static int vzDomainCreate(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkStart);
}

static int vzDomainDestroy(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkKill);
}

static int vzDomainShutdown(virDomainPtr domain)
{
    return prlsdkDomainChangeState(domain, prlsdkStop);
}

static int vzDomainReboot(virDomainPtr domain,
                          unsigned int flags)
{
    virCheckFlags(0, -1);
    return prlsdkDomainChangeState(domain, prlsdkRestart);
}

static int vzDomainIsActive(virDomainPtr domain)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    ret = virDomainObjIsActive(dom);
    virObjectUnlock(dom);

    return ret;
}

static int
vzDomainCreateWithFlags(virDomainPtr domain, unsigned int flags)
{
    /* we don't support any create flags */
    virCheckFlags(0, -1);

    return vzDomainCreate(domain);
}

static int
vzDomainUndefineFlags(virDomainPtr domain,
                      unsigned int flags)
{
    vzConnPtr privconn = domain->conn->privateData;
    virDomainObjPtr dom = NULL;
    int ret;

    virCheckFlags(VIR_DOMAIN_UNDEFINE_MANAGED_SAVE |
                  VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    ret = prlsdkUnregisterDomain(privconn->driver, dom, flags);
    if (ret)
        virObjectUnlock(dom);

    return ret;
}

static int
vzDomainUndefine(virDomainPtr domain)
{
    return vzDomainUndefineFlags(domain, 0);
}

static int
vzDomainHasManagedSaveImage(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom = NULL;
    int state, reason;
    int ret = 0;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    state = virDomainObjGetState(dom, &reason);
    if (state == VIR_DOMAIN_SHUTOFF && reason == VIR_DOMAIN_SHUTOFF_SAVED)
        ret = 1;
    virObjectUnlock(dom);

    return ret;
}

static int
vzDomainManagedSave(virDomainPtr domain, unsigned int flags)
{
    vzConnPtr privconn = domain->conn->privateData;
    virDomainObjPtr dom = NULL;
    int state, reason;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_SAVE_RUNNING |
                  VIR_DOMAIN_SAVE_PAUSED, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    state = virDomainObjGetState(dom, &reason);

    if (state == VIR_DOMAIN_RUNNING && (flags & VIR_DOMAIN_SAVE_PAUSED)) {
        ret = prlsdkDomainChangeStateLocked(privconn->driver, dom, prlsdkPause);
        if (ret)
            goto cleanup;
    }

    ret = prlsdkDomainChangeStateLocked(privconn->driver, dom, prlsdkSuspend);

 cleanup:
    virObjectUnlock(dom);
    return ret;
}

static int
vzDomainManagedSaveRemove(virDomainPtr domain, unsigned int flags)
{
    virDomainObjPtr dom = NULL;
    int state, reason;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    state = virDomainObjGetState(dom, &reason);

    if (!(state == VIR_DOMAIN_SHUTOFF && reason == VIR_DOMAIN_SHUTOFF_SAVED))
        goto cleanup;

    ret = prlsdkDomainManagedSaveRemove(dom);

 cleanup:
    virObjectUnlock(dom);
    return ret;
}

static int vzCheckConfigUpdateFlags(virDomainObjPtr dom, unsigned int *flags)
{
    if (virDomainObjUpdateModificationImpact(dom, flags) < 0)
        return -1;

    if (!(*flags & VIR_DOMAIN_AFFECT_CONFIG)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain config update needs VIR_DOMAIN_AFFECT_CONFIG "
                         "flag to be set"));
        return -1;
    }

    if (virDomainObjIsActive(dom) && !(*flags & VIR_DOMAIN_AFFECT_LIVE)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Updates on a running domain need "
                         "VIR_DOMAIN_AFFECT_LIVE flag"));
        return -1;
    }

    return 0;
}

static int vzDomainAttachDeviceFlags(virDomainPtr dom, const char *xml,
                                     unsigned int flags)
{
    int ret = -1;
    vzConnPtr privconn = dom->conn->privateData;
    virDomainDeviceDefPtr dev = NULL;
    virDomainObjPtr privdom = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(privdom = vzDomObjFromDomain(dom)))
        return -1;

    if (vzCheckConfigUpdateFlags(privdom, &flags) < 0)
        goto cleanup;

    dev = virDomainDeviceDefParse(xml, privdom->def, privconn->driver->caps,
                                  privconn->driver->xmlopt, VIR_DOMAIN_XML_INACTIVE);
    if (dev == NULL)
        goto cleanup;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        ret = prlsdkAttachVolume(privconn->driver, privdom, dev->data.disk);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("disk attach failed"));
            goto cleanup;
        }
        break;
    case VIR_DOMAIN_DEVICE_NET:
        ret = prlsdkAttachNet(privconn->driver, privdom, dev->data.net);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("network attach failed"));
            goto cleanup;
        }
        break;
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("device type '%s' cannot be attached"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    ret = 0;
 cleanup:
    virObjectUnlock(privdom);
    return ret;
}

static int vzDomainAttachDevice(virDomainPtr dom, const char *xml)
{
    return vzDomainAttachDeviceFlags(dom, xml,
                                     VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_AFFECT_LIVE);
}

static int vzDomainDetachDeviceFlags(virDomainPtr dom, const char *xml,
                                     unsigned int flags)
{
    int ret = -1;
    vzConnPtr privconn = dom->conn->privateData;
    virDomainDeviceDefPtr dev = NULL;
    virDomainObjPtr privdom = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    privdom = vzDomObjFromDomain(dom);
    if (privdom == NULL)
        return -1;

    if (vzCheckConfigUpdateFlags(privdom, &flags) < 0)
        goto cleanup;

    dev = virDomainDeviceDefParse(xml, privdom->def, privconn->driver->caps,
                                  privconn->driver->xmlopt, VIR_DOMAIN_XML_INACTIVE);
    if (dev == NULL)
        goto cleanup;

    switch (dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        ret = prlsdkDetachVolume(privdom, dev->data.disk);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("disk detach failed"));
            goto cleanup;
        }
        break;
    case VIR_DOMAIN_DEVICE_NET:
        ret = prlsdkDetachNet(privconn->driver, privdom, dev->data.net);
        if (ret) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("network detach failed"));
            goto cleanup;
        }
        break;
    default:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("device type '%s' cannot be detached"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    ret = 0;
 cleanup:
    virObjectUnlock(privdom);

    return ret;
}

static int vzDomainDetachDevice(virDomainPtr dom, const char *xml)
{
    return vzDomainDetachDeviceFlags(dom, xml,
                                     VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_AFFECT_LIVE);
}

static unsigned long long
vzDomainGetMaxMemory(virDomainPtr domain)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    ret = virDomainDefGetMemoryActual(dom->def);
    virObjectUnlock(dom);
    return ret;
}

static int
vzDomainBlockStats(virDomainPtr domain, const char *path,
                   virDomainBlockStatsPtr stats)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;
    size_t i;
    int idx;

    if (!(dom = vzDomObjFromDomainRef(domain)))
        return -1;

    if (*path) {
        if ((idx = virDomainDiskIndexByName(dom->def, path, false)) < 0) {
            virReportError(VIR_ERR_INVALID_ARG, _("invalid path: %s"), path);
            goto cleanup;
        }
        if (prlsdkGetBlockStats(dom, dom->def->disks[idx], stats) < 0)
            goto cleanup;
    } else {
        virDomainBlockStatsStruct s;

#define PARALLELS_ZERO_STATS(VAR, TYPE, NAME)      \
        stats->VAR = 0;

        PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_ZERO_STATS)

#undef PARALLELS_ZERO_STATS

        for (i = 0; i < dom->def->ndisks; i++) {
            if (prlsdkGetBlockStats(dom, dom->def->disks[i], &s) < 0)
                goto cleanup;

#define PARALLELS_SUM_STATS(VAR, TYPE, NAME)        \
    if (s.VAR != -1)                                \
        stats->VAR += s.VAR;

        PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_SUM_STATS)

#undef PARALLELS_SUM_STATS
        }
    }
    stats->errs = -1;
    ret = 0;

 cleanup:
    if (dom)
        virDomainObjEndAPI(&dom);

    return ret;
}

static int
vzDomainBlockStatsFlags(virDomainPtr domain,
                        const char *path,
                        virTypedParameterPtr params,
                        int *nparams,
                        unsigned int flags)
{
    virDomainBlockStatsStruct stats;
    int ret = -1;
    size_t i;

    virCheckFlags(VIR_TYPED_PARAM_STRING_OKAY, -1);
    /* We don't return strings, and thus trivially support this flag.  */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (vzDomainBlockStats(domain, path, &stats) < 0)
        goto cleanup;

    if (*nparams == 0) {
#define PARALLELS_COUNT_STATS(VAR, TYPE, NAME)       \
        if ((stats.VAR) != -1)                       \
            ++*nparams;

        PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_COUNT_STATS)

#undef PARALLELS_COUNT_STATS
        ret = 0;
        goto cleanup;
    }

    i = 0;
#define PARALLELS_BLOCK_STATS_ASSIGN_PARAM(VAR, TYPE, NAME)                    \
    if (i < *nparams && (stats.VAR) != -1) {                                   \
        if (virTypedParameterAssign(params + i, TYPE,                          \
                                    VIR_TYPED_PARAM_LLONG, (stats.VAR)) < 0)   \
            goto cleanup;                                                      \
        i++;                                                                   \
    }

    PARALLELS_BLOCK_STATS_FOREACH(PARALLELS_BLOCK_STATS_ASSIGN_PARAM)

#undef PARALLELS_BLOCK_STATS_ASSIGN_PARAM

    *nparams = i;
    ret = 0;

 cleanup:
    return ret;
}

static int
vzDomainInterfaceStats(virDomainPtr domain,
                         const char *path,
                         virDomainInterfaceStatsPtr stats)
{
    virDomainObjPtr dom = NULL;
    int ret;

    if (!(dom = vzDomObjFromDomainRef(domain)))
        return -1;

    ret = prlsdkGetNetStats(dom, path, stats);
    virDomainObjEndAPI(&dom);

    return ret;
}

static int
vzDomainMemoryStats(virDomainPtr domain,
                    virDomainMemoryStatPtr stats,
                    unsigned int nr_stats,
                    unsigned int flags)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    virCheckFlags(0, -1);
    if (!(dom = vzDomObjFromDomainRef(domain)))
        return -1;

    ret = prlsdkGetMemoryStats(dom, stats, nr_stats);
    virDomainObjEndAPI(&dom);

    return ret;
}

static int
vzDomainGetVcpusFlags(virDomainPtr dom,
                      unsigned int flags)
{
    virDomainObjPtr privdom = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);

    if (!(privdom = vzDomObjFromDomain(dom)))
        goto cleanup;

    if (flags & VIR_DOMAIN_VCPU_MAXIMUM)
        ret = virDomainDefGetVcpusMax(privdom->def);
    else
        ret = virDomainDefGetVcpus(privdom->def);

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);

    return ret;
}

static int vzDomainGetMaxVcpus(virDomainPtr dom)
{
    return vzDomainGetVcpusFlags(dom, (VIR_DOMAIN_AFFECT_LIVE |
                                       VIR_DOMAIN_VCPU_MAXIMUM));
}

static int vzDomainIsUpdated(virDomainPtr dom)
{
    virDomainObjPtr privdom;
    int ret = -1;

    /* As far as VZ domains are always updated (e.g. current==persistent),
     * we just check for domain existence */
    if (!(privdom = vzDomObjFromDomain(dom)))
        goto cleanup;

    ret = 0;

 cleanup:
    if (privdom)
        virObjectUnlock(privdom);
    return ret;
}

static int vzConnectGetMaxVcpus(virConnectPtr conn ATTRIBUTE_UNUSED,
                                const char *type)
{
    /* As far as we have no limitation for containers
     * we report maximum */
    if (type == NULL || STRCASEEQ(type, "vz") || STRCASEEQ(type, "parallels"))
        return 1028;

    virReportError(VIR_ERR_INVALID_ARG,
                   _("unknown type '%s'"), type);
    return -1;
}

static int
vzNodeGetCPUStats(virConnectPtr conn ATTRIBUTE_UNUSED,
                  int cpuNum,
                  virNodeCPUStatsPtr params,
                  int *nparams,
                  unsigned int flags)
{
    return nodeGetCPUStats(cpuNum, params, nparams, flags);
}

static int
vzNodeGetMemoryStats(virConnectPtr conn ATTRIBUTE_UNUSED,
                     int cellNum,
                     virNodeMemoryStatsPtr params,
                     int *nparams,
                     unsigned int flags)
{
    return nodeGetMemoryStats(NULL, cellNum, params, nparams, flags);
}

static int
vzNodeGetCellsFreeMemory(virConnectPtr conn ATTRIBUTE_UNUSED,
                         unsigned long long *freeMems,
                         int startCell,
                         int maxCells)
{
    return nodeGetCellsFreeMemory(freeMems, startCell, maxCells);
}

static unsigned long long
vzNodeGetFreeMemory(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    unsigned long long freeMem;
    if (nodeGetMemory(NULL, &freeMem) < 0)
        return 0;
    return freeMem;
}

static int
vzConnectRegisterCloseCallback(virConnectPtr conn,
                               virConnectCloseFunc cb,
                               void *opaque,
                               virFreeCallback freecb)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;

    if (virConnectCloseCallbackDataGetCallback(privconn->closeCallback) != NULL) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("A close callback is already registered"));
        goto cleanup;
    }

    virConnectCloseCallbackDataRegister(privconn->closeCallback, conn, cb,
                                        opaque, freecb);
    ret = 0;

 cleanup:

    return ret;
}

static int
vzConnectUnregisterCloseCallback(virConnectPtr conn, virConnectCloseFunc cb)
{
    vzConnPtr privconn = conn->privateData;
    int ret = -1;


    if (virConnectCloseCallbackDataGetCallback(privconn->closeCallback) != cb) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("A different callback was requested"));
        goto cleanup;
    }

    virConnectCloseCallbackDataUnregister(privconn->closeCallback, cb);
    ret = 0;

 cleanup:

    return ret;
}

static int vzDomainSetMemoryFlagsImpl(virDomainPtr domain, unsigned long memory,
                                      unsigned int flags, bool useflags)
{
    virDomainObjPtr dom = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(dom = vzDomObjFromDomain(domain)))
        return -1;

    if (useflags && vzCheckConfigUpdateFlags(dom, &flags) < 0)
        goto cleanup;

    ret = prlsdkSetMemsize(dom, memory >> 10);

 cleanup:

    virObjectUnlock(dom);
    return ret;
}

static int vzDomainSetMemoryFlags(virDomainPtr domain, unsigned long memory,
                                  unsigned int flags)
{
    return vzDomainSetMemoryFlagsImpl(domain, memory, flags, true);
}

static int vzDomainSetMemory(virDomainPtr domain, unsigned long memory)
{
    return vzDomainSetMemoryFlagsImpl(domain, memory, 0, false);
}

static virHypervisorDriver vzHypervisorDriver = {
    .name = "vz",
    .connectOpen = vzConnectOpen,            /* 0.10.0 */
    .connectClose = vzConnectClose,          /* 0.10.0 */
    .connectGetVersion = vzConnectGetVersion,   /* 0.10.0 */
    .connectGetHostname = vzConnectGetHostname,      /* 0.10.0 */
    .connectGetSysinfo = vzConnectGetSysinfo, /* 1.3.4 */
    .connectGetMaxVcpus = vzConnectGetMaxVcpus, /* 1.2.21 */
    .nodeGetInfo = vzNodeGetInfo,      /* 0.10.0 */
    .nodeGetCPUStats = vzNodeGetCPUStats,      /* 1.2.21 */
    .nodeGetMemoryStats = vzNodeGetMemoryStats, /* 1.2.21 */
    .nodeGetCellsFreeMemory = vzNodeGetCellsFreeMemory, /* 1.2.21 */
    .nodeGetFreeMemory = vzNodeGetFreeMemory, /* 1.2.21 */
    .connectGetCapabilities = vzConnectGetCapabilities,      /* 0.10.0 */
    .connectBaselineCPU = vzConnectBaselineCPU, /* 1.2.6 */
    .connectListDomains = vzConnectListDomains,      /* 0.10.0 */
    .connectNumOfDomains = vzConnectNumOfDomains,    /* 0.10.0 */
    .connectListDefinedDomains = vzConnectListDefinedDomains,        /* 0.10.0 */
    .connectNumOfDefinedDomains = vzConnectNumOfDefinedDomains,      /* 0.10.0 */
    .connectListAllDomains = vzConnectListAllDomains, /* 0.10.0 */
    .domainLookupByID = vzDomainLookupByID,    /* 0.10.0 */
    .domainLookupByUUID = vzDomainLookupByUUID,        /* 0.10.0 */
    .domainLookupByName = vzDomainLookupByName,        /* 0.10.0 */
    .domainGetOSType = vzDomainGetOSType,    /* 0.10.0 */
    .domainGetInfo = vzDomainGetInfo,  /* 0.10.0 */
    .domainGetState = vzDomainGetState,        /* 0.10.0 */
    .domainGetXMLDesc = vzDomainGetXMLDesc,    /* 0.10.0 */
    .domainIsPersistent = vzDomainIsPersistent,        /* 0.10.0 */
    .domainGetAutostart = vzDomainGetAutostart,        /* 0.10.0 */
    .domainGetVcpus = vzDomainGetVcpus, /* 1.2.6 */
    .domainSuspend = vzDomainSuspend,    /* 0.10.0 */
    .domainResume = vzDomainResume,    /* 0.10.0 */
    .domainDestroy = vzDomainDestroy,  /* 0.10.0 */
    .domainShutdown = vzDomainShutdown, /* 0.10.0 */
    .domainCreate = vzDomainCreate,    /* 0.10.0 */
    .domainCreateWithFlags = vzDomainCreateWithFlags, /* 1.2.10 */
    .domainReboot = vzDomainReboot, /* 1.3.0 */
    .domainDefineXML = vzDomainDefineXML,      /* 0.10.0 */
    .domainDefineXMLFlags = vzDomainDefineXMLFlags, /* 1.2.12 */
    .domainUndefine = vzDomainUndefine, /* 1.2.10 */
    .domainUndefineFlags = vzDomainUndefineFlags, /* 1.2.10 */
    .domainAttachDevice = vzDomainAttachDevice, /* 1.2.15 */
    .domainAttachDeviceFlags = vzDomainAttachDeviceFlags, /* 1.2.15 */
    .domainDetachDevice = vzDomainDetachDevice, /* 1.2.15 */
    .domainDetachDeviceFlags = vzDomainDetachDeviceFlags, /* 1.2.15 */
    .domainIsActive = vzDomainIsActive, /* 1.2.10 */
    .domainIsUpdated = vzDomainIsUpdated,     /* 1.2.21 */
    .domainGetVcpusFlags = vzDomainGetVcpusFlags, /* 1.2.21 */
    .domainGetMaxVcpus = vzDomainGetMaxVcpus, /* 1.2.21 */
    .connectDomainEventRegisterAny = vzConnectDomainEventRegisterAny, /* 1.2.10 */
    .connectDomainEventDeregisterAny = vzConnectDomainEventDeregisterAny, /* 1.2.10 */
    .nodeGetCPUMap = vzNodeGetCPUMap, /* 1.2.8 */
    .connectIsEncrypted = vzConnectIsEncrypted, /* 1.2.5 */
    .connectIsSecure = vzConnectIsSecure, /* 1.2.5 */
    .connectIsAlive = vzConnectIsAlive, /* 1.2.5 */
    .domainHasManagedSaveImage = vzDomainHasManagedSaveImage, /* 1.2.13 */
    .domainManagedSave = vzDomainManagedSave, /* 1.2.14 */
    .domainManagedSaveRemove = vzDomainManagedSaveRemove, /* 1.2.14 */
    .domainGetMaxMemory = vzDomainGetMaxMemory, /* 1.2.15 */
    .domainBlockStats = vzDomainBlockStats, /* 1.2.17 */
    .domainBlockStatsFlags = vzDomainBlockStatsFlags, /* 1.2.17 */
    .domainInterfaceStats = vzDomainInterfaceStats, /* 1.2.17 */
    .domainMemoryStats = vzDomainMemoryStats, /* 1.2.17 */
    .connectRegisterCloseCallback = vzConnectRegisterCloseCallback, /* 1.3.2 */
    .connectUnregisterCloseCallback = vzConnectUnregisterCloseCallback, /* 1.3.2 */
    .domainSetMemoryFlags = vzDomainSetMemoryFlags, /* 1.3.4 */
    .domainSetMemory = vzDomainSetMemory, /* 1.3.4 */
};

static virConnectDriver vzConnectDriver = {
    .hypervisorDriver = &vzHypervisorDriver,
};

static int
vzStateCleanup(void)
{
    virObjectUnref(vz_driver);
    vz_driver = NULL;
    virMutexDestroy(&vz_driver_lock);
    prlsdkDeinit();
    return 0;
}

static int
vzStateInitialize(bool privileged ATTRIBUTE_UNUSED,
                  virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                  void *opaque ATTRIBUTE_UNUSED)
{
    if (prlsdkInit() < 0) {
        VIR_DEBUG("%s", _("Can't initialize Parallels SDK"));
        return -1;
    }

   if (virMutexInit(&vz_driver_lock) < 0)
        goto error;

    /* Failing to create driver here is not fatal and only means
     * that next driver client will try once more when connecting */
    vz_driver = vzDriverObjNew();
    return 0;

 error:
    vzStateCleanup();
    return -1;
}

static virStateDriver vzStateDriver = {
    .name = "vz",
    .stateInitialize = vzStateInitialize,
    .stateCleanup = vzStateCleanup,
};

/* Parallels domain type backward compatibility*/
static virHypervisorDriver parallelsHypervisorDriver;
static virConnectDriver parallelsConnectDriver;

/**
 * vzRegister:
 *
 * Registers the vz driver
 */
int
vzRegister(void)
{
    char *prlctl_path;

    prlctl_path = virFindFileInPath(PRLCTL);
    if (!prlctl_path) {
        VIR_DEBUG("%s", _("Can't find prlctl command in the PATH env"));
        return 0;
    }

    VIR_FREE(prlctl_path);

    /* Backward compatibility with Parallels domain type */
    parallelsHypervisorDriver = vzHypervisorDriver;
    parallelsHypervisorDriver.name = "Parallels";
    parallelsConnectDriver = vzConnectDriver;
    parallelsConnectDriver.hypervisorDriver = &parallelsHypervisorDriver;
    if (virRegisterConnectDriver(&parallelsConnectDriver, true) < 0)
        return -1;

    if (virRegisterConnectDriver(&vzConnectDriver, true) < 0)
        return -1;

    if (virRegisterStateDriver(&vzStateDriver) < 0)
        return -1;

    return 0;
}
