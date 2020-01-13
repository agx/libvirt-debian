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
#define LIBVIRT_QEMU_CAPSPRIV_H_ALLOW
#include "qemu/qemu_capspriv.h"
#define LIBVIRT_QEMU_MONITOR_PRIV_H_ALLOW
#include "qemu/qemu_monitor_priv.h"
#define LIBVIRT_QEMU_PROCESSPRIV_H_ALLOW
#include "qemu/qemu_processpriv.h"

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct _testQemuData testQemuData;
typedef testQemuData *testQemuDataPtr;
struct _testQemuData {
    virQEMUDriver driver;
    const char *inputDir;
    const char *outputDir;
    const char *prefix;
    const char *version;
    const char *archName;
    const char *suffix;
    int ret;
};


static int
testQemuDataInit(testQemuDataPtr data)
{
    if (qemuTestDriverInit(&data->driver) < 0)
        return -1;

    data->outputDir = TEST_QEMU_CAPS_PATH;

    data->ret = 0;

    return 0;
}


static void
testQemuDataReset(testQemuDataPtr data)
{
    qemuTestDriverFree(&data->driver);
}


static int
testQemuCaps(const void *opaque)
{
    int ret = -1;
    testQemuData *data = (void *) opaque;
    char *repliesFile = NULL;
    char *capsFile = NULL;
    qemuMonitorTestPtr mon = NULL;
    virQEMUCapsPtr capsActual = NULL;
    char *binary = NULL;
    char *actual = NULL;
    unsigned int fakeMicrocodeVersion = 0;
    const char *p;

    repliesFile = g_strdup_printf("%s/%s_%s.%s.%s",
                                  data->inputDir, data->prefix, data->version,
                                  data->archName, data->suffix);
    capsFile = g_strdup_printf("%s/%s_%s.%s.xml",
                               data->outputDir, data->prefix, data->version,
                               data->archName);

    if (!(mon = qemuMonitorTestNewFromFileFull(repliesFile, &data->driver, NULL,
                                               NULL)))
        goto cleanup;

    if (qemuProcessQMPInitMonitor(qemuMonitorTestGetMonitor(mon)) < 0)
        goto cleanup;

    binary = g_strdup_printf("/usr/bin/qemu-system-%s",
                             data->archName);

    if (!(capsActual = virQEMUCapsNewBinary(binary)) ||
        virQEMUCapsInitQMPMonitor(capsActual,
                                  qemuMonitorTestGetMonitor(mon)) < 0)
        goto cleanup;

    if (virQEMUCapsGet(capsActual, QEMU_CAPS_KVM)) {
        qemuMonitorResetCommandID(qemuMonitorTestGetMonitor(mon));

        if (qemuProcessQMPInitMonitor(qemuMonitorTestGetMonitor(mon)) < 0)
            goto cleanup;

        if (virQEMUCapsInitQMPMonitorTCG(capsActual,
                                         qemuMonitorTestGetMonitor(mon)) < 0)
            goto cleanup;

        /* calculate fake microcode version based on filename for a reproducible
         * number for testing which does not change with the contents */
        for (p = data->archName; *p; p++)
            fakeMicrocodeVersion += *p;

        fakeMicrocodeVersion *= 100000;

        for (p = data->version; *p; p++)
            fakeMicrocodeVersion += *p;

        virQEMUCapsSetMicrocodeVersion(capsActual, fakeMicrocodeVersion);
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
    VIR_FREE(binary);
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
    virQEMUCapsPtr orig = NULL;
    virQEMUCapsPtr copy = NULL;
    char *actual = NULL;

    capsFile = g_strdup_printf("%s/%s_%s.%s.xml",
                               data->outputDir, data->prefix, data->version,
                               data->archName);

    if (!(orig = qemuTestParseCapabilitiesArch(
              virArchFromString(data->archName), capsFile)))
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
    virObjectUnref(orig);
    virObjectUnref(copy);
    VIR_FREE(actual);
    return ret;
}


static int
doCapsTest(const char *inputDir,
           const char *prefix,
           const char *version,
           const char *archName,
           const char *suffix,
           void *opaque)
{
    testQemuDataPtr data = (testQemuDataPtr) opaque;
    g_autofree char *title = NULL;
    g_autofree char *copyTitle = NULL;

    title = g_strdup_printf("%s (%s)", version, archName);
    copyTitle = g_strdup_printf("copy %s (%s)", version, archName);

    data->inputDir = inputDir;
    data->prefix = prefix;
    data->version = version;
    data->archName = archName;
    data->suffix = suffix;

    if (virTestRun(title, testQemuCaps, data) < 0)
        data->ret = -1;

    if (virTestRun(copyTitle, testQemuCapsCopy, data) < 0)
        data->ret = -1;

    return 0;
}


static int
mymain(void)
{
    testQemuData data;

    virEventRegisterDefaultImpl();

    if (testQemuDataInit(&data) < 0)
        return EXIT_FAILURE;

    if (testQemuCapsIterate(".replies", doCapsTest, &data) < 0)
        return EXIT_FAILURE;

    /*
     * Run "tests/qemucapsprobe /path/to/qemu/binary >foo.replies"
     * to generate updated or new *.replies data files.
     *
     * If you manually edit replies files you can run
     * "tests/qemucapsfixreplies foo.replies" to fix the replies ids.
     *
     * Once a replies file has been generated and tweaked if necessary,
     * you can drop it into tests/qemucapabilitiesdata/ (with a sensible
     * name - look at what's already there for inspiration) and test
     * programs will automatically pick it up.
     *
     * To generate the corresponding output files after a new replies
     * file has been added, run "VIR_TEST_REGENERATE_OUTPUT=1 make check".
     */

    testQemuDataReset(&data);

    return (data.ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain)
