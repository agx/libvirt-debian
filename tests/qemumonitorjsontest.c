/*
 * Copyright (C) 2011-2012 Red Hat, Inc.
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
#include "virthread.h"
#include "virerror.h"


#define VIR_FROM_THIS VIR_FROM_NONE

static int
testQemuMonitorJSONGetStatus(const void *data)
{
    virCapsPtr caps = (virCapsPtr)data;
    qemuMonitorTestPtr test = qemuMonitorTestNew(true, caps);
    int ret = -1;
    bool running = false;
    virDomainPausedReason reason = 0;

    if (!test)
        return -1;

    if (qemuMonitorTestAddItem(test, "query-status",
                               "{ "
                               "    \"return\": { "
                               "        \"status\": \"running\", "
                               "        \"singlestep\": false, "
                               "        \"running\": true "
                               "    } "
                               "}") < 0)
        goto cleanup;
    if (qemuMonitorTestAddItem(test, "query-status",
                               "{ "
                               "    \"return\": { "
                               "        \"singlestep\": false, "
                               "        \"running\": false "
                               "    } "
                               "}") < 0)
        goto cleanup;
    if (qemuMonitorTestAddItem(test, "query-status",
                               "{ "
                               "    \"return\": { "
                               "        \"status\": \"inmigrate\", "
                               "        \"singlestep\": false, "
                               "        \"running\": false "
                               "    } "
                               "}") < 0)
        goto cleanup;

    if (qemuMonitorGetStatus(qemuMonitorTestGetMonitor(test),
                             &running, &reason) < 0)
        goto cleanup;

    if (!running) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       "Running was not true");
        goto cleanup;
    }

    if (reason != VIR_DOMAIN_PAUSED_UNKNOWN) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Reason was unexpectedly set to %d", reason);
        goto cleanup;
    }

    if (qemuMonitorGetStatus(qemuMonitorTestGetMonitor(test),
                             &running, &reason) < 0)
        goto cleanup;

    if (running) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       "Running was not false");
        goto cleanup;
    }

    if (reason != VIR_DOMAIN_PAUSED_UNKNOWN) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Reason was unexpectedly set to %d", reason);
        goto cleanup;
    }

    if (qemuMonitorGetStatus(qemuMonitorTestGetMonitor(test),
                             &running, &reason) < 0)
        goto cleanup;

    if (running) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       "Running was not false");
        goto cleanup;
    }

    if (reason != VIR_DOMAIN_PAUSED_MIGRATION) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Reason was unexpectedly set to %d", reason);
        goto cleanup;
    }

    ret = 0;

cleanup:
    qemuMonitorTestFree(test);
    return ret;
}

static int
testQemuMonitorJSONGetVersion(const void *data)
{
    virCapsPtr caps = (virCapsPtr)data;
    qemuMonitorTestPtr test = qemuMonitorTestNew(true, caps);
    int ret = -1;
    int major;
    int minor;
    int micro;
    char *package;

    if (!test)
        return -1;

    if (qemuMonitorTestAddItem(test, "query-version",
                               "{ "
                               "  \"return\":{ "
                               "     \"qemu\":{ "
                               "        \"major\":1, "
                               "        \"minor\":2, "
                               "        \"micro\":3 "
                               "      },"
                               "     \"package\":\"\""
                               "  }"
                               "}") < 0)
        goto cleanup;

    if (qemuMonitorTestAddItem(test, "query-version",
                               "{ "
                               "  \"return\":{ "
                               "     \"qemu\":{ "
                               "        \"major\":0, "
                               "        \"minor\":11, "
                               "        \"micro\":6 "
                               "      },"
                               "     \"package\":\"2.283.el6\""
                               "  }"
                               "}") < 0)
        goto cleanup;

    if (qemuMonitorGetVersion(qemuMonitorTestGetMonitor(test),
                              &major, &minor, &micro,
                              &package) < 0)
        goto cleanup;

    if (major != 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Major %d was not 1", major);
        goto cleanup;
    }
    if (minor != 2) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Minor %d was not 2", major);
        goto cleanup;
    }
    if (micro != 3) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Micro %d was not 3", major);
        goto cleanup;
    }

    if (STRNEQ(package, "")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Package %s was not ''", package);
        goto cleanup;
    }

    if (qemuMonitorGetVersion(qemuMonitorTestGetMonitor(test),
                              &major, &minor, &micro,
                              &package) < 0)
        goto cleanup;

    if (major != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Major %d was not 0", major);
        goto cleanup;
    }
    if (minor != 11) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Minor %d was not 11", major);
        goto cleanup;
    }
    if (micro != 6) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Micro %d was not 6", major);
        goto cleanup;
    }

    if (STRNEQ(package, "2.283.el6")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "Package %s was not '2.283.el6'", package);
        goto cleanup;
    }

    ret = 0;

cleanup:
    qemuMonitorTestFree(test);
    return ret;
}

static int
testQemuMonitorJSONGetMachines(const void *data)
{
    virCapsPtr caps = (virCapsPtr)data;
    qemuMonitorTestPtr test = qemuMonitorTestNew(true, caps);
    int ret = -1;
    qemuMonitorMachineInfoPtr *info;
    int ninfo;
    const char *null = NULL;

    if (!test)
        return -1;

    if (qemuMonitorTestAddItem(test, "query-machines",
                               "{ "
                               "  \"return\": [ "
                               "   { "
                               "     \"name\": \"pc-1.0\" "
                               "   }, "
                               "   { "
                               "     \"name\": \"pc-1.1\" "
                               "   }, "
                               "   { "
                               "     \"name\": \"pc-1.2\", "
                               "     \"is-default\": true, "
                               "     \"alias\": \"pc\" "
                               "   } "
                               "  ]"
                               "}") < 0)
        goto cleanup;

    if ((ninfo = qemuMonitorGetMachines(qemuMonitorTestGetMonitor(test),
                                        &info)) < 0)
        goto cleanup;

    if (ninfo != 3) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "ninfo %d is not 3", ninfo);
        goto cleanup;
    }

#define CHECK(i, wantname, wantisDefault, wantalias)                    \
    do {                                                                \
        if (STRNEQ(info[i]->name, (wantname))) {                        \
            virReportError(VIR_ERR_INTERNAL_ERROR,                      \
                           "name %s is not %s",                         \
                           info[i]->name, (wantname));                  \
            goto cleanup;                                               \
        }                                                               \
        if (info[i]->isDefault != (wantisDefault)) {                    \
            virReportError(VIR_ERR_INTERNAL_ERROR,                      \
                           "isDefault %d is not %d",                    \
                           info[i]->isDefault, (wantisDefault));        \
            goto cleanup;                                               \
        }                                                               \
        if (STRNEQ_NULLABLE(info[i]->alias, (wantalias))) {             \
            virReportError(VIR_ERR_INTERNAL_ERROR,                      \
                           "alias %s is not %s",                        \
                           info[i]->alias, NULLSTR(wantalias));         \
            goto cleanup;                                               \
        }                                                               \
    } while (0)

    CHECK(0, "pc-1.0", false, null);
    CHECK(1, "pc-1.1", false, null);
    CHECK(2, "pc-1.2", true, "pc");

#undef CHECK

    ret = 0;

cleanup:
    qemuMonitorTestFree(test);
    return ret;
}


static int
testQemuMonitorJSONGetCPUDefinitions(const void *data)
{
    virCapsPtr caps = (virCapsPtr)data;
    qemuMonitorTestPtr test = qemuMonitorTestNew(true, caps);
    int ret = -1;
    char **cpus = NULL;
    int ncpus;

    if (!test)
        return -1;

    if (qemuMonitorTestAddItem(test, "query-cpu-definitions",
                               "{ "
                               "  \"return\": [ "
                               "   { "
                               "     \"name\": \"qemu64\" "
                               "   }, "
                               "   { "
                               "     \"name\": \"Opteron_G4\" "
                               "   }, "
                               "   { "
                               "     \"name\": \"Westmere\" "
                               "   } "
                               "  ]"
                               "}") < 0)
        goto cleanup;

    if ((ncpus = qemuMonitorGetCPUDefinitions(qemuMonitorTestGetMonitor(test),
                                              &cpus)) < 0)
        goto cleanup;

    if (ncpus != 3) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "ncpus %d is not 3", ncpus);
        goto cleanup;
    }

#define CHECK(i, wantname)                                              \
    do {                                                                \
        if (STRNEQ(cpus[i], (wantname))) {                              \
            virReportError(VIR_ERR_INTERNAL_ERROR,                      \
                           "name %s is not %s",                         \
                           cpus[i], (wantname));                        \
            goto cleanup;                                               \
        }                                                               \
    } while (0)

    CHECK(0, "qemu64");
    CHECK(1, "Opteron_G4");
    CHECK(2, "Westmere");

#undef CHECK

    ret = 0;

cleanup:
    qemuMonitorTestFree(test);
    return ret;
}


static int
testQemuMonitorJSONGetCommands(const void *data)
{
    virCapsPtr caps = (virCapsPtr)data;
    qemuMonitorTestPtr test = qemuMonitorTestNew(true, caps);
    int ret = -1;
    char **commands = NULL;
    int ncommands;

    if (!test)
        return -1;

    if (qemuMonitorTestAddItem(test, "query-commands",
                               "{ "
                               "  \"return\": [ "
                               "   { "
                               "     \"name\": \"system_wakeup\" "
                               "   }, "
                               "   { "
                               "     \"name\": \"cont\" "
                               "   }, "
                               "   { "
                               "     \"name\": \"quit\" "
                               "   } "
                               "  ]"
                               "}") < 0)
        goto cleanup;

    if ((ncommands = qemuMonitorGetCommands(qemuMonitorTestGetMonitor(test),
                                        &commands)) < 0)
        goto cleanup;

    if (ncommands != 3) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "ncommands %d is not 3", ncommands);
        goto cleanup;
    }

#define CHECK(i, wantname)                                              \
    do {                                                                \
        if (STRNEQ(commands[i], (wantname))) {                          \
            virReportError(VIR_ERR_INTERNAL_ERROR,                      \
                           "name %s is not %s",                         \
                           commands[i], (wantname));                    \
            goto cleanup;                                               \
        }                                                               \
    } while (0)

    CHECK(0, "system_wakeup");
    CHECK(1, "cont");
    CHECK(2, "quit");

#undef CHECK
    ret = 0;

cleanup:
    qemuMonitorTestFree(test);
    return ret;
}


static int
mymain(void)
{
    int ret = 0;
    virCapsPtr caps;

    if (virThreadInitialize() < 0)
        exit(EXIT_FAILURE);

    if (!(caps = testQemuCapsInit()))
        exit(EXIT_FAILURE);

    virEventRegisterDefaultImpl();

#define DO_TEST(name) \
    if (virtTestRun(# name, 1, testQemuMonitorJSON ## name, caps) < 0) \
        ret = -1

    DO_TEST(GetStatus);
    DO_TEST(GetVersion);
    DO_TEST(GetMachines);
    DO_TEST(GetCPUDefinitions);
    DO_TEST(GetCommands);

    virCapabilitiesFree(caps);

    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)
