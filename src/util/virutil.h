/*
 * virutil.h: common, generic utility functions
 *
 * Copyright (C) 2010-2014 Red Hat, Inc.
 * Copyright (C) 2006, 2007 Binary Karma
 * Copyright (C) 2006 Shuveb Hussain
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

#include "internal.h"
#include <unistd.h>
#include <sys/types.h>


int virSetBlocking(int fd, bool blocking) G_GNUC_WARN_UNUSED_RESULT;
int virSetNonBlock(int fd) G_GNUC_WARN_UNUSED_RESULT;
int virSetInherit(int fd, bool inherit) G_GNUC_WARN_UNUSED_RESULT;
int virSetCloseExec(int fd) G_GNUC_WARN_UNUSED_RESULT;
int virSetSockReuseAddr(int fd, bool fatal) G_GNUC_WARN_UNUSED_RESULT;

int virSetUIDGID(uid_t uid, gid_t gid, gid_t *groups, int ngroups);
int virSetUIDGIDWithCaps(uid_t uid, gid_t gid, gid_t *groups, int ngroups,
                         unsigned long long capBits,
                         bool clearExistingCaps);

void virWaitForDevices(void);

int virScaleInteger(unsigned long long *value, const char *suffix,
                    unsigned long long scale, unsigned long long limit)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

int virHexToBin(unsigned char c);

int virParseVersionString(const char *str, unsigned long *version,
                          bool allowMissing);

char *virFormatIntDecimal(char *buf, size_t buflen, int val)
    ATTRIBUTE_NONNULL(1) G_GNUC_WARN_UNUSED_RESULT;

unsigned long long
virFormatIntPretty(unsigned long long val,
                   const char **unit);

int virDiskNameParse(const char *name, int *disk, int *partition);
int virDiskNameToIndex(const char* str);
char *virIndexToDiskName(int idx, const char *prefix);

/* No-op workarounds for functionality missing in mingw.  */
#ifndef HAVE_GETUID
static inline int getuid(void)
{ return 0; }
#endif

#ifndef HAVE_GETEUID
static inline int geteuid(void)
{ return 0; }
#endif

#ifndef HAVE_GETGID
static inline int getgid(void)
{ return 0; }
#endif

#ifndef HAVE_GETEGID
static inline int getegid(void)
{ return 0; }
#endif

#ifdef FUNC_PTHREAD_SIGMASK_BROKEN
# undef pthread_sigmask
static inline int pthread_sigmask(int how,
                                  const void *set,
                                  void *old)
{
    (void) how;
    (void) set;
    (void) old;
    return 0;
}
#endif

char *virGetHostname(void);
char *virGetHostnameQuiet(void);

char *virGetUserDirectory(void);
char *virGetUserDirectoryByUID(uid_t uid);
char *virGetUserConfigDirectory(void);
char *virGetUserCacheDirectory(void);
char *virGetUserRuntimeDirectory(void);
char *virGetUserShell(uid_t uid);
char *virGetUserName(uid_t uid) G_GNUC_NO_INLINE;
char *virGetGroupName(gid_t gid) G_GNUC_NO_INLINE;
int virGetGroupList(uid_t uid, gid_t group, gid_t **groups)
    ATTRIBUTE_NONNULL(3);
int virGetUserID(const char *name,
                 uid_t *uid) G_GNUC_WARN_UNUSED_RESULT;
int virGetGroupID(const char *name,
                  gid_t *gid) G_GNUC_WARN_UNUSED_RESULT;

bool virDoesUserExist(const char *name);
bool virDoesGroupExist(const char *name);


bool virIsDevMapperDevice(const char *dev_name) ATTRIBUTE_NONNULL(1);

bool virValidateWWN(const char *wwn);

int virGetDeviceID(const char *path,
                   int *maj,
                   int *min);
int virSetDeviceUnprivSGIO(const char *path,
                           const char *sysfs_dir,
                           int unpriv_sgio);
int virGetDeviceUnprivSGIO(const char *path,
                           const char *sysfs_dir,
                           int *unpriv_sgio);
char *virGetUnprivSGIOSysfsPath(const char *path,
                                const char *sysfs_dir);

int virParseOwnershipIds(const char *label, uid_t *uidPtr, gid_t *gidPtr);


time_t virGetSelfLastChanged(void);
void virUpdateSelfLastChanged(const char *path);

long virGetSystemPageSize(void) G_GNUC_NO_INLINE;
long virGetSystemPageSizeKB(void) G_GNUC_NO_INLINE;

unsigned long long virMemoryLimitTruncate(unsigned long long value);
bool virMemoryLimitIsSet(unsigned long long value);
unsigned long long virMemoryMaxValue(bool ulong) G_GNUC_NO_INLINE;

bool virHostHasIOMMU(void);

char *virHostGetDRMRenderNode(void) G_GNUC_NO_INLINE;

/**
 * VIR_ASSIGN_IS_OVERFLOW:
 * @rvalue: value that is checked (evaluated twice)
 * @lvalue: value that the check is against (used in typeof())
 *
 * This macro assigns @lvalue to @rvalue and evaluates as true if the value of
 * @rvalue did not fit into the @lvalue.
 */
#define VIR_ASSIGN_IS_OVERFLOW(lvalue, rvalue) \
    (((lvalue) = (rvalue)) != (rvalue))
