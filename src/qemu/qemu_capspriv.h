/*
 * qemu_capspriv.h: private declarations for QEMU capabilities generation
 *
 * Copyright (C) 2015 Samsung Electronics Co. Ltd
 * Copyright (C) 2015 Pavel Fedin
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
 * Author: Pavel Fedin <p.fedin@samsung.com>
 */

#ifndef __QEMU_CAPSRIV_H_ALLOW__
# error "qemu_capspriv.h may only be included by qemu_capabilities.c or test suites"
#endif

#ifndef __QEMU_CAPSPRIV_H__
# define __QEMU_CAPSPRIV_H__

struct _virQEMUCapsCache {
    virMutex lock;
    virHashTablePtr binaries;
    char *libDir;
    char *cacheDir;
    uid_t runUid;
    gid_t runGid;
};

virQEMUCapsPtr virQEMUCapsNewCopy(virQEMUCapsPtr qemuCaps);

virQEMUCapsPtr
virQEMUCapsNewForBinaryInternal(virCapsPtr caps,
                                const char *binary,
                                const char *libDir,
                                const char *cacheDir,
                                uid_t runUid,
                                gid_t runGid,
                                bool qmpOnly);

int virQEMUCapsLoadCache(virCapsPtr caps,
                         virQEMUCapsPtr qemuCaps,
                         const char *filename,
                         time_t *qemuctime,
                         time_t *selfctime,
                         unsigned long *selfvers);
char *virQEMUCapsFormatCache(virQEMUCapsPtr qemuCaps,
                             time_t selfCTime,
                             unsigned long selfVersion);

void
virQEMUCapsSetArch(virQEMUCapsPtr qemuCaps,
                   virArch arch);

void
virQEMUCapsInitHostCPUModel(virQEMUCapsPtr qemuCaps,
                            virCapsPtr caps);
#endif
