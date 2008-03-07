/*
 * Copyright (C) 2007, 2008 Red Hat, Inc.
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Authors:
 *     Mark McLoughlin <markmc@redhat.com>
 */

#include <config.h>

#if WITH_QEMU

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#include "internal.h"
#include "iptables.h"
#include "util.h"

#define qemudLog(level, msg...) fprintf(stderr, msg)

enum {
    ADD = 0,
    REMOVE
};

typedef struct
{
    char  *rule;
    char **argv;
    int    command_idx;
} iptRule;

typedef struct
{
    char  *table;
    char  *chain;

    int      nrules;
    iptRule *rules;

#ifdef ENABLE_IPTABLES_LOKKIT

    char   dir[PATH_MAX];
    char   path[PATH_MAX];

#endif /* ENABLE_IPTABLES_LOKKIT */

} iptRules;

struct _iptablesContext
{
    iptRules *input_filter;
    iptRules *forward_filter;
    iptRules *nat_postrouting;
};

#ifdef ENABLE_IPTABLES_LOKKIT
static void
notifyRulesUpdated(const char *table,
                   const char *path)
{
    char arg[PATH_MAX];
    char *argv[4];

    snprintf(arg, sizeof(arg), "--custom-rules=ipv4:%s:%s", table, path);

    argv[0] = (char *) LOKKIT_PATH;
    argv[1] = (char *) "--nostart";
    argv[2] = arg;
    argv[3] = NULL;

    if (virRun(NULL, argv, NULL) < 0)
        qemudLog(QEMUD_WARN, _("Failed to run '" LOKKIT_PATH
                               " %s' : %s"), arg, strerror(errno));
}

static int
stripLine(char *str, int len, const char *line)
{
    char *s, *p;
    int changed;

    changed = 0;
    s = str;

    while ((p = strchr(s, '\n'))) {
        if (p == s || strncmp(s, line, p - s) != 0) {
            s = ++p;
            continue;
        }

        ++p;
        memmove(s, p, len - (p - str) + 1);
        len -= p - s;
        changed = 1;
    }

    if (strcmp(s, line) == 0) {
        *s = '\0';
        changed = 1;
    }

    return changed;
}

static void
notifyRulesRemoved(const char *table,
                   const char *path)
{
/* 10 MB limit on config file size as a sanity check */
#define MAX_FILE_LEN (1024*1024*10)

    char arg[PATH_MAX];
    char *content;
    int len;
    FILE *f = NULL;

    len = virFileReadAll(SYSCONF_DIR "/sysconfig/system-config-firewall",
                         MAX_FILE_LEN, &content);
    if (len < 0) {
        qemudLog(QEMUD_WARN, "%s", _("Failed to read " SYSCONF_DIR
                                     "/sysconfig/system-config-firewall"));
        return;
    }

    snprintf(arg, sizeof(arg), "--custom-rules=ipv4:%s:%s", table, path);

    if (!stripLine(content, len, arg)) {
        free(content);
        return;
    }

    if (!(f = fopen(SYSCONF_DIR "/sysconfig/system-config-firewall", "w")))
        goto write_error;

    if (fputs(content, f) == EOF)
        goto write_error;

    if (fclose(f) == EOF) {
        f = NULL;
        goto write_error;
    }

    free(content);

    return;

 write_error:
    qemudLog(QEMUD_WARN, _("Failed to write to " SYSCONF_DIR
                           "/sysconfig/system-config-firewall : %s"),
             strerror(errno));
    if (f)
        fclose(f);
    free(content);

#undef MAX_FILE_LEN
}

static int
writeRules(const char *path,
           const iptRule *rules,
           int nrules)
{
    char tmp[PATH_MAX];
    FILE *f;
    int istmp;
    int i;

    if (nrules == 0 && unlink(path) == 0)
        return 0;

    if (snprintf(tmp, PATH_MAX, "%s.new", path) >= PATH_MAX)
        return EINVAL;

    istmp = 1;

    if (!(f = fopen(tmp, "w"))) {
        istmp = 0;
        if (!(f = fopen(path, "w")))
            return errno;
    }

    for (i = 0; i < nrules; i++) {
        if (fputs(rules[i].rule, f) == EOF ||
            fputc('\n', f) == EOF) {
            fclose(f);
            if (istmp)
                unlink(tmp);
            return errno;
        }
    }

    fclose(f);

    if (istmp && rename(tmp, path) < 0) {
        unlink(tmp);
        return errno;
    }

    if (istmp)
        unlink(tmp);

    return 0;
}
#endif /* ENABLE_IPTABLES_LOKKIT */

static void
iptRulesSave(iptRules *rules)
{
#ifdef ENABLE_IPTABLES_LOKKIT
    int err;

    if ((err = virFileMakePath(rules->dir))) {
        qemudLog(QEMUD_WARN, _("Failed to create directory %s : %s"),
                 rules->dir, strerror(err));
        return;
    }

    if ((err = writeRules(rules->path, rules->rules, rules->nrules))) {
        qemudLog(QEMUD_WARN, _("Failed to saves iptables rules to %s : %s"),
                 rules->path, strerror(err));
        return;
    }

    if (rules->nrules > 0)
        notifyRulesUpdated(rules->table, rules->path);
    else
        notifyRulesRemoved(rules->table, rules->path);
#else
    (void) rules;
#endif /* ENABLE_IPTABLES_LOKKIT */
}

static void
iptRuleFree(iptRule *rule)
{
    free(rule->rule);
    rule->rule = NULL;

    if (rule->argv) {
        int i = 0;
        while (rule->argv[i])
            free(rule->argv[i++]);
        free(rule->argv);
        rule->argv = NULL;
    }
}

static int
iptRulesAppend(iptRules *rules,
               char *rule,
               char **argv,
               int command_idx)
{
    iptRule *r;

    if (!(r = realloc(rules->rules, sizeof(*r) * (rules->nrules+1)))) {
        int i = 0;
        while (argv[i])
            free(argv[i++]);
        free(argv);
        return ENOMEM;
    }

    rules->rules = r;

    rules->rules[rules->nrules].rule        = rule;
    rules->rules[rules->nrules].argv        = argv;
    rules->rules[rules->nrules].command_idx = command_idx;

    rules->nrules++;

    return 0;
}

static int
iptRulesRemove(iptRules *rules,
               char *rule)
{
    int i;

    for (i = 0; i < rules->nrules; i++)
        if (!strcmp(rules->rules[i].rule, rule))
            break;

    if (i >= rules->nrules)
        return EINVAL;

    iptRuleFree(&rules->rules[i]);

    memmove(&rules->rules[i],
            &rules->rules[i+1],
            (rules->nrules - i - 1) * sizeof (iptRule));

    rules->nrules--;

    return 0;
}

static void
iptRulesFree(iptRules *rules)
{
    int i;

    if (rules->table) {
        free(rules->table);
        rules->table = NULL;
    }

    if (rules->chain) {
        free(rules->chain);
        rules->chain = NULL;
    }


    if (rules->rules) {
        for (i = 0; i < rules->nrules; i++)
            iptRuleFree(&rules->rules[i]);

        free(rules->rules);
        rules->rules = NULL;

        rules->nrules = 0;
    }

#ifdef ENABLE_IPTABLES_LOKKIT
    rules->dir[0] = '\0';
    rules->path[0] = '\0';
#endif /* ENABLE_IPTABLES_LOKKIT */

    free(rules);
}

static iptRules *
iptRulesNew(const char *table,
            const char *chain)
{
    iptRules *rules;

    if (!(rules = calloc(1, sizeof (*rules))))
        return NULL;

    if (!(rules->table = strdup(table)))
        goto error;

    if (!(rules->chain = strdup(chain)))
        goto error;

    rules->rules = NULL;
    rules->nrules = 0;

#ifdef ENABLE_IPTABLES_LOKKIT
    if (virFileBuildPath(LOCAL_STATE_DIR "/lib/libvirt/iptables", table, NULL,
                         rules->dir, sizeof(rules->dir)) < 0)
        goto error;

    if (virFileBuildPath(rules->dir, chain, ".chain", rules->path, sizeof(rules->path)) < 0)
        goto error;
#endif /* ENABLE_IPTABLES_LOKKIT */

    return rules;

 error:
    iptRulesFree(rules);
    return NULL;
}

static char *
argvToString(char **argv)
{
    int len, i;
    char *ret, *p;

    for (len = 1, i = 0; argv[i]; i++)
        len += strlen(argv[i]) + 1;

    if (!(p = ret = (char *)malloc(len)))
        return NULL;

    for (i = 0; argv[i]; i++) {
        if (i != 0)
            *(p++) = ' ';

        strcpy(p, argv[i]);
        p += strlen(argv[i]);
    }

    *p = '\0';

    return ret;
}

static int
iptablesAddRemoveRule(iptRules *rules, int action, const char *arg, ...)
{
    va_list args;
    int retval = ENOMEM;
    char **argv;
    char *rule = NULL;
    const char *s;
    int n, command_idx;

    n = 1 + /* /sbin/iptables  */
        2 + /*   --table foo   */
        2 + /*   --insert bar  */
        1;  /*   arg           */

    va_start(args, arg);
    while ((s = va_arg(args, const char *)))
        n++;

    va_end(args);

    if (!(argv = calloc(n + 1, sizeof(*argv))))
        goto error;

    n = 0;

    if (!(argv[n++] = strdup(IPTABLES_PATH)))
        goto error;

    if (!(argv[n++] = strdup("--table")))
        goto error;

    if (!(argv[n++] = strdup(rules->table)))
        goto error;

    command_idx = n;

    if (!(argv[n++] = strdup("--insert")))
        goto error;

    if (!(argv[n++] = strdup(rules->chain)))
        goto error;

    if (!(argv[n++] = strdup(arg)))
        goto error;

    va_start(args, arg);

    while ((s = va_arg(args, const char *)))
        if (!(argv[n++] = strdup(s)))
            goto error;

    va_end(args);

    if (!(rule = argvToString(&argv[command_idx])))
        goto error;

    if (action == REMOVE) {
        free(argv[command_idx]);
        if (!(argv[command_idx] = strdup("--delete")))
            goto error;
    }

    if (virRun(NULL, argv, NULL) < 0) {
        retval = errno;
        goto error;
    }

    if (action == ADD) {
        retval = iptRulesAppend(rules, rule, argv, command_idx);
        rule = NULL;
        argv = NULL;
    } else {
        retval = iptRulesRemove(rules, rule);
    }

 error:
    free(rule);

    if (argv) {
        n = 0;
        while (argv[n])
            free(argv[n++]);
        free(argv);
    }

    return retval;
}

/**
 * iptablesContextNew:
 *
 * Create a new IPtable context
 *
 * Returns a pointer to the new structure or NULL in case of error
 */
iptablesContext *
iptablesContextNew(void)
{
    iptablesContext *ctx;

    if (!(ctx = calloc(1, sizeof (*ctx))))
        return NULL;

    if (!(ctx->input_filter = iptRulesNew("filter", "INPUT")))
        goto error;

    if (!(ctx->forward_filter = iptRulesNew("filter", "FORWARD")))
        goto error;

    if (!(ctx->nat_postrouting = iptRulesNew("nat", "POSTROUTING")))
        goto error;

    return ctx;

 error:
    iptablesContextFree(ctx);
    return NULL;
}

/**
 * iptablesContextFree:
 * @ctx: pointer to the IP table context
 *
 * Free the resources associated with an IP table context
 */
void
iptablesContextFree(iptablesContext *ctx)
{
    if (ctx->input_filter)
        iptRulesFree(ctx->input_filter);
    if (ctx->forward_filter)
        iptRulesFree(ctx->forward_filter);
    if (ctx->nat_postrouting)
        iptRulesFree(ctx->nat_postrouting);
    free(ctx);
}

/**
 * iptablesSaveRules:
 * @ctx: pointer to the IP table context
 *
 * Saves all the IP table rules associated with a context
 * to disk so that if iptables is restarted, the rules
 * will automatically be reload.
 */
void
iptablesSaveRules(iptablesContext *ctx)
{
    iptRulesSave(ctx->input_filter);
    iptRulesSave(ctx->forward_filter);
    iptRulesSave(ctx->nat_postrouting);
}

static void
iptRulesReload(iptRules *rules)
{
    int i;

    for (i = 0; i < rules->nrules; i++) {
        iptRule *rule = &rules->rules[i];
        char *orig;

        orig = rule->argv[rule->command_idx];
        rule->argv[rule->command_idx] = (char *) "--delete";

        if (virRun(NULL, rule->argv, NULL) < 0)
            qemudLog(QEMUD_WARN,
                     _("Failed to remove iptables rule '%s'"
                       " from chain '%s' in table '%s': %s"),
                     rule->rule, rules->chain, rules->table, strerror(errno));

        rule->argv[rule->command_idx] = orig;
    }

    for (i = 0; i < rules->nrules; i++)
        if (virRun(NULL, rules->rules[i].argv, NULL) < 0)
            qemudLog(QEMUD_WARN, _("Failed to add iptables rule '%s'"
                                   " to chain '%s' in table '%s': %s"),
                     rules->rules[i].rule, rules->chain, rules->table, strerror(errno));
}

/**
 * iptablesReloadRules:
 * @ctx: pointer to the IP table context
 *
 * Reloads all the IP table rules associated to a context
 */
void
iptablesReloadRules(iptablesContext *ctx)
{
    iptRulesReload(ctx->input_filter);
    iptRulesReload(ctx->forward_filter);
    iptRulesReload(ctx->nat_postrouting);
}

static int
iptablesInput(iptablesContext *ctx,
              const char *iface,
              int port,
              int action,
              int tcp)
{
    char portstr[32];

    snprintf(portstr, sizeof(portstr), "%d", port);
    portstr[sizeof(portstr) - 1] = '\0';

    return iptablesAddRemoveRule(ctx->input_filter,
                                 action,
                                 "--in-interface", iface,
                                 "--protocol", tcp ? "tcp" : "udp",
                                 "--destination-port", portstr,
                                 "--jump", "ACCEPT",
                                 NULL);
}

/**
 * iptablesAddTcpInput:
 * @ctx: pointer to the IP table context
 * @iface: the interface name
 * @port: the TCP port to add
 *
 * Add an input to the IP table allowing access to the given @port on
 * the given @iface interface for TCP packets
 *
 * Returns 0 in case of success or an error code in case of error
 */

int
iptablesAddTcpInput(iptablesContext *ctx,
                    const char *iface,
                    int port)
{
    return iptablesInput(ctx, iface, port, ADD, 1);
}

/**
 * iptablesRemoveTcpInput:
 * @ctx: pointer to the IP table context
 * @iface: the interface name
 * @port: the TCP port to remove
 *
 * Removes an input from the IP table, hence forbidding access to the given
 * @port on the given @iface interface for TCP packets
 *
 * Returns 0 in case of success or an error code in case of error
 */
int
iptablesRemoveTcpInput(iptablesContext *ctx,
                       const char *iface,
                       int port)
{
    return iptablesInput(ctx, iface, port, REMOVE, 1);
}

/**
 * iptablesAddUdpInput:
 * @ctx: pointer to the IP table context
 * @iface: the interface name
 * @port: the UDP port to add
 *
 * Add an input to the IP table allowing access to the given @port on
 * the given @iface interface for UDP packets
 *
 * Returns 0 in case of success or an error code in case of error
 */

int
iptablesAddUdpInput(iptablesContext *ctx,
                    const char *iface,
                    int port)
{
    return iptablesInput(ctx, iface, port, ADD, 0);
}

/**
 * iptablesRemoveUdpInput:
 * @ctx: pointer to the IP table context
 * @iface: the interface name
 * @port: the UDP port to remove
 *
 * Removes an input from the IP table, hence forbidding access to the given
 * @port on the given @iface interface for UDP packets
 *
 * Returns 0 in case of success or an error code in case of error
 */
int
iptablesRemoveUdpInput(iptablesContext *ctx,
                       const char *iface,
                       int port)
{
    return iptablesInput(ctx, iface, port, REMOVE, 0);
}


/* Allow all traffic coming from the bridge, with a valid network address
 * to proceed to WAN
 */
static int
iptablesForwardAllowOut(iptablesContext *ctx,
                         const char *network,
                         const char *iface,
                         const char *physdev,
                         int action)
{
    if (physdev && physdev[0]) {
        return iptablesAddRemoveRule(ctx->forward_filter,
                                     action,
                                     "--source", network,
                                     "--in-interface", iface,
                                     "--out-interface", physdev,
                                     "--jump", "ACCEPT",
                                     NULL);
    } else {
        return iptablesAddRemoveRule(ctx->forward_filter,
                                     action,
                                     "--source", network,
                                     "--in-interface", iface,
                                     "--jump", "ACCEPT",
                                     NULL);
    }
}

/**
 * iptablesAddForwardAllowOut:
 * @ctx: pointer to the IP table context
 * @network: the source network name
 * @iface: the source interface name
 * @physdev: the physical output device
 *
 * Add a rule to the IP table context to allow the traffic for the
 * network @network via interface @iface to be forwarded to
 * @physdev device. This allow the outbound traffic on a bridge.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesAddForwardAllowOut(iptablesContext *ctx,
                            const char *network,
                            const char *iface,
                            const char *physdev)
{
    return iptablesForwardAllowOut(ctx, network, iface, physdev, ADD);
}

/**
 * iptablesRemoveForwardAllowOut:
 * @ctx: pointer to the IP table context
 * @network: the source network name
 * @iface: the source interface name
 * @physdev: the physical output device
 *
 * Remove a rule from the IP table context hence forbidding forwarding
 * of the traffic for the network @network via interface @iface
 * to the @physdev device output. This stops the outbound traffic on a bridge.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesRemoveForwardAllowOut(iptablesContext *ctx,
                               const char *network,
                               const char *iface,
                               const char *physdev)
{
    return iptablesForwardAllowOut(ctx, network, iface, physdev, REMOVE);
}


/* Allow all traffic destined to the bridge, with a valid network address
 * and associated with an existing connection
 */
static int
iptablesForwardAllowIn(iptablesContext *ctx,
                       const char *network,
                       const char *iface,
                       const char *physdev,
                       int action)
{
    if (physdev && physdev[0]) {
        return iptablesAddRemoveRule(ctx->forward_filter,
                                     action,
                                     "--destination", network,
                                     "--in-interface", physdev,
                                     "--out-interface", iface,
                                     "--match", "state",
                                     "--state", "ESTABLISHED,RELATED",
                                     "--jump", "ACCEPT",
                                     NULL);
    } else {
        return iptablesAddRemoveRule(ctx->forward_filter,
                                     action,
                                     "--destination", network,
                                     "--out-interface", iface,
                                     "--match", "state",
                                     "--state", "ESTABLISHED,RELATED",
                                     "--jump", "ACCEPT",
                                     NULL);
    }
}

/**
 * iptablesAddForwardAllowIn:
 * @ctx: pointer to the IP table context
 * @network: the source network name
 * @iface: the output interface name
 * @physdev: the physical input device or NULL
 *
 * Add rules to the IP table context to allow the traffic for the
 * network @network on @physdev device to be forwarded to
 * interface @iface. This allow the inbound traffic on a bridge.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesAddForwardAllowIn(iptablesContext *ctx,
                          const char *network,
                          const char *iface,
                          const char *physdev)
{
    return iptablesForwardAllowIn(ctx, network, iface, physdev, ADD);
}

/**
 * iptablesRemoveForwardAllowIn:
 * @ctx: pointer to the IP table context
 * @network: the source network name
 * @iface: the output interface name
 * @physdev: the physical input device or NULL
 *
 * Remove rules from the IP table context hence forbidding the traffic for
 * network @network on @physdev device to be forwarded to
 * interface @iface. This stops the inbound traffic on a bridge.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesRemoveForwardAllowIn(iptablesContext *ctx,
                             const char *network,
                             const char *iface,
                             const char *physdev)
{
    return iptablesForwardAllowIn(ctx, network, iface, physdev, REMOVE);
}


/* Allow all traffic between guests on the same bridge,
 * with a valid network address
 */
static int
iptablesForwardAllowCross(iptablesContext *ctx,
                          const char *iface,
                          int action)
{
    return iptablesAddRemoveRule(ctx->forward_filter,
                                 action,
                                 "--in-interface", iface,
                                 "--out-interface", iface,
                                 "--jump", "ACCEPT",
                                 NULL);
}

/**
 * iptablesAddForwardAllowCross:
 * @ctx: pointer to the IP table context
 * @iface: the input/output interface name
 *
 * Add rules to the IP table context to allow traffic to cross that
 * interface. It allows all traffic between guests on the same bridge
 * represented by that interface.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesAddForwardAllowCross(iptablesContext *ctx,
                             const char *iface) {
    return iptablesForwardAllowCross(ctx, iface, ADD);
}

/**
 * iptablesRemoveForwardAllowCross:
 * @ctx: pointer to the IP table context
 * @iface: the input/output interface name
 *
 * Remove rules to the IP table context to block traffic to cross that
 * interface. It forbids traffic between guests on the same bridge
 * represented by that interface.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesRemoveForwardAllowCross(iptablesContext *ctx,
                                const char *iface) {
    return iptablesForwardAllowCross(ctx, iface, REMOVE);
}


/* Drop all traffic trying to forward from the bridge.
 * ie the bridge is the in interface
 */
static int
iptablesForwardRejectOut(iptablesContext *ctx,
                         const char *iface,
                         int action)
{
    return iptablesAddRemoveRule(ctx->forward_filter,
                                     action,
                                     "--in-interface", iface,
                                     "--jump", "REJECT",
                                     NULL);
}

/**
 * iptablesAddForwardRejectOut:
 * @ctx: pointer to the IP table context
 * @iface: the output interface name
 *
 * Add rules to the IP table context to forbid all traffic to that
 * interface. It forbids forwarding from the bridge to that interface.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesAddForwardRejectOut(iptablesContext *ctx,
                            const char *iface)
{
    return iptablesForwardRejectOut(ctx, iface, ADD);
}

/**
 * iptablesRemoveForwardRejectOut:
 * @ctx: pointer to the IP table context
 * @iface: the output interface name
 *
 * Remove rules from the IP table context forbidding all traffic to that
 * interface. It reallow forwarding from the bridge to that interface.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesRemoveForwardRejectOut(iptablesContext *ctx,
                               const char *iface)
{
    return iptablesForwardRejectOut(ctx, iface, REMOVE);
}




/* Drop all traffic trying to forward to the bridge.
 * ie the bridge is the out interface
 */
static int
iptablesForwardRejectIn(iptablesContext *ctx,
                        const char *iface,
                        int action)
{
    return iptablesAddRemoveRule(ctx->forward_filter,
                                 action,
                                 "--out-interface", iface,
                                 "--jump", "REJECT",
                                 NULL);
}

/**
 * iptablesAddForwardRejectIn:
 * @ctx: pointer to the IP table context
 * @iface: the input interface name
 *
 * Add rules to the IP table context to forbid all traffic from that
 * interface. It forbids forwarding from that interface to the bridge.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesAddForwardRejectIn(iptablesContext *ctx,
                           const char *iface)
{
    return iptablesForwardRejectIn(ctx, iface, ADD);
}

/**
 * iptablesRemoveForwardRejectIn:
 * @ctx: pointer to the IP table context
 * @iface: the input interface name
 *
 * Remove rules from the IP table context forbidding all traffic from that
 * interface. It allows forwarding from that interface to the bridge.
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesRemoveForwardRejectIn(iptablesContext *ctx,
                              const char *iface)
{
    return iptablesForwardRejectIn(ctx, iface, REMOVE);
}


/* Masquerade all traffic coming from the network associated
 * with the bridge
 */
static int
iptablesForwardMasquerade(iptablesContext *ctx,
                       const char *network,
                       const char *physdev,
                       int action)
{
    if (physdev && physdev[0]) {
        return iptablesAddRemoveRule(ctx->nat_postrouting,
                                     action,
                                     "--source", network,
                                     "--out-interface", physdev,
                                     "--jump", "MASQUERADE",
                                     NULL);
    } else {
        return iptablesAddRemoveRule(ctx->nat_postrouting,
                                     action,
                                     "--source", network,
                                     "--jump", "MASQUERADE",
                                     NULL);
    }
}

/**
 * iptablesAddForwardMasquerade:
 * @ctx: pointer to the IP table context
 * @network: the source network name
 * @physdev: the physical input device or NULL
 *
 * Add rules to the IP table context to allow masquerading
 * network @network on @physdev. This allow the bridge to
 * masquerade for that network (on @physdev).
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesAddForwardMasquerade(iptablesContext *ctx,
                             const char *network,
                             const char *physdev)
{
    return iptablesForwardMasquerade(ctx, network, physdev, ADD);
}

/**
 * iptablesRemoveForwardMasquerade:
 * @ctx: pointer to the IP table context
 * @network: the source network name
 * @physdev: the physical input device or NULL
 *
 * Remove rules from the IP table context to stop masquerading
 * network @network on @physdev. This stops the bridge from
 * masquerading for that network (on @physdev).
 *
 * Returns 0 in case of success or an error code otherwise
 */
int
iptablesRemoveForwardMasquerade(iptablesContext *ctx,
                                const char *network,
                                const char *physdev)
{
    return iptablesForwardMasquerade(ctx, network, physdev, REMOVE);
}

#endif /* WITH_QEMU */

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
