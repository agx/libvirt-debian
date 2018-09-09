/*
 * Copyright (C) 2011-2013 Red Hat, Inc.
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

#include "testutils.h"
#include "testutilsqemu.h"
#include "qemumonitortestutils.h"
#define __QEMU_CAPSPRIV_H_ALLOW__
#include "qemu/qemu_capspriv.h"
#define __QEMU_MONITOR_PRIV_H_ALLOW__
#include "qemu/qemu_monitor_priv.h"

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct _testQemuData testQemuData;
typedef testQemuData *testQemuDataPtr;
struct _testQemuData {
    virQEMUDriver driver;
    const char *archName;
    const char *base;
};


static int
testQemuCaps(const void *opaque)
{
    int ret = -1;
    testQemuData *data = (void *) opaque;
    char *repliesFile = NULL;
    char *capsFile = NULL;
    qemuMonitorTestPtr mon = NULL;
    virQEMUCapsPtr capsActual = NULL;
    char *actual = NULL;

    if (virAsprintf(&repliesFile, "%s/qemucapabilitiesdata/%s.%s.replies",
                    abs_srcdir, data->base, data->archName) < 0 ||
        virAsprintf(&capsFile, "%s/qemucapabilitiesdata/%s.%s.xml",
                    abs_srcdir, data->base, data->archName) < 0)
        goto cleanup;

    if (!(mon = qemuMonitorTestNewFromFileFull(repliesFile, &data->driver, NULL)))
        goto cleanup;

    if (!(capsActual = virQEMUCapsNew()) ||
        virQEMUCapsInitQMPMonitor(capsActual,
                                  qemuMonitorTestGetMonitor(mon)) < 0)
        goto cleanup;

    if (virQEMUCapsGet(capsActual, QEMU_CAPS_KVM)) {
        qemuMonitorResetCommandID(qemuMonitorTestGetMonitor(mon));
        if (virQEMUCapsInitQMPMonitorTCG(capsActual,
                                         qemuMonitorTestGetMonitor(mon)) < 0)
            goto cleanup;

        /* Fill microcodeVersion with a "random" value which is the file
         * length to provide a reproducible number for testing.
         */
        virQEMUCapsSetMicrocodeVersion(capsActual, virFileLength(repliesFile, -1));
    }

    if (!(actual = virQEMUCapsFormatCache(capsActual)))
        goto cleanup;

    if (virTestCompareToFile(actual, capsFile) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(repliesFile);
    VIR_FREE(capsFile);
    VIR_FREE(actual);
    qemuMonitorTestFree(mon);
    virObjectUnref(capsActual);
    return ret;
}


static int
testQemuCapsCopy(const void *opaque)
{
    int ret = -1;
    const testQemuData *data = opaque;
    char *capsFile = NULL;
    virCapsPtr caps = NULL;
    virQEMUCapsPtr orig = NULL;
    virQEMUCapsPtr copy = NULL;
    char *actual = NULL;

    if (virAsprintf(&capsFile, "%s/qemucapabilitiesdata/%s.%s.xml",
                    abs_srcdir, data->base, data->archName) < 0)
        goto cleanup;

    if (!(caps = virCapabilitiesNew(virArchFromString(data->archName),
                                    false, false)))
        goto cleanup;

    if (!(orig = qemuTestParseCapabilities(caps, capsFile)))
        goto cleanup;

    if (!(copy = virQEMUCapsNewCopy(orig)))
        goto cleanup;

    if (!(actual = virQEMUCapsFormatCache(copy)))
        goto cleanup;

    if (virTestCompareToFile(actual, capsFile) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(capsFile);
    virObjectUnref(caps);
    virObjectUnref(orig);
    virObjectUnref(copy);
    VIR_FREE(actual);
    return ret;
}


static int
mymain(void)
{
    int ret = 0;
    testQemuData data;

#if !WITH_YAJL
    fputs("libvirt not compiled with JSON support, skipping this test\n", stderr);
    return EXIT_AM_SKIP;
#endif

    if (virThreadInitialize() < 0 ||
        qemuTestDriverInit(&data.driver) < 0)
        return EXIT_FAILURE;

    virEventRegisterDefaultImpl();

#define DO_TEST(arch, name) \
    do { \
        data.archName = arch; \
        data.base = name; \
        if (virTestRun(name "(" arch ")", testQemuCaps, &data) < 0) \
            ret = -1; \
        if (virTestRun("copy " name "(" arch ")", \
                       testQemuCapsCopy, &data) < 0) \
            ret = -1; \
    } while (0)

    DO_TEST("x86_64", "caps_1.5.3");
    DO_TEST("x86_64", "caps_1.6.0");
    DO_TEST("x86_64", "caps_1.7.0");
    DO_TEST("x86_64", "caps_2.1.1");
    DO_TEST("x86_64", "caps_2.4.0");
    DO_TEST("x86_64", "caps_2.5.0");
    DO_TEST("x86_64", "caps_2.6.0");
    DO_TEST("x86_64", "caps_2.7.0");
    DO_TEST("x86_64", "caps_2.8.0");
    DO_TEST("x86_64", "caps_2.9.0");
    DO_TEST("x86_64", "caps_2.10.0");
    DO_TEST("x86_64", "caps_2.11.0");
    DO_TEST("x86_64", "caps_2.12.0");
    DO_TEST("x86_64", "caps_3.0.0");
    DO_TEST("aarch64", "caps_2.6.0");
    DO_TEST("aarch64", "caps_2.10.0");
    DO_TEST("aarch64", "caps_2.12.0");
    DO_TEST("ppc64", "caps_2.6.0");
    DO_TEST("ppc64", "caps_2.9.0");
    DO_TEST("ppc64", "caps_2.10.0");
    DO_TEST("ppc64", "caps_2.12.0");
    DO_TEST("ppc64", "caps_3.0.0");
    DO_TEST("s390x", "caps_2.7.0");
    DO_TEST("s390x", "caps_2.8.0");
    DO_TEST("s390x", "caps_2.9.0");
    DO_TEST("s390x", "caps_2.10.0");
    DO_TEST("s390x", "caps_2.11.0");
    DO_TEST("s390x", "caps_2.12.0");
    DO_TEST("riscv32", "caps_3.0.0");
    DO_TEST("riscv64", "caps_3.0.0");

    /*
     * Run "tests/qemucapsprobe /path/to/qemu/binary >foo.replies"
     * to generate updated or new *.replies data files.
     *
     * If you manually edit replies files you can run
     * "tests/qemucapsfixreplies foo.replies" to fix the replies ids.
     */

    qemuTestDriverFree(&data.driver);

    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain)
