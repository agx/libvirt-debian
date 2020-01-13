/*
 * remote_daemon_dispatch.c: handlers for RPC method calls
 *
 * Copyright (C) 2007-2018 Red Hat, Inc.
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

#include "virerror.h"

#include "remote_daemon_dispatch.h"
#include "remote_daemon.h"
#include "libvirt_internal.h"
#include "datatypes.h"
#include "viralloc.h"
#include "virlog.h"
#include "remote_daemon_stream.h"
#include "viruuid.h"
#include "vircommand.h"
#include "intprops.h"
#include "virnetserverservice.h"
#include "virnetserver.h"
#include "virfile.h"
#include "virtypedparam.h"
#include "virdbus.h"
#include "virprocess.h"
#include "remote_protocol.h"
#include "qemu_protocol.h"
#include "lxc_protocol.h"
#include "virstring.h"
#include "object_event.h"
#include "domain_conf.h"
#include "network_conf.h"
#include "virprobe.h"
#include "viraccessapicheck.h"
#include "viraccessapicheckqemu.h"
#include "virpolkit.h"
#include "virthreadjob.h"
#include "configmake.h"
#include "access/viraccessapicheck.h"

#define VIR_FROM_THIS VIR_FROM_RPC

VIR_LOG_INIT("daemon.remote");

#if SIZEOF_LONG < 8
# define HYPER_TO_TYPE(_type, _to, _from) \
    do { \
        if ((_from) != (_type)(_from)) { \
            virReportError(VIR_ERR_OVERFLOW, \
                           _("conversion from hyper to %s overflowed"), \
                           #_type); \
            goto cleanup; \
        } \
        (_to) = (_from); \
    } while (0)

# define HYPER_TO_LONG(_to, _from) HYPER_TO_TYPE(long, _to, _from)
# define HYPER_TO_ULONG(_to, _from) HYPER_TO_TYPE(unsigned long, _to, _from)
#else
# define HYPER_TO_LONG(_to, _from) (_to) = (_from)
# define HYPER_TO_ULONG(_to, _from) (_to) = (_from)
#endif

struct daemonClientEventCallback {
    virNetServerClientPtr client;
    virNetServerProgramPtr program;
    int eventID;
    int callbackID;
    bool legacy;
};

static virDomainPtr get_nonnull_domain(virConnectPtr conn, remote_nonnull_domain domain);
static virNetworkPtr get_nonnull_network(virConnectPtr conn, remote_nonnull_network network);
static virNetworkPortPtr get_nonnull_network_port(virConnectPtr conn, remote_nonnull_network_port port);
static virInterfacePtr get_nonnull_interface(virConnectPtr conn, remote_nonnull_interface iface);
static virStoragePoolPtr get_nonnull_storage_pool(virConnectPtr conn, remote_nonnull_storage_pool pool);
static virStorageVolPtr get_nonnull_storage_vol(virConnectPtr conn, remote_nonnull_storage_vol vol);
static virSecretPtr get_nonnull_secret(virConnectPtr conn, remote_nonnull_secret secret);
static virNWFilterPtr get_nonnull_nwfilter(virConnectPtr conn, remote_nonnull_nwfilter nwfilter);
static virNWFilterBindingPtr get_nonnull_nwfilter_binding(virConnectPtr conn, remote_nonnull_nwfilter_binding binding);
static virDomainCheckpointPtr get_nonnull_domain_checkpoint(virDomainPtr dom, remote_nonnull_domain_checkpoint checkpoint);
static virDomainSnapshotPtr get_nonnull_domain_snapshot(virDomainPtr dom, remote_nonnull_domain_snapshot snapshot);
static virNodeDevicePtr get_nonnull_node_device(virConnectPtr conn, remote_nonnull_node_device dev);
static void make_nonnull_domain(remote_nonnull_domain *dom_dst, virDomainPtr dom_src);
static void make_nonnull_network(remote_nonnull_network *net_dst, virNetworkPtr net_src);
static void make_nonnull_network_port(remote_nonnull_network_port *port_dst, virNetworkPortPtr port_src);
static void make_nonnull_interface(remote_nonnull_interface *interface_dst, virInterfacePtr interface_src);
static void make_nonnull_storage_pool(remote_nonnull_storage_pool *pool_dst, virStoragePoolPtr pool_src);
static void make_nonnull_storage_vol(remote_nonnull_storage_vol *vol_dst, virStorageVolPtr vol_src);
static void make_nonnull_node_device(remote_nonnull_node_device *dev_dst, virNodeDevicePtr dev_src);
static void make_nonnull_secret(remote_nonnull_secret *secret_dst, virSecretPtr secret_src);
static void make_nonnull_nwfilter(remote_nonnull_nwfilter *net_dst, virNWFilterPtr nwfilter_src);
static void make_nonnull_nwfilter_binding(remote_nonnull_nwfilter_binding *binding_dst, virNWFilterBindingPtr binding_src);
static void make_nonnull_domain_checkpoint(remote_nonnull_domain_checkpoint *checkpoint_dst, virDomainCheckpointPtr checkpoint_src);
static void make_nonnull_domain_snapshot(remote_nonnull_domain_snapshot *snapshot_dst, virDomainSnapshotPtr snapshot_src);

static int
remoteSerializeDomainDiskErrors(virDomainDiskErrorPtr errors,
                                int nerrors,
                                remote_domain_disk_error **ret_errors_val,
                                u_int *ret_errors_len);

static virConnectPtr
remoteGetHypervisorConn(virNetServerClientPtr client);
static virConnectPtr
remoteGetInterfaceConn(virNetServerClientPtr client);
static virConnectPtr
remoteGetNetworkConn(virNetServerClientPtr client);
static virConnectPtr
remoteGetNodeDevConn(virNetServerClientPtr client);
static virConnectPtr
remoteGetNWFilterConn(virNetServerClientPtr client);
static virConnectPtr
remoteGetSecretConn(virNetServerClientPtr client);
static virConnectPtr
remoteGetStorageConn(virNetServerClientPtr client);


#include "remote_daemon_dispatch_stubs.h"
#include "qemu_daemon_dispatch_stubs.h"
#include "lxc_daemon_dispatch_stubs.h"


/* Prototypes */
static void
remoteDispatchObjectEventSend(virNetServerClientPtr client,
                              virNetServerProgramPtr program,
                              int procnr,
                              xdrproc_t proc,
                              void *data);

static void
remoteEventCallbackFree(void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    if (!callback)
        return;
    virObjectUnref(callback->program);
    virObjectUnref(callback->client);
    VIR_FREE(callback);
}


static bool
remoteRelayDomainEventCheckACL(virNetServerClientPtr client,
                               virConnectPtr conn, virDomainPtr dom)
{
    virDomainDef def;
    g_autoptr(virIdentity) identity = NULL;
    bool ret = false;

    /* For now, we just create a virDomainDef with enough contents to
     * satisfy what viraccessdriverpolkit.c references.  This is a bit
     * fragile, but I don't know of anything better.  */
    memset(&def, 0, sizeof(def));
    def.name = dom->name;
    memcpy(def.uuid, dom->uuid, VIR_UUID_BUFLEN);

    if (!(identity = virNetServerClientGetIdentity(client)))
        goto cleanup;
    if (virIdentitySetCurrent(identity) < 0)
        goto cleanup;
    ret = virConnectDomainEventRegisterAnyCheckACL(conn, &def);

 cleanup:
    ignore_value(virIdentitySetCurrent(NULL));
    return ret;
}


static bool
remoteRelayNetworkEventCheckACL(virNetServerClientPtr client,
                                virConnectPtr conn, virNetworkPtr net)
{
    virNetworkDef def;
    g_autoptr(virIdentity) identity = NULL;
    bool ret = false;

    /* For now, we just create a virNetworkDef with enough contents to
     * satisfy what viraccessdriverpolkit.c references.  This is a bit
     * fragile, but I don't know of anything better.  */
    def.name = net->name;
    memcpy(def.uuid, net->uuid, VIR_UUID_BUFLEN);

    if (!(identity = virNetServerClientGetIdentity(client)))
        goto cleanup;
    if (virIdentitySetCurrent(identity) < 0)
        goto cleanup;
    ret = virConnectNetworkEventRegisterAnyCheckACL(conn, &def);

 cleanup:
    ignore_value(virIdentitySetCurrent(NULL));
    return ret;
}

static bool
remoteRelayStoragePoolEventCheckACL(virNetServerClientPtr client,
                                    virConnectPtr conn,
                                    virStoragePoolPtr pool)
{
    virStoragePoolDef def;
    g_autoptr(virIdentity) identity = NULL;
    bool ret = false;

    /* For now, we just create a virStoragePoolDef with enough contents to
     * satisfy what viraccessdriverpolkit.c references.  This is a bit
     * fragile, but I don't know of anything better.  */
    def.name = pool->name;
    memcpy(def.uuid, pool->uuid, VIR_UUID_BUFLEN);

    if (!(identity = virNetServerClientGetIdentity(client)))
        goto cleanup;
    if (virIdentitySetCurrent(identity) < 0)
        goto cleanup;
    ret = virConnectStoragePoolEventRegisterAnyCheckACL(conn, &def);

 cleanup:
    ignore_value(virIdentitySetCurrent(NULL));
    return ret;
}

static bool
remoteRelayNodeDeviceEventCheckACL(virNetServerClientPtr client,
                                   virConnectPtr conn,
                                   virNodeDevicePtr dev)
{
    virNodeDeviceDef def;
    g_autoptr(virIdentity) identity = NULL;
    bool ret = false;

    /* For now, we just create a virNodeDeviceDef with enough contents to
     * satisfy what viraccessdriverpolkit.c references.  This is a bit
     * fragile, but I don't know of anything better.  */
    def.name = dev->name;

    if (!(identity = virNetServerClientGetIdentity(client)))
        goto cleanup;
    if (virIdentitySetCurrent(identity) < 0)
        goto cleanup;
    ret = virConnectNodeDeviceEventRegisterAnyCheckACL(conn, &def);

 cleanup:
    ignore_value(virIdentitySetCurrent(NULL));
    return ret;
}

static bool
remoteRelaySecretEventCheckACL(virNetServerClientPtr client,
                               virConnectPtr conn,
                               virSecretPtr secret)
{
    virSecretDef def;
    g_autoptr(virIdentity) identity = NULL;
    bool ret = false;

    /* For now, we just create a virSecretDef with enough contents to
     * satisfy what viraccessdriverpolkit.c references.  This is a bit
     * fragile, but I don't know of anything better.  */
    memcpy(def.uuid, secret->uuid, VIR_UUID_BUFLEN);
    def.usage_type = secret->usageType;
    def.usage_id = secret->usageID;

    if (!(identity = virNetServerClientGetIdentity(client)))
        goto cleanup;
    if (virIdentitySetCurrent(identity) < 0)
        goto cleanup;
    ret = virConnectSecretEventRegisterAnyCheckACL(conn, &def);

 cleanup:
    ignore_value(virIdentitySetCurrent(NULL));
    return ret;
}

static bool
remoteRelayDomainQemuMonitorEventCheckACL(virNetServerClientPtr client,
                                          virConnectPtr conn, virDomainPtr dom)
{
    virDomainDef def;
    g_autoptr(virIdentity) identity = NULL;
    bool ret = false;

    /* For now, we just create a virDomainDef with enough contents to
     * satisfy what viraccessdriverpolkit.c references.  This is a bit
     * fragile, but I don't know of anything better.  */
    def.name = dom->name;
    memcpy(def.uuid, dom->uuid, VIR_UUID_BUFLEN);

    if (!(identity = virNetServerClientGetIdentity(client)))
        goto cleanup;
    if (virIdentitySetCurrent(identity) < 0)
        goto cleanup;
    ret = virConnectDomainQemuMonitorEventRegisterCheckACL(conn, &def);

 cleanup:
    ignore_value(virIdentitySetCurrent(NULL));
    return ret;
}


static int
remoteRelayDomainEventLifecycle(virConnectPtr conn,
                                virDomainPtr dom,
                                int event,
                                int detail,
                                void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_lifecycle_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain lifecycle event %d %d, callback %d legacy %d",
              event, detail, callback->callbackID, callback->legacy);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);
    data.event = event;
    data.detail = detail;

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_LIFECYCLE,
                                      (xdrproc_t)xdr_remote_domain_event_lifecycle_msg,
                                      &data);
    } else {
        remote_domain_event_callback_lifecycle_msg msg = { callback->callbackID,
                                                           data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_LIFECYCLE,
                                      (xdrproc_t)xdr_remote_domain_event_callback_lifecycle_msg,
                                      &msg);
    }

    return 0;
}

static int
remoteRelayDomainEventReboot(virConnectPtr conn,
                             virDomainPtr dom,
                             void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_reboot_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain reboot event %s %d, callback %d legacy %d",
              dom->name, dom->id, callback->callbackID, callback->legacy);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_REBOOT,
                                      (xdrproc_t)xdr_remote_domain_event_reboot_msg, &data);
    } else {
        remote_domain_event_callback_reboot_msg msg = { callback->callbackID,
                                                        data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_REBOOT,
                                      (xdrproc_t)xdr_remote_domain_event_callback_reboot_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventRTCChange(virConnectPtr conn,
                                virDomainPtr dom,
                                long long offset,
                                void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_rtc_change_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain rtc change event %s %d %lld, callback %d legacy %d",
              dom->name, dom->id, offset,
              callback->callbackID, callback->legacy);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);
    data.offset = offset;

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_RTC_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_rtc_change_msg, &data);
    } else {
        remote_domain_event_callback_rtc_change_msg msg = { callback->callbackID,
                                                            data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_RTC_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_callback_rtc_change_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventWatchdog(virConnectPtr conn,
                               virDomainPtr dom,
                               int action,
                               void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_watchdog_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain watchdog event %s %d %d, callback %d",
              dom->name, dom->id, action, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);
    data.action = action;

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_WATCHDOG,
                                      (xdrproc_t)xdr_remote_domain_event_watchdog_msg, &data);
    } else {
        remote_domain_event_callback_watchdog_msg msg = { callback->callbackID,
                                                          data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_WATCHDOG,
                                      (xdrproc_t)xdr_remote_domain_event_callback_watchdog_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventIOError(virConnectPtr conn,
                              virDomainPtr dom,
                              const char *srcPath,
                              const char *devAlias,
                              int action,
                              void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_io_error_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain io error %s %d %s %s %d, callback %d",
              dom->name, dom->id, srcPath, devAlias, action,
              callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.srcPath = g_strdup(srcPath);
    data.devAlias = g_strdup(devAlias);
    make_nonnull_domain(&data.dom, dom);
    data.action = action;

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_IO_ERROR,
                                      (xdrproc_t)xdr_remote_domain_event_io_error_msg, &data);
    } else {
        remote_domain_event_callback_io_error_msg msg = { callback->callbackID,
                                                          data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_IO_ERROR,
                                      (xdrproc_t)xdr_remote_domain_event_callback_io_error_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventIOErrorReason(virConnectPtr conn,
                                    virDomainPtr dom,
                                    const char *srcPath,
                                    const char *devAlias,
                                    int action,
                                    const char *reason,
                                    void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_io_error_reason_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain io error %s %d %s %s %d %s, callback %d",
              dom->name, dom->id, srcPath, devAlias, action, reason,
              callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.srcPath = g_strdup(srcPath);
    data.devAlias = g_strdup(devAlias);
    data.reason = g_strdup(reason);
    data.action = action;
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_IO_ERROR_REASON,
                                      (xdrproc_t)xdr_remote_domain_event_io_error_reason_msg, &data);
    } else {
        remote_domain_event_callback_io_error_reason_msg msg = { callback->callbackID,
                                                                 data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_IO_ERROR_REASON,
                                      (xdrproc_t)xdr_remote_domain_event_callback_io_error_reason_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventGraphics(virConnectPtr conn,
                               virDomainPtr dom,
                               int phase,
                               virDomainEventGraphicsAddressPtr local,
                               virDomainEventGraphicsAddressPtr remote,
                               const char *authScheme,
                               virDomainEventGraphicsSubjectPtr subject,
                               void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_graphics_msg data;
    size_t i;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain graphics event %s %d %d - %d %s %s  - %d %s %s - %s, callback %d",
              dom->name, dom->id, phase,
              local->family, local->service, local->node,
              remote->family, remote->service, remote->node,
              authScheme, callback->callbackID);

    VIR_DEBUG("Subject %d", subject->nidentity);
    for (i = 0; i < subject->nidentity; i++)
        VIR_DEBUG("  %s=%s", subject->identities[i].type, subject->identities[i].name);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.phase = phase;
    data.local.family = local->family;
    data.remote.family = remote->family;
    data.authScheme = g_strdup(authScheme);

    data.local.node = g_strdup(local->node);

    data.local.service = g_strdup(local->service);

    data.remote.node = g_strdup(remote->node);

    data.remote.service = g_strdup(remote->service);

    data.subject.subject_len = subject->nidentity;
    data.subject.subject_val = g_new0(remote_domain_event_graphics_identity,
                                      data.subject.subject_len);

    for (i = 0; i < data.subject.subject_len; i++) {
        data.subject.subject_val[i].type = g_strdup(subject->identities[i].type);
        data.subject.subject_val[i].name = g_strdup(subject->identities[i].name);
    }
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_GRAPHICS,
                                      (xdrproc_t)xdr_remote_domain_event_graphics_msg, &data);
    } else {
        remote_domain_event_callback_graphics_msg msg = { callback->callbackID,
                                                          data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_GRAPHICS,
                                      (xdrproc_t)xdr_remote_domain_event_callback_graphics_msg, &msg);
    }

    return 0;
}

static int
remoteRelayDomainEventBlockJob(virConnectPtr conn,
                               virDomainPtr dom,
                               const char *path,
                               int type,
                               int status,
                               void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_block_job_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain block job event %s %d %s %i, %i, callback %d",
              dom->name, dom->id, path, type, status, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.path = g_strdup(path);
    data.type = type;
    data.status = status;
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_BLOCK_JOB,
                                      (xdrproc_t)xdr_remote_domain_event_block_job_msg, &data);
    } else {
        remote_domain_event_callback_block_job_msg msg = { callback->callbackID,
                                                           data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_BLOCK_JOB,
                                      (xdrproc_t)xdr_remote_domain_event_callback_block_job_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventControlError(virConnectPtr conn,
                                   virDomainPtr dom,
                                   void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_control_error_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain control error %s %d, callback %d",
              dom->name, dom->id, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CONTROL_ERROR,
                                      (xdrproc_t)xdr_remote_domain_event_control_error_msg, &data);
    } else {
        remote_domain_event_callback_control_error_msg msg = { callback->callbackID,
                                                               data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_CONTROL_ERROR,
                                      (xdrproc_t)xdr_remote_domain_event_callback_control_error_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventDiskChange(virConnectPtr conn,
                                 virDomainPtr dom,
                                 const char *oldSrcPath,
                                 const char *newSrcPath,
                                 const char *devAlias,
                                 int reason,
                                 void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_disk_change_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain %s %d disk change %s %s %s %d, callback %d",
              dom->name, dom->id, oldSrcPath, newSrcPath, devAlias, reason,
              callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    if (oldSrcPath) {
        data.oldSrcPath = g_new0(remote_nonnull_string, 1);
        *(data.oldSrcPath) = g_strdup(oldSrcPath);
    }

    if (newSrcPath) {
        data.newSrcPath = g_new0(remote_nonnull_string, 1);
        *(data.newSrcPath) = g_strdup(newSrcPath);
    }

    data.devAlias = g_strdup(devAlias);
    data.reason = reason;
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_DISK_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_disk_change_msg, &data);
    } else {
        remote_domain_event_callback_disk_change_msg msg = { callback->callbackID,
                                                             data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_DISK_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_callback_disk_change_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventTrayChange(virConnectPtr conn,
                                 virDomainPtr dom,
                                 const char *devAlias,
                                 int reason,
                                 void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_tray_change_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain %s %d tray change devAlias: %s reason: %d, callback %d",
              dom->name, dom->id, devAlias, reason, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));

    data.devAlias = g_strdup(devAlias);
    data.reason = reason;
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_TRAY_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_tray_change_msg, &data);
    } else {
        remote_domain_event_callback_tray_change_msg msg = { callback->callbackID,
                                                             data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_TRAY_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_callback_tray_change_msg, &msg);
    }

    return 0;
}

static int
remoteRelayDomainEventPMWakeup(virConnectPtr conn,
                               virDomainPtr dom,
                               int reason,
                               void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_pmwakeup_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain %s %d system pmwakeup, callback %d",
              dom->name, dom->id, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_PMWAKEUP,
                                      (xdrproc_t)xdr_remote_domain_event_pmwakeup_msg, &data);
    } else {
        remote_domain_event_callback_pmwakeup_msg msg = { callback->callbackID,
                                                          reason, data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_PMWAKEUP,
                                      (xdrproc_t)xdr_remote_domain_event_callback_pmwakeup_msg, &msg);
    }

    return 0;
}

static int
remoteRelayDomainEventPMSuspend(virConnectPtr conn,
                                virDomainPtr dom,
                                int reason,
                                void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_pmsuspend_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain %s %d system pmsuspend, callback %d",
              dom->name, dom->id, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_PMSUSPEND,
                                      (xdrproc_t)xdr_remote_domain_event_pmsuspend_msg, &data);
    } else {
        remote_domain_event_callback_pmsuspend_msg msg = { callback->callbackID,
                                                           reason, data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_PMSUSPEND,
                                      (xdrproc_t)xdr_remote_domain_event_callback_pmsuspend_msg, &msg);
    }

    return 0;
}

static int
remoteRelayDomainEventBalloonChange(virConnectPtr conn,
                                    virDomainPtr dom,
                                    unsigned long long actual,
                                    void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_balloon_change_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain balloon change event %s %d %lld, callback %d",
              dom->name, dom->id, actual, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);
    data.actual = actual;

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_BALLOON_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_balloon_change_msg, &data);
    } else {
        remote_domain_event_callback_balloon_change_msg msg = { callback->callbackID,
                                                                data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_BALLOON_CHANGE,
                                      (xdrproc_t)xdr_remote_domain_event_callback_balloon_change_msg, &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventPMSuspendDisk(virConnectPtr conn,
                                    virDomainPtr dom,
                                    int reason,
                                    void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_pmsuspend_disk_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain %s %d system pmsuspend-disk, callback %d",
              dom->name, dom->id, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_PMSUSPEND_DISK,
                                      (xdrproc_t)xdr_remote_domain_event_pmsuspend_disk_msg, &data);
    } else {
        remote_domain_event_callback_pmsuspend_disk_msg msg = { callback->callbackID,
                                                                reason, data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_PMSUSPEND_DISK,
                                      (xdrproc_t)xdr_remote_domain_event_callback_pmsuspend_disk_msg, &msg);
    }

    return 0;
}

static int
remoteRelayDomainEventDeviceRemoved(virConnectPtr conn,
                                    virDomainPtr dom,
                                    const char *devAlias,
                                    void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_device_removed_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain device removed event %s %d %s, callback %d",
              dom->name, dom->id, devAlias, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));

    data.devAlias = g_strdup(devAlias);

    make_nonnull_domain(&data.dom, dom);

    if (callback->legacy) {
        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_DEVICE_REMOVED,
                                      (xdrproc_t)xdr_remote_domain_event_device_removed_msg,
                                      &data);
    } else {
        remote_domain_event_callback_device_removed_msg msg = { callback->callbackID,
                                                                data };

        remoteDispatchObjectEventSend(callback->client, callback->program,
                                      REMOTE_PROC_DOMAIN_EVENT_CALLBACK_DEVICE_REMOVED,
                                      (xdrproc_t)xdr_remote_domain_event_callback_device_removed_msg,
                                      &msg);
    }

    return 0;
}


static int
remoteRelayDomainEventBlockJob2(virConnectPtr conn,
                                virDomainPtr dom,
                                const char *dst,
                                int type,
                                int status,
                                void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_block_job_2_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain block job 2 event %s %d %s %i, %i, callback %d",
              dom->name, dom->id, dst, type, status, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.callbackID = callback->callbackID;
    data.dst = g_strdup(dst);
    data.type = type;
    data.status = status;
    make_nonnull_domain(&data.dom, dom);

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_BLOCK_JOB_2,
                                  (xdrproc_t)xdr_remote_domain_event_block_job_2_msg, &data);

    return 0;
}


static int
remoteRelayDomainEventTunable(virConnectPtr conn,
                              virDomainPtr dom,
                              virTypedParameterPtr params,
                              int nparams,
                              void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_tunable_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain tunable event %s %d, callback %d, params %p %d",
              dom->name, dom->id, callback->callbackID, params, nparams);

    /* build return data */
    memset(&data, 0, sizeof(data));

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_EVENT_TUNABLE_MAX,
                                (virTypedParameterRemotePtr *) &data.params.params_val,
                                &data.params.params_len,
                                VIR_TYPED_PARAM_STRING_OKAY) < 0)
        return -1;

    data.callbackID = callback->callbackID;
    make_nonnull_domain(&data.dom, dom);


    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_TUNABLE,
                                  (xdrproc_t)xdr_remote_domain_event_callback_tunable_msg,
                                  &data);

    return 0;
}


static int
remoteRelayDomainEventAgentLifecycle(virConnectPtr conn,
                                     virDomainPtr dom,
                                     int state,
                                     int reason,
                                     void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_agent_lifecycle_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain agent lifecycle event %s %d, callback %d, "
              " state %d, reason %d",
              dom->name, dom->id, callback->callbackID, state, reason);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.callbackID = callback->callbackID;
    make_nonnull_domain(&data.dom, dom);
    data.state = state;
    data.reason = reason;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_AGENT_LIFECYCLE,
                                  (xdrproc_t)xdr_remote_domain_event_callback_agent_lifecycle_msg,
                                  &data);

    return 0;
}


static int
remoteRelayDomainEventDeviceAdded(virConnectPtr conn,
                                  virDomainPtr dom,
                                  const char *devAlias,
                                  void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_device_added_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain device added event %s %d %s, callback %d",
              dom->name, dom->id, devAlias, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));

    data.devAlias = g_strdup(devAlias);
    make_nonnull_domain(&data.dom, dom);
    data.callbackID = callback->callbackID;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_DEVICE_ADDED,
                                  (xdrproc_t)xdr_remote_domain_event_callback_device_added_msg,
                                  &data);

    return 0;
}


static int
remoteRelayDomainEventMigrationIteration(virConnectPtr conn,
                                         virDomainPtr dom,
                                         int iteration,
                                         void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_migration_iteration_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain migration pass event %s %d, "
              "callback %d, iteration %d",
              dom->name, dom->id, callback->callbackID, iteration);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.callbackID = callback->callbackID;
    make_nonnull_domain(&data.dom, dom);

    data.iteration = iteration;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_MIGRATION_ITERATION,
                                  (xdrproc_t)xdr_remote_domain_event_callback_migration_iteration_msg,
                                  &data);

    return 0;
}


static int
remoteRelayDomainEventJobCompleted(virConnectPtr conn,
                                   virDomainPtr dom,
                                   virTypedParameterPtr params,
                                   int nparams,
                                   void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_job_completed_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain migration completed event %s %d, "
              "callback %d, params %p %d",
              dom->name, dom->id, callback->callbackID, params, nparams);

    /* build return data */
    memset(&data, 0, sizeof(data));

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_JOB_STATS_MAX,
                                (virTypedParameterRemotePtr *) &data.params.params_val,
                                &data.params.params_len,
                                VIR_TYPED_PARAM_STRING_OKAY) < 0)
        return -1;

    data.callbackID = callback->callbackID;
    make_nonnull_domain(&data.dom, dom);

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_JOB_COMPLETED,
                                  (xdrproc_t)xdr_remote_domain_event_callback_job_completed_msg,
                                  &data);
    return 0;
}


static int
remoteRelayDomainEventDeviceRemovalFailed(virConnectPtr conn,
                                          virDomainPtr dom,
                                          const char *devAlias,
                                          void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_device_removal_failed_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain device removal failed event %s %d %s, callback %d",
              dom->name, dom->id, devAlias, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));

    data.devAlias = g_strdup(devAlias);

    make_nonnull_domain(&data.dom, dom);
    data.callbackID = callback->callbackID;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_DEVICE_REMOVAL_FAILED,
                                  (xdrproc_t)xdr_remote_domain_event_callback_device_removal_failed_msg,
                                  &data);

    return 0;
}


static int
remoteRelayDomainEventMetadataChange(virConnectPtr conn,
                                     virDomainPtr dom,
                                     int type,
                                     const char *nsuri,
                                     void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_callback_metadata_change_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain metadata change %s %d %d %s, callback %d",
              dom->name, dom->id, type, NULLSTR(nsuri), callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));

    data.type = type;
    if (nsuri) {
        data.nsuri = g_new0(remote_nonnull_string, 1);
        *(data.nsuri) = g_strdup(nsuri);
    }

    make_nonnull_domain(&data.dom, dom);
    data.callbackID = callback->callbackID;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_CALLBACK_METADATA_CHANGE,
                                  (xdrproc_t)xdr_remote_domain_event_callback_metadata_change_msg,
                                  &data);

    return 0;
}


static int
remoteRelayDomainEventBlockThreshold(virConnectPtr conn,
                                     virDomainPtr dom,
                                     const char *dev,
                                     const char *path,
                                     unsigned long long threshold,
                                     unsigned long long excess,
                                     void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_domain_event_block_threshold_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainEventCheckACL(callback->client, conn, dom))
        return -1;

    VIR_DEBUG("Relaying domain block threshold event %s %d %s %s %llu %llu, callback %d",
              dom->name, dom->id, dev, NULLSTR(path), threshold, excess, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.callbackID = callback->callbackID;
    data.dev = g_strdup(dev);
    if (path) {
        data.path = g_new0(remote_nonnull_string, 1);
        *(data.path) = g_strdup(path);
    }
    data.threshold = threshold;
    data.excess = excess;
    make_nonnull_domain(&data.dom, dom);

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_DOMAIN_EVENT_BLOCK_THRESHOLD,
                                  (xdrproc_t)xdr_remote_domain_event_block_threshold_msg, &data);

    return 0;
}


static virConnectDomainEventGenericCallback domainEventCallbacks[] = {
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventLifecycle),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventReboot),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventRTCChange),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventWatchdog),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventIOError),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventGraphics),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventIOErrorReason),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventControlError),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventBlockJob),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventDiskChange),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventTrayChange),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventPMWakeup),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventPMSuspend),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventBalloonChange),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventPMSuspendDisk),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventDeviceRemoved),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventBlockJob2),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventTunable),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventAgentLifecycle),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventDeviceAdded),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventMigrationIteration),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventJobCompleted),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventDeviceRemovalFailed),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventMetadataChange),
    VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventBlockThreshold),
};

verify(G_N_ELEMENTS(domainEventCallbacks) == VIR_DOMAIN_EVENT_ID_LAST);

static int
remoteRelayNetworkEventLifecycle(virConnectPtr conn,
                                 virNetworkPtr net,
                                 int event,
                                 int detail,
                                 void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_network_event_lifecycle_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayNetworkEventCheckACL(callback->client, conn, net))
        return -1;

    VIR_DEBUG("Relaying network lifecycle event %d, detail %d, callback %d",
              event, detail, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_network(&data.net, net);
    data.callbackID = callback->callbackID;
    data.event = event;
    data.detail = detail;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_NETWORK_EVENT_LIFECYCLE,
                                  (xdrproc_t)xdr_remote_network_event_lifecycle_msg, &data);

    return 0;
}

static virConnectNetworkEventGenericCallback networkEventCallbacks[] = {
    VIR_NETWORK_EVENT_CALLBACK(remoteRelayNetworkEventLifecycle),
};

verify(G_N_ELEMENTS(networkEventCallbacks) == VIR_NETWORK_EVENT_ID_LAST);

static int
remoteRelayStoragePoolEventLifecycle(virConnectPtr conn,
                                     virStoragePoolPtr pool,
                                     int event,
                                     int detail,
                                     void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_storage_pool_event_lifecycle_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayStoragePoolEventCheckACL(callback->client, conn, pool))
        return -1;

    VIR_DEBUG("Relaying storage pool lifecycle event %d, detail %d, callback %d",
              event, detail, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_storage_pool(&data.pool, pool);
    data.callbackID = callback->callbackID;
    data.event = event;
    data.detail = detail;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_STORAGE_POOL_EVENT_LIFECYCLE,
                                  (xdrproc_t)xdr_remote_storage_pool_event_lifecycle_msg,
                                  &data);

    return 0;
}

static int
remoteRelayStoragePoolEventRefresh(virConnectPtr conn,
                                   virStoragePoolPtr pool,
                                   void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_storage_pool_event_refresh_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayStoragePoolEventCheckACL(callback->client, conn, pool))
        return -1;

    VIR_DEBUG("Relaying storage pool refresh event callback %d",
              callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_storage_pool(&data.pool, pool);
    data.callbackID = callback->callbackID;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_STORAGE_POOL_EVENT_REFRESH,
                                  (xdrproc_t)xdr_remote_storage_pool_event_refresh_msg,
                                  &data);

    return 0;
}

static virConnectStoragePoolEventGenericCallback storageEventCallbacks[] = {
    VIR_STORAGE_POOL_EVENT_CALLBACK(remoteRelayStoragePoolEventLifecycle),
    VIR_STORAGE_POOL_EVENT_CALLBACK(remoteRelayStoragePoolEventRefresh),
};

verify(G_N_ELEMENTS(storageEventCallbacks) == VIR_STORAGE_POOL_EVENT_ID_LAST);

static int
remoteRelayNodeDeviceEventLifecycle(virConnectPtr conn,
                                    virNodeDevicePtr dev,
                                    int event,
                                    int detail,
                                    void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_node_device_event_lifecycle_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayNodeDeviceEventCheckACL(callback->client, conn, dev))
        return -1;

    VIR_DEBUG("Relaying node device lifecycle event %d, detail %d, callback %d",
              event, detail, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_node_device(&data.dev, dev);
    data.callbackID = callback->callbackID;
    data.event = event;
    data.detail = detail;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_NODE_DEVICE_EVENT_LIFECYCLE,
                                  (xdrproc_t)xdr_remote_node_device_event_lifecycle_msg,
                                  &data);

    return 0;
}

static int
remoteRelayNodeDeviceEventUpdate(virConnectPtr conn,
                                 virNodeDevicePtr dev,
                                 void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_node_device_event_update_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayNodeDeviceEventCheckACL(callback->client, conn, dev))
        return -1;

    VIR_DEBUG("Relaying node device update event callback %d",
              callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_node_device(&data.dev, dev);
    data.callbackID = callback->callbackID;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_NODE_DEVICE_EVENT_UPDATE,
                                  (xdrproc_t)xdr_remote_node_device_event_update_msg,
                                  &data);

    return 0;
}

static virConnectNodeDeviceEventGenericCallback nodeDeviceEventCallbacks[] = {
    VIR_NODE_DEVICE_EVENT_CALLBACK(remoteRelayNodeDeviceEventLifecycle),
    VIR_NODE_DEVICE_EVENT_CALLBACK(remoteRelayNodeDeviceEventUpdate),
};

verify(G_N_ELEMENTS(nodeDeviceEventCallbacks) == VIR_NODE_DEVICE_EVENT_ID_LAST);

static int
remoteRelaySecretEventLifecycle(virConnectPtr conn,
                                virSecretPtr secret,
                                int event,
                                int detail,
                                void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_secret_event_lifecycle_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelaySecretEventCheckACL(callback->client, conn, secret))
        return -1;

    VIR_DEBUG("Relaying node secretice lifecycle event %d, detail %d, callback %d",
              event, detail, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_secret(&data.secret, secret);
    data.callbackID = callback->callbackID;
    data.event = event;
    data.detail = detail;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_SECRET_EVENT_LIFECYCLE,
                                  (xdrproc_t)xdr_remote_secret_event_lifecycle_msg,
                                  &data);

    return 0;
}

static int
remoteRelaySecretEventValueChanged(virConnectPtr conn,
                                   virSecretPtr secret,
                                   void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    remote_secret_event_value_changed_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelaySecretEventCheckACL(callback->client, conn, secret))
        return -1;

    VIR_DEBUG("Relaying node secret value changed callback %d",
              callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    make_nonnull_secret(&data.secret, secret);
    data.callbackID = callback->callbackID;

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  REMOTE_PROC_SECRET_EVENT_VALUE_CHANGED,
                                  (xdrproc_t)xdr_remote_secret_event_value_changed_msg,
                                  &data);

    return 0;
}

static virConnectSecretEventGenericCallback secretEventCallbacks[] = {
    VIR_SECRET_EVENT_CALLBACK(remoteRelaySecretEventLifecycle),
    VIR_SECRET_EVENT_CALLBACK(remoteRelaySecretEventValueChanged),
};

verify(G_N_ELEMENTS(secretEventCallbacks) == VIR_SECRET_EVENT_ID_LAST);

static void
remoteRelayDomainQemuMonitorEvent(virConnectPtr conn,
                                  virDomainPtr dom,
                                  const char *event,
                                  long long seconds,
                                  unsigned int micros,
                                  const char *details,
                                  void *opaque)
{
    daemonClientEventCallbackPtr callback = opaque;
    qemu_domain_monitor_event_msg data;

    if (callback->callbackID < 0 ||
        !remoteRelayDomainQemuMonitorEventCheckACL(callback->client, conn,
                                                   dom))
        return;

    VIR_DEBUG("Relaying qemu monitor event %s %s, callback %d",
              event, details, callback->callbackID);

    /* build return data */
    memset(&data, 0, sizeof(data));
    data.callbackID = callback->callbackID;
    data.event = g_strdup(event);
    data.seconds = seconds;
    data.micros = micros;
    if (details) {
        if (VIR_ALLOC(data.details) < 0)
            goto error;
        *(data.details) = g_strdup(details);
    }
    make_nonnull_domain(&data.dom, dom);

    remoteDispatchObjectEventSend(callback->client, callback->program,
                                  QEMU_PROC_DOMAIN_MONITOR_EVENT,
                                  (xdrproc_t)xdr_qemu_domain_monitor_event_msg,
                                  &data);
    return;

 error:
    xdr_free((xdrproc_t)xdr_qemu_domain_monitor_event_msg,
             (char *) &data);
    return;
}

static
void remoteRelayConnectionClosedEvent(virConnectPtr conn G_GNUC_UNUSED, int reason, void *opaque)
{
    virNetServerClientPtr client = opaque;

    VIR_DEBUG("Relaying connection closed event, reason %d", reason);

    remote_connect_event_connection_closed_msg msg = { reason };
    remoteDispatchObjectEventSend(client, remoteProgram,
                                  REMOTE_PROC_CONNECT_EVENT_CONNECTION_CLOSED,
                                  (xdrproc_t)xdr_remote_connect_event_connection_closed_msg,
                                  &msg);
}

#define DEREG_CB(conn, eventCallbacks, neventCallbacks, deregFcn, name) \
    do { \
        size_t i; \
        if (neventCallbacks && !conn) { \
            VIR_WARN("Have %zu %s event callbacks but no connection", \
                     neventCallbacks, name); \
            break; \
        } \
        for (i = 0; i < neventCallbacks; i++) { \
            int callbackID = eventCallbacks[i]->callbackID; \
            if (callbackID < 0) { \
                VIR_WARN("unexpected incomplete %s callback %zu", name, i); \
                continue; \
            } \
            VIR_DEBUG("Deregistering remote %s event relay %d", \
                      name, callbackID); \
            eventCallbacks[i]->callbackID = -1; \
            if (deregFcn(conn, callbackID) < 0) \
                VIR_WARN("unexpected %s event deregister failure", name); \
        } \
        VIR_FREE(eventCallbacks); \
        neventCallbacks = 0; \
    } while (0);


static void
remoteClientFreePrivateCallbacks(struct daemonClientPrivate *priv)
{
    g_autoptr(virIdentity) sysident = virIdentityGetSystem();
    virIdentitySetCurrent(sysident);

    DEREG_CB(priv->conn, priv->domainEventCallbacks,
             priv->ndomainEventCallbacks,
             virConnectDomainEventDeregisterAny, "domain");
    DEREG_CB(priv->networkConn, priv->networkEventCallbacks,
             priv->nnetworkEventCallbacks,
             virConnectNetworkEventDeregisterAny, "network");
    DEREG_CB(priv->storageConn, priv->storageEventCallbacks,
             priv->nstorageEventCallbacks,
             virConnectStoragePoolEventDeregisterAny, "storage");
    DEREG_CB(priv->nodedevConn, priv->nodeDeviceEventCallbacks,
             priv->nnodeDeviceEventCallbacks,
             virConnectNodeDeviceEventDeregisterAny, "node device");
    DEREG_CB(priv->secretConn, priv->secretEventCallbacks,
             priv->nsecretEventCallbacks,
             virConnectSecretEventDeregisterAny, "secret");
    DEREG_CB(priv->conn, priv->qemuEventCallbacks,
             priv->nqemuEventCallbacks,
             virConnectDomainQemuMonitorEventDeregister, "qemu monitor");

    if (priv->closeRegistered && priv->conn) {
        if (virConnectUnregisterCloseCallback(priv->conn,
                                              remoteRelayConnectionClosedEvent) < 0)
            VIR_WARN("unexpected close callback event deregister failure");
    }

    virIdentitySetCurrent(NULL);
}
#undef DEREG_CB


/*
 * You must hold lock for at least the client
 * We don't free stuff here, merely disconnect the client's
 * network socket & resources.
 * We keep the libvirt connection open until any async
 * jobs have finished, then clean it up elsewhere
 */
void remoteClientFree(void *data)
{
    struct daemonClientPrivate *priv = data;

    if (priv->conn)
        virConnectClose(priv->conn);
    if (priv->interfaceConn)
        virConnectClose(priv->interfaceConn);
    if (priv->networkConn)
        virConnectClose(priv->networkConn);
    if (priv->nodedevConn)
        virConnectClose(priv->nodedevConn);
    if (priv->nwfilterConn)
        virConnectClose(priv->nwfilterConn);
    if (priv->secretConn)
        virConnectClose(priv->secretConn);
    if (priv->storageConn)
        virConnectClose(priv->storageConn);

    VIR_FREE(priv);
}


static void remoteClientCloseFunc(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv = virNetServerClientGetPrivateData(client);

    daemonRemoveAllClientStreams(priv->streams);

    remoteClientFreePrivateCallbacks(priv);
}


static int
remoteOpenConn(const char *uri,
               bool readonly,
               bool preserveIdentity,
               virConnectPtr *conn)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int ret = -1;

    VIR_DEBUG("Getting secondary uri=%s readonly=%d preserveIdent=%d conn=%p",
              NULLSTR(uri), readonly, preserveIdentity, conn);

    if (*conn)
        return 0;

    if (!uri) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("connection not open"));
        return -1;
    }

    if (preserveIdentity) {
        g_autoptr(virIdentity) ident = NULL;

        if (!(ident = virIdentityGetCurrent()))
            return -1;

        if (virIdentityGetParameters(ident, &params, &nparams) < 0)
            goto error;
    }

    VIR_DEBUG("Opening driver %s", uri);
    if (readonly)
        *conn = virConnectOpenReadOnly(uri);
    else
        *conn = virConnectOpen(uri);
    if (!*conn)
        goto error;
    VIR_DEBUG("Opened driver %p", *conn);

    if (preserveIdentity) {
        if (virConnectSetIdentity(*conn, params, nparams, 0) < 0)
            goto error;

        VIR_DEBUG("Forwarded current identity to secondary driver");
    }

    ret = 0;
 cleanup:
    virTypedParamsFree(params, nparams);
    return ret;

 error:
    if (*conn) {
        virConnectClose(*conn);
        *conn = NULL;
    }
    goto cleanup;
}


static virConnectPtr
remoteGetHypervisorConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (!priv->conn) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("hypervisor connection not open"));
        return NULL;
    }

    return priv->conn;
}


static virConnectPtr
remoteGetInterfaceConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (remoteOpenConn(priv->interfaceURI,
                       priv->readonly,
                       true,
                       &priv->interfaceConn) < 0)
        return NULL;

    return priv->interfaceConn;
}


static virConnectPtr
remoteGetNetworkConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (remoteOpenConn(priv->networkURI,
                       priv->readonly,
                       true,
                       &priv->networkConn) < 0)
        return NULL;

    return priv->networkConn;
}


static virConnectPtr
remoteGetNodeDevConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (remoteOpenConn(priv->nodedevURI,
                       priv->readonly,
                       true,
                       &priv->nodedevConn) < 0)
        return NULL;

    return priv->nodedevConn;
}


static virConnectPtr
remoteGetNWFilterConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (remoteOpenConn(priv->nwfilterURI,
                       priv->readonly,
                       true,
                       &priv->nwfilterConn) < 0)
        return NULL;

    return priv->nwfilterConn;
}


static virConnectPtr
remoteGetSecretConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (remoteOpenConn(priv->secretURI,
                       priv->readonly,
                       true,
                       &priv->secretConn) < 0)
        return NULL;

    return priv->secretConn;
}


static virConnectPtr
remoteGetStorageConn(virNetServerClientPtr client)
{
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    if (remoteOpenConn(priv->storageURI,
                       priv->readonly,
                       true,
                       &priv->storageConn) < 0)
        return NULL;

    return priv->storageConn;
}


void *remoteClientNew(virNetServerClientPtr client,
                      void *opaque G_GNUC_UNUSED)
{
    struct daemonClientPrivate *priv;

    if (VIR_ALLOC(priv) < 0)
        return NULL;

    if (virMutexInit(&priv->lock) < 0) {
        VIR_FREE(priv);
        virReportSystemError(errno, "%s", _("unable to init mutex"));
        return NULL;
    }

    virNetServerClientSetCloseHook(client, remoteClientCloseFunc);
    return priv;
}

/*----- Functions. -----*/

#ifdef VIRTPROXYD
/*
 * When running in virtproxyd regular auto-probing of drivers
 * does not work as we don't have any drivers present (except
 * stateless ones inside libvirt.so). All the interesting
 * drivers are in separate daemons. Thus when we get a NULL
 * URI we need to simulate probing that virConnectOpen would
 * previously do. We use the existance of the UNIX domain
 * socket as our hook for probing.
 *
 * This assumes no stale sockets left over from a now dead
 * daemon, but that's reasonable since libvirtd unlinks
 * sockets it creates on shutdown, or uses systemd activation
 *
 * We only try to probe for primary hypervisor drivers,
 * not the secondary drivers.
 */
static int
remoteDispatchProbeURI(bool readonly,
                       char **probeduri)
{
    *probeduri = NULL;
    VIR_DEBUG("Probing for driver daemon sockets");

    /*
     * If running root, either the daemon is running and the socket
     * exists, or we're using socket activation so the socket exists
     * too.
     *
     * If running non-root, chances are that the daemon won't be
     * running, nor any socket activation is used. We need to
     * be able to auto-spawn the daemon. We thus just check to
     * see what daemons are installed. This is not a big deal as
     * only QEMU & VBox run as non-root, anyway.
     */
    if (geteuid() != 0) {
        /* Order these the same as virDriverLoadModule
         * calls in daemonInitialize */
        const char *drivers[] = {
# ifdef WITH_QEMU
            "qemu",
# endif
# ifdef WITH_VBOX
            "vbox",
# endif
        };
        ssize_t i;

        for (i = 0; i < (ssize_t) G_N_ELEMENTS(drivers) && !*probeduri; i++) {
            g_autofree char *daemonname = NULL;
            g_autofree char *daemonpath = NULL;

            daemonname = g_strdup_printf("virt%sd", drivers[i]);

            if (!(daemonpath = virFileFindResource(daemonname, "src", SBINDIR)))
                return -1;

            if (!virFileExists(daemonpath)) {
                VIR_DEBUG("Missing daemon %s for driver %s", daemonpath, drivers[i]);
                continue;
            }

            *probeduri = g_strdup_printf("%s:///session", drivers[i]);

            VIR_DEBUG("Probed URI %s via daemon %s", *probeduri, daemonpath);
            return 0;
        }
    } else {
        /* Order these the same as virDriverLoadModule
         * calls in daemonInitialize */
        const char *drivers[] = {
# ifdef WITH_LIBXL
            "libxl",
# endif
# ifdef WITH_QEMU
            "qemu",
# endif
# ifdef WITH_LXC
            "lxc",
# endif
# ifdef WITH_VBOX
            "vbox",
# endif
# ifdef WITH_BHYVE
            "bhyve",
# endif
# ifdef WITH_VZ
            "vz",
# endif
        };
        ssize_t i;

        for (i = 0; i < (ssize_t) G_N_ELEMENTS(drivers) && !*probeduri; i++) {
            g_autofree char *sockname = NULL;

            sockname = g_strdup_printf("%s/libvirt/virt%sd-%s", RUNSTATEDIR,
                                       drivers[i], readonly ? "sock-ro" : "sock");

            if (!virFileExists(sockname)) {
                VIR_DEBUG("Missing sock %s for driver %s", sockname, drivers[i]);
                continue;
            }

            *probeduri = g_strdup_printf("%s:///system", drivers[i]);

            VIR_DEBUG("Probed URI %s via sock %s", *probeduri, sockname);
            return 0;
        }
    }

    /* Even if we didn't probe any socket, we won't
     * return error. Just let virConnectOpen's normal
     * logic run which will likely return an error anyway
     */
    VIR_DEBUG("No driver sock exists");
    return 0;
}
#endif /* VIRTPROXYD */


static int
remoteDispatchConnectOpen(virNetServerPtr server G_GNUC_UNUSED,
                          virNetServerClientPtr client,
                          virNetMessagePtr msg G_GNUC_UNUSED,
                          virNetMessageErrorPtr rerr,
                          struct remote_connect_open_args *args)
{
    const char *name;
#ifdef VIRTPROXYD
    g_autofree char *probeduri = NULL;
#endif
    unsigned int flags;
    struct daemonClientPrivate *priv = virNetServerClientGetPrivateData(client);
    int rv = -1;
#ifdef MODULE_NAME
    const char *type = NULL;
#endif /* !MODULE_NAME */
    bool preserveIdentity = false;

    VIR_DEBUG("priv=%p conn=%p", priv, priv->conn);
    virMutexLock(&priv->lock);
    /* Already opened? */
    if (priv->conn) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("connection already open"));
        goto cleanup;
    }

    name = args->name ? *args->name : NULL;

    /* If this connection arrived on a readonly socket, force
     * the connection to be readonly.
     */
    flags = args->flags;
    if (virNetServerClientGetReadonly(client))
        flags |= VIR_CONNECT_RO;

    priv->readonly = flags & VIR_CONNECT_RO;

#ifdef VIRTPROXYD
    if (!name || STREQ(name, "")) {
        if (remoteDispatchProbeURI(priv->readonly, &probeduri) < 0)
            goto cleanup;

        name = probeduri;
    }

    preserveIdentity = true;
#endif /* VIRTPROXYD */

    VIR_DEBUG("Opening driver %s", name);
    if (remoteOpenConn(name,
                       priv->readonly,
                       preserveIdentity,
                       &priv->conn) < 0)
        goto cleanup;
    VIR_DEBUG("Opened %p", priv->conn);

#ifdef MODULE_NAME
    /*
     * For per-driver daemons, we must setup connection URIs
     * for sub-drivers.
     */
    if (!(type = virConnectGetType(priv->conn)))
        goto cleanup;

    VIR_DEBUG("Primary driver type is '%s'", type);
    if (STREQ(type, "QEMU") ||
        STREQ(type, "LIBXL") ||
        STREQ(type, "LXC") ||
        STREQ(type, "VBOX") ||
        STREQ(type, "bhyve") ||
        STREQ(type, "vz") ||
        STREQ(type, "Parallels")) {
        VIR_DEBUG("Hypervisor driver found, setting URIs for secondary drivers");
        if (getuid() == 0) {
            priv->interfaceURI = "interface:///system";
            priv->networkURI = "network:///system";
            priv->nodedevURI = "nodedev:///system";
            priv->nwfilterURI = "nwfilter:///system";
            priv->secretURI = "secret:///system";
            priv->storageURI = "storage:///system";
        } else {
            priv->interfaceURI = "interface:///session";
            priv->networkURI = "network:///session";
            priv->nodedevURI = "nodedev:///session";
            /* No nwfilterURI as this is a root-only driver */
            priv->secretURI = "secret:///session";
            priv->storageURI = "storage:///session";
        }
    } else if (STREQ(type, "interface")) {
        VIR_DEBUG("Interface driver found");
        priv->interfaceConn = virObjectRef(priv->conn);
    } else if (STREQ(type, "network")) {
        VIR_DEBUG("Network driver found");
        priv->networkConn = virObjectRef(priv->conn);
    } else if (STREQ(type, "nodedev")) {
        VIR_DEBUG("Nodedev driver found");
        priv->nodedevConn = virObjectRef(priv->conn);
    } else if (STREQ(type, "nwfilter")) {
        VIR_DEBUG("NWFilter driver found");
        priv->nwfilterConn = virObjectRef(priv->conn);
    } else if (STREQ(type, "secret")) {
        VIR_DEBUG("Secret driver found");
        priv->secretConn = virObjectRef(priv->conn);
    } else if (STREQ(type, "storage")) {
        VIR_DEBUG("Storage driver found");
        priv->storageConn = virObjectRef(priv->conn);

        /* Co-open the secret driver, as apps using the storage driver may well
         * need access to secrets for storage auth
         */
        if (getuid() == 0)
            priv->secretURI = "secret:///system";
        else
            priv->secretURI = "secret:///session";
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected driver type '%s' opened"), type);
        goto cleanup;
    }
#else /* !MODULE_NAME */
    /*
     * For libvirtd/virtproxyd one connection handles
     * all drivers
     */
    VIR_DEBUG("Pointing secondary drivers to primary");
    priv->interfaceConn = virObjectRef(priv->conn);
    priv->networkConn = virObjectRef(priv->conn);
    priv->nodedevConn = virObjectRef(priv->conn);
    priv->nwfilterConn = virObjectRef(priv->conn);
    priv->secretConn = virObjectRef(priv->conn);
    priv->storageConn = virObjectRef(priv->conn);
#endif /* !MODULE_NAME */

    /* force update the @readonly attribute which was inherited from the
     * virNetServerService object - this is important for sockets that are RW
     * by default, but do accept RO flags, e.g. TCP
     */
    virNetServerClientSetReadonly(client, (flags & VIR_CONNECT_RO));
    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        if (priv->conn) {
            virObjectUnref(priv->conn);
            priv->conn = NULL;
        }
    }
    virMutexUnlock(&priv->lock);
    return rv;
}


static int
remoteDispatchConnectClose(virNetServerPtr server G_GNUC_UNUSED,
                           virNetServerClientPtr client,
                           virNetMessagePtr msg G_GNUC_UNUSED,
                           virNetMessageErrorPtr rerr G_GNUC_UNUSED)
{
    virNetServerClientDelayedClose(client);
    return 0;
}


static int
remoteDispatchConnectSetIdentity(virNetServerPtr server G_GNUC_UNUSED,
                                 virNetServerClientPtr client,
                                 virNetMessagePtr msg G_GNUC_UNUSED,
                                 virNetMessageErrorPtr rerr,
                                 remote_connect_set_identity_args *args)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);
    g_autoptr(virIdentity) ident = NULL;
    if (!conn)
        goto cleanup;

    VIR_DEBUG("Received forwarded identity");
    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  REMOTE_CONNECT_IDENTITY_PARAMS_MAX,
                                  &params,
                                  &nparams) < 0)
        goto cleanup;

    VIR_TYPED_PARAMS_DEBUG(params, nparams);

    if (virConnectSetIdentityEnsureACL(conn) < 0)
        goto cleanup;

    if (!(ident = virIdentityNew()))
        goto cleanup;

    if (virIdentitySetParameters(ident, params, nparams) < 0)
        goto cleanup;

    virNetServerClientSetIdentity(client, ident);

    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}



static int
remoteDispatchDomainGetSchedulerType(virNetServerPtr server G_GNUC_UNUSED,
                                     virNetServerClientPtr client,
                                     virNetMessagePtr msg G_GNUC_UNUSED,
                                     virNetMessageErrorPtr rerr,
                                     remote_domain_get_scheduler_type_args *args,
                                     remote_domain_get_scheduler_type_ret *ret)
{
    virDomainPtr dom = NULL;
    char *type;
    int nparams;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (!(type = virDomainGetSchedulerType(dom, &nparams)))
        goto cleanup;

    ret->type = type;
    ret->nparams = nparams;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetSchedulerParameters(virNetServerPtr server G_GNUC_UNUSED,
                                           virNetServerClientPtr client,
                                           virNetMessagePtr msg G_GNUC_UNUSED,
                                           virNetMessageErrorPtr rerr,
                                           remote_domain_get_scheduler_parameters_args *args,
                                           remote_domain_get_scheduler_parameters_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->nparams > REMOTE_DOMAIN_SCHEDULER_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetSchedulerParameters(dom, params, &nparams) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_SCHEDULER_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                0) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetSchedulerParametersFlags(virNetServerPtr server G_GNUC_UNUSED,
                                                virNetServerClientPtr client,
                                                virNetMessagePtr msg G_GNUC_UNUSED,
                                                virNetMessageErrorPtr rerr,
                                                remote_domain_get_scheduler_parameters_flags_args *args,
                                                remote_domain_get_scheduler_parameters_flags_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->nparams > REMOTE_DOMAIN_SCHEDULER_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetSchedulerParametersFlags(dom, params, &nparams,
                                             args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_SCHEDULER_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainMemoryStats(virNetServerPtr server G_GNUC_UNUSED,
                                virNetServerClientPtr client,
                                virNetMessagePtr msg G_GNUC_UNUSED,
                                virNetMessageErrorPtr rerr,
                                remote_domain_memory_stats_args *args,
                                remote_domain_memory_stats_ret *ret)
{
    virDomainPtr dom = NULL;
    virDomainMemoryStatPtr stats = NULL;
    int nr_stats;
    size_t i;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->maxStats > REMOTE_DOMAIN_MEMORY_STATS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("maxStats > REMOTE_DOMAIN_MEMORY_STATS_MAX"));
        goto cleanup;
    }

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    /* Allocate stats array for making dispatch call */
    if (VIR_ALLOC_N(stats, args->maxStats) < 0)
        goto cleanup;

    nr_stats = virDomainMemoryStats(dom, stats, args->maxStats, args->flags);
    if (nr_stats < 0)
        goto cleanup;

    /* Allocate return buffer */
    if (VIR_ALLOC_N(ret->stats.stats_val, args->maxStats) < 0)
        goto cleanup;

    /* Copy the stats into the xdr return structure */
    for (i = 0; i < nr_stats; i++) {
        ret->stats.stats_val[i].tag = stats[i].tag;
        ret->stats.stats_val[i].val = stats[i].val;
    }
    ret->stats.stats_len = nr_stats;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    VIR_FREE(stats);
    return rv;
}

static int
remoteDispatchDomainBlockPeek(virNetServerPtr server G_GNUC_UNUSED,
                              virNetServerClientPtr client,
                              virNetMessagePtr msg G_GNUC_UNUSED,
                              virNetMessageErrorPtr rerr,
                              remote_domain_block_peek_args *args,
                              remote_domain_block_peek_ret *ret)
{
    virDomainPtr dom = NULL;
    char *path;
    unsigned long long offset;
    size_t size;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;
    path = args->path;
    offset = args->offset;
    size = args->size;
    flags = args->flags;

    if (size > REMOTE_DOMAIN_BLOCK_PEEK_BUFFER_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("size > maximum buffer size"));
        goto cleanup;
    }

    ret->buffer.buffer_len = size;
    if (VIR_ALLOC_N(ret->buffer.buffer_val, size) < 0)
        goto cleanup;

    if (virDomainBlockPeek(dom, path, offset, size,
                           ret->buffer.buffer_val, flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(ret->buffer.buffer_val);
    }
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainBlockStatsFlags(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_block_stats_flags_args *args,
                                    remote_domain_block_stats_flags_ret *ret)
{
    virTypedParameterPtr params = NULL;
    virDomainPtr dom = NULL;
    const char *path = args->path;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;
    flags = args->flags;

    if (args->nparams > REMOTE_DOMAIN_BLOCK_STATS_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (virDomainBlockStatsFlags(dom, path, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    /* Serialize the block stats. */
    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_BLOCK_STATS_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainMemoryPeek(virNetServerPtr server G_GNUC_UNUSED,
                               virNetServerClientPtr client,
                               virNetMessagePtr msg G_GNUC_UNUSED,
                               virNetMessageErrorPtr rerr,
                               remote_domain_memory_peek_args *args,
                               remote_domain_memory_peek_ret *ret)
{
    virDomainPtr dom = NULL;
    unsigned long long offset;
    size_t size;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;
    offset = args->offset;
    size = args->size;
    flags = args->flags;

    if (size > REMOTE_DOMAIN_MEMORY_PEEK_BUFFER_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("size > maximum buffer size"));
        goto cleanup;
    }

    ret->buffer.buffer_len = size;
    if (VIR_ALLOC_N(ret->buffer.buffer_val, size) < 0)
        goto cleanup;

    if (virDomainMemoryPeek(dom, offset, size,
                            ret->buffer.buffer_val, flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(ret->buffer.buffer_val);
    }
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetSecurityLabel(virNetServerPtr server G_GNUC_UNUSED,
                                     virNetServerClientPtr client,
                                     virNetMessagePtr msg G_GNUC_UNUSED,
                                     virNetMessageErrorPtr rerr,
                                     remote_domain_get_security_label_args *args,
                                     remote_domain_get_security_label_ret *ret)
{
    virDomainPtr dom = NULL;
    virSecurityLabelPtr seclabel = NULL;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (VIR_ALLOC(seclabel) < 0)
        goto cleanup;

    if (virDomainGetSecurityLabel(dom, seclabel) < 0)
        goto cleanup;

    ret->label.label_len = strlen(seclabel->label) + 1;
    if (VIR_ALLOC_N(ret->label.label_val, ret->label.label_len) < 0)
        goto cleanup;
    strcpy(ret->label.label_val, seclabel->label);
    ret->enforcing = seclabel->enforcing;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    VIR_FREE(seclabel);
    return rv;
}

static int
remoteDispatchDomainGetSecurityLabelList(virNetServerPtr server G_GNUC_UNUSED,
                                         virNetServerClientPtr client,
                                         virNetMessagePtr msg G_GNUC_UNUSED,
                                         virNetMessageErrorPtr rerr,
                                         remote_domain_get_security_label_list_args *args,
                                         remote_domain_get_security_label_list_ret *ret)
{
    virDomainPtr dom = NULL;
    virSecurityLabelPtr seclabels = NULL;
    int len, rv = -1;
    size_t i;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if ((len = virDomainGetSecurityLabelList(dom, &seclabels)) < 0) {
        ret->ret = len;
        ret->labels.labels_len = 0;
        ret->labels.labels_val = NULL;
        goto done;
    }

    if (VIR_ALLOC_N(ret->labels.labels_val, len) < 0)
        goto cleanup;

    for (i = 0; i < len; i++) {
        size_t label_len = strlen(seclabels[i].label) + 1;
        remote_domain_get_security_label_ret *cur = &ret->labels.labels_val[i];
        cur->label.label_val = g_strdup(seclabels[i].label);
        cur->label.label_len = label_len;
        cur->enforcing = seclabels[i].enforcing;
    }
    ret->labels.labels_len = ret->ret = len;

 done:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    VIR_FREE(seclabels);
    return rv;
}

static int
remoteDispatchNodeGetSecurityModel(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr msg G_GNUC_UNUSED,
                                   virNetMessageErrorPtr rerr,
                                   remote_node_get_security_model_ret *ret)
{
    virSecurityModel secmodel;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    memset(&secmodel, 0, sizeof(secmodel));
    if (virNodeGetSecurityModel(conn, &secmodel) < 0)
        goto cleanup;

    ret->model.model_len = strlen(secmodel.model) + 1;
    if (VIR_ALLOC_N(ret->model.model_val, ret->model.model_len) < 0)
        goto cleanup;
    strcpy(ret->model.model_val, secmodel.model);

    ret->doi.doi_len = strlen(secmodel.doi) + 1;
    if (VIR_ALLOC_N(ret->doi.doi_val, ret->doi.doi_len) < 0)
        goto cleanup;
    strcpy(ret->doi.doi_val, secmodel.doi);

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchDomainGetVcpuPinInfo(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr msg G_GNUC_UNUSED,
                                   virNetMessageErrorPtr rerr,
                                   remote_domain_get_vcpu_pin_info_args *args,
                                   remote_domain_get_vcpu_pin_info_ret *ret)
{
    virDomainPtr dom = NULL;
    unsigned char *cpumaps = NULL;
    int num;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (args->ncpumaps > REMOTE_VCPUINFO_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("ncpumaps > REMOTE_VCPUINFO_MAX"));
        goto cleanup;
    }

    if (INT_MULTIPLY_OVERFLOW(args->ncpumaps, args->maplen) ||
        args->ncpumaps * args->maplen > REMOTE_CPUMAPS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("maxinfo * maplen > REMOTE_CPUMAPS_MAX"));
        goto cleanup;
    }

    /* Allocate buffers to take the results. */
    if (args->maplen > 0 &&
        VIR_ALLOC_N(cpumaps, args->ncpumaps * args->maplen) < 0)
        goto cleanup;

    if ((num = virDomainGetVcpuPinInfo(dom,
                                       args->ncpumaps,
                                       cpumaps,
                                       args->maplen,
                                       args->flags)) < 0)
        goto cleanup;

    ret->num = num;
    /* Don't need to allocate/copy the cpumaps if we make the reasonable
     * assumption that unsigned char and char are the same size.
     * Note that remoteDispatchClientRequest will free.
     */
    ret->cpumaps.cpumaps_len = args->ncpumaps * args->maplen;
    ret->cpumaps.cpumaps_val = (char *) cpumaps;
    cpumaps = NULL;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    VIR_FREE(cpumaps);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainPinEmulator(virNetServerPtr server G_GNUC_UNUSED,
                                virNetServerClientPtr client,
                                virNetMessagePtr msg G_GNUC_UNUSED,
                                virNetMessageErrorPtr rerr,
                                remote_domain_pin_emulator_args *args)
{
    int rv = -1;
    virDomainPtr dom = NULL;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainPinEmulator(dom,
                             (unsigned char *) args->cpumap.cpumap_val,
                             args->cpumap.cpumap_len,
                             args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainGetEmulatorPinInfo(virNetServerPtr server G_GNUC_UNUSED,
                                       virNetServerClientPtr client,
                                       virNetMessagePtr msg G_GNUC_UNUSED,
                                       virNetMessageErrorPtr rerr,
                                       remote_domain_get_emulator_pin_info_args *args,
                                       remote_domain_get_emulator_pin_info_ret *ret)
{
    virDomainPtr dom = NULL;
    unsigned char *cpumaps = NULL;
    int r;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    /* Allocate buffers to take the results */
    if (args->maplen > 0 &&
        VIR_ALLOC_N(cpumaps, args->maplen) < 0)
        goto cleanup;

    if ((r = virDomainGetEmulatorPinInfo(dom,
                                         cpumaps,
                                         args->maplen,
                                         args->flags)) < 0)
        goto cleanup;

    ret->ret = r;
    ret->cpumaps.cpumaps_len = args->maplen;
    ret->cpumaps.cpumaps_val = (char *) cpumaps;
    cpumaps = NULL;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    VIR_FREE(cpumaps);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetVcpus(virNetServerPtr server G_GNUC_UNUSED,
                             virNetServerClientPtr client,
                             virNetMessagePtr msg G_GNUC_UNUSED,
                             virNetMessageErrorPtr rerr,
                             remote_domain_get_vcpus_args *args,
                             remote_domain_get_vcpus_ret *ret)
{
    virDomainPtr dom = NULL;
    virVcpuInfoPtr info = NULL;
    unsigned char *cpumaps = NULL;
    int info_len;
    size_t i;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (args->maxinfo > REMOTE_VCPUINFO_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("maxinfo > REMOTE_VCPUINFO_MAX"));
        goto cleanup;
    }

    if (INT_MULTIPLY_OVERFLOW(args->maxinfo, args->maplen) ||
        args->maxinfo * args->maplen > REMOTE_CPUMAPS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("maxinfo * maplen > REMOTE_CPUMAPS_MAX"));
        goto cleanup;
    }

    /* Allocate buffers to take the results. */
    if (VIR_ALLOC_N(info, args->maxinfo) < 0)
        goto cleanup;
    if (args->maplen > 0 &&
        VIR_ALLOC_N(cpumaps, args->maxinfo * args->maplen) < 0)
        goto cleanup;

    if ((info_len = virDomainGetVcpus(dom,
                                      info, args->maxinfo,
                                      cpumaps, args->maplen)) < 0)
        goto cleanup;

    /* Allocate the return buffer for info. */
    ret->info.info_len = info_len;
    if (VIR_ALLOC_N(ret->info.info_val, info_len) < 0)
        goto cleanup;

    for (i = 0; i < info_len; ++i) {
        ret->info.info_val[i].number = info[i].number;
        ret->info.info_val[i].state = info[i].state;
        ret->info.info_val[i].cpu_time = info[i].cpuTime;
        ret->info.info_val[i].cpu = info[i].cpu;
    }

    /* Don't need to allocate/copy the cpumaps if we make the reasonable
     * assumption that unsigned char and char are the same size.
     * Note that remoteDispatchClientRequest will free.
     */
    ret->cpumaps.cpumaps_len = args->maxinfo * args->maplen;
    ret->cpumaps.cpumaps_val = (char *) cpumaps;
    cpumaps = NULL;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(ret->info.info_val);
    }
    VIR_FREE(cpumaps);
    VIR_FREE(info);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetIOThreadInfo(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_get_iothread_info_args *args,
                                    remote_domain_get_iothread_info_ret *ret)
{
    int rv = -1;
    size_t i;
    virDomainIOThreadInfoPtr *info = NULL;
    virDomainPtr dom = NULL;
    remote_domain_iothread_info *dst;
    int ninfo = 0;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if ((ninfo = virDomainGetIOThreadInfo(dom, &info, args->flags)) < 0)
        goto cleanup;

    if (ninfo > REMOTE_IOTHREAD_INFO_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many IOThreads in info: %d for limit %d"),
                       ninfo, REMOTE_IOTHREAD_INFO_MAX);
        goto cleanup;
    }

    if (ninfo) {
        if (VIR_ALLOC_N(ret->info.info_val, ninfo) < 0)
            goto cleanup;

        ret->info.info_len = ninfo;

        for (i = 0; i < ninfo; i++) {
            dst = &ret->info.info_val[i];
            dst->iothread_id = info[i]->iothread_id;

            /* No need to allocate/copy the cpumap if we make the reasonable
             * assumption that unsigned char and char are the same size.
             */
            dst->cpumap.cpumap_len = info[i]->cpumaplen;
            dst->cpumap.cpumap_val = (char *)info[i]->cpumap;
            info[i]->cpumap = NULL;
        }
    } else {
        ret->info.info_len = 0;
        ret->info.info_val = NULL;
    }

    ret->ret = ninfo;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    if (ninfo >= 0)
        for (i = 0; i < ninfo; i++)
            virDomainIOThreadInfoFree(info[i]);
    VIR_FREE(info);

    return rv;
}

static int
remoteDispatchDomainMigratePrepare(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr msg G_GNUC_UNUSED,
                                   virNetMessageErrorPtr rerr,
                                   remote_domain_migrate_prepare_args *args,
                                   remote_domain_migrate_prepare_ret *ret)
{
    char *cookie = NULL;
    int cookielen = 0;
    char *uri_in;
    char **uri_out;
    char *dname;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    uri_in = args->uri_in == NULL ? NULL : *args->uri_in;
    dname = args->dname == NULL ? NULL : *args->dname;

    /* Wacky world of XDR ... */
    if (VIR_ALLOC(uri_out) < 0)
        goto cleanup;

    if (virDomainMigratePrepare(conn, &cookie, &cookielen,
                                uri_in, uri_out,
                                args->flags, dname, args->resource) < 0)
        goto cleanup;

    /* remoteDispatchClientRequest will free cookie, uri_out and
     * the string if there is one.
     */
    ret->cookie.cookie_len = cookielen;
    ret->cookie.cookie_val = cookie;
    if (*uri_out == NULL) {
        ret->uri_out = NULL;
    } else {
        ret->uri_out = uri_out;
        uri_out = NULL;
    }

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    VIR_FREE(uri_out);
    return rv;
}

static int
remoteDispatchDomainMigratePrepare2(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_migrate_prepare2_args *args,
                                    remote_domain_migrate_prepare2_ret *ret)
{
    char *cookie = NULL;
    int cookielen = 0;
    char *uri_in;
    char **uri_out;
    char *dname;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    uri_in = args->uri_in == NULL ? NULL : *args->uri_in;
    dname = args->dname == NULL ? NULL : *args->dname;

    /* Wacky world of XDR ... */
    if (VIR_ALLOC(uri_out) < 0)
        goto cleanup;

    if (virDomainMigratePrepare2(conn, &cookie, &cookielen,
                                 uri_in, uri_out,
                                 args->flags, dname, args->resource,
                                 args->dom_xml) < 0)
        goto cleanup;

    /* remoteDispatchClientRequest will free cookie, uri_out and
     * the string if there is one.
     */
    ret->cookie.cookie_len = cookielen;
    ret->cookie.cookie_val = cookie;
    ret->uri_out = *uri_out == NULL ? NULL : uri_out;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(uri_out);
    }
    return rv;
}

static int
remoteDispatchDomainGetMemoryParameters(virNetServerPtr server G_GNUC_UNUSED,
                                        virNetServerClientPtr client,
                                        virNetMessagePtr msg G_GNUC_UNUSED,
                                        virNetMessageErrorPtr rerr,
                                        remote_domain_get_memory_parameters_args *args,
                                        remote_domain_get_memory_parameters_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_DOMAIN_MEMORY_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetMemoryParameters(dom, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_MEMORY_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetNumaParameters(virNetServerPtr server G_GNUC_UNUSED,
                                      virNetServerClientPtr client,
                                      virNetMessagePtr msg G_GNUC_UNUSED,
                                      virNetMessageErrorPtr rerr,
                                      remote_domain_get_numa_parameters_args *args,
                                      remote_domain_get_numa_parameters_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_DOMAIN_NUMA_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetNumaParameters(dom, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_NUMA_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetBlkioParameters(virNetServerPtr server G_GNUC_UNUSED,
                                       virNetServerClientPtr client,
                                       virNetMessagePtr msg G_GNUC_UNUSED,
                                       virNetMessageErrorPtr rerr,
                                       remote_domain_get_blkio_parameters_args *args,
                                       remote_domain_get_blkio_parameters_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_DOMAIN_BLKIO_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetBlkioParameters(dom, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_BLKIO_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchNodeGetCPUStats(virNetServerPtr server G_GNUC_UNUSED,
                              virNetServerClientPtr client,
                              virNetMessagePtr msg G_GNUC_UNUSED,
                              virNetMessageErrorPtr rerr,
                              remote_node_get_cpu_stats_args *args,
                              remote_node_get_cpu_stats_ret *ret)
{
    virNodeCPUStatsPtr params = NULL;
    size_t i;
    int cpuNum = args->cpuNum;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_NODE_CPU_STATS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (virNodeGetCPUStats(conn, cpuNum, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of stats
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    /* Serialise the memory parameters. */
    ret->params.params_len = nparams;
    if (VIR_ALLOC_N(ret->params.params_val, nparams) < 0)
        goto cleanup;

    for (i = 0; i < nparams; ++i) {
        /* remoteDispatchClientRequest will free this: */
        ret->params.params_val[i].field = g_strdup(params[i].field);

        ret->params.params_val[i].value = params[i].value;
    }

 success:
    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        if (ret->params.params_val) {
            for (i = 0; i < nparams; i++)
                VIR_FREE(ret->params.params_val[i].field);
            VIR_FREE(ret->params.params_val);
        }
    }
    VIR_FREE(params);
    return rv;
}

static int
remoteDispatchNodeGetMemoryStats(virNetServerPtr server G_GNUC_UNUSED,
                                 virNetServerClientPtr client,
                                 virNetMessagePtr msg G_GNUC_UNUSED,
                                 virNetMessageErrorPtr rerr,
                                 remote_node_get_memory_stats_args *args,
                                 remote_node_get_memory_stats_ret *ret)
{
    virNodeMemoryStatsPtr params = NULL;
    size_t i;
    int cellNum = args->cellNum;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_NODE_MEMORY_STATS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (virNodeGetMemoryStats(conn, cellNum, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    /* Serialise the memory parameters. */
    ret->params.params_len = nparams;
    if (VIR_ALLOC_N(ret->params.params_val, nparams) < 0)
        goto cleanup;

    for (i = 0; i < nparams; ++i) {
        /* remoteDispatchClientRequest will free this: */
        ret->params.params_val[i].field = g_strdup(params[i].field);

        ret->params.params_val[i].value = params[i].value;
    }

 success:
    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        if (ret->params.params_val) {
            for (i = 0; i < nparams; i++)
                VIR_FREE(ret->params.params_val[i].field);
            VIR_FREE(ret->params.params_val);
        }
    }
    VIR_FREE(params);
    return rv;
}

static int
remoteDispatchDomainGetLaunchSecurityInfo(virNetServerPtr server G_GNUC_UNUSED,
                                          virNetServerClientPtr client,
                                          virNetMessagePtr msg G_GNUC_UNUSED,
                                          virNetMessageErrorPtr rerr,
                                          remote_domain_get_launch_security_info_args *args,
                                          remote_domain_get_launch_security_info_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetLaunchSecurityInfo(dom, &params, &nparams, args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_LAUNCH_SECURITY_INFO_PARAMS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetPerfEvents(virNetServerPtr server G_GNUC_UNUSED,
                                  virNetServerClientPtr client,
                                  virNetMessagePtr msg G_GNUC_UNUSED,
                                  virNetMessageErrorPtr rerr,
                                  remote_domain_get_perf_events_args *args,
                                  remote_domain_get_perf_events_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetPerfEvents(dom, &params, &nparams, args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_PERF_EVENTS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                0) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetBlockJobInfo(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_get_block_job_info_args *args,
                                    remote_domain_get_block_job_info_ret *ret)
{
    virDomainPtr dom = NULL;
    virDomainBlockJobInfo tmp;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    rv = virDomainGetBlockJobInfo(dom, args->path, &tmp, args->flags);
    if (rv <= 0)
        goto cleanup;

    ret->type = tmp.type;
    ret->bandwidth = tmp.bandwidth;
    ret->cur = tmp.cur;
    ret->end = tmp.end;
    ret->found = 1;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetBlockIoTune(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr hdr G_GNUC_UNUSED,
                                   virNetMessageErrorPtr rerr,
                                   remote_domain_get_block_io_tune_args *args,
                                   remote_domain_get_block_io_tune_ret *ret)
{
    virDomainPtr dom = NULL;
    int rv = -1;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->nparams > REMOTE_DOMAIN_BLOCK_IO_TUNE_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }

    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetBlockIoTune(dom, args->disk ? *args->disk : NULL,
                                params, &nparams, args->flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    /* Serialize the block I/O tuning parameters. */
    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_BLOCK_IO_TUNE_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

/*-------------------------------------------------------------*/

static int
remoteDispatchAuthList(virNetServerPtr server,
                       virNetServerClientPtr client,
                       virNetMessagePtr msg G_GNUC_UNUSED,
                       virNetMessageErrorPtr rerr,
                       remote_auth_list_ret *ret)
{
    int rv = -1;
    int auth = virNetServerClientGetAuth(client);
    uid_t callerUid;
    gid_t callerGid;
    pid_t callerPid;
    unsigned long long timestamp;

    /* If the client is root then we want to bypass the
     * policykit auth to avoid root being denied if
     * some piece of polkit isn't present/running
     */
    if (auth == VIR_NET_SERVER_SERVICE_AUTH_POLKIT) {
        if (virNetServerClientGetUNIXIdentity(client, &callerUid, &callerGid,
                                              &callerPid, &timestamp) < 0) {
            /* Don't do anything on error - it'll be validated at next
             * phase of auth anyway */
            virResetLastError();
        } else if (callerUid == 0) {
            char *ident;
            ident = g_strdup_printf("pid:%lld,uid:%d", (long long)callerPid,
                                    (int)callerUid);
            VIR_INFO("Bypass polkit auth for privileged client %s", ident);
            virNetServerSetClientAuthenticated(server, client);
            auth = VIR_NET_SERVER_SERVICE_AUTH_NONE;
            VIR_FREE(ident);
        }
    }

    ret->types.types_len = 1;
    if (VIR_ALLOC_N(ret->types.types_val, ret->types.types_len) < 0)
        goto cleanup;

    switch ((virNetServerServiceAuthMethods) auth) {
    case VIR_NET_SERVER_SERVICE_AUTH_NONE:
        ret->types.types_val[0] = REMOTE_AUTH_NONE;
        break;
    case VIR_NET_SERVER_SERVICE_AUTH_POLKIT:
        ret->types.types_val[0] = REMOTE_AUTH_POLKIT;
        break;
    case VIR_NET_SERVER_SERVICE_AUTH_SASL:
        ret->types.types_val[0] = REMOTE_AUTH_SASL;
        break;
    }

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}


#ifdef WITH_SASL
/*
 * Initializes the SASL session in prepare for authentication
 * and gives the client a list of allowed mechanisms to choose
 */
static int
remoteDispatchAuthSaslInit(virNetServerPtr server G_GNUC_UNUSED,
                           virNetServerClientPtr client,
                           virNetMessagePtr msg G_GNUC_UNUSED,
                           virNetMessageErrorPtr rerr,
                           remote_auth_sasl_init_ret *ret)
{
    virNetSASLSessionPtr sasl = NULL;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);

    virMutexLock(&priv->lock);

    VIR_DEBUG("Initialize SASL auth %d", virNetServerClientGetFD(client));
    if (virNetServerClientGetAuth(client) != VIR_NET_SERVER_SERVICE_AUTH_SASL ||
        priv->sasl != NULL) {
        VIR_ERROR(_("client tried invalid SASL init request"));
        goto authfail;
    }

    sasl = virNetSASLSessionNewServer(saslCtxt,
                                      "libvirt",
                                      virNetServerClientLocalAddrStringSASL(client),
                                      virNetServerClientRemoteAddrStringSASL(client));
    if (!sasl)
        goto authfail;

    /* Inform SASL that we've got an external SSF layer from TLS */
    if (virNetServerClientHasTLSSession(client)) {
        int ssf;

        if ((ssf = virNetServerClientGetTLSKeySize(client)) < 0)
            goto authfail;

        ssf *= 8; /* key size is bytes, sasl wants bits */

        VIR_DEBUG("Setting external SSF %d", ssf);
        if (virNetSASLSessionExtKeySize(sasl, ssf) < 0)
            goto authfail;
    }

    if (virNetServerClientIsSecure(client))
        /* If we've got TLS or UNIX domain sock, we don't care about SSF */
        virNetSASLSessionSecProps(sasl, 0, 0, true);
    else
        /* Plain TCP, better get an SSF layer */
        virNetSASLSessionSecProps(sasl,
                                  56,  /* Good enough to require kerberos */
                                  100000,  /* Arbitrary big number */
                                  false); /* No anonymous */

    if (!(ret->mechlist = virNetSASLSessionListMechanisms(sasl)))
        goto authfail;
    VIR_DEBUG("Available mechanisms for client: '%s'", ret->mechlist);

    priv->sasl = sasl;
    virMutexUnlock(&priv->lock);
    return 0;

 authfail:
    virResetLastError();
    virReportError(VIR_ERR_AUTH_FAILED, "%s",
                   _("authentication failed"));
    virNetMessageSaveError(rerr);
    PROBE(RPC_SERVER_CLIENT_AUTH_FAIL,
          "client=%p auth=%d",
          client, REMOTE_AUTH_SASL);
    virObjectUnref(sasl);
    virMutexUnlock(&priv->lock);
    return -1;
}

/*
 * Returns 0 if ok, -1 on error, -2 if rejected
 */
static int
remoteSASLFinish(virNetServerPtr server,
                 virNetServerClientPtr client)
{
    g_autoptr(virIdentity) clnt_identity = NULL;
    const char *identity;
    struct daemonClientPrivate *priv = virNetServerClientGetPrivateData(client);
    int ssf;

    /* TLS or UNIX domain sockets trivially OK */
    if (!virNetServerClientIsSecure(client)) {
        if ((ssf = virNetSASLSessionGetKeySize(priv->sasl)) < 0)
            return -1;

        VIR_DEBUG("negotiated an SSF of %d", ssf);
        if (ssf < 56) { /* 56 is good for Kerberos */
            VIR_ERROR(_("negotiated SSF %d was not strong enough"), ssf);
            return -2;
        }
    }

    if (!(identity = virNetSASLSessionGetIdentity(priv->sasl)))
        return -2;

    if (!virNetSASLContextCheckIdentity(saslCtxt, identity))
        return -2;

    if (!(clnt_identity = virNetServerClientGetIdentity(client)))
        return -1;

    virNetServerSetClientAuthenticated(server, client);
    virNetServerClientSetSASLSession(client, priv->sasl);
    virIdentitySetSASLUserName(clnt_identity, identity);

    VIR_DEBUG("Authentication successful %d", virNetServerClientGetFD(client));

    PROBE(RPC_SERVER_CLIENT_AUTH_ALLOW,
          "client=%p auth=%d identity=%s",
          client, REMOTE_AUTH_SASL, identity);

    virObjectUnref(priv->sasl);
    priv->sasl = NULL;

    return 0;
}

/*
 * This starts the SASL authentication negotiation.
 */
static int
remoteDispatchAuthSaslStart(virNetServerPtr server,
                            virNetServerClientPtr client,
                            virNetMessagePtr msg G_GNUC_UNUSED,
                            virNetMessageErrorPtr rerr,
                            remote_auth_sasl_start_args *args,
                            remote_auth_sasl_start_ret *ret)
{
    const char *serverout;
    size_t serveroutlen;
    int err;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    const char *identity;

    virMutexLock(&priv->lock);

    VIR_DEBUG("Start SASL auth %d", virNetServerClientGetFD(client));
    if (virNetServerClientGetAuth(client) != VIR_NET_SERVER_SERVICE_AUTH_SASL ||
        priv->sasl == NULL) {
        VIR_ERROR(_("client tried invalid SASL start request"));
        goto authfail;
    }

    VIR_DEBUG("Using SASL mechanism %s. Data %d bytes, nil: %d",
              args->mech, args->data.data_len, args->nil);
    err = virNetSASLSessionServerStart(priv->sasl,
                                       args->mech,
                                       /* NB, distinction of NULL vs "" is *critical* in SASL */
                                       args->nil ? NULL : args->data.data_val,
                                       args->data.data_len,
                                       &serverout,
                                       &serveroutlen);
    if (err != VIR_NET_SASL_COMPLETE &&
        err != VIR_NET_SASL_CONTINUE)
        goto authfail;

    if (serveroutlen > REMOTE_AUTH_SASL_DATA_MAX) {
        VIR_ERROR(_("sasl start reply data too long %d"), (int)serveroutlen);
        goto authfail;
    }

    /* NB, distinction of NULL vs "" is *critical* in SASL */
    if (serverout) {
        if (VIR_ALLOC_N(ret->data.data_val, serveroutlen) < 0)
            goto authfail;
        memcpy(ret->data.data_val, serverout, serveroutlen);
    } else {
        ret->data.data_val = NULL;
    }
    ret->nil = serverout ? 0 : 1;
    ret->data.data_len = serveroutlen;

    VIR_DEBUG("SASL return data %d bytes, nil; %d", ret->data.data_len, ret->nil);
    if (err == VIR_NET_SASL_CONTINUE) {
        ret->complete = 0;
    } else {
        /* Check username whitelist ACL */
        if ((err = remoteSASLFinish(server, client)) < 0) {
            if (err == -2)
                goto authdeny;
            else
                goto authfail;
        }

        ret->complete = 1;
    }

    virMutexUnlock(&priv->lock);
    return 0;

 authfail:
    PROBE(RPC_SERVER_CLIENT_AUTH_FAIL,
          "client=%p auth=%d",
          client, REMOTE_AUTH_SASL);
    goto error;

 authdeny:
    identity = virNetSASLSessionGetIdentity(priv->sasl);
    PROBE(RPC_SERVER_CLIENT_AUTH_DENY,
          "client=%p auth=%d identity=%s",
          client, REMOTE_AUTH_SASL, identity);
    goto error;

 error:
    virObjectUnref(priv->sasl);
    priv->sasl = NULL;
    virResetLastError();
    virReportError(VIR_ERR_AUTH_FAILED, "%s",
                   _("authentication failed"));
    virNetMessageSaveError(rerr);
    virMutexUnlock(&priv->lock);
    return -1;
}


static int
remoteDispatchAuthSaslStep(virNetServerPtr server,
                           virNetServerClientPtr client,
                           virNetMessagePtr msg G_GNUC_UNUSED,
                           virNetMessageErrorPtr rerr,
                           remote_auth_sasl_step_args *args,
                           remote_auth_sasl_step_ret *ret)
{
    const char *serverout;
    size_t serveroutlen;
    int err;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    const char *identity;

    virMutexLock(&priv->lock);

    VIR_DEBUG("Step SASL auth %d", virNetServerClientGetFD(client));
    if (virNetServerClientGetAuth(client) != VIR_NET_SERVER_SERVICE_AUTH_SASL ||
        priv->sasl == NULL) {
        VIR_ERROR(_("client tried invalid SASL start request"));
        goto authfail;
    }

    VIR_DEBUG("Step using SASL Data %d bytes, nil: %d",
              args->data.data_len, args->nil);
    err = virNetSASLSessionServerStep(priv->sasl,
                                      /* NB, distinction of NULL vs "" is *critical* in SASL */
                                      args->nil ? NULL : args->data.data_val,
                                      args->data.data_len,
                                      &serverout,
                                      &serveroutlen);
    if (err != VIR_NET_SASL_COMPLETE &&
        err != VIR_NET_SASL_CONTINUE)
        goto authfail;

    if (serveroutlen > REMOTE_AUTH_SASL_DATA_MAX) {
        VIR_ERROR(_("sasl step reply data too long %d"),
                  (int)serveroutlen);
        goto authfail;
    }

    /* NB, distinction of NULL vs "" is *critical* in SASL */
    if (serverout) {
        if (VIR_ALLOC_N(ret->data.data_val, serveroutlen) < 0)
            goto authfail;
        memcpy(ret->data.data_val, serverout, serveroutlen);
    } else {
        ret->data.data_val = NULL;
    }
    ret->nil = serverout ? 0 : 1;
    ret->data.data_len = serveroutlen;

    VIR_DEBUG("SASL return data %d bytes, nil; %d", ret->data.data_len, ret->nil);
    if (err == VIR_NET_SASL_CONTINUE) {
        ret->complete = 0;
    } else {
        /* Check username whitelist ACL */
        if ((err = remoteSASLFinish(server, client)) < 0) {
            if (err == -2)
                goto authdeny;
            else
                goto authfail;
        }

        ret->complete = 1;
    }

    virMutexUnlock(&priv->lock);
    return 0;

 authfail:
    PROBE(RPC_SERVER_CLIENT_AUTH_FAIL,
          "client=%p auth=%d",
          client, REMOTE_AUTH_SASL);
    goto error;

 authdeny:
    identity = virNetSASLSessionGetIdentity(priv->sasl);
    PROBE(RPC_SERVER_CLIENT_AUTH_DENY,
          "client=%p auth=%d identity=%s",
          client, REMOTE_AUTH_SASL, identity);
    goto error;

 error:
    virObjectUnref(priv->sasl);
    priv->sasl = NULL;
    virResetLastError();
    virReportError(VIR_ERR_AUTH_FAILED, "%s",
                   _("authentication failed"));
    virNetMessageSaveError(rerr);
    virMutexUnlock(&priv->lock);
    return -1;
}
#else
static int
remoteDispatchAuthSaslInit(virNetServerPtr server G_GNUC_UNUSED,
                           virNetServerClientPtr client G_GNUC_UNUSED,
                           virNetMessagePtr msg G_GNUC_UNUSED,
                           virNetMessageErrorPtr rerr,
                           remote_auth_sasl_init_ret *ret G_GNUC_UNUSED)
{
    VIR_WARN("Client tried unsupported SASL auth");
    virReportError(VIR_ERR_AUTH_FAILED, "%s",
                   _("authentication failed"));
    virNetMessageSaveError(rerr);
    return -1;
}
static int
remoteDispatchAuthSaslStart(virNetServerPtr server G_GNUC_UNUSED,
                            virNetServerClientPtr client G_GNUC_UNUSED,
                            virNetMessagePtr msg G_GNUC_UNUSED,
                            virNetMessageErrorPtr rerr,
                            remote_auth_sasl_start_args *args G_GNUC_UNUSED,
                            remote_auth_sasl_start_ret *ret G_GNUC_UNUSED)
{
    VIR_WARN("Client tried unsupported SASL auth");
    virReportError(VIR_ERR_AUTH_FAILED, "%s",
                   _("authentication failed"));
    virNetMessageSaveError(rerr);
    return -1;
}
static int
remoteDispatchAuthSaslStep(virNetServerPtr server G_GNUC_UNUSED,
                           virNetServerClientPtr client G_GNUC_UNUSED,
                           virNetMessagePtr msg G_GNUC_UNUSED,
                           virNetMessageErrorPtr rerr,
                           remote_auth_sasl_step_args *args G_GNUC_UNUSED,
                           remote_auth_sasl_step_ret *ret G_GNUC_UNUSED)
{
    VIR_WARN("Client tried unsupported SASL auth");
    virReportError(VIR_ERR_AUTH_FAILED, "%s",
                   _("authentication failed"));
    virNetMessageSaveError(rerr);
    return -1;
}
#endif



static int
remoteDispatchAuthPolkit(virNetServerPtr server,
                         virNetServerClientPtr client,
                         virNetMessagePtr msg G_GNUC_UNUSED,
                         virNetMessageErrorPtr rerr,
                         remote_auth_polkit_ret *ret)
{
    pid_t callerPid = -1;
    gid_t callerGid = -1;
    uid_t callerUid = -1;
    unsigned long long timestamp;
    const char *action;
    char *ident = NULL;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    int rv;

    virMutexLock(&priv->lock);
    action = virNetServerClientGetReadonly(client) ?
        "org.libvirt.unix.monitor" :
        "org.libvirt.unix.manage";

    VIR_DEBUG("Start PolicyKit auth %d", virNetServerClientGetFD(client));
    if (virNetServerClientGetAuth(client) != VIR_NET_SERVER_SERVICE_AUTH_POLKIT) {
        VIR_ERROR(_("client tried invalid PolicyKit init request"));
        goto authfail;
    }

    if (virNetServerClientGetUNIXIdentity(client, &callerUid, &callerGid,
                                          &callerPid, &timestamp) < 0) {
        goto authfail;
    }

    if (timestamp == 0) {
        VIR_WARN("Failing polkit auth due to missing client (pid=%lld) start time",
                 (long long)callerPid);
        goto authfail;
    }

    VIR_INFO("Checking PID %lld running as %d",
             (long long) callerPid, callerUid);

    rv = virPolkitCheckAuth(action,
                            callerPid,
                            timestamp,
                            callerUid,
                            NULL,
                            true);
    if (rv == -1)
        goto authfail;
    else if (rv == -2)
        goto authdeny;

    PROBE(RPC_SERVER_CLIENT_AUTH_ALLOW,
          "client=%p auth=%d identity=%s",
          client, REMOTE_AUTH_POLKIT, ident);
    VIR_INFO("Policy allowed action %s from pid %lld, uid %d",
             action, (long long) callerPid, callerUid);
    ret->complete = 1;

    virNetServerSetClientAuthenticated(server, client);
    virMutexUnlock(&priv->lock);

    return 0;

 error:
    virNetMessageSaveError(rerr);
    virMutexUnlock(&priv->lock);
    return -1;

 authfail:
    PROBE(RPC_SERVER_CLIENT_AUTH_FAIL,
          "client=%p auth=%d",
          client, REMOTE_AUTH_POLKIT);
    goto error;

 authdeny:
    PROBE(RPC_SERVER_CLIENT_AUTH_DENY,
          "client=%p auth=%d identity=%s",
          client, REMOTE_AUTH_POLKIT, ident);
    goto error;
}


static int
remoteDispatchNodeDeviceGetParent(virNetServerPtr server G_GNUC_UNUSED,
                                  virNetServerClientPtr client,
                                  virNetMessagePtr msg G_GNUC_UNUSED,
                                  virNetMessageErrorPtr rerr,
                                  remote_node_device_get_parent_args *args,
                                  remote_node_device_get_parent_ret *ret)
{
    virNodeDevicePtr dev = NULL;
    const char *parent = NULL;
    int rv = -1;
    virConnectPtr conn = remoteGetNodeDevConn(client);

    if (!conn)
        goto cleanup;

    if (!(dev = virNodeDeviceLookupByName(conn, args->name)))
        goto cleanup;

    parent = virNodeDeviceGetParent(dev);

    if (parent == NULL) {
        ret->parentName = NULL;
    } else {
        /* remoteDispatchClientRequest will free this. */
        if (VIR_ALLOC(ret->parentName) < 0)
            goto cleanup;
        *(ret->parentName) = g_strdup(parent);
    }

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dev);
    return rv;
}

static int
remoteDispatchConnectRegisterCloseCallback(virNetServerPtr server G_GNUC_UNUSED,
                                           virNetServerClientPtr client,
                                           virNetMessagePtr msg G_GNUC_UNUSED,
                                           virNetMessageErrorPtr rerr)
{
    int rv = -1;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (virConnectRegisterCloseCallback(conn,
                                        remoteRelayConnectionClosedEvent,
                                        client, NULL) < 0)
        goto cleanup;

    priv->closeRegistered = true;
    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchConnectUnregisterCloseCallback(virNetServerPtr server G_GNUC_UNUSED,
                                             virNetServerClientPtr client,
                                             virNetMessagePtr msg G_GNUC_UNUSED,
                                             virNetMessageErrorPtr rerr)
{
    int rv = -1;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (virConnectUnregisterCloseCallback(conn,
                                          remoteRelayConnectionClosedEvent) < 0)
        goto cleanup;

    priv->closeRegistered = false;
    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchConnectDomainEventRegister(virNetServerPtr server G_GNUC_UNUSED,
                                         virNetServerClientPtr client,
                                         virNetMessagePtr msg G_GNUC_UNUSED,
                                         virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                         remote_connect_domain_event_register_ret *ret G_GNUC_UNUSED)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; or you can use VIR_APPEND_ELEMENT_COPY to avoid
     * clearing 'callback' and having to juggle the pointer
     * between 'ref' and 'callback'.
     */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = VIR_DOMAIN_EVENT_ID_LIFECYCLE;
    callback->callbackID = -1;
    callback->legacy = true;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->domainEventCallbacks,
                           priv->ndomainEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectDomainEventRegisterAny(conn,
                                                       NULL,
                                                       VIR_DOMAIN_EVENT_ID_LIFECYCLE,
                                                       VIR_DOMAIN_EVENT_CALLBACK(remoteRelayDomainEventLifecycle),
                                                       ref,
                                                       remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->domainEventCallbacks,
                     priv->ndomainEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchConnectDomainEventDeregister(virNetServerPtr server G_GNUC_UNUSED,
                                           virNetServerClientPtr client,
                                           virNetMessagePtr msg G_GNUC_UNUSED,
                                           virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                           remote_connect_domain_event_deregister_ret *ret G_GNUC_UNUSED)
{
    int callbackID = -1;
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->ndomainEventCallbacks; i++) {
        if (priv->domainEventCallbacks[i]->eventID == VIR_DOMAIN_EVENT_ID_LIFECYCLE) {
            callbackID = priv->domainEventCallbacks[i]->callbackID;
            break;
        }
    }

    if (callbackID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("domain event %d not registered"),
                       VIR_DOMAIN_EVENT_ID_LIFECYCLE);
        goto cleanup;
    }

    if (virConnectDomainEventDeregisterAny(conn, callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->domainEventCallbacks, i,
                       priv->ndomainEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static void
remoteDispatchObjectEventSend(virNetServerClientPtr client,
                              virNetServerProgramPtr program,
                              int procnr,
                              xdrproc_t proc,
                              void *data)
{
    virNetMessagePtr msg;

    if (!(msg = virNetMessageNew(false)))
        goto cleanup;

    msg->header.prog = virNetServerProgramGetID(program);
    msg->header.vers = virNetServerProgramGetVersion(program);
    msg->header.proc = procnr;
    msg->header.type = VIR_NET_MESSAGE;
    msg->header.serial = 1;
    msg->header.status = VIR_NET_OK;

    if (virNetMessageEncodeHeader(msg) < 0)
        goto cleanup;

    if (virNetMessageEncodePayload(msg, proc, data) < 0)
        goto cleanup;

    VIR_DEBUG("Queue event %d %zu", procnr, msg->bufferLength);
    if (virNetServerClientSendMessage(client, msg) < 0)
        goto cleanup;

    xdr_free(proc, data);
    return;

 cleanup:
    virNetMessageFree(msg);
    xdr_free(proc, data);
}

static int
remoteDispatchSecretGetValue(virNetServerPtr server G_GNUC_UNUSED,
                             virNetServerClientPtr client,
                             virNetMessagePtr msg G_GNUC_UNUSED,
                             virNetMessageErrorPtr rerr,
                             remote_secret_get_value_args *args,
                             remote_secret_get_value_ret *ret)
{
    virSecretPtr secret = NULL;
    size_t value_size;
    unsigned char *value;
    int rv = -1;
    virConnectPtr conn = remoteGetSecretConn(client);

    if (!conn)
        goto cleanup;

    if (!(secret = get_nonnull_secret(conn, args->secret)))
        goto cleanup;

    if (!(value = virSecretGetValue(secret, &value_size, args->flags)))
        goto cleanup;

    ret->value.value_len = value_size;
    ret->value.value_val = (char *)value;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(secret);
    return rv;
}

static int
remoteDispatchDomainGetState(virNetServerPtr server G_GNUC_UNUSED,
                             virNetServerClientPtr client,
                             virNetMessagePtr msg G_GNUC_UNUSED,
                             virNetMessageErrorPtr rerr,
                             remote_domain_get_state_args *args,
                             remote_domain_get_state_ret *ret)
{
    virDomainPtr dom = NULL;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetState(dom, &ret->state, &ret->reason, args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


/* Due to back-compat reasons, two RPC calls map to the same libvirt
 * API of virConnectDomainEventRegisterAny.  A client should only use
 * the new call if they have probed
 * VIR_DRV_SUPPORTS_FEATURE(VIR_DRV_FEATURE_REMOTE_EVENT_CALLBACK),
 * and must not mix the two styles.  */
static int
remoteDispatchConnectDomainEventRegisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                            virNetServerClientPtr client,
                                            virNetMessagePtr msg G_GNUC_UNUSED,
                                            virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                            remote_connect_domain_event_register_any_args *args)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    /* We intentionally do not use VIR_DOMAIN_EVENT_ID_LAST here; any
     * new domain events added after this point should only use the
     * modern callback style of RPC.  */
    if (args->eventID > VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED ||
        args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("unsupported event ID %d"),
                       args->eventID);
        goto cleanup;
    }

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = args->eventID;
    callback->callbackID = -1;
    callback->legacy = true;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->domainEventCallbacks,
                           priv->ndomainEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectDomainEventRegisterAny(conn,
                                                       NULL,
                                                       args->eventID,
                                                       domainEventCallbacks[args->eventID],
                                                       ref,
                                                       remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->domainEventCallbacks,
                     priv->ndomainEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}


static int
remoteDispatchConnectDomainEventCallbackRegisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                                    virNetServerClientPtr client,
                                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                                    virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                    remote_connect_domain_event_callback_register_any_args *args,
                                                    remote_connect_domain_event_callback_register_any_ret *ret)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virDomainPtr dom = NULL;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (args->dom &&
        !(dom = get_nonnull_domain(conn, *args->dom)))
        goto cleanup;

    if (args->eventID >= VIR_DOMAIN_EVENT_ID_LAST || args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("unsupported event ID %d"),
                       args->eventID);
        goto cleanup;
    }

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = args->eventID;
    callback->callbackID = -1;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->domainEventCallbacks,
                           priv->ndomainEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectDomainEventRegisterAny(conn,
                                                       dom,
                                                       args->eventID,
                                                       domainEventCallbacks[args->eventID],
                                                       ref,
                                                       remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->domainEventCallbacks,
                     priv->ndomainEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;
    ret->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchConnectDomainEventDeregisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                              virNetServerClientPtr client,
                                              virNetMessagePtr msg G_GNUC_UNUSED,
                                              virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                              remote_connect_domain_event_deregister_any_args *args)
{
    int callbackID = -1;
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    /* We intentionally do not use VIR_DOMAIN_EVENT_ID_LAST here; any
     * new domain events added after this point should only use the
     * modern callback style of RPC.  */
    if (args->eventID > VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED ||
        args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("unsupported event ID %d"),
                       args->eventID);
        goto cleanup;
    }

    for (i = 0; i < priv->ndomainEventCallbacks; i++) {
        if (priv->domainEventCallbacks[i]->eventID == args->eventID) {
            callbackID = priv->domainEventCallbacks[i]->callbackID;
            break;
        }
    }
    if (callbackID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("domain event %d not registered"), args->eventID);
        goto cleanup;
    }

    if (virConnectDomainEventDeregisterAny(conn, callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->domainEventCallbacks, i,
                       priv->ndomainEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}


static int
remoteDispatchConnectDomainEventCallbackDeregisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                                      virNetServerClientPtr client,
                                                      virNetMessagePtr msg G_GNUC_UNUSED,
                                                      virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                      remote_connect_domain_event_callback_deregister_any_args *args)
{
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->ndomainEventCallbacks; i++) {
        if (priv->domainEventCallbacks[i]->callbackID == args->callbackID)
            break;
    }
    if (i == priv->ndomainEventCallbacks) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("domain event callback %d not registered"),
                       args->callbackID);
        goto cleanup;
    }

    if (virConnectDomainEventDeregisterAny(conn, args->callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->domainEventCallbacks, i,
                       priv->ndomainEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}


static int
qemuDispatchDomainMonitorCommand(virNetServerPtr server G_GNUC_UNUSED,
                                 virNetServerClientPtr client,
                                 virNetMessagePtr msg G_GNUC_UNUSED,
                                 virNetMessageErrorPtr rerr,
                                 qemu_domain_monitor_command_args *args,
                                 qemu_domain_monitor_command_ret *ret)
{
    virDomainPtr dom = NULL;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainQemuMonitorCommand(dom, args->cmd, &ret->result,
                                    args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainMigrateBegin3(virNetServerPtr server G_GNUC_UNUSED,
                                  virNetServerClientPtr client,
                                  virNetMessagePtr msg G_GNUC_UNUSED,
                                  virNetMessageErrorPtr rerr,
                                  remote_domain_migrate_begin3_args *args,
                                  remote_domain_migrate_begin3_ret *ret)
{
    char *xml = NULL;
    virDomainPtr dom = NULL;
    char *dname;
    char *xmlin;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    xmlin = args->xmlin == NULL ? NULL : *args->xmlin;
    dname = args->dname == NULL ? NULL : *args->dname;

    if (!(xml = virDomainMigrateBegin3(dom, xmlin,
                                       &cookieout, &cookieoutlen,
                                       args->flags, dname, args->resource)))
        goto cleanup;

    /* remoteDispatchClientRequest will free cookie and
     * the xml string if there is one.
     */
    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;
    ret->xml = xml;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainMigratePrepare3(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_migrate_prepare3_args *args,
                                    remote_domain_migrate_prepare3_ret *ret)
{
    char *cookieout = NULL;
    int cookieoutlen = 0;
    char *uri_in;
    char **uri_out;
    char *dname;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    uri_in = args->uri_in == NULL ? NULL : *args->uri_in;
    dname = args->dname == NULL ? NULL : *args->dname;

    /* Wacky world of XDR ... */
    if (VIR_ALLOC(uri_out) < 0)
        goto cleanup;

    if (virDomainMigratePrepare3(conn,
                                 args->cookie_in.cookie_in_val,
                                 args->cookie_in.cookie_in_len,
                                 &cookieout, &cookieoutlen,
                                 uri_in, uri_out,
                                 args->flags, dname, args->resource,
                                 args->dom_xml) < 0)
        goto cleanup;

    /* remoteDispatchClientRequest will free cookie, uri_out and
     * the string if there is one.
     */
    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;
    ret->uri_out = *uri_out == NULL ? NULL : uri_out;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(uri_out);
    }
    return rv;
}


static int
remoteDispatchDomainMigratePerform3(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_migrate_perform3_args *args,
                                    remote_domain_migrate_perform3_ret *ret)
{
    virDomainPtr dom = NULL;
    char *xmlin;
    char *dname;
    char *uri;
    char *dconnuri;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    xmlin = args->xmlin == NULL ? NULL : *args->xmlin;
    dname = args->dname == NULL ? NULL : *args->dname;
    uri = args->uri == NULL ? NULL : *args->uri;
    dconnuri = args->dconnuri == NULL ? NULL : *args->dconnuri;

    if (virDomainMigratePerform3(dom, xmlin,
                                 args->cookie_in.cookie_in_val,
                                 args->cookie_in.cookie_in_len,
                                 &cookieout, &cookieoutlen,
                                 dconnuri, uri,
                                 args->flags, dname, args->resource) < 0)
        goto cleanup;

    /* remoteDispatchClientRequest will free cookie
     */
    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainMigrateFinish3(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr msg G_GNUC_UNUSED,
                                   virNetMessageErrorPtr rerr,
                                   remote_domain_migrate_finish3_args *args,
                                   remote_domain_migrate_finish3_ret *ret)
{
    virDomainPtr dom = NULL;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    char *uri;
    char *dconnuri;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    uri = args->uri == NULL ? NULL : *args->uri;
    dconnuri = args->dconnuri == NULL ? NULL : *args->dconnuri;

    if (!(dom = virDomainMigrateFinish3(conn, args->dname,
                                        args->cookie_in.cookie_in_val,
                                        args->cookie_in.cookie_in_len,
                                        &cookieout, &cookieoutlen,
                                        dconnuri, uri,
                                        args->flags,
                                        args->cancelled)))
        goto cleanup;

    make_nonnull_domain(&ret->dom, dom);

    /* remoteDispatchClientRequest will free cookie
     */
    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(cookieout);
    }
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainMigrateConfirm3(virNetServerPtr server G_GNUC_UNUSED,
                                    virNetServerClientPtr client,
                                    virNetMessagePtr msg G_GNUC_UNUSED,
                                    virNetMessageErrorPtr rerr,
                                    remote_domain_migrate_confirm3_args *args)
{
    virDomainPtr dom = NULL;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainMigrateConfirm3(dom,
                                 args->cookie_in.cookie_in_val,
                                 args->cookie_in.cookie_in_len,
                                 args->flags, args->cancelled) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int remoteDispatchConnectSupportsFeature(virNetServerPtr server G_GNUC_UNUSED,
                                                virNetServerClientPtr client,
                                                virNetMessagePtr msg G_GNUC_UNUSED,
                                                virNetMessageErrorPtr rerr,
                                                remote_connect_supports_feature_args *args,
                                                remote_connect_supports_feature_ret *ret)
{
    int rv = -1;
    int supported = -1;
    virConnectPtr conn = NULL;

    /* This feature is checked before opening the connection, thus we must
     * check it first.
     */
    if (args->feature == VIR_DRV_FEATURE_PROGRAM_KEEPALIVE) {
        if (virNetServerClientStartKeepAlive(client) < 0)
            goto cleanup;
        supported = 1;
        goto done;
    }

    conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    switch ((virDrvFeature) args->feature) {
    case VIR_DRV_FEATURE_FD_PASSING:
    case VIR_DRV_FEATURE_REMOTE_EVENT_CALLBACK:
    case VIR_DRV_FEATURE_REMOTE_CLOSE_CALLBACK:
        supported = 1;
        break;
    case VIR_DRV_FEATURE_MIGRATION_V1:
    case VIR_DRV_FEATURE_REMOTE:
    case VIR_DRV_FEATURE_MIGRATION_V2:
    case VIR_DRV_FEATURE_MIGRATION_P2P:
    case VIR_DRV_FEATURE_MIGRATION_DIRECT:
    case VIR_DRV_FEATURE_MIGRATION_V3:
    case VIR_DRV_FEATURE_MIGRATE_CHANGE_PROTECTION:
    case VIR_DRV_FEATURE_TYPED_PARAM_STRING:
    case VIR_DRV_FEATURE_XML_MIGRATABLE:
    case VIR_DRV_FEATURE_MIGRATION_OFFLINE:
    case VIR_DRV_FEATURE_MIGRATION_PARAMS:
    default:
        if ((supported = virConnectSupportsFeature(conn, args->feature)) < 0)
            goto cleanup;
        break;
    case VIR_DRV_FEATURE_PROGRAM_KEEPALIVE:
        /* should not be possible! */
        goto cleanup;
    }

 done:
    ret->supported = supported;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}


static int
remoteDispatchDomainOpenGraphics(virNetServerPtr server G_GNUC_UNUSED,
                                 virNetServerClientPtr client,
                                 virNetMessagePtr msg,
                                 virNetMessageErrorPtr rerr,
                                 remote_domain_open_graphics_args *args)
{
    virDomainPtr dom = NULL;
    int rv = -1;
    int fd = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if ((fd = virNetMessageDupFD(msg, 0)) < 0)
        goto cleanup;

    if (virDomainOpenGraphics(dom,
                              args->idx,
                              fd,
                              args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainOpenGraphicsFd(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr msg,
                                   virNetMessageErrorPtr rerr,
                                   remote_domain_open_graphics_fd_args *args)
{
    virDomainPtr dom = NULL;
    int rv = -1;
    int fd = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if ((fd = virDomainOpenGraphicsFD(dom,
                                      args->idx,
                                      args->flags)) < 0)
        goto cleanup;

    if (virNetMessageAddFD(msg, fd) < 0)
        goto cleanup;

    /* return 1 here to let virNetServerProgramDispatchCall know
     * we are passing a FD */
    rv = 1;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    if (rv < 0)
        virNetMessageSaveError(rerr);

    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainGetInterfaceParameters(virNetServerPtr server G_GNUC_UNUSED,
                                           virNetServerClientPtr client,
                                           virNetMessagePtr msg G_GNUC_UNUSED,
                                           virNetMessageErrorPtr rerr,
                                           remote_domain_get_interface_parameters_args *args,
                                           remote_domain_get_interface_parameters_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    const char *device = args->device;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_DOMAIN_INTERFACE_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetInterfaceParameters(dom, device, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_INTERFACE_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetCPUStats(virNetServerPtr server G_GNUC_UNUSED,
                                virNetServerClientPtr client,
                                virNetMessagePtr hdr G_GNUC_UNUSED,
                                virNetMessageErrorPtr rerr,
                                remote_domain_get_cpu_stats_args *args,
                                remote_domain_get_cpu_stats_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int rv = -1;
    int percpu_len = 0;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->nparams > REMOTE_NODE_CPU_STATS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->ncpus > REMOTE_DOMAIN_GET_CPU_STATS_NCPUS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("ncpus too large"));
        goto cleanup;
    }

    if (args->nparams > 0 &&
        VIR_ALLOC_N(params, args->ncpus * args->nparams) < 0)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    percpu_len = virDomainGetCPUStats(dom, params, args->nparams,
                                      args->start_cpu, args->ncpus,
                                      args->flags);
    if (percpu_len < 0)
        goto cleanup;
    /* If nparams == 0, the function returns a single value */
    if (args->nparams == 0)
        goto success;

    if (virTypedParamsSerialize(params, args->nparams * args->ncpus,
                                REMOTE_DOMAIN_GET_CPU_STATS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

 success:
    rv = 0;
    ret->nparams = percpu_len;
    if (args->nparams && !(args->flags & VIR_TYPED_PARAM_STRING_OKAY)) {
        size_t i;

        for (i = 0; i < percpu_len; i++) {
            if (params[i].type == VIR_TYPED_PARAM_STRING)
                ret->nparams--;
        }
    }

 cleanup:
    if (rv < 0)
         virNetMessageSaveError(rerr);
    virTypedParamsFree(params, args->ncpus * args->nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetDiskErrors(virNetServerPtr server G_GNUC_UNUSED,
                                  virNetServerClientPtr client,
                                  virNetMessagePtr msg G_GNUC_UNUSED,
                                  virNetMessageErrorPtr rerr,
                                  remote_domain_get_disk_errors_args *args,
                                  remote_domain_get_disk_errors_ret *ret)
{
    int rv = -1;
    virDomainPtr dom = NULL;
    virDomainDiskErrorPtr errors = NULL;
    int len = 0;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (args->maxerrors > REMOTE_DOMAIN_DISK_ERRORS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("maxerrors too large"));
        goto cleanup;
    }

    if (args->maxerrors &&
        VIR_ALLOC_N(errors, args->maxerrors) < 0)
        goto cleanup;

    if ((len = virDomainGetDiskErrors(dom, errors,
                                      args->maxerrors,
                                      args->flags)) < 0)
        goto cleanup;

    ret->nerrors = len;
    if (errors &&
        remoteSerializeDomainDiskErrors(errors, len,
                                        &ret->errors.errors_val,
                                        &ret->errors.errors_len) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    if (errors && len > 0) {
        size_t i;
        for (i = 0; i < len; i++)
            VIR_FREE(errors[i].disk);
    }
    VIR_FREE(errors);
    return rv;
}


static int
remoteDispatchNodeGetSevInfo(virNetServerPtr server G_GNUC_UNUSED,
                             virNetServerClientPtr client,
                             virNetMessagePtr msg G_GNUC_UNUSED,
                             virNetMessageErrorPtr rerr,
                             remote_node_get_sev_info_args *args,
                             remote_node_get_sev_info_ret *ret)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (virNodeGetSEVInfo(conn, &params, &nparams, args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_NODE_SEV_INFO_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    return rv;
}


static int
remoteDispatchNodeGetMemoryParameters(virNetServerPtr server G_GNUC_UNUSED,
                                      virNetServerClientPtr client,
                                      virNetMessagePtr msg G_GNUC_UNUSED,
                                      virNetMessageErrorPtr rerr,
                                      remote_node_get_memory_parameters_args *args,
                                      remote_node_get_memory_parameters_ret *ret)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    unsigned int flags;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    if (args->nparams > REMOTE_NODE_MEMORY_PARAMETERS_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("nparams too large"));
        goto cleanup;
    }
    if (args->nparams && VIR_ALLOC_N(params, args->nparams) < 0)
        goto cleanup;
    nparams = args->nparams;

    if (virNodeGetMemoryParameters(conn, params, &nparams, flags) < 0)
        goto cleanup;

    /* In this case, we need to send back the number of parameters
     * supported
     */
    if (args->nparams == 0) {
        ret->nparams = nparams;
        goto success;
    }

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_NODE_MEMORY_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

 success:
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    return rv;
}

static int
remoteDispatchNodeGetCPUMap(virNetServerPtr server G_GNUC_UNUSED,
                            virNetServerClientPtr client,
                            virNetMessagePtr msg G_GNUC_UNUSED,
                            virNetMessageErrorPtr rerr,
                            remote_node_get_cpu_map_args *args,
                            remote_node_get_cpu_map_ret *ret)
{
    unsigned char *cpumap = NULL;
    unsigned int online = 0;
    unsigned int flags;
    int cpunum;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    flags = args->flags;

    cpunum = virNodeGetCPUMap(conn, args->need_map ? &cpumap : NULL,
                              args->need_online ? &online : NULL, flags);
    if (cpunum < 0)
        goto cleanup;

    /* 'serialize' return cpumap */
    if (args->need_map) {
        ret->cpumap.cpumap_len = VIR_CPU_MAPLEN(cpunum);
        ret->cpumap.cpumap_val = (char *) cpumap;
        cpumap = NULL;
    }

    ret->online = online;
    ret->ret = cpunum;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    VIR_FREE(cpumap);
    return rv;
}

static int
lxcDispatchDomainOpenNamespace(virNetServerPtr server G_GNUC_UNUSED,
                               virNetServerClientPtr client,
                               virNetMessagePtr msg G_GNUC_UNUSED,
                               virNetMessageErrorPtr rerr,
                               lxc_domain_open_namespace_args *args)
{
    int rv = -1;
    int *fdlist = NULL;
    int ret;
    virDomainPtr dom = NULL;
    size_t i;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    ret = virDomainLxcOpenNamespace(dom,
                                    &fdlist,
                                    args->flags);
    if (ret < 0)
        goto cleanup;

    /* We shouldn't have received any from the client,
     * but in case they're playing games with us, prevent
     * a resource leak
     */
    for (i = 0; i < msg->nfds; i++)
        VIR_FORCE_CLOSE(msg->fds[i]);
    VIR_FREE(msg->fds);
    msg->nfds = 0;

    msg->fds = fdlist;
    msg->nfds = ret;

    rv = 1;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainGetJobStats(virNetServerPtr server G_GNUC_UNUSED,
                                virNetServerClientPtr client,
                                virNetMessagePtr msg G_GNUC_UNUSED,
                                virNetMessageErrorPtr rerr,
                                remote_domain_get_job_stats_args *args,
                                remote_domain_get_job_stats_ret *ret)
{
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetJobStats(dom, &ret->type, &params,
                             &nparams, args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_JOB_STATS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                0) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainMigrateBegin3Params(virNetServerPtr server G_GNUC_UNUSED,
                                        virNetServerClientPtr client,
                                        virNetMessagePtr msg G_GNUC_UNUSED,
                                        virNetMessageErrorPtr rerr,
                                        remote_domain_migrate_begin3_params_args *args,
                                        remote_domain_migrate_begin3_params_ret *ret)
{
    char *xml = NULL;
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->params.params_len > REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many migration parameters '%d' for limit '%d'"),
                       args->params.params_len, REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX);
        goto cleanup;
    }

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  0, &params, &nparams) < 0)
        goto cleanup;

    if (!(xml = virDomainMigrateBegin3Params(dom, params, nparams,
                                             &cookieout, &cookieoutlen,
                                             args->flags)))
        goto cleanup;

    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;
    ret->xml = xml;

    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}

static int
remoteDispatchDomainMigratePrepare3Params(virNetServerPtr server G_GNUC_UNUSED,
                                          virNetServerClientPtr client,
                                          virNetMessagePtr msg G_GNUC_UNUSED,
                                          virNetMessageErrorPtr rerr,
                                          remote_domain_migrate_prepare3_params_args *args,
                                          remote_domain_migrate_prepare3_params_ret *ret)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    char **uri_out;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->params.params_len > REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many migration parameters '%d' for limit '%d'"),
                       args->params.params_len, REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX);
        goto cleanup;
    }

    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  0, &params, &nparams) < 0)
        goto cleanup;

    /* Wacky world of XDR ... */
    if (VIR_ALLOC(uri_out) < 0)
        goto cleanup;

    if (virDomainMigratePrepare3Params(conn, params, nparams,
                                       args->cookie_in.cookie_in_val,
                                       args->cookie_in.cookie_in_len,
                                       &cookieout, &cookieoutlen,
                                       uri_out, args->flags) < 0)
        goto cleanup;

    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;
    ret->uri_out = !*uri_out ? NULL : uri_out;

    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(uri_out);
    }
    return rv;
}

static int
remoteDispatchDomainMigratePrepareTunnel3Params(virNetServerPtr server G_GNUC_UNUSED,
                                                virNetServerClientPtr client,
                                                virNetMessagePtr msg,
                                                virNetMessageErrorPtr rerr,
                                                remote_domain_migrate_prepare_tunnel3_params_args *args,
                                                remote_domain_migrate_prepare_tunnel3_params_ret *ret)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    int rv = -1;
    virStreamPtr st = NULL;
    daemonClientStreamPtr stream = NULL;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->params.params_len > REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many migration parameters '%d' for limit '%d'"),
                       args->params.params_len, REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX);
        goto cleanup;
    }

    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  0, &params, &nparams) < 0)
        goto cleanup;

    if (!(st = virStreamNew(conn, VIR_STREAM_NONBLOCK)) ||
        !(stream = daemonCreateClientStream(client, st, remoteProgram,
                                            &msg->header, false)))
        goto cleanup;

    if (virDomainMigratePrepareTunnel3Params(conn, st, params, nparams,
                                             args->cookie_in.cookie_in_val,
                                             args->cookie_in.cookie_in_len,
                                             &cookieout, &cookieoutlen,
                                             args->flags) < 0)
        goto cleanup;

    if (daemonAddClientStream(client, stream, false) < 0)
        goto cleanup;

    ret->cookie_out.cookie_out_val = cookieout;
    ret->cookie_out.cookie_out_len = cookieoutlen;
    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(cookieout);
        if (stream) {
            virStreamAbort(st);
            daemonFreeClientStream(client, stream);
        } else {
            virObjectUnref(st);
        }
    }
    return rv;
}


static int
remoteDispatchDomainMigratePerform3Params(virNetServerPtr server G_GNUC_UNUSED,
                                          virNetServerClientPtr client,
                                          virNetMessagePtr msg G_GNUC_UNUSED,
                                          virNetMessageErrorPtr rerr,
                                          remote_domain_migrate_perform3_params_args *args,
                                          remote_domain_migrate_perform3_params_ret *ret)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    virDomainPtr dom = NULL;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    char *dconnuri;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->params.params_len > REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many migration parameters '%d' for limit '%d'"),
                       args->params.params_len, REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX);
        goto cleanup;
    }

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  0, &params, &nparams) < 0)
        goto cleanup;

    dconnuri = args->dconnuri == NULL ? NULL : *args->dconnuri;

    if (virDomainMigratePerform3Params(dom, dconnuri, params, nparams,
                                       args->cookie_in.cookie_in_val,
                                       args->cookie_in.cookie_in_len,
                                       &cookieout, &cookieoutlen,
                                       args->flags) < 0)
        goto cleanup;

    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;

    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainMigrateFinish3Params(virNetServerPtr server G_GNUC_UNUSED,
                                         virNetServerClientPtr client,
                                         virNetMessagePtr msg G_GNUC_UNUSED,
                                         virNetMessageErrorPtr rerr,
                                         remote_domain_migrate_finish3_params_args *args,
                                         remote_domain_migrate_finish3_params_ret *ret)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    virDomainPtr dom = NULL;
    char *cookieout = NULL;
    int cookieoutlen = 0;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->params.params_len > REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many migration parameters '%d' for limit '%d'"),
                       args->params.params_len, REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX);
        goto cleanup;
    }

    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  0, &params, &nparams) < 0)
        goto cleanup;

    dom = virDomainMigrateFinish3Params(conn, params, nparams,
                                        args->cookie_in.cookie_in_val,
                                        args->cookie_in.cookie_in_len,
                                        &cookieout, &cookieoutlen,
                                        args->flags, args->cancelled);
    if (!dom)
        goto cleanup;

    make_nonnull_domain(&ret->dom, dom);

    ret->cookie_out.cookie_out_len = cookieoutlen;
    ret->cookie_out.cookie_out_val = cookieout;

    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(cookieout);
    }
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchDomainMigrateConfirm3Params(virNetServerPtr server G_GNUC_UNUSED,
                                          virNetServerClientPtr client,
                                          virNetMessagePtr msg G_GNUC_UNUSED,
                                          virNetMessageErrorPtr rerr,
                                          remote_domain_migrate_confirm3_params_args *args)
{
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    virDomainPtr dom = NULL;
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->params.params_len > REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many migration parameters '%d' for limit '%d'"),
                       args->params.params_len, REMOTE_DOMAIN_MIGRATE_PARAM_LIST_MAX);
        goto cleanup;
    }

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virTypedParamsDeserialize((virTypedParameterRemotePtr) args->params.params_val,
                                  args->params.params_len,
                                  0, &params, &nparams) < 0)
        goto cleanup;

    if (virDomainMigrateConfirm3Params(dom, params, nparams,
                                       args->cookie_in.cookie_in_val,
                                       args->cookie_in.cookie_in_len,
                                       args->flags, args->cancelled) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    virTypedParamsFree(params, nparams);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchConnectGetCPUModelNames(virNetServerPtr server G_GNUC_UNUSED,
                                      virNetServerClientPtr client,
                                      virNetMessagePtr msg G_GNUC_UNUSED,
                                      virNetMessageErrorPtr rerr,
                                      remote_connect_get_cpu_model_names_args *args,
                                      remote_connect_get_cpu_model_names_ret *ret)
{
    int len, rv = -1;
    char **models = NULL;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    len = virConnectGetCPUModelNames(conn, args->arch,
                                     args->need_results ? &models : NULL,
                                     args->flags);
    if (len < 0)
        goto cleanup;

    if (len > REMOTE_CONNECT_CPU_MODELS_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many CPU models '%d' for limit '%d'"),
                       len, REMOTE_CONNECT_CPU_MODELS_MAX);
        goto cleanup;
    }

    if (len && models) {
        ret->models.models_val = models;
        ret->models.models_len = len;
        models = NULL;
    } else {
        ret->models.models_val = NULL;
        ret->models.models_len = 0;
    }

    ret->ret = len;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virStringListFree(models);
    return rv;
}


static int
remoteDispatchDomainCreateXMLWithFiles(virNetServerPtr server G_GNUC_UNUSED,
                                       virNetServerClientPtr client,
                                       virNetMessagePtr msg G_GNUC_UNUSED,
                                       virNetMessageErrorPtr rerr,
                                       remote_domain_create_xml_with_files_args *args,
                                       remote_domain_create_xml_with_files_ret *ret)
{
    int rv = -1;
    virDomainPtr dom = NULL;
    int *files = NULL;
    unsigned int nfiles = 0;
    size_t i;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (VIR_ALLOC_N(files, msg->nfds) < 0)
        goto cleanup;
    for (i = 0; i < msg->nfds; i++) {
        if ((files[i] = virNetMessageDupFD(msg, i)) < 0)
            goto cleanup;
        nfiles++;
    }

    if ((dom = virDomainCreateXMLWithFiles(conn, args->xml_desc,
                                           nfiles, files,
                                           args->flags)) == NULL)
        goto cleanup;

    make_nonnull_domain(&ret->dom, dom);

    rv = 0;

 cleanup:
    for (i = 0; i < nfiles; i++)
        VIR_FORCE_CLOSE(files[i]);
    VIR_FREE(files);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int remoteDispatchDomainCreateWithFiles(virNetServerPtr server G_GNUC_UNUSED,
                                               virNetServerClientPtr client,
                                               virNetMessagePtr msg G_GNUC_UNUSED,
                                               virNetMessageErrorPtr rerr,
                                               remote_domain_create_with_files_args *args,
                                               remote_domain_create_with_files_ret *ret)
{
    int rv = -1;
    virDomainPtr dom = NULL;
    int *files = NULL;
    unsigned int nfiles = 0;
    size_t i;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (VIR_ALLOC_N(files, msg->nfds) < 0)
        goto cleanup;
    for (i = 0; i < msg->nfds; i++) {
        if ((files[i] = virNetMessageDupFD(msg, i)) < 0)
            goto cleanup;
        nfiles++;
    }

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainCreateWithFiles(dom,
                                 nfiles, files,
                                 args->flags) < 0)
        goto cleanup;

    make_nonnull_domain(&ret->dom, dom);

    rv = 0;

 cleanup:
    for (i = 0; i < nfiles; i++)
        VIR_FORCE_CLOSE(files[i]);
    VIR_FREE(files);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchConnectNetworkEventRegisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                             virNetServerClientPtr client,
                                             virNetMessagePtr msg G_GNUC_UNUSED,
                                             virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                             remote_connect_network_event_register_any_args *args,
                                             remote_connect_network_event_register_any_ret *ret)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    virNetworkPtr net = NULL;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetNetworkConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (args->net &&
        !(net = get_nonnull_network(conn, *args->net)))
        goto cleanup;

    if (args->eventID >= VIR_NETWORK_EVENT_ID_LAST || args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported network event ID %d"), args->eventID);
        goto cleanup;
    }

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = args->eventID;
    callback->callbackID = -1;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->networkEventCallbacks,
                           priv->nnetworkEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectNetworkEventRegisterAny(conn,
                                                        net,
                                                        args->eventID,
                                                        networkEventCallbacks[args->eventID],
                                                        ref,
                                                        remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->networkEventCallbacks,
                     priv->nnetworkEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;
    ret->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(net);
    return rv;
}


static int
remoteDispatchConnectNetworkEventDeregisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                               virNetServerClientPtr client,
                                               virNetMessagePtr msg G_GNUC_UNUSED,
                                               virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                               remote_connect_network_event_deregister_any_args *args)
{
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetNetworkConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->nnetworkEventCallbacks; i++) {
        if (priv->networkEventCallbacks[i]->callbackID == args->callbackID)
            break;
    }
    if (i == priv->nnetworkEventCallbacks) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network event callback %d not registered"),
                       args->callbackID);
        goto cleanup;
    }

    if (virConnectNetworkEventDeregisterAny(conn, args->callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->networkEventCallbacks, i,
                       priv->nnetworkEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchConnectStoragePoolEventRegisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                                 virNetServerClientPtr client,
                                                 virNetMessagePtr msg G_GNUC_UNUSED,
                                                 virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                 remote_connect_storage_pool_event_register_any_args *args,
                                                 remote_connect_storage_pool_event_register_any_ret *ret)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virStoragePoolPtr  pool = NULL;
    virConnectPtr conn = remoteGetStorageConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (args->pool &&
        !(pool = get_nonnull_storage_pool(conn, *args->pool)))
        goto cleanup;

    if (args->eventID >= VIR_STORAGE_POOL_EVENT_ID_LAST || args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported storage pool event ID %d"), args->eventID);
        goto cleanup;
    }

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = args->eventID;
    callback->callbackID = -1;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->storageEventCallbacks,
                           priv->nstorageEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectStoragePoolEventRegisterAny(conn,
                                                            pool,
                                                            args->eventID,
                                                            storageEventCallbacks[args->eventID],
                                                            ref,
                                                            remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->storageEventCallbacks,
                     priv->nstorageEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;
    ret->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(pool);
    return rv;
}

static int
remoteDispatchConnectStoragePoolEventDeregisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                               virNetServerClientPtr client,
                                               virNetMessagePtr msg G_GNUC_UNUSED,
                                               virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                               remote_connect_storage_pool_event_deregister_any_args *args)
{
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetStorageConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->nstorageEventCallbacks; i++) {
        if (priv->storageEventCallbacks[i]->callbackID == args->callbackID)
            break;
    }
    if (i == priv->nstorageEventCallbacks) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("storage pool event callback %d not registered"),
                       args->callbackID);
        goto cleanup;
    }

    if (virConnectStoragePoolEventDeregisterAny(conn, args->callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->storageEventCallbacks, i,
                       priv->nstorageEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchConnectNodeDeviceEventRegisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                                virNetServerClientPtr client,
                                                virNetMessagePtr msg G_GNUC_UNUSED,
                                                virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                remote_connect_node_device_event_register_any_args *args,
                                                remote_connect_node_device_event_register_any_ret *ret)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virNodeDevicePtr  dev = NULL;
    virConnectPtr conn = remoteGetNodeDevConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (args->dev &&
        !(dev = get_nonnull_node_device(conn, *args->dev)))
        goto cleanup;

    if (args->eventID >= VIR_NODE_DEVICE_EVENT_ID_LAST || args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported node device event ID %d"), args->eventID);
        goto cleanup;
    }

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = args->eventID;
    callback->callbackID = -1;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->nodeDeviceEventCallbacks,
                           priv->nnodeDeviceEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectNodeDeviceEventRegisterAny(conn,
                                                           dev,
                                                           args->eventID,
                                                           nodeDeviceEventCallbacks[args->eventID],
                                                           ref,
                                                           remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->nodeDeviceEventCallbacks,
                     priv->nnodeDeviceEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;
    ret->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dev);
    return rv;
}

static int
remoteDispatchConnectNodeDeviceEventDeregisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                                  virNetServerClientPtr client,
                                                  virNetMessagePtr msg G_GNUC_UNUSED,
                                                  virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                  remote_connect_node_device_event_deregister_any_args *args)
{
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetNodeDevConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->nnodeDeviceEventCallbacks; i++) {
        if (priv->nodeDeviceEventCallbacks[i]->callbackID == args->callbackID)
            break;
    }
    if (i == priv->nnodeDeviceEventCallbacks) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("node device event callback %d not registered"),
                       args->callbackID);
        goto cleanup;
    }

    if (virConnectNodeDeviceEventDeregisterAny(conn, args->callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->nodeDeviceEventCallbacks, i,
                       priv->nnodeDeviceEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchConnectSecretEventRegisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                            virNetServerClientPtr client,
                                            virNetMessagePtr msg G_GNUC_UNUSED,
                                            virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                            remote_connect_secret_event_register_any_args *args,
                                            remote_connect_secret_event_register_any_ret *ret)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virSecretPtr secret = NULL;
    virConnectPtr conn = remoteGetSecretConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (args->secret &&
        !(secret = get_nonnull_secret(conn, *args->secret)))
        goto cleanup;

    if (args->eventID >= VIR_SECRET_EVENT_ID_LAST || args->eventID < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported secret event ID %d"), args->eventID);
        goto cleanup;
    }

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(remoteProgram);
    callback->eventID = args->eventID;
    callback->callbackID = -1;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->secretEventCallbacks,
                           priv->nsecretEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectSecretEventRegisterAny(conn,
                                                       secret,
                                                       args->eventID,
                                                       secretEventCallbacks[args->eventID],
                                                       ref,
                                                       remoteEventCallbackFree)) < 0) {
        VIR_SHRINK_N(priv->secretEventCallbacks,
                     priv->nsecretEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;
    ret->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(secret);
    return rv;
}

static int
remoteDispatchConnectSecretEventDeregisterAny(virNetServerPtr server G_GNUC_UNUSED,
                                                  virNetServerClientPtr client,
                                                  virNetMessagePtr msg G_GNUC_UNUSED,
                                                  virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                  remote_connect_secret_event_deregister_any_args *args)
{
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetSecretConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->nsecretEventCallbacks; i++) {
        if (priv->secretEventCallbacks[i]->callbackID == args->callbackID)
            break;
    }
    if (i == priv->nsecretEventCallbacks) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("node device event callback %d not registered"),
                       args->callbackID);
        goto cleanup;
    }

    if (virConnectSecretEventDeregisterAny(conn, args->callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->secretEventCallbacks, i,
                       priv->nsecretEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
qemuDispatchConnectDomainMonitorEventRegister(virNetServerPtr server G_GNUC_UNUSED,
                                              virNetServerClientPtr client,
                                              virNetMessagePtr msg G_GNUC_UNUSED,
                                              virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                              qemu_connect_domain_monitor_event_register_args *args,
                                              qemu_connect_domain_monitor_event_register_ret *ret)
{
    int callbackID;
    int rv = -1;
    daemonClientEventCallbackPtr callback = NULL;
    daemonClientEventCallbackPtr ref;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virDomainPtr dom = NULL;
    const char *event = args->event ? *args->event : NULL;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    if (args->dom &&
        !(dom = get_nonnull_domain(conn, *args->dom)))
        goto cleanup;

    /* If we call register first, we could append a complete callback
     * to our array, but on OOM append failure, we'd have to then hope
     * deregister works to undo our register.  So instead we append an
     * incomplete callback to our array, then register, then fix up
     * our callback; but since VIR_APPEND_ELEMENT clears 'callback' on
     * success, we use 'ref' to save a copy of the pointer.  */
    if (VIR_ALLOC(callback) < 0)
        goto cleanup;
    callback->client = virObjectRef(client);
    callback->program = virObjectRef(qemuProgram);
    callback->eventID = -1;
    callback->callbackID = -1;
    ref = callback;
    if (VIR_APPEND_ELEMENT(priv->qemuEventCallbacks,
                           priv->nqemuEventCallbacks,
                           callback) < 0)
        goto cleanup;

    if ((callbackID = virConnectDomainQemuMonitorEventRegister(conn,
                                                               dom,
                                                               event,
                                                               remoteRelayDomainQemuMonitorEvent,
                                                               ref,
                                                               remoteEventCallbackFree,
                                                               args->flags)) < 0) {
        VIR_SHRINK_N(priv->qemuEventCallbacks,
                     priv->nqemuEventCallbacks, 1);
        callback = ref;
        goto cleanup;
    }

    ref->callbackID = callbackID;
    ret->callbackID = callbackID;

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    remoteEventCallbackFree(callback);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
qemuDispatchConnectDomainMonitorEventDeregister(virNetServerPtr server G_GNUC_UNUSED,
                                                virNetServerClientPtr client,
                                                virNetMessagePtr msg G_GNUC_UNUSED,
                                                virNetMessageErrorPtr rerr G_GNUC_UNUSED,
                                                qemu_connect_domain_monitor_event_deregister_args *args)
{
    int rv = -1;
    size_t i;
    struct daemonClientPrivate *priv =
        virNetServerClientGetPrivateData(client);
    virConnectPtr conn = remoteGetHypervisorConn(client);

    virMutexLock(&priv->lock);

    if (!conn)
        goto cleanup;

    for (i = 0; i < priv->nqemuEventCallbacks; i++) {
        if (priv->qemuEventCallbacks[i]->callbackID == args->callbackID)
            break;
    }
    if (i == priv->nqemuEventCallbacks) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("qemu monitor event callback %d not registered"),
                       args->callbackID);
        goto cleanup;
    }

    if (virConnectDomainQemuMonitorEventDeregister(conn,
                                                   args->callbackID) < 0)
        goto cleanup;

    VIR_DELETE_ELEMENT(priv->qemuEventCallbacks, i,
                       priv->nqemuEventCallbacks);

    rv = 0;

 cleanup:
    virMutexUnlock(&priv->lock);
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}

static int
remoteDispatchDomainGetTime(virNetServerPtr server G_GNUC_UNUSED,
                            virNetServerClientPtr client,
                            virNetMessagePtr msg G_GNUC_UNUSED,
                            virNetMessageErrorPtr rerr,
                            remote_domain_get_time_args *args,
                            remote_domain_get_time_ret *ret)
{
    int rv = -1;
    virDomainPtr dom = NULL;
    long long seconds;
    unsigned int nseconds;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetTime(dom, &seconds, &nseconds, args->flags) < 0)
        goto cleanup;

    ret->seconds = seconds;
    ret->nseconds = nseconds;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(dom);
    return rv;
}


static int
remoteDispatchNodeGetFreePages(virNetServerPtr server G_GNUC_UNUSED,
                               virNetServerClientPtr client,
                               virNetMessagePtr msg G_GNUC_UNUSED,
                               virNetMessageErrorPtr rerr,
                               remote_node_get_free_pages_args *args,
                               remote_node_get_free_pages_ret *ret)
{
    int rv = -1;
    int len;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->pages.pages_len * args->cellCount > REMOTE_NODE_MAX_CELLS) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("the result won't fit into REMOTE_NODE_MAX_CELLS"));
        goto cleanup;
    }

    /* Allocate return buffer. */
    if (VIR_ALLOC_N(ret->counts.counts_val,
                    args->pages.pages_len * args->cellCount) < 0)
        goto cleanup;

    if ((len = virNodeGetFreePages(conn,
                                   args->pages.pages_len,
                                   args->pages.pages_val,
                                   args->startCell,
                                   args->cellCount,
                                   (unsigned long long *) ret->counts.counts_val,
                                   args->flags)) <= 0)
        goto cleanup;

    ret->counts.counts_len = len;
    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        VIR_FREE(ret->counts.counts_val);
    }
    return rv;
}

/* Copy contents of virNetworkDHCPLeasePtr to remote_network_dhcp_lease */
static int
remoteSerializeDHCPLease(remote_network_dhcp_lease *lease_dst, virNetworkDHCPLeasePtr lease_src)
{
    char **mac_tmp = NULL;
    char **iaid_tmp = NULL;
    char **hostname_tmp = NULL;
    char **clientid_tmp = NULL;

    lease_dst->expirytime = lease_src->expirytime;
    lease_dst->type = lease_src->type;
    lease_dst->prefix = lease_src->prefix;

    lease_dst->iface = g_strdup(lease_src->iface);
    lease_dst->ipaddr = g_strdup(lease_src->ipaddr);

    if (lease_src->mac) {
        if (VIR_ALLOC(mac_tmp) < 0)
            goto error;
        *mac_tmp = g_strdup(lease_src->mac);
    }
    if (lease_src->iaid) {
        if (VIR_ALLOC(iaid_tmp) < 0)
            goto error;
        *iaid_tmp = g_strdup(lease_src->iaid);
    }
    if (lease_src->hostname) {
        if (VIR_ALLOC(hostname_tmp) < 0)
            goto error;
        *hostname_tmp = g_strdup(lease_src->hostname);
    }
    if (lease_src->clientid) {
        if (VIR_ALLOC(clientid_tmp) < 0)
            goto error;
        *clientid_tmp = g_strdup(lease_src->clientid);
    }

    lease_dst->mac = mac_tmp;
    lease_dst->iaid = iaid_tmp;
    lease_dst->hostname = hostname_tmp;
    lease_dst->clientid = clientid_tmp;

    return 0;

 error:
    if (mac_tmp)
        VIR_FREE(*mac_tmp);
    if (iaid_tmp)
        VIR_FREE(*iaid_tmp);
    if (hostname_tmp)
        VIR_FREE(*hostname_tmp);
    if (clientid_tmp)
        VIR_FREE(*clientid_tmp);
    VIR_FREE(mac_tmp);
    VIR_FREE(iaid_tmp);
    VIR_FREE(hostname_tmp);
    VIR_FREE(clientid_tmp);
    VIR_FREE(lease_dst->ipaddr);
    VIR_FREE(lease_dst->iface);
    return -1;
}


static int
remoteDispatchNetworkGetDHCPLeases(virNetServerPtr server G_GNUC_UNUSED,
                                   virNetServerClientPtr client,
                                   virNetMessagePtr msg G_GNUC_UNUSED,
                                   virNetMessageErrorPtr rerr,
                                   remote_network_get_dhcp_leases_args *args,
                                   remote_network_get_dhcp_leases_ret *ret)
{
    int rv = -1;
    size_t i;
    virNetworkDHCPLeasePtr *leases = NULL;
    virNetworkPtr net = NULL;
    int nleases = 0;
    virConnectPtr conn = remoteGetNetworkConn(client);

    if (!conn)
        goto cleanup;

    if (!(net = get_nonnull_network(conn, args->net)))
        goto cleanup;

    if ((nleases = virNetworkGetDHCPLeases(net,
                                           args->mac ? *args->mac : NULL,
                                           args->need_results ? &leases : NULL,
                                           args->flags)) < 0)
        goto cleanup;

    if (nleases > REMOTE_NETWORK_DHCP_LEASES_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Number of leases is %d, which exceeds max limit: %d"),
                       nleases, REMOTE_NETWORK_DHCP_LEASES_MAX);
        goto cleanup;
    }

    if (leases && nleases) {
        if (VIR_ALLOC_N(ret->leases.leases_val, nleases) < 0)
            goto cleanup;

        ret->leases.leases_len = nleases;

        for (i = 0; i < nleases; i++) {
            if (remoteSerializeDHCPLease(ret->leases.leases_val + i, leases[i]) < 0)
                goto cleanup;
        }

    } else {
        ret->leases.leases_len = 0;
        ret->leases.leases_val = NULL;
    }

    ret->ret = nleases;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    if (leases && nleases > 0)
        for (i = 0; i < nleases; i++)
            virNetworkDHCPLeaseFree(leases[i]);
    VIR_FREE(leases);
    virObjectUnref(net);
    return rv;
}


static int
remoteDispatchConnectGetAllDomainStats(virNetServerPtr server G_GNUC_UNUSED,
                                       virNetServerClientPtr client,
                                       virNetMessagePtr msg G_GNUC_UNUSED,
                                       virNetMessageErrorPtr rerr,
                                       remote_connect_get_all_domain_stats_args *args,
                                       remote_connect_get_all_domain_stats_ret *ret)
{
    int rv = -1;
    size_t i;
    virDomainStatsRecordPtr *retStats = NULL;
    int nrecords = 0;
    virDomainPtr *doms = NULL;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (args->doms.doms_len) {
        if (VIR_ALLOC_N(doms, args->doms.doms_len + 1) < 0)
            goto cleanup;

        for (i = 0; i < args->doms.doms_len; i++) {
            if (!(doms[i] = get_nonnull_domain(conn, args->doms.doms_val[i])))
                goto cleanup;
        }

        if ((nrecords = virDomainListGetStats(doms,
                                              args->stats,
                                              &retStats,
                                              args->flags)) < 0)
            goto cleanup;
    } else {
        if ((nrecords = virConnectGetAllDomainStats(conn,
                                                    args->stats,
                                                    &retStats,
                                                    args->flags)) < 0)
            goto cleanup;
    }

    if (nrecords) {
        if (nrecords > REMOTE_DOMAIN_LIST_MAX) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Number of domain stats records is %d, "
                             "which exceeds max limit: %d"),
                           nrecords, REMOTE_DOMAIN_LIST_MAX);
            goto cleanup;
        }

        if (VIR_ALLOC_N(ret->retStats.retStats_val, nrecords) < 0)
            goto cleanup;

        ret->retStats.retStats_len = nrecords;

        for (i = 0; i < nrecords; i++) {
            remote_domain_stats_record *dst = ret->retStats.retStats_val + i;

            make_nonnull_domain(&dst->dom, retStats[i]->dom);

            if (virTypedParamsSerialize(retStats[i]->params,
                                        retStats[i]->nparams,
                                        REMOTE_CONNECT_GET_ALL_DOMAIN_STATS_MAX,
                                        (virTypedParameterRemotePtr *) &dst->params.params_val,
                                        &dst->params.params_len,
                                        VIR_TYPED_PARAM_STRING_OKAY) < 0)
                goto cleanup;
        }
    } else {
        ret->retStats.retStats_len = 0;
        ret->retStats.retStats_val = NULL;
    }

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);
        xdr_free((xdrproc_t)xdr_remote_connect_get_all_domain_stats_ret,
                 (char *) ret);
    }

    virDomainStatsRecordListFree(retStats);
    virObjectListFree(doms);

    return rv;
}


static int
remoteDispatchNodeAllocPages(virNetServerPtr server G_GNUC_UNUSED,
                             virNetServerClientPtr client,
                             virNetMessagePtr msg G_GNUC_UNUSED,
                             virNetMessageErrorPtr rerr,
                             remote_node_alloc_pages_args *args,
                             remote_node_alloc_pages_ret *ret)
{
    int rv = -1;
    int len;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if ((len = virNodeAllocPages(conn,
                                 args->pageSizes.pageSizes_len,
                                 args->pageSizes.pageSizes_val,
                                 (unsigned long long *) args->pageCounts.pageCounts_val,
                                 args->startCell,
                                 args->cellCount,
                                 args->flags)) < 0)
        goto cleanup;

    ret->ret = len;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    return rv;
}


static int
remoteDispatchDomainGetFSInfo(virNetServerPtr server G_GNUC_UNUSED,
                              virNetServerClientPtr client,
                              virNetMessagePtr msg G_GNUC_UNUSED,
                              virNetMessageErrorPtr rerr,
                              remote_domain_get_fsinfo_args *args,
                              remote_domain_get_fsinfo_ret *ret)
{
    int rv = -1;
    size_t i, j;
    virDomainFSInfoPtr *info = NULL;
    virDomainPtr dom = NULL;
    remote_domain_fsinfo *dst;
    int ninfo = 0;
    size_t ndisk;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if ((ninfo = virDomainGetFSInfo(dom, &info, args->flags)) < 0)
        goto cleanup;

    if (ninfo > REMOTE_DOMAIN_FSINFO_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many mountpoints in fsinfo: %d for limit %d"),
                       ninfo, REMOTE_DOMAIN_FSINFO_MAX);
        goto cleanup;
    }

    if (ninfo) {
        if (VIR_ALLOC_N(ret->info.info_val, ninfo) < 0)
            goto cleanup;

        ret->info.info_len = ninfo;

        for (i = 0; i < ninfo; i++) {
            dst = &ret->info.info_val[i];
            dst->mountpoint = g_strdup(info[i]->mountpoint);

            dst->name = g_strdup(info[i]->name);

            dst->fstype = g_strdup(info[i]->fstype);

            ndisk = info[i]->ndevAlias;
            if (ndisk > REMOTE_DOMAIN_FSINFO_DISKS_MAX) {
                virReportError(VIR_ERR_RPC,
                               _("Too many disks in fsinfo: %zd for limit %d"),
                               ndisk, REMOTE_DOMAIN_FSINFO_DISKS_MAX);
                goto cleanup;
            }

            if (ndisk > 0) {
                if (VIR_ALLOC_N(dst->dev_aliases.dev_aliases_val, ndisk) < 0)
                    goto cleanup;

                for (j = 0; j < ndisk; j++)
                    dst->dev_aliases.dev_aliases_val[j] = g_strdup(info[i]->devAlias[j]);

                dst->dev_aliases.dev_aliases_len = ndisk;
            } else {
                dst->dev_aliases.dev_aliases_val = NULL;
                dst->dev_aliases.dev_aliases_len = 0;
            }
        }

    } else {
        ret->info.info_len = 0;
        ret->info.info_val = NULL;
    }

    ret->ret = ninfo;

    rv = 0;

 cleanup:
    if (rv < 0) {
        virNetMessageSaveError(rerr);

        if (ret->info.info_val && ninfo > 0) {
            for (i = 0; i < ninfo; i++) {
                dst = &ret->info.info_val[i];
                VIR_FREE(dst->mountpoint);
                if (dst->dev_aliases.dev_aliases_val) {
                    for (j = 0; j < dst->dev_aliases.dev_aliases_len; j++)
                        VIR_FREE(dst->dev_aliases.dev_aliases_val[j]);
                    VIR_FREE(dst->dev_aliases.dev_aliases_val);
                }
            }
            VIR_FREE(ret->info.info_val);
        }
    }
    virObjectUnref(dom);
    if (ninfo >= 0)
        for (i = 0; i < ninfo; i++)
            virDomainFSInfoFree(info[i]);
    VIR_FREE(info);

    return rv;
}


static int
remoteSerializeDomainInterface(virDomainInterfacePtr *ifaces,
                               unsigned int ifaces_count,
                               remote_domain_interface_addresses_ret *ret)
{
    size_t i, j;

    if (ifaces_count > REMOTE_DOMAIN_INTERFACE_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Number of interfaces, %d exceeds the max limit: %d"),
                       ifaces_count, REMOTE_DOMAIN_INTERFACE_MAX);
        return -1;
    }

    if (VIR_ALLOC_N(ret->ifaces.ifaces_val, ifaces_count) < 0)
        return -1;

    ret->ifaces.ifaces_len = ifaces_count;

    for (i = 0; i < ifaces_count; i++) {
        virDomainInterfacePtr iface = ifaces[i];
        remote_domain_interface *iface_ret = &(ret->ifaces.ifaces_val[i]);

        iface_ret->name = g_strdup(iface->name);

        if (iface->hwaddr) {
            if (VIR_ALLOC(iface_ret->hwaddr) < 0)
                goto cleanup;
            *iface_ret->hwaddr = g_strdup(iface->hwaddr);
        }

        if (iface->naddrs > REMOTE_DOMAIN_IP_ADDR_MAX) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Number of interfaces, %d exceeds the max limit: %d"),
                           iface->naddrs, REMOTE_DOMAIN_IP_ADDR_MAX);
            goto cleanup;
        }

        if (VIR_ALLOC_N(iface_ret->addrs.addrs_val,
                        iface->naddrs) < 0)
            goto cleanup;

        iface_ret->addrs.addrs_len = iface->naddrs;

        for (j = 0; j < iface->naddrs; j++) {
            virDomainIPAddressPtr ip_addr = &(iface->addrs[j]);
            remote_domain_ip_addr *ip_addr_ret =
                &(iface_ret->addrs.addrs_val[j]);

            ip_addr_ret->addr = g_strdup(ip_addr->addr);

            ip_addr_ret->prefix = ip_addr->prefix;
            ip_addr_ret->type = ip_addr->type;
        }
    }

    return 0;

 cleanup:
    if (ret->ifaces.ifaces_val) {
        for (i = 0; i < ifaces_count; i++) {
            remote_domain_interface *iface_ret = &(ret->ifaces.ifaces_val[i]);
            VIR_FREE(iface_ret->name);
            if (iface_ret->hwaddr) {
                VIR_FREE(*iface_ret->hwaddr);
                VIR_FREE(iface_ret->hwaddr);
            }
            for (j = 0; j < iface_ret->addrs.addrs_len; j++) {
                remote_domain_ip_addr *ip_addr =
                    &(iface_ret->addrs.addrs_val[j]);
                VIR_FREE(ip_addr->addr);
            }
        }
        VIR_FREE(ret->ifaces.ifaces_val);
    }

    return -1;
}


static int
remoteDispatchDomainInterfaceAddresses(virNetServerPtr server G_GNUC_UNUSED,
                                       virNetServerClientPtr client,
                                       virNetMessagePtr msg G_GNUC_UNUSED,
                                       virNetMessageErrorPtr rerr,
                                       remote_domain_interface_addresses_args *args,
                                       remote_domain_interface_addresses_ret *ret)
{
    size_t i;
    int rv = -1;
    virDomainPtr dom = NULL;
    virDomainInterfacePtr *ifaces = NULL;
    int ifaces_count = 0;
    virConnectPtr conn = remoteGetHypervisorConn(client);

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if ((ifaces_count = virDomainInterfaceAddresses(dom, &ifaces, args->source, args->flags)) < 0)
        goto cleanup;

    if (remoteSerializeDomainInterface(ifaces, ifaces_count, ret) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);

    virObjectUnref(dom);

    if (ifaces && ifaces_count > 0) {
        for (i = 0; i < ifaces_count; i++)
            virDomainInterfaceFree(ifaces[i]);
    }
    VIR_FREE(ifaces);

    return rv;
}


static int
remoteDispatchStorageVolGetInfoFlags(virNetServerPtr server G_GNUC_UNUSED,
                                     virNetServerClientPtr client,
                                     virNetMessagePtr msg G_GNUC_UNUSED,
                                     virNetMessageErrorPtr rerr,
                                     remote_storage_vol_get_info_flags_args *args,
                                     remote_storage_vol_get_info_flags_ret *ret)
{
    int rv = -1;
    virStorageVolPtr vol = NULL;
    virStorageVolInfo tmp;
    virConnectPtr conn = remoteGetStorageConn(client);

    if (!conn)
        goto cleanup;

    if (!(vol = get_nonnull_storage_vol(conn, args->vol)))
        goto cleanup;

    if (virStorageVolGetInfoFlags(vol, &tmp, args->flags) < 0)
        goto cleanup;

    ret->type = tmp.type;
    ret->capacity = tmp.capacity;
    ret->allocation = tmp.allocation;
    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(vol);
    return rv;
}


static int
remoteDispatchNetworkPortGetParameters(virNetServerPtr server G_GNUC_UNUSED,
                                       virNetServerClientPtr client,
                                       virNetMessagePtr msg G_GNUC_UNUSED,
                                       virNetMessageErrorPtr rerr,
                                       remote_network_port_get_parameters_args *args,
                                       remote_network_port_get_parameters_ret *ret)
{
    int rv = -1;
    virNetworkPortPtr port = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    virConnectPtr conn = remoteGetNetworkConn(client);

    if (!conn)
        goto cleanup;

    if (!(port = get_nonnull_network_port(conn, args->port)))
        goto cleanup;

    if (virNetworkPortGetParameters(port, &params, &nparams, args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_NETWORK_PORT_PARAMETERS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                args->flags) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virObjectUnref(port);
    virTypedParamsFree(params, nparams);
    return rv;
}


/*----- Helpers. -----*/

/* get_nonnull_domain and get_nonnull_network turn an on-wire
 * (name, uuid) pair into virDomainPtr or virNetworkPtr object.
 * virDomainPtr or virNetworkPtr cannot be NULL.
 *
 * NB. If these return NULL then the caller must return an error.
 */
static virDomainPtr
get_nonnull_domain(virConnectPtr conn, remote_nonnull_domain domain)
{
    return virGetDomain(conn, domain.name, BAD_CAST domain.uuid, domain.id);
}

static virNetworkPtr
get_nonnull_network(virConnectPtr conn, remote_nonnull_network network)
{
    return virGetNetwork(conn, network.name, BAD_CAST network.uuid);
}

static virNetworkPortPtr
get_nonnull_network_port(virConnectPtr conn, remote_nonnull_network_port port)
{
    virNetworkPortPtr ret;
    virNetworkPtr net;
    net = virGetNetwork(conn, port.net.name, BAD_CAST port.net.uuid);
    if (!net)
        return NULL;
    ret = virGetNetworkPort(net, BAD_CAST port.uuid);
    virObjectUnref(net);
    return ret;
}

static virInterfacePtr
get_nonnull_interface(virConnectPtr conn, remote_nonnull_interface iface)
{
    return virGetInterface(conn, iface.name, iface.mac);
}

static virStoragePoolPtr
get_nonnull_storage_pool(virConnectPtr conn, remote_nonnull_storage_pool pool)
{
    return virGetStoragePool(conn, pool.name, BAD_CAST pool.uuid,
                             NULL, NULL);
}

static virStorageVolPtr
get_nonnull_storage_vol(virConnectPtr conn, remote_nonnull_storage_vol vol)
{
    return virGetStorageVol(conn, vol.pool, vol.name, vol.key, NULL, NULL);
}

static virSecretPtr
get_nonnull_secret(virConnectPtr conn, remote_nonnull_secret secret)
{
    return virGetSecret(conn, BAD_CAST secret.uuid, secret.usageType, secret.usageID);
}

static virNWFilterPtr
get_nonnull_nwfilter(virConnectPtr conn, remote_nonnull_nwfilter nwfilter)
{
    return virGetNWFilter(conn, nwfilter.name, BAD_CAST nwfilter.uuid);
}

static virNWFilterBindingPtr
get_nonnull_nwfilter_binding(virConnectPtr conn, remote_nonnull_nwfilter_binding binding)
{
    return virGetNWFilterBinding(conn, binding.portdev, binding.filtername);
}

static virDomainCheckpointPtr
get_nonnull_domain_checkpoint(virDomainPtr dom, remote_nonnull_domain_checkpoint checkpoint)
{
    return virGetDomainCheckpoint(dom, checkpoint.name);
}

static virDomainSnapshotPtr
get_nonnull_domain_snapshot(virDomainPtr dom, remote_nonnull_domain_snapshot snapshot)
{
    return virGetDomainSnapshot(dom, snapshot.name);
}

static virNodeDevicePtr
get_nonnull_node_device(virConnectPtr conn, remote_nonnull_node_device dev)
{
    return virGetNodeDevice(conn, dev.name);
}

static void
make_nonnull_domain(remote_nonnull_domain *dom_dst, virDomainPtr dom_src)
{
    dom_dst->id = dom_src->id;
    dom_dst->name = g_strdup(dom_src->name);
    memcpy(dom_dst->uuid, dom_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_network(remote_nonnull_network *net_dst, virNetworkPtr net_src)
{
    net_dst->name = g_strdup(net_src->name);
    memcpy(net_dst->uuid, net_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_network_port(remote_nonnull_network_port *port_dst, virNetworkPortPtr port_src)
{
    port_dst->net.name = g_strdup(port_src->net->name);
    memcpy(port_dst->net.uuid, port_src->net->uuid, VIR_UUID_BUFLEN);
    memcpy(port_dst->uuid, port_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_interface(remote_nonnull_interface *interface_dst,
                       virInterfacePtr interface_src)
{
    interface_dst->name = g_strdup(interface_src->name);
    interface_dst->mac = g_strdup(interface_src->mac);
}

static void
make_nonnull_storage_pool(remote_nonnull_storage_pool *pool_dst, virStoragePoolPtr pool_src)
{
    pool_dst->name = g_strdup(pool_src->name);
    memcpy(pool_dst->uuid, pool_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_storage_vol(remote_nonnull_storage_vol *vol_dst, virStorageVolPtr vol_src)
{
    vol_dst->pool = g_strdup(vol_src->pool);
    vol_dst->name = g_strdup(vol_src->name);
    vol_dst->key = g_strdup(vol_src->key);
}

static void
make_nonnull_node_device(remote_nonnull_node_device *dev_dst, virNodeDevicePtr dev_src)
{
    dev_dst->name = g_strdup(dev_src->name);
}

static void
make_nonnull_secret(remote_nonnull_secret *secret_dst, virSecretPtr secret_src)
{
    memcpy(secret_dst->uuid, secret_src->uuid, VIR_UUID_BUFLEN);
    secret_dst->usageType = secret_src->usageType;
    secret_dst->usageID = g_strdup(secret_src->usageID);
}

static void
make_nonnull_nwfilter(remote_nonnull_nwfilter *nwfilter_dst, virNWFilterPtr nwfilter_src)
{
    nwfilter_dst->name = g_strdup(nwfilter_src->name);
    memcpy(nwfilter_dst->uuid, nwfilter_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_nwfilter_binding(remote_nonnull_nwfilter_binding *binding_dst, virNWFilterBindingPtr binding_src)
{
    binding_dst->portdev = g_strdup(binding_src->portdev);
    binding_dst->filtername = g_strdup(binding_src->filtername);
}

static void
make_nonnull_domain_checkpoint(remote_nonnull_domain_checkpoint *checkpoint_dst, virDomainCheckpointPtr checkpoint_src)
{
    checkpoint_dst->name = g_strdup(checkpoint_src->name);
    make_nonnull_domain(&checkpoint_dst->dom, checkpoint_src->domain);
}

static void
make_nonnull_domain_snapshot(remote_nonnull_domain_snapshot *snapshot_dst, virDomainSnapshotPtr snapshot_src)
{
    snapshot_dst->name = g_strdup(snapshot_src->name);
    make_nonnull_domain(&snapshot_dst->dom, snapshot_src->domain);
}

static int
remoteSerializeDomainDiskErrors(virDomainDiskErrorPtr errors,
                                int nerrors,
                                remote_domain_disk_error **ret_errors_val,
                                u_int *ret_errors_len)
{
    remote_domain_disk_error *val = NULL;
    size_t i = 0;

    if (VIR_ALLOC_N(val, nerrors) < 0)
        goto error;

    for (i = 0; i < nerrors; i++) {
        val[i].disk = g_strdup(errors[i].disk);
        val[i].error = errors[i].error;
    }

    *ret_errors_len = nerrors;
    *ret_errors_val = val;

    return 0;

 error:
    if (val) {
        size_t j;
        for (j = 0; j < i; j++)
            VIR_FREE(val[j].disk);
        VIR_FREE(val);
    }
    return -1;
}

static int
remoteDispatchDomainGetGuestInfo(virNetServerPtr server G_GNUC_UNUSED,
                                 virNetServerClientPtr client,
                                 virNetMessagePtr msg G_GNUC_UNUSED,
                                 virNetMessageErrorPtr rerr,
                                 remote_domain_get_guest_info_args *args,
                                 remote_domain_get_guest_info_ret *ret)
{
    int rv = -1;
    virConnectPtr conn = remoteGetHypervisorConn(client);
    virDomainPtr dom = NULL;
    virTypedParameterPtr params = NULL;
    int nparams = 0;

    if (!conn)
        goto cleanup;

    if (!(dom = get_nonnull_domain(conn, args->dom)))
        goto cleanup;

    if (virDomainGetGuestInfo(dom, args->types, &params, &nparams, args->flags) < 0)
        goto cleanup;

    if (virTypedParamsSerialize(params, nparams,
                                REMOTE_DOMAIN_GUEST_INFO_PARAMS_MAX,
                                (virTypedParameterRemotePtr *) &ret->params.params_val,
                                &ret->params.params_len,
                                VIR_TYPED_PARAM_STRING_OKAY) < 0)
        goto cleanup;

    rv = 0;

 cleanup:
    if (rv < 0)
        virNetMessageSaveError(rerr);
    virTypedParamsFree(params, nparams);
    virObjectUnref(dom);

    return rv;
}
