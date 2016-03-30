/*
 * qemu_command.h: QEMU command generation
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

#ifndef __QEMU_COMMAND_H__
# define __QEMU_COMMAND_H__

# include "domain_addr.h"
# include "domain_conf.h"
# include "vircommand.h"
# include "capabilities.h"
# include "qemu_conf.h"
# include "qemu_domain.h"
# include "qemu_domain_address.h"
# include "qemu_capabilities.h"
# include "logging/log_manager.h"

/* Config type for XML import/export conversions */
# define QEMU_CONFIG_FORMAT_ARGV "qemu-argv"

# define QEMU_DRIVE_HOST_PREFIX "drive-"
# define QEMU_FSDEV_HOST_PREFIX "fsdev-"

VIR_ENUM_DECL(qemuVideo)

typedef struct _qemuBuildCommandLineCallbacks qemuBuildCommandLineCallbacks;
typedef qemuBuildCommandLineCallbacks *qemuBuildCommandLineCallbacksPtr;
struct _qemuBuildCommandLineCallbacks {
    char *(*qemuGetSCSIDeviceSgName) (const char *sysfs_prefix,
                                      const char *adapter,
                                      unsigned int bus,
                                      unsigned int target,
                                      unsigned long long unit);
};

extern qemuBuildCommandLineCallbacks buildCommandLineCallbacks;

char *qemuBuildObjectCommandlineFromJSON(const char *type,
                                         const char *alias,
                                         virJSONValuePtr props);

virCommandPtr qemuBuildCommandLine(virConnectPtr conn,
                                   virQEMUDriverPtr driver,
                                   virLogManagerPtr logManager,
                                   virDomainDefPtr def,
                                   virDomainChrSourceDefPtr monitor_chr,
                                   bool monitor_json,
                                   virQEMUCapsPtr qemuCaps,
                                   const char *migrateURI,
                                   virDomainSnapshotObjPtr snapshot,
                                   virNetDevVPortProfileOp vmop,
                                   qemuBuildCommandLineCallbacksPtr callbacks,
                                   bool standalone,
                                   bool enableFips,
                                   virBitmapPtr nodeset,
                                   size_t *nnicindexes,
                                   int **nicindexes,
                                   const char *domainLibDir,
                                   const char *domainChannelTargetDir)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(11)
    ATTRIBUTE_NONNULL(17) ATTRIBUTE_NONNULL(18);

/* Generate '-device' string for chardev device */
int
qemuBuildChrDeviceStr(char **deviceStr,
                      const virDomainDef *vmdef,
                      virDomainChrDefPtr chr,
                      virQEMUCapsPtr qemuCaps);

/* With vlan == -1, use netdev syntax, else old hostnet */
char *qemuBuildHostNetStr(virDomainNetDefPtr net,
                          virQEMUDriverPtr driver,
                          char type_sep,
                          int vlan,
                          char **tapfd,
                          size_t tapfdSize,
                          char **vhostfd,
                          size_t vhostfdSize);

/* Legacy, pre device support */
char *qemuBuildNicStr(virDomainNetDefPtr net,
                      const char *prefix,
                      int vlan);

/* Current, best practice */
char *qemuBuildNicDevStr(virDomainDefPtr def,
                         virDomainNetDefPtr net,
                         int vlan,
                         int bootindex,
                         size_t vhostfdSize,
                         virQEMUCapsPtr qemuCaps);

char *qemuDeviceDriveHostAlias(virDomainDiskDefPtr disk,
                               virQEMUCapsPtr qemuCaps);

/* Both legacy & current support */
char *qemuBuildDriveStr(virConnectPtr conn,
                        virDomainDiskDefPtr disk,
                        bool bootable,
                        virQEMUCapsPtr qemuCaps);

/* Current, best practice */
char *qemuBuildDriveDevStr(const virDomainDef *def,
                           virDomainDiskDefPtr disk,
                           int bootindex,
                           virQEMUCapsPtr qemuCaps);

/* Current, best practice */
char *qemuBuildControllerDevStr(const virDomainDef *domainDef,
                                virDomainControllerDefPtr def,
                                virQEMUCapsPtr qemuCaps,
                                int *nusbcontroller);

char *qemuBuildMemballoonDevStr(const virDomainDef *domainDef,
                                virDomainMemballoonDefPtr dev,
                                virQEMUCapsPtr qemuCaps);

int qemuBuildMemoryBackendStr(unsigned long long size,
                              unsigned long long pagesize,
                              int guestNode,
                              virBitmapPtr userNodeset,
                              virBitmapPtr autoNodeset,
                              virDomainDefPtr def,
                              virQEMUCapsPtr qemuCaps,
                              virQEMUDriverConfigPtr cfg,
                              const char **backendType,
                              virJSONValuePtr *backendProps,
                              bool force);

char *qemuBuildMemoryDeviceStr(virDomainMemoryDefPtr mem);

/* Current, best practice */
char *qemuBuildPCIHostdevDevStr(const virDomainDef *def,
                                virDomainHostdevDefPtr dev,
                                int bootIndex,
                                const char *configfd,
                                virQEMUCapsPtr qemuCaps);

char *qemuBuildRNGDevStr(const virDomainDef *def,
                         virDomainRNGDefPtr dev,
                         virQEMUCapsPtr qemuCaps);
int qemuBuildRNGBackendProps(virDomainRNGDefPtr rng,
                             virQEMUCapsPtr qemuCaps,
                             const char **type,
                             virJSONValuePtr *props);

char *qemuBuildShmemDevStr(virDomainDefPtr def,
                           virDomainShmemDefPtr shmem,
                           virQEMUCapsPtr qemuCaps);
char *qemuBuildShmemBackendStr(virLogManagerPtr logManager,
                               virCommandPtr cmd,
                               virDomainDefPtr def,
                               virDomainShmemDefPtr shmem,
                               virQEMUCapsPtr qemuCaps);


int qemuOpenPCIConfig(virDomainHostdevDefPtr dev);

/* Current, best practice */
char *qemuBuildUSBHostdevDevStr(const virDomainDef *def,
                                virDomainHostdevDefPtr dev,
                                virQEMUCapsPtr qemuCaps);

char *qemuBuildSCSIHostdevDrvStr(virConnectPtr conn,
                                 virDomainHostdevDefPtr dev,
                                 virQEMUCapsPtr qemuCaps,
                                 qemuBuildCommandLineCallbacksPtr callbacks)
    ATTRIBUTE_NONNULL(4);
char *qemuBuildSCSIHostdevDevStr(const virDomainDef *def,
                                 virDomainHostdevDefPtr dev,
                                 virQEMUCapsPtr qemuCaps);

char *qemuBuildRedirdevDevStr(const virDomainDef *def,
                              virDomainRedirdevDefPtr dev,
                              virQEMUCapsPtr qemuCaps);

int qemuNetworkPrepareDevices(virDomainDefPtr def);

int qemuGetDriveSourceString(virStorageSourcePtr src,
                             virConnectPtr conn,
                             char **source);

int qemuCheckDiskConfig(virDomainDiskDefPtr disk);

bool
qemuCheckFips(void);

bool qemuCheckCCWS390AddressSupport(const virDomainDef *def,
                                    virDomainDeviceInfo info,
                                    virQEMUCapsPtr qemuCaps,
                                    const char *devicename);

#endif /* __QEMU_COMMAND_H__*/
