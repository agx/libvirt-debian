/*
 * virsh.c: a Xen shell used to exercise the libvirt API
 *
 * Copyright (C) 2005, 2007-2008 Red Hat, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Daniel Veillard <veillard@redhat.com>
 * Karel Zak <kzak@redhat.com>
 * Daniel P. Berrange <berrange@redhat.com>
 *
 *
 * $Id: virsh.c,v 1.134 2008/02/27 16:14:44 rjones Exp $
 */

#include <config.h>

#include "libvirt/libvirt.h"
#include "libvirt/virterror.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/time.h>
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <time.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <test.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#ifdef HAVE_READLINE_READLINE_H
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "buf.h"
#include "console.h"
#include "util.h"
#include "util-lib.h"

static char *progname;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define VIRSH_MAX_XML_FILE 10*1024*1024

#define VSH_PROMPT_RW    "virsh # "
#define VSH_PROMPT_RO    "virsh > "

#define GETTIMEOFDAY(T) gettimeofday(T, NULL)
#define DIFF_MSEC(T, U) \
        ((((int) ((T)->tv_sec - (U)->tv_sec)) * 1000000.0 + \
          ((int) ((T)->tv_usec - (U)->tv_usec))) / 1000.0)

/**
 * The log configuration
 */
#define MSG_BUFFER    4096
#define SIGN_NAME     "virsh"
#define DIR_MODE      (S_IWUSR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)  /* 0755 */
#define FILE_MODE     (S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH)                                /* 0644 */
#define LOCK_MODE     (S_IWUSR | S_IRUSR)                                                    /* 0600 */
#define LVL_DEBUG     "DEBUG"
#define LVL_INFO      "INFO"
#define LVL_NOTICE    "NOTICE"
#define LVL_WARNING   "WARNING"
#define LVL_ERROR     "ERROR"

/**
 * vshErrorLevel:
 *
 * Indicates the level of an log message
 */
typedef enum {
    VSH_ERR_DEBUG = 0,
    VSH_ERR_INFO,
    VSH_ERR_NOTICE,
    VSH_ERR_WARNING,
    VSH_ERR_ERROR
} vshErrorLevel;

/*
 * The error handler for virtsh
 */
static void
virshErrorHandler(void *unused, virErrorPtr error)
{
    if ((unused != NULL) || (error == NULL))
        return;

    /* Suppress the VIR_ERR_NO_XEN error which fails as non-root */
    if ((error->code == VIR_ERR_NO_XEN) || (error->code == VIR_ERR_OK))
        return;

    virDefaultErrorFunc(error);
}

/*
 * virsh command line grammar:
 *
 *    command_line    =     <command>\n | <command>; <command>; ...
 *
 *    command         =    <keyword> <option> <data>
 *
 *    option          =     <bool_option> | <int_option> | <string_option>
 *    data            =     <string>
 *
 *    bool_option     =     --optionname
 *    int_option      =     --optionname <number>
 *    string_option   =     --optionname <string>
 *
 *    keyword         =     [a-zA-Z]
 *    number          =     [0-9]+
 *    string          =     [^[:blank:]] | "[[:alnum:]]"$
 *
 */

/*
 * vshCmdOptType - command option type
 */
typedef enum {
    VSH_OT_NONE = 0,            /* none */
    VSH_OT_BOOL,                /* boolean option */
    VSH_OT_STRING,              /* string option */
    VSH_OT_INT,                 /* int option */
    VSH_OT_DATA                 /* string data (as non-option) */
} vshCmdOptType;

/*
 * Command Option Flags
 */
#define VSH_OFLAG_NONE    0     /* without flags */
#define VSH_OFLAG_REQ    (1 << 1)       /* option required */

/* dummy */
typedef struct __vshControl vshControl;
typedef struct __vshCmd vshCmd;

/*
 * vshCmdInfo -- information about command
 */
typedef struct {
    const char *name;           /* name of information */
    const char *data;           /* information */
} vshCmdInfo;

/*
 * vshCmdOptDef - command option definition
 */
typedef struct {
    const char *name;           /* the name of option */
    vshCmdOptType type;         /* option type */
    int flag;                   /* flags */
    const char *help;           /* help string */
} vshCmdOptDef;

/*
 * vshCmdOpt - command options
 */
typedef struct vshCmdOpt {
    vshCmdOptDef *def;          /* pointer to relevant option */
    char *data;                 /* allocated data */
    struct vshCmdOpt *next;
} vshCmdOpt;

/*
 * vshCmdDef - command definition
 */
typedef struct {
    const char *name;
    int (*handler) (vshControl *, vshCmd *);    /* command handler */
    vshCmdOptDef *opts;         /* definition of command options */
    vshCmdInfo *info;           /* details about command */
} vshCmdDef;

/*
 * vshCmd - parsed command
 */
typedef struct __vshCmd {
    vshCmdDef *def;             /* command definition */
    vshCmdOpt *opts;            /* list of command arguments */
    struct __vshCmd *next;      /* next command */
} __vshCmd;

/*
 * vshControl
 */
typedef struct __vshControl {
    char *name;                 /* connection name */
    virConnectPtr conn;         /* connection to hypervisor (MAY BE NULL) */
    vshCmd *cmd;                /* the current command */
    char *cmdstr;               /* string with command */
#ifndef __MINGW32__
    uid_t uid;                  /* process owner */
#endif /* __MINGW32__ */
    int imode;                  /* interactive mode? */
    int quiet;                  /* quiet mode */
    int debug;                  /* print debug messages? */
    int timing;                 /* print timing info? */
    int readonly;               /* connect readonly (first time only, not
                                 * during explicit connect command)
                                 */
    char *logfile;              /* log file name */
    int log_fd;                 /* log file descriptor */
} __vshControl;


static vshCmdDef commands[];

static void vshError(vshControl * ctl, int doexit, const char *format, ...)
    ATTRIBUTE_FORMAT(printf, 3, 4);
static int vshInit(vshControl * ctl);
static int vshDeinit(vshControl * ctl);
static void vshUsage(vshControl * ctl, const char *cmdname);
static void vshOpenLogFile(vshControl *ctl);
static void vshOutputLogFile(vshControl *ctl, int log_level, const char *format, va_list ap);
static void vshCloseLogFile(vshControl *ctl);

static int vshParseArgv(vshControl * ctl, int argc, char **argv);

static const char *vshCmddefGetInfo(vshCmdDef * cmd, const char *info);
static vshCmdDef *vshCmddefSearch(const char *cmdname);
static int vshCmddefHelp(vshControl * ctl, const char *name, int withprog);

static vshCmdOpt *vshCommandOpt(vshCmd * cmd, const char *name);
static int vshCommandOptInt(vshCmd * cmd, const char *name, int *found);
static char *vshCommandOptString(vshCmd * cmd, const char *name,
                                 int *found);
#if 0
static int vshCommandOptStringList(vshCmd * cmd, const char *name, char ***data);
#endif
static int vshCommandOptBool(vshCmd * cmd, const char *name);

#define VSH_BYID     (1 << 1)
#define VSH_BYUUID   (1 << 2)
#define VSH_BYNAME   (1 << 3)

static virDomainPtr vshCommandOptDomainBy(vshControl * ctl, vshCmd * cmd,
                                          const char *optname, char **name, int flag);

/* default is lookup by Id, Name and UUID */
#define vshCommandOptDomain(_ctl, _cmd, _optname, _name)            \
    vshCommandOptDomainBy(_ctl, _cmd, _optname, _name,              \
                          VSH_BYID|VSH_BYUUID|VSH_BYNAME)

static virNetworkPtr vshCommandOptNetworkBy(vshControl * ctl, vshCmd * cmd,
                            const char *optname, char **name, int flag);

/* default is lookup by Name and UUID */
#define vshCommandOptNetwork(_ctl, _cmd, _optname, _name)           \
    vshCommandOptNetworkBy(_ctl, _cmd, _optname, _name,             \
                           VSH_BYUUID|VSH_BYNAME)

static virStoragePoolPtr vshCommandOptPoolBy(vshControl * ctl, vshCmd * cmd,
                            const char *optname, char **name, int flag);

/* default is lookup by Name and UUID */
#define vshCommandOptPool(_ctl, _cmd, _optname, _name)           \
    vshCommandOptPoolBy(_ctl, _cmd, _optname, _name,             \
                           VSH_BYUUID|VSH_BYNAME)

static virStorageVolPtr vshCommandOptVolBy(vshControl * ctl, vshCmd * cmd,
                                           const char *optname,
                                           const char *pooloptname,
                                           char **name, int flag);

/* default is lookup by Name and UUID */
#define vshCommandOptVol(_ctl, _cmd,_optname, _pooloptname, _name)   \
    vshCommandOptVolBy(_ctl, _cmd, _optname, _pooloptname, _name,     \
                           VSH_BYUUID|VSH_BYNAME)

static void vshPrintExtra(vshControl * ctl, const char *format, ...)
    ATTRIBUTE_FORMAT(printf, 2, 3);
static void vshDebug(vshControl * ctl, int level, const char *format, ...)
    ATTRIBUTE_FORMAT(printf, 3, 4);

/* XXX: add batch support */
#define vshPrint(_ctl, ...)   fprintf(stdout, __VA_ARGS__)

static const char *vshDomainStateToString(int state);
static const char *vshDomainVcpuStateToString(int state);
static int vshConnectionUsability(vshControl * ctl, virConnectPtr conn,
                                  int showerror);

static void *_vshMalloc(vshControl * ctl, size_t sz, const char *filename, int line);
#define vshMalloc(_ctl, _sz)    _vshMalloc(_ctl, _sz, __FILE__, __LINE__)

static void *_vshCalloc(vshControl * ctl, size_t nmemb, size_t sz, const char *filename, int line);
#define vshCalloc(_ctl, _nmemb, _sz)    _vshCalloc(_ctl, _nmemb, _sz, __FILE__, __LINE__)

static void *_vshRealloc(vshControl * ctl, void *ptr, size_t sz, const char *filename, int line);
#define vshRealloc(_ctl, _ptr, _sz)    _vshRealloc(_ctl, _ptr, _sz, __FILE__, __LINE__)

static char *_vshStrdup(vshControl * ctl, const char *s, const char *filename, int line);
#define vshStrdup(_ctl, _s)    _vshStrdup(_ctl, _s, __FILE__, __LINE__)


static int idsorter(const void *a, const void *b) {
  const int *ia = (const int *)a;
  const int *ib = (const int *)b;

  if (*ia > *ib)
    return 1;
  else if (*ia < *ib)
    return -1;
  return 0;
}
static int namesorter(const void *a, const void *b) {
  const char **sa = (const char**)a;
  const char **sb = (const char**)b;

  return strcasecmp(*sa, *sb);
}


/* ---------------
 * Commands
 * ---------------
 */

/*
 * "help" command
 */
static vshCmdInfo info_help[] = {
    {"syntax", "help [<command>]"},
    {"help", gettext_noop("print help")},
    {"desc", gettext_noop("Prints global help or command specific help.")},

    {NULL, NULL}
};

static vshCmdOptDef opts_help[] = {
    {"command", VSH_OT_DATA, 0, gettext_noop("name of command")},
    {NULL, 0, 0, NULL}
};

static int
cmdHelp(vshControl * ctl, vshCmd * cmd)
{
    const char *cmdname = vshCommandOptString(cmd, "command", NULL);

    if (!cmdname) {
        vshCmdDef *def;

        vshPrint(ctl, "%s", _("Commands:\n\n"));
        for (def = commands; def->name; def++)
            vshPrint(ctl, "    %-15s %s\n", def->name,
                     N_(vshCmddefGetInfo(def, "help")));
        return TRUE;
    }
    return vshCmddefHelp(ctl, cmdname, FALSE);
}

/*
 * "autostart" command
 */
static vshCmdInfo info_autostart[] = {
    {"syntax", "autostart [--disable] <domain>"},
    {"help", gettext_noop("autostart a domain")},
    {"desc",
     gettext_noop("Configure a domain to be automatically started at boot.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_autostart[] = {
    {"domain",  VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"disable", VSH_OT_BOOL, 0, gettext_noop("disable autostarting")},
    {NULL, 0, 0, NULL}
};

static int
cmdAutostart(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *name;
    int autostart;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    autostart = !vshCommandOptBool(cmd, "disable");

    if (virDomainSetAutostart(dom, autostart) < 0) {
        if (autostart)
	    vshError(ctl, FALSE, _("Failed to mark domain %s as autostarted"),
                     name);
	else
	    vshError(ctl, FALSE, _("Failed to unmark domain %s as autostarted"),
                     name);
        virDomainFree(dom);
        return FALSE;
    }

    if (autostart)
	vshPrint(ctl, _("Domain %s marked as autostarted\n"), name);
    else
	vshPrint(ctl, _("Domain %s unmarked as autostarted\n"), name);

    virDomainFree(dom);
    return TRUE;
}

/*
 * "connect" command
 */
static vshCmdInfo info_connect[] = {
    {"syntax", "connect [name] [--readonly]"},
    {"help", gettext_noop("(re)connect to hypervisor")},
    {"desc",
     gettext_noop("Connect to local hypervisor. This is built-in command after shell start up.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_connect[] = {
    {"name",     VSH_OT_DATA, 0, gettext_noop("hypervisor connection URI")},
    {"readonly", VSH_OT_BOOL, 0, gettext_noop("read-only connection")},
    {NULL, 0, 0, NULL}
};

static int
cmdConnect(vshControl * ctl, vshCmd * cmd)
{
    int ro = vshCommandOptBool(cmd, "readonly");

    if (ctl->conn) {
        if (virConnectClose(ctl->conn) != 0) {
            vshError(ctl, FALSE, "%s",
                     _("Failed to disconnect from the hypervisor"));
            return FALSE;
        }
        ctl->conn = NULL;
    }

    free(ctl->name);
    ctl->name = vshStrdup(ctl, vshCommandOptString(cmd, "name", NULL));

    if (!ro) {
        ctl->conn = virConnectOpen(ctl->name);
        ctl->readonly = 0;
    } else {
        ctl->conn = virConnectOpenReadOnly(ctl->name);
        ctl->readonly = 1;
    }

    if (!ctl->conn)
        vshError(ctl, FALSE, "%s", _("Failed to connect to the hypervisor"));

    return ctl->conn ? TRUE : FALSE;
}

/*
 * "console" command
 */
static vshCmdInfo info_console[] = {
    {"syntax", "console <domain>"},
    {"help", gettext_noop("connect to the guest console")},
    {"desc",
     gettext_noop("Connect the virtual serial console for the guest")},
    {NULL, NULL}
};

static vshCmdOptDef opts_console[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

#ifndef __MINGW32__

static int
cmdConsole(vshControl * ctl, vshCmd * cmd)
{
    xmlDocPtr xml = NULL;
    xmlXPathObjectPtr obj = NULL;
    xmlXPathContextPtr ctxt = NULL;
    virDomainPtr dom;
    int ret = FALSE;
    char *doc;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    doc = virDomainGetXMLDesc(dom, 0);
    if (!doc)
        goto cleanup;

    xml = xmlReadDoc((const xmlChar *) doc, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOWARNING);
    free(doc);
    if (!xml)
        goto cleanup;
    ctxt = xmlXPathNewContext(xml);
    if (!ctxt)
        goto cleanup;

    obj = xmlXPathEval(BAD_CAST "string(/domain/devices/console/@tty)", ctxt);
    if ((obj != NULL) && ((obj->type == XPATH_STRING) &&
                          (obj->stringval != NULL) && (obj->stringval[0] != 0))) {
        if (vshRunConsole((const char *)obj->stringval) == 0)
            ret = TRUE;
    } else {
        vshPrintExtra(ctl, "%s", _("No console available for domain\n"));
    }
    xmlXPathFreeObject(obj);

 cleanup:
    xmlXPathFreeContext(ctxt);
    if (xml)
        xmlFreeDoc(xml);
    virDomainFree(dom);
    return ret;
}

#else /* __MINGW32__ */

static int
cmdConsole(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    vshError (ctl, FALSE, "%s", _("console not implemented on this platform"));
    return FALSE;
}

#endif /* __MINGW32__ */

/*
 * "list" command
 */
static vshCmdInfo info_list[] = {
    {"syntax", "list [--inactive | --all]"},
    {"help", gettext_noop("list domains")},
    {"desc", gettext_noop("Returns list of domains.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_list[] = {
    {"inactive", VSH_OT_BOOL, 0, gettext_noop("list inactive domains")},
    {"all", VSH_OT_BOOL, 0, gettext_noop("list inactive & active domains")},
    {NULL, 0, 0, NULL}
};


static int
cmdList(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    int inactive = vshCommandOptBool(cmd, "inactive");
    int all = vshCommandOptBool(cmd, "all");
    int active = !inactive || all ? 1 : 0;
    int *ids = NULL, maxid = 0, i;
    char **names = NULL;
    int maxname = 0;
    inactive |= all;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (active) {
        maxid = virConnectNumOfDomains(ctl->conn);
        if (maxid < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list active domains"));
            return FALSE;
        }
        if (maxid) {
            ids = vshMalloc(ctl, sizeof(int) * maxid);

            if ((maxid = virConnectListDomains(ctl->conn, &ids[0], maxid)) < 0) {
                vshError(ctl, FALSE, "%s", _("Failed to list active domains"));
                free(ids);
                return FALSE;
            }

            qsort(&ids[0], maxid, sizeof(int), idsorter);
        }
    }
    if (inactive) {
        maxname = virConnectNumOfDefinedDomains(ctl->conn);
        if (maxname < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list inactive domains"));
            free(ids);
            return FALSE;
        }
        if (maxname) {
            names = vshMalloc(ctl, sizeof(char *) * maxname);

            if ((maxname = virConnectListDefinedDomains(ctl->conn, names, maxname)) < 0) {
                vshError(ctl, FALSE, "%s", _("Failed to list inactive domains"));
                free(ids);
                free(names);
                return FALSE;
            }

            qsort(&names[0], maxname, sizeof(char*), namesorter);
        }
    }
    vshPrintExtra(ctl, "%3s %-20s %s\n", _("Id"), _("Name"), _("State"));
    vshPrintExtra(ctl, "----------------------------------\n");

    for (i = 0; i < maxid; i++) {
        virDomainInfo info;
        virDomainPtr dom = virDomainLookupByID(ctl->conn, ids[i]);
        const char *state;

        /* this kind of work with domains is not atomic operation */
        if (!dom)
            continue;

        if (virDomainGetInfo(dom, &info) < 0)
            state = _("no state");
        else
            state = N_(vshDomainStateToString(info.state));

        vshPrint(ctl, "%3d %-20s %s\n",
                 virDomainGetID(dom),
                 virDomainGetName(dom),
                 state);
        virDomainFree(dom);
    }
    for (i = 0; i < maxname; i++) {
        virDomainInfo info;
        virDomainPtr dom = virDomainLookupByName(ctl->conn, names[i]);
        const char *state;

        /* this kind of work with domains is not atomic operation */
        if (!dom) {
            free(names[i]);
            continue;
        }

        if (virDomainGetInfo(dom, &info) < 0)
            state = _("no state");
        else
            state = N_(vshDomainStateToString(info.state));

        vshPrint(ctl, "%3s %-20s %s\n", "-", names[i], state);

        virDomainFree(dom);
        free(names[i]);
    }
    free(ids);
    free(names);
    return TRUE;
}

/*
 * "domstate" command
 */
static vshCmdInfo info_domstate[] = {
    {"syntax", "domstate <domain>"},
    {"help", gettext_noop("domain state")},
    {"desc", gettext_noop("Returns state about a running domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_domstate[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdDomstate(vshControl * ctl, vshCmd * cmd)
{
    virDomainInfo info;
    virDomainPtr dom;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    if (virDomainGetInfo(dom, &info) == 0)
        vshPrint(ctl, "%s\n",
                 N_(vshDomainStateToString(info.state)));
    else
        ret = FALSE;

    virDomainFree(dom);
    return ret;
}

/* "domblkstat" command
 */
static vshCmdInfo info_domblkstat[] = {
    {"syntax", "domblkstat <domain> <dev>"},
    {"help", gettext_noop("get device block stats for a domain")},
    {"desc", gettext_noop("Get device block stats for a running domain.")},
    {NULL,NULL}
};

static vshCmdOptDef opts_domblkstat[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"device", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("block device")},
    {NULL, 0, 0, NULL}
};

static int
cmdDomblkstat (vshControl *ctl, vshCmd *cmd)
{
    virDomainPtr dom;
    char *name, *device;
    struct _virDomainBlockStats stats;

    if (!vshConnectionUsability (ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain (ctl, cmd, "domain", &name)))
        return FALSE;

    if (!(device = vshCommandOptString (cmd, "device", NULL)))
        return FALSE;

    if (virDomainBlockStats (dom, device, &stats, sizeof stats) == -1) {
        vshError (ctl, FALSE, _("Failed to get block stats %s %s"),
                  name, device);
        virDomainFree(dom);
        return FALSE;
    }

    if (stats.rd_req >= 0)
        vshPrint (ctl, "%s rd_req %lld\n", device, stats.rd_req);

    if (stats.rd_bytes >= 0)
        vshPrint (ctl, "%s rd_bytes %lld\n", device, stats.rd_bytes);

    if (stats.wr_req >= 0)
        vshPrint (ctl, "%s wr_req %lld\n", device, stats.wr_req);

    if (stats.wr_bytes >= 0)
        vshPrint (ctl, "%s wr_bytes %lld\n", device, stats.wr_bytes);

    if (stats.errs >= 0)
        vshPrint (ctl, "%s errs %lld\n", device, stats.errs);

    virDomainFree(dom);
    return TRUE;
}

/* "domifstat" command
 */
static vshCmdInfo info_domifstat[] = {
    {"syntax", "domifstat <domain> <dev>"},
    {"help", gettext_noop("get network interface stats for a domain")},
    {"desc", gettext_noop("Get network interface stats for a running domain.")},
    {NULL,NULL}
};

static vshCmdOptDef opts_domifstat[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"interface", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("interface device")},
    {NULL, 0, 0, NULL}
};

static int
cmdDomIfstat (vshControl *ctl, vshCmd *cmd)
{
    virDomainPtr dom;
    char *name, *device;
    struct _virDomainInterfaceStats stats;

    if (!vshConnectionUsability (ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain (ctl, cmd, "domain", &name)))
        return FALSE;

    if (!(device = vshCommandOptString (cmd, "interface", NULL)))
        return FALSE;

    if (virDomainInterfaceStats (dom, device, &stats, sizeof stats) == -1) {
        vshError (ctl, FALSE, _("Failed to get interface stats %s %s"),
                  name, device);
        virDomainFree(dom);
        return FALSE;
    }

    if (stats.rx_bytes >= 0)
        vshPrint (ctl, "%s rx_bytes %lld\n", device, stats.rx_bytes);

    if (stats.rx_packets >= 0)
        vshPrint (ctl, "%s rx_packets %lld\n", device, stats.rx_packets);

    if (stats.rx_errs >= 0)
        vshPrint (ctl, "%s rx_errs %lld\n", device, stats.rx_errs);

    if (stats.rx_drop >= 0)
        vshPrint (ctl, "%s rx_drop %lld\n", device, stats.rx_drop);

    if (stats.tx_bytes >= 0)
        vshPrint (ctl, "%s tx_bytes %lld\n", device, stats.tx_bytes);

    if (stats.tx_packets >= 0)
        vshPrint (ctl, "%s tx_packets %lld\n", device, stats.tx_packets);

    if (stats.tx_errs >= 0)
        vshPrint (ctl, "%s tx_errs %lld\n", device, stats.tx_errs);

    if (stats.tx_drop >= 0)
        vshPrint (ctl, "%s tx_drop %lld\n", device, stats.tx_drop);

    virDomainFree(dom);
    return TRUE;
}

/*
 * "suspend" command
 */
static vshCmdInfo info_suspend[] = {
    {"syntax", "suspend <domain>"},
    {"help", gettext_noop("suspend a domain")},
    {"desc", gettext_noop("Suspend a running domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_suspend[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdSuspend(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *name;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainSuspend(dom) == 0) {
        vshPrint(ctl, _("Domain %s suspended\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to suspend domain %s"), name);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "create" command
 */
static vshCmdInfo info_create[] = {
    {"syntax", "create a domain from an XML <file>"},
    {"help", gettext_noop("create a domain from an XML file")},
    {"desc", gettext_noop("Create a domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_create[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML domain description")},
    {NULL, 0, 0, NULL}
};

static int
cmdCreate(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0)
        return FALSE;

    dom = virDomainCreateLinux(ctl->conn, buffer, 0);
    free (buffer);

    if (dom != NULL) {
        vshPrint(ctl, _("Domain %s created from %s\n"),
                 virDomainGetName(dom), from);
	virDomainFree(dom);
    } else {
        vshError(ctl, FALSE, _("Failed to create domain from %s"), from);
        ret = FALSE;
    }
    return ret;
}

/*
 * "define" command
 */
static vshCmdInfo info_define[] = {
    {"syntax", "define a domain from an XML <file>"},
    {"help", gettext_noop("define (but don't start) a domain from an XML file")},
    {"desc", gettext_noop("Define a domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_define[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML domain description")},
    {NULL, 0, 0, NULL}
};

static int
cmdDefine(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0)
        return FALSE;

    dom = virDomainDefineXML(ctl->conn, buffer);
    free (buffer);

    if (dom != NULL) {
        vshPrint(ctl, _("Domain %s defined from %s\n"),
                 virDomainGetName(dom), from);
        virDomainFree(dom);
    } else {
        vshError(ctl, FALSE, _("Failed to define domain from %s"), from);
        ret = FALSE;
    }
    return ret;
}

/*
 * "undefine" command
 */
static vshCmdInfo info_undefine[] = {
    {"syntax", "undefine <domain>"},
    {"help", gettext_noop("undefine an inactive domain")},
    {"desc", gettext_noop("Undefine the configuration for an inactive domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_undefine[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdUndefine(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainUndefine(dom) == 0) {
        vshPrint(ctl, _("Domain %s has been undefined\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to undefine domain %s"), name);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}


/*
 * "start" command
 */
static vshCmdInfo info_start[] = {
    {"syntax", "start <domain>"},
    {"help", gettext_noop("start a (previously defined) inactive domain")},
    {"desc", gettext_noop("Start a domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_start[] = {
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("name of the inactive domain")},
    {NULL, 0, 0, NULL}
};

static int
cmdStart(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomainBy(ctl, cmd, "name", NULL, VSH_BYNAME)))
        return FALSE;

    if (virDomainGetID(dom) != (unsigned int)-1) {
        vshError(ctl, FALSE, "%s", _("Domain is already active"));
	virDomainFree(dom);
        return FALSE;
    }

    if (virDomainCreate(dom) == 0) {
        vshPrint(ctl, _("Domain %s started\n"),
                 virDomainGetName(dom));
    } else {
        vshError(ctl, FALSE, _("Failed to start domain %s"),
                 virDomainGetName(dom));
        ret = FALSE;
    }
    virDomainFree(dom);
    return ret;
}

/*
 * "save" command
 */
static vshCmdInfo info_save[] = {
    {"syntax", "save <domain> <file>"},
    {"help", gettext_noop("save a domain state to a file")},
    {"desc", gettext_noop("Save a running domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_save[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("where to save the data")},
    {NULL, 0, 0, NULL}
};

static int
cmdSave(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *name;
    char *to;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(to = vshCommandOptString(cmd, "file", NULL)))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainSave(dom, to) == 0) {
        vshPrint(ctl, _("Domain %s saved to %s\n"), name, to);
    } else {
        vshError(ctl, FALSE, _("Failed to save domain %s to %s"), name, to);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "schedinfo" command
 */
static vshCmdInfo info_schedinfo[] = {
    {"syntax", "schedinfo <domain>"},
    {"help", gettext_noop("show/set scheduler parameters")},
    {"desc", gettext_noop("Show/Set scheduler parameters.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_schedinfo[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"weight", VSH_OT_INT, VSH_OFLAG_NONE, gettext_noop("weight for XEN_CREDIT")},
    {"cap", VSH_OT_INT, VSH_OFLAG_NONE, gettext_noop("cap for XEN_CREDIT")},
    {NULL, 0, 0, NULL}
};

static int
cmdSchedinfo(vshControl * ctl, vshCmd * cmd)
{
    char *schedulertype;
    virDomainPtr dom;
    virSchedParameterPtr params = NULL;
    int i, ret;
    int nparams = 0;
    int nr_inputparams = 0;
    int inputparams = 0;
    int weightfound = 0;
    int weight = 0;
    int capfound = 0;
    int cap = 0;
    char str_weight[] = "weight";
    char str_cap[]    = "cap";
    int ret_val = FALSE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    /* Currently supports Xen Credit only */
    if(vshCommandOptBool(cmd, "weight")) {
        weight = vshCommandOptInt(cmd, "weight", &weightfound);
        if (!weightfound) {
            vshError(ctl, FALSE, "%s", _("Invalid value of weight"));
            goto cleanup;
        } else {
            nr_inputparams++;
        }
    }

    if(vshCommandOptBool(cmd, "cap")) {
        cap = vshCommandOptInt(cmd, "cap", &capfound);
        if (!capfound) {
            vshError(ctl, FALSE, "%s", _("Invalid value of cap"));
            goto cleanup;
        } else {
            nr_inputparams++;
        }
    }

    params = vshMalloc(ctl, sizeof (virSchedParameter) * nr_inputparams);
    if (params == NULL) {
        goto cleanup;
    }

    if (weightfound) {
         strncpy(params[inputparams].field,str_weight,sizeof(str_weight));
         params[inputparams].type = VIR_DOMAIN_SCHED_FIELD_UINT;
         params[inputparams].value.ui = weight;
         inputparams++;
    }

    if (capfound) {
         strncpy(params[inputparams].field,str_cap,sizeof(str_cap));
         params[inputparams].type = VIR_DOMAIN_SCHED_FIELD_UINT;
         params[inputparams].value.ui = cap;
         inputparams++;
    }
    /* End Currently supports Xen Credit only */

    assert (inputparams == nr_inputparams);

    /* Set SchedulerParameters */
    if (inputparams > 0) {
        ret = virDomainSetSchedulerParameters(dom, params, inputparams);
        if (ret == -1) {
            goto cleanup;
	}
    }
    free(params);
    params = NULL;

    /* Print SchedulerType */
    schedulertype = virDomainGetSchedulerType(dom, &nparams);
    if (schedulertype!= NULL){
        vshPrint(ctl, "%-15s: %s\n", _("Scheduler"),
             schedulertype);
        free(schedulertype);
    } else {
        vshPrint(ctl, "%-15s: %s\n", _("Scheduler"), _("Unknown"));
        goto cleanup;
    }

    /* Get SchedulerParameters */
    params = vshMalloc(ctl, sizeof(virSchedParameter)* nparams);
    if (params == NULL) {
        goto cleanup;
    }
    for (i = 0; i < nparams; i++){
        params[i].type = 0;
        memset (params[i].field, 0, sizeof params[i].field);
    }
    ret = virDomainGetSchedulerParameters(dom, params, &nparams);
    if (ret == -1) {
        goto cleanup;
    }
    ret_val = TRUE;
    if(nparams){
        for (i = 0; i < nparams; i++){
            switch (params[i].type) {
            case VIR_DOMAIN_SCHED_FIELD_INT:
                 printf("%-15s: %d\n",  params[i].field, params[i].value.i);
                 break;
            case VIR_DOMAIN_SCHED_FIELD_UINT:
                 printf("%-15s: %u\n",  params[i].field, params[i].value.ui);
                 break;
            case VIR_DOMAIN_SCHED_FIELD_LLONG:
                 printf("%-15s: %Ld\n",  params[i].field, params[i].value.l);
                 break;
            case VIR_DOMAIN_SCHED_FIELD_ULLONG:
                 printf("%-15s: %Lu\n",  params[i].field, params[i].value.ul);
                 break;
            case VIR_DOMAIN_SCHED_FIELD_DOUBLE:
                 printf("%-15s: %f\n",  params[i].field, params[i].value.d);
                 break;
            case VIR_DOMAIN_SCHED_FIELD_BOOLEAN:
                 printf("%-15s: %d\n",  params[i].field, params[i].value.b);
                 break;
            default:
                 printf("not implemented scheduler parameter type\n");
            }
        }
    }
 cleanup:
    free(params);
    virDomainFree(dom);
    return ret_val;
}

/*
 * "restore" command
 */
static vshCmdInfo info_restore[] = {
    {"syntax", "restore a domain from <file>"},
    {"help", gettext_noop("restore a domain from a saved state in a file")},
    {"desc", gettext_noop("Restore a domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_restore[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("the state to restore")},
    {NULL, 0, 0, NULL}
};

static int
cmdRestore(vshControl * ctl, vshCmd * cmd)
{
    char *from;
    int found;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virDomainRestore(ctl->conn, from) == 0) {
        vshPrint(ctl, _("Domain restored from %s\n"), from);
    } else {
        vshError(ctl, FALSE, _("Failed to restore domain from %s"), from);
        ret = FALSE;
    }
    return ret;
}

/*
 * "dump" command
 */
static vshCmdInfo info_dump[] = {
    {"syntax", "dump <domain> <file>"},
    {"help", gettext_noop("dump the core of a domain to a file for analysis")},
    {"desc", gettext_noop("Core dump a domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_dump[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("where to dump the core")},
    {NULL, 0, 0, NULL}
};

static int
cmdDump(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *name;
    char *to;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(to = vshCommandOptString(cmd, "file", NULL)))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainCoreDump(dom, to, 0) == 0) {
        vshPrint(ctl, _("Domain %s dumpd to %s\n"), name, to);
    } else {
        vshError(ctl, FALSE, _("Failed to core dump domain %s to %s"),
                 name, to);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "resume" command
 */
static vshCmdInfo info_resume[] = {
    {"syntax", "resume <domain>"},
    {"help", gettext_noop("resume a domain")},
    {"desc", gettext_noop("Resume a previously suspended domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_resume[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdResume(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainResume(dom) == 0) {
        vshPrint(ctl, _("Domain %s resumed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to resume domain %s"), name);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "shutdown" command
 */
static vshCmdInfo info_shutdown[] = {
    {"syntax", "shutdown <domain>"},
    {"help", gettext_noop("gracefully shutdown a domain")},
    {"desc", gettext_noop("Run shutdown in the target domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_shutdown[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdShutdown(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainShutdown(dom) == 0) {
        vshPrint(ctl, _("Domain %s is being shutdown\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to shutdown domain %s"), name);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "reboot" command
 */
static vshCmdInfo info_reboot[] = {
    {"syntax", "reboot <domain>"},
    {"help", gettext_noop("reboot a domain")},
    {"desc", gettext_noop("Run a reboot command in the target domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_reboot[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdReboot(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainReboot(dom, 0) == 0) {
        vshPrint(ctl, _("Domain %s is being rebooted\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to reboot domain %s"), name);
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "destroy" command
 */
static vshCmdInfo info_destroy[] = {
    {"syntax", "destroy <domain>"},
    {"help", gettext_noop("destroy a domain")},
    {"desc", gettext_noop("Destroy a given domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_destroy[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdDestroy(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", &name)))
        return FALSE;

    if (virDomainDestroy(dom) == 0) {
        vshPrint(ctl, _("Domain %s destroyed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to destroy domain %s"), name);
        ret = FALSE;
        virDomainFree(dom);
    }

    return ret;
}

/*
 * "dominfo" command
 */
static vshCmdInfo info_dominfo[] = {
    {"syntax", "dominfo <domain>"},
    {"help", gettext_noop("domain information")},
    {"desc", gettext_noop("Returns basic information about the domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_dominfo[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdDominfo(vshControl * ctl, vshCmd * cmd)
{
    virDomainInfo info;
    virDomainPtr dom;
    int ret = TRUE;
    unsigned int id;
    char *str, uuid[VIR_UUID_STRING_BUFLEN];

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    id = virDomainGetID(dom);
    if (id == ((unsigned int)-1))
        vshPrint(ctl, "%-15s %s\n", _("Id:"), "-");
    else
        vshPrint(ctl, "%-15s %d\n", _("Id:"), id);
    vshPrint(ctl, "%-15s %s\n", _("Name:"), virDomainGetName(dom));

    if (virDomainGetUUIDString(dom, &uuid[0])==0)
        vshPrint(ctl, "%-15s %s\n", _("UUID:"), uuid);

    if ((str = virDomainGetOSType(dom))) {
        vshPrint(ctl, "%-15s %s\n", _("OS Type:"), str);
        free(str);
    }

    if (virDomainGetInfo(dom, &info) == 0) {
        vshPrint(ctl, "%-15s %s\n", _("State:"),
                 N_(vshDomainStateToString(info.state)));

        vshPrint(ctl, "%-15s %d\n", _("CPU(s):"), info.nrVirtCpu);

        if (info.cpuTime != 0) {
            double cpuUsed = info.cpuTime;

            cpuUsed /= 1000000000.0;

            vshPrint(ctl, "%-15s %.1lfs\n", _("CPU time:"), cpuUsed);
        }

        if (info.maxMem != UINT_MAX)
            vshPrint(ctl, "%-15s %lu kB\n", _("Max memory:"),
                 info.maxMem);
        else
            vshPrint(ctl, "%-15s %-15s\n", _("Max memory:"),
                 _("no limit"));

        vshPrint(ctl, "%-15s %lu kB\n", _("Used memory:"),
                 info.memory);

    } else {
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "freecell" command
 */
static vshCmdInfo info_freecell[] = {
    {"syntax", "freecell [<cellno>]"},
    {"help", gettext_noop("NUMA free memory")},
    {"desc", gettext_noop("display available free memory for the NUMA cell.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_freecell[] = {
    {"cellno", VSH_OT_DATA, 0, gettext_noop("NUMA cell number")},
    {NULL, 0, 0, NULL}
};

static int
cmdFreecell(vshControl * ctl, vshCmd * cmd)
{
    int ret;
    int cell, cell_given;
    unsigned long long memory;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    cell = vshCommandOptInt(cmd, "cellno", &cell_given);
    if (!cell_given) {
        memory = virNodeGetFreeMemory(ctl->conn);
    } else {
	ret = virNodeGetCellsFreeMemory(ctl->conn, &memory, cell, 1);
	if (ret != 1)
	    return FALSE;
    }

    if (cell == -1)
	vshPrint(ctl, "%s: %llu kB\n", _("Total"), memory);
    else
	vshPrint(ctl, "%d: %llu kB\n", cell, memory);

    return TRUE;
}

/*
 * "vcpuinfo" command
 */
static vshCmdInfo info_vcpuinfo[] = {
    {"syntax", "vcpuinfo <domain>"},
    {"help", gettext_noop("domain vcpu information")},
    {"desc", gettext_noop("Returns basic information about the domain virtual CPUs.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vcpuinfo[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdVcpuinfo(vshControl * ctl, vshCmd * cmd)
{
    virDomainInfo info;
    virDomainPtr dom;
    virNodeInfo nodeinfo;
    virVcpuInfoPtr cpuinfo;
    unsigned char *cpumap;
    int ncpus;
    size_t cpumaplen;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    if (virNodeGetInfo(ctl->conn, &nodeinfo) != 0) {
        virDomainFree(dom);
        return FALSE;
    }

    if (virDomainGetInfo(dom, &info) != 0) {
        virDomainFree(dom);
        return FALSE;
    }

    cpuinfo = vshMalloc(ctl, sizeof(virVcpuInfo)*info.nrVirtCpu);
    cpumaplen = VIR_CPU_MAPLEN(VIR_NODEINFO_MAXCPUS(nodeinfo));
    cpumap = vshMalloc(ctl, info.nrVirtCpu * cpumaplen);

    if ((ncpus = virDomainGetVcpus(dom,
                                   cpuinfo, info.nrVirtCpu,
                                   cpumap, cpumaplen)) >= 0) {
        int n;
        for (n = 0 ; n < ncpus ; n++) {
            unsigned int m;
            vshPrint(ctl, "%-15s %d\n", _("VCPU:"), n);
            vshPrint(ctl, "%-15s %d\n", _("CPU:"), cpuinfo[n].cpu);
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     N_(vshDomainVcpuStateToString(cpuinfo[n].state)));
            if (cpuinfo[n].cpuTime != 0) {
                double cpuUsed = cpuinfo[n].cpuTime;

                cpuUsed /= 1000000000.0;

                vshPrint(ctl, "%-15s %.1lfs\n", _("CPU time:"), cpuUsed);
            }
            vshPrint(ctl, "%-15s ", _("CPU Affinity:"));
            for (m = 0 ; m < VIR_NODEINFO_MAXCPUS(nodeinfo) ; m++) {
                vshPrint(ctl, "%c", VIR_CPU_USABLE(cpumap, cpumaplen, n, m) ? 'y' : '-');
            }
            vshPrint(ctl, "\n");
            if (n < (ncpus - 1)) {
                vshPrint(ctl, "\n");
            }
        }
    } else {
        if (info.state == VIR_DOMAIN_SHUTOFF) {
            vshError(ctl, FALSE, "%s",
                 _("Domain shut off, virtual CPUs not present."));
        }
        ret = FALSE;
    }

    free(cpumap);
    free(cpuinfo);
    virDomainFree(dom);
    return ret;
}

/*
 * "vcpupin" command
 */
static vshCmdInfo info_vcpupin[] = {
    {"syntax", "vcpupin <domain> <vcpu> <cpulist>"},
    {"help", gettext_noop("control domain vcpu affinity")},
    {"desc", gettext_noop("Pin domain VCPUs to host physical CPUs.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vcpupin[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"vcpu", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vcpu number")},
    {"cpulist", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("host cpu number(s) (comma separated)")},
    {NULL, 0, 0, NULL}
};

static int
cmdVcpupin(vshControl * ctl, vshCmd * cmd)
{
    virDomainInfo info;
    virDomainPtr dom;
    virNodeInfo nodeinfo;
    int vcpu;
    char *cpulist;
    int ret = TRUE;
    int vcpufound = 0;
    unsigned char *cpumap;
    int cpumaplen;
    int i;
    enum { expect_num, expect_num_or_comma } state;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    vcpu = vshCommandOptInt(cmd, "vcpu", &vcpufound);
    if (!vcpufound) {
        virDomainFree(dom);
        return FALSE;
    }

    if (!(cpulist = vshCommandOptString(cmd, "cpulist", NULL))) {
        virDomainFree(dom);
        return FALSE;
    }

    if (virNodeGetInfo(ctl->conn, &nodeinfo) != 0) {
        virDomainFree(dom);
        return FALSE;
    }

    if (virDomainGetInfo(dom, &info) != 0) {
        virDomainFree(dom);
        return FALSE;
    }

    if (vcpu >= info.nrVirtCpu) {
        virDomainFree(dom);
        return FALSE;
    }

    /* Check that the cpulist parameter is a comma-separated list of
     * numbers and give an intelligent error message if not.
     */
    if (cpulist[0] == '\0') {
        vshError(ctl, FALSE, "%s", _("cpulist: Invalid format. Empty string."));
        virDomainFree (dom);
        return FALSE;
    }

    state = expect_num;
    for (i = 0; cpulist[i]; i++) {
        switch (state) {
        case expect_num:
            if (!isdigit (cpulist[i])) {
                vshError( ctl, FALSE, _("cpulist: %s: Invalid format. Expecting digit at position %d (near '%c')."), cpulist, i, cpulist[i]);
                virDomainFree (dom);
                return FALSE;
            }
            state = expect_num_or_comma;
            break;
        case expect_num_or_comma:
            if (cpulist[i] == ',')
                state = expect_num;
            else if (!isdigit (cpulist[i])) {
                vshError(ctl, FALSE, _("cpulist: %s: Invalid format. Expecting digit or comma at position %d (near '%c')."), cpulist, i, cpulist[i]);
                virDomainFree (dom);
                return FALSE;
            }
        }
    }
    if (state == expect_num) {
        vshError(ctl, FALSE, _("cpulist: %s: Invalid format. Trailing comma at position %d."), cpulist, i);
        virDomainFree (dom);
        return FALSE;
    }

    cpumaplen = VIR_CPU_MAPLEN(VIR_NODEINFO_MAXCPUS(nodeinfo));
    cpumap = vshCalloc(ctl, 1, cpumaplen);

    do {
        unsigned int cpu = atoi(cpulist);

        if (cpu < VIR_NODEINFO_MAXCPUS(nodeinfo)) {
            VIR_USE_CPU(cpumap, cpu);
        } else {
            vshError(ctl, FALSE, _("Physical CPU %d doesn't exist."), cpu);
            free(cpumap);
            virDomainFree(dom);
            return FALSE;
        }
        cpulist = strchr(cpulist, ',');
        if (cpulist)
            cpulist++;
    } while (cpulist);

    if (virDomainPinVcpu(dom, vcpu, cpumap, cpumaplen) != 0) {
        ret = FALSE;
    }

    free(cpumap);
    virDomainFree(dom);
    return ret;
}

/*
 * "setvcpus" command
 */
static vshCmdInfo info_setvcpus[] = {
    {"syntax", "setvcpus <domain> <count>"},
    {"help", gettext_noop("change number of virtual CPUs")},
    {"desc", gettext_noop("Change the number of virtual CPUs active in the guest domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_setvcpus[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"count", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("number of virtual CPUs")},
    {NULL, 0, 0, NULL}
};

static int
cmdSetvcpus(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int count;
    int maxcpu;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    count = vshCommandOptInt(cmd, "count", &count);
    if (count <= 0) {
        vshError(ctl, FALSE, "%s", _("Invalid number of virtual CPUs."));
        virDomainFree(dom);
        return FALSE;
    }

    maxcpu = virDomainGetMaxVcpus(dom);
    if (maxcpu <= 0) {
        virDomainFree(dom);
        return FALSE;
    }

    if (count > maxcpu) {
        vshError(ctl, FALSE, "%s", _("Too many virtual CPUs."));
        virDomainFree(dom);
        return FALSE;
    }

    if (virDomainSetVcpus(dom, count) != 0) {
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "setmemory" command
 */
static vshCmdInfo info_setmem[] = {
    {"syntax", "setmem <domain> <kilobytes>"},
    {"help", gettext_noop("change memory allocation")},
    {"desc", gettext_noop("Change the current memory allocation in the guest domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_setmem[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"kilobytes", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("number of kilobytes of memory")},
    {NULL, 0, 0, NULL}
};

static int
cmdSetmem(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    virDomainInfo info;
    int kilobytes;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    kilobytes = vshCommandOptInt(cmd, "kilobytes", &kilobytes);
    if (kilobytes <= 0) {
        virDomainFree(dom);
        vshError(ctl, FALSE, _("Invalid value of %d for memory size"), kilobytes);
        return FALSE;
    }

    if (virDomainGetInfo(dom, &info) != 0) {
        virDomainFree(dom);
        vshError(ctl, FALSE, "%s", _("Unable to verify MaxMemorySize"));
        return FALSE;
    }

    if (kilobytes > info.maxMem) {
        virDomainFree(dom);
        vshError(ctl, FALSE, _("Invalid value of %d for memory size"), kilobytes);
        return FALSE;
    }

    if (virDomainSetMemory(dom, kilobytes) != 0) {
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "setmaxmem" command
 */
static vshCmdInfo info_setmaxmem[] = {
    {"syntax", "setmaxmem <domain> <kilobytes>"},
    {"help", gettext_noop("change maximum memory limit")},
    {"desc", gettext_noop("Change the maximum memory allocation limit in the guest domain.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_setmaxmem[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"kilobytes", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("maximum memory limit in kilobytes")},
    {NULL, 0, 0, NULL}
};

static int
cmdSetmaxmem(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    virDomainInfo info;
    int kilobytes;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    kilobytes = vshCommandOptInt(cmd, "kilobytes", &kilobytes);
    if (kilobytes <= 0) {
        virDomainFree(dom);
        vshError(ctl, FALSE, _("Invalid value of %d for memory size"), kilobytes);
        return FALSE;
    }

    if (virDomainGetInfo(dom, &info) != 0) {
        virDomainFree(dom);
        vshError(ctl, FALSE, "%s", _("Unable to verify current MemorySize"));
        return FALSE;
    }

    if (kilobytes < info.memory) {
        if (virDomainSetMemory(dom, kilobytes) != 0) {
            virDomainFree(dom);
            vshError(ctl, FALSE, "%s", _("Unable to shrink current MemorySize"));
            return FALSE;
        }
    }

    if (virDomainSetMaxMemory(dom, kilobytes) != 0) {
        vshError(ctl, FALSE, "%s", _("Unable to change MaxMemorySize"));
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "nodeinfo" command
 */
static vshCmdInfo info_nodeinfo[] = {
    {"syntax", "nodeinfo"},
    {"help", gettext_noop("node information")},
    {"desc", gettext_noop("Returns basic information about the node.")},
    {NULL, NULL}
};

static int
cmdNodeinfo(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    virNodeInfo info;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (virNodeGetInfo(ctl->conn, &info) < 0) {
        vshError(ctl, FALSE, "%s", _("failed to get node information"));
        return FALSE;
    }
    vshPrint(ctl, "%-20s %s\n", _("CPU model:"), info.model);
    vshPrint(ctl, "%-20s %d\n", _("CPU(s):"), info.cpus);
    vshPrint(ctl, "%-20s %d MHz\n", _("CPU frequency:"), info.mhz);
    vshPrint(ctl, "%-20s %d\n", _("CPU socket(s):"), info.sockets);
    vshPrint(ctl, "%-20s %d\n", _("Core(s) per socket:"), info.cores);
    vshPrint(ctl, "%-20s %d\n", _("Thread(s) per core:"), info.threads);
    vshPrint(ctl, "%-20s %d\n", _("NUMA cell(s):"), info.nodes);
    vshPrint(ctl, "%-20s %lu kB\n", _("Memory size:"), info.memory);

    return TRUE;
}

/*
 * "capabilities" command
 */
static vshCmdInfo info_capabilities[] = {
    {"syntax", "capabilities"},
    {"help", gettext_noop("capabilities")},
    {"desc", gettext_noop("Returns capabilities of hypervisor/driver.")},
    {NULL, NULL}
};

static int
cmdCapabilities (vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    char *caps;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if ((caps = virConnectGetCapabilities (ctl->conn)) == NULL) {
        vshError(ctl, FALSE, "%s", _("failed to get capabilities"));
        return FALSE;
    }
    vshPrint (ctl, "%s\n", caps);
    free (caps);

    return TRUE;
}

/*
 * "dumpxml" command
 */
static vshCmdInfo info_dumpxml[] = {
    {"syntax", "dumpxml <domain>"},
    {"help", gettext_noop("domain information in XML")},
    {"desc", gettext_noop("Output the domain information as an XML dump to stdout.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_dumpxml[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdDumpXML(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    int ret = TRUE;
    char *dump;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    dump = virDomainGetXMLDesc(dom, 0);
    if (dump != NULL) {
        printf("%s", dump);
        free(dump);
    } else {
        ret = FALSE;
    }

    virDomainFree(dom);
    return ret;
}

/*
 * "domname" command
 */
static vshCmdInfo info_domname[] = {
    {"syntax", "domname <domain>"},
    {"help", gettext_noop("convert a domain id or UUID to domain name")},
    {NULL, NULL}
};

static vshCmdOptDef opts_domname[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdDomname(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;
    if (!(dom = vshCommandOptDomainBy(ctl, cmd, "domain", NULL,
                                      VSH_BYID|VSH_BYUUID)))
        return FALSE;

    vshPrint(ctl, "%s\n", virDomainGetName(dom));
    virDomainFree(dom);
    return TRUE;
}

/*
 * "domid" command
 */
static vshCmdInfo info_domid[] = {
    {"syntax", "domid <domain>"},
    {"help", gettext_noop("convert a domain name or UUID to domain id")},
    {NULL, NULL}
};

static vshCmdOptDef opts_domid[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdDomid(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    unsigned int id;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;
    if (!(dom = vshCommandOptDomainBy(ctl, cmd, "domain", NULL,
                                      VSH_BYNAME|VSH_BYUUID)))
        return FALSE;

    id = virDomainGetID(dom);
    if (id == ((unsigned int)-1))
        vshPrint(ctl, "%s\n", "-");
    else
        vshPrint(ctl, "%d\n", id);
    virDomainFree(dom);
    return TRUE;
}

/*
 * "domuuid" command
 */
static vshCmdInfo info_domuuid[] = {
    {"syntax", "domuuid <domain>"},
    {"help", gettext_noop("convert a domain name or id to domain UUID")},
    {NULL, NULL}
};

static vshCmdOptDef opts_domuuid[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain id or name")},
    {NULL, 0, 0, NULL}
};

static int
cmdDomuuid(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;
    if (!(dom = vshCommandOptDomainBy(ctl, cmd, "domain", NULL,
                                      VSH_BYNAME|VSH_BYID)))
        return FALSE;

    if (virDomainGetUUIDString(dom, uuid) != -1)
        vshPrint(ctl, "%s\n", uuid);
    else
        vshError(ctl, FALSE, "%s", _("failed to get domain UUID"));

    virDomainFree(dom);
    return TRUE;
}

/*
 * "migrate" command
 */
static vshCmdInfo info_migrate[] = {
    {"syntax", "migrate [--live] <domain> <desturi> [<migrateuri>]"},
    {"help", gettext_noop("migrate domain to another host")},
    {"desc", gettext_noop("Migrate domain to another host.  Add --live for live migration.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_migrate[] = {
    {"live", VSH_OT_BOOL, 0, gettext_noop("live migration")},
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"desturi", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("connection URI of the destination host")},
    {"migrateuri", VSH_OT_DATA, 0, gettext_noop("migration URI, usually can be omitted")},
    {NULL, 0, 0, NULL}
};

static int
cmdMigrate (vshControl *ctl, vshCmd *cmd)
{
    virDomainPtr dom = NULL;
    const char *desturi;
    const char *migrateuri;
    int flags = 0, found, ret = FALSE;
    virConnectPtr dconn = NULL;
    virDomainPtr ddom = NULL;

    if (!vshConnectionUsability (ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain (ctl, cmd, "domain", NULL)))
        return FALSE;

    desturi = vshCommandOptString (cmd, "desturi", &found);
    if (!found) {
        vshError (ctl, FALSE, "%s", _("migrate: Missing desturi"));
        goto done;
    }

    migrateuri = vshCommandOptString (cmd, "migrateuri", &found);
    if (!found) migrateuri = NULL;

    if (vshCommandOptBool (cmd, "live"))
        flags |= VIR_MIGRATE_LIVE;

    /* Temporarily connect to the destination host. */
    dconn = virConnectOpen (desturi);
    if (!dconn) goto done;

    /* Migrate. */
    ddom = virDomainMigrate (dom, dconn, flags, NULL, migrateuri, 0);
    if (!ddom) goto done;

    ret = TRUE;

 done:
    if (dom) virDomainFree (dom);
    if (ddom) virDomainFree (ddom);
    if (dconn) virConnectClose (dconn);
    return ret;
}

/*
 * "net-autostart" command
 */
static vshCmdInfo info_network_autostart[] = {
    {"syntax", "net-autostart [--disable] <network>"},
    {"help", gettext_noop("autostart a network")},
    {"desc",
     gettext_noop("Configure a network to be automatically started at boot.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_autostart[] = {
    {"network",  VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network name or uuid")},
    {"disable", VSH_OT_BOOL, 0, gettext_noop("disable autostarting")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkAutostart(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    char *name;
    int autostart;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(network = vshCommandOptNetwork(ctl, cmd, "network", &name)))
        return FALSE;

    autostart = !vshCommandOptBool(cmd, "disable");

    if (virNetworkSetAutostart(network, autostart) < 0) {
        if (autostart)
	    vshError(ctl, FALSE, _("failed to mark network %s as autostarted"),
                                   name);
	else
	    vshError(ctl, FALSE,_("failed to unmark network %s as autostarted"),
                                   name);
        virNetworkFree(network);
        return FALSE;
    }

    if (autostart)
	vshPrint(ctl, _("Network %s marked as autostarted\n"), name);
    else
	vshPrint(ctl, _("Network %s unmarked as autostarted\n"), name);

    return TRUE;
}

/*
 * "net-create" command
 */
static vshCmdInfo info_network_create[] = {
    {"syntax", "create a network from an XML <file>"},
    {"help", gettext_noop("create a network from an XML file")},
    {"desc", gettext_noop("Create a network.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_create[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML network description")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkCreate(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0)
        return FALSE;

    network = virNetworkCreateXML(ctl->conn, buffer);
    free (buffer);

    if (network != NULL) {
        vshPrint(ctl, _("Network %s created from %s\n"),
                 virNetworkGetName(network), from);
    } else {
        vshError(ctl, FALSE, _("Failed to create network from %s"), from);
        ret = FALSE;
    }
    return ret;
}


/*
 * "net-define" command
 */
static vshCmdInfo info_network_define[] = {
    {"syntax", "define a network from an XML <file>"},
    {"help", gettext_noop("define (but don't start) a network from an XML file")},
    {"desc", gettext_noop("Define a network.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_define[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML network description")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkDefine(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0)
        return FALSE;

    network = virNetworkDefineXML(ctl->conn, buffer);
    free (buffer);

    if (network != NULL) {
        vshPrint(ctl, _("Network %s defined from %s\n"),
                 virNetworkGetName(network), from);
    } else {
        vshError(ctl, FALSE, _("Failed to define network from %s"), from);
        ret = FALSE;
    }
    return ret;
}


/*
 * "net-destroy" command
 */
static vshCmdInfo info_network_destroy[] = {
    {"syntax", "net-destroy <network>"},
    {"help", gettext_noop("destroy a network")},
    {"desc", gettext_noop("Destroy a given network.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_destroy[] = {
    {"network", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkDestroy(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(network = vshCommandOptNetwork(ctl, cmd, "network", &name)))
        return FALSE;

    if (virNetworkDestroy(network) == 0) {
        vshPrint(ctl, _("Network %s destroyed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to destroy network %s"), name);
        ret = FALSE;
        virNetworkFree(network);
    }

    return ret;
}


/*
 * "net-dumpxml" command
 */
static vshCmdInfo info_network_dumpxml[] = {
    {"syntax", "net-dumpxml <network>"},
    {"help", gettext_noop("network information in XML")},
    {"desc", gettext_noop("Output the network information as an XML dump to stdout.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_dumpxml[] = {
    {"network", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkDumpXML(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    int ret = TRUE;
    char *dump;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(network = vshCommandOptNetwork(ctl, cmd, "network", NULL)))
        return FALSE;

    dump = virNetworkGetXMLDesc(network, 0);
    if (dump != NULL) {
        printf("%s", dump);
        free(dump);
    } else {
        ret = FALSE;
    }

    virNetworkFree(network);
    return ret;
}


/*
 * "net-list" command
 */
static vshCmdInfo info_network_list[] = {
    {"syntax", "net-list [ --inactive | --all ]"},
    {"help", gettext_noop("list networks")},
    {"desc", gettext_noop("Returns list of networks.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_list[] = {
    {"inactive", VSH_OT_BOOL, 0, gettext_noop("list inactive networks")},
    {"all", VSH_OT_BOOL, 0, gettext_noop("list inactive & active networks")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkList(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    int inactive = vshCommandOptBool(cmd, "inactive");
    int all = vshCommandOptBool(cmd, "all");
    int active = !inactive || all ? 1 : 0;
    int maxactive = 0, maxinactive = 0, i;
    char **activeNames = NULL, **inactiveNames = NULL;
    inactive |= all;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (active) {
        maxactive = virConnectNumOfNetworks(ctl->conn);
        if (maxactive < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list active networks"));
            return FALSE;
        }
        if (maxactive) {
            activeNames = vshMalloc(ctl, sizeof(char *) * maxactive);

            if ((maxactive = virConnectListNetworks(ctl->conn, activeNames,
	                                            maxactive)) < 0) {
                vshError(ctl, FALSE, "%s", _("Failed to list active networks"));
                free(activeNames);
                return FALSE;
            }

            qsort(&activeNames[0], maxactive, sizeof(char *), namesorter);
        }
    }
    if (inactive) {
        maxinactive = virConnectNumOfDefinedNetworks(ctl->conn);
        if (maxinactive < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list inactive networks"));
            free(activeNames);
            return FALSE;
        }
        if (maxinactive) {
            inactiveNames = vshMalloc(ctl, sizeof(char *) * maxinactive);

            if ((maxinactive = virConnectListDefinedNetworks(ctl->conn, inactiveNames, maxinactive)) < 0) {
                vshError(ctl, FALSE, "%s", _("Failed to list inactive networks"));
                free(activeNames);
                free(inactiveNames);
                return FALSE;
            }

            qsort(&inactiveNames[0], maxinactive, sizeof(char*), namesorter);
        }
    }
    vshPrintExtra(ctl, "%-20s %-10s %-10s\n", _("Name"), _("State"), _("Autostart"));
    vshPrintExtra(ctl, "-----------------------------------------\n");

    for (i = 0; i < maxactive; i++) {
        virNetworkPtr network = virNetworkLookupByName(ctl->conn, activeNames[i]);
        const char *autostartStr;
        int autostart = 0;

        /* this kind of work with networks is not atomic operation */
        if (!network) {
            free(activeNames[i]);
            continue;
        }

        if (virNetworkGetAutostart(network, &autostart) < 0)
            autostartStr = _("no autostart");
        else
            autostartStr = autostart ? "yes" : "no";

        vshPrint(ctl, "%-20s %-10s %-10s\n",
                 virNetworkGetName(network),
                 _("active"),
                 autostartStr);
        virNetworkFree(network);
        free(activeNames[i]);
    }
    for (i = 0; i < maxinactive; i++) {
        virNetworkPtr network = virNetworkLookupByName(ctl->conn, inactiveNames[i]);
        const char *autostartStr;
        int autostart = 0;

        /* this kind of work with networks is not atomic operation */
        if (!network) {
            free(inactiveNames[i]);
            continue;
        }

        if (virNetworkGetAutostart(network, &autostart) < 0)
            autostartStr = _("no autostart");
        else
            autostartStr = autostart ? "yes" : "no";

        vshPrint(ctl, "%-20s %s %s\n",
                 inactiveNames[i],
                 _("inactive"),
                 autostartStr);

        virNetworkFree(network);
        free(inactiveNames[i]);
    }
    free(activeNames);
    free(inactiveNames);
    return TRUE;
}


/*
 * "net-name" command
 */
static vshCmdInfo info_network_name[] = {
    {"syntax", "net-name <network>"},
    {"help", gettext_noop("convert a network UUID to network name")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_name[] = {
    {"network", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkName(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;
    if (!(network = vshCommandOptNetworkBy(ctl, cmd, "network", NULL,
					   VSH_BYUUID)))
        return FALSE;

    vshPrint(ctl, "%s\n", virNetworkGetName(network));
    virNetworkFree(network);
    return TRUE;
}


/*
 * "net-start" command
 */
static vshCmdInfo info_network_start[] = {
    {"syntax", "start <network>"},
    {"help", gettext_noop("start a (previously defined) inactive network")},
    {"desc", gettext_noop("Start a network.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_start[] = {
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("name of the inactive network")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkStart(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(network = vshCommandOptNetworkBy(ctl, cmd, "name", NULL, VSH_BYNAME)))
         return FALSE;

    if (virNetworkCreate(network) == 0) {
        vshPrint(ctl, _("Network %s started\n"),
                 virNetworkGetName(network));
    } else {
        vshError(ctl, FALSE, _("Failed to start network %s"),
                 virNetworkGetName(network));
        ret = FALSE;
    }
    return ret;
}


/*
 * "net-undefine" command
 */
static vshCmdInfo info_network_undefine[] = {
    {"syntax", "net-undefine <network>"},
    {"help", gettext_noop("undefine an inactive network")},
    {"desc", gettext_noop("Undefine the configuration for an inactive network.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_undefine[] = {
    {"network", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkUndefine(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(network = vshCommandOptNetwork(ctl, cmd, "network", &name)))
        return FALSE;

    if (virNetworkUndefine(network) == 0) {
        vshPrint(ctl, _("Network %s has been undefined\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to undefine network %s"), name);
        ret = FALSE;
    }

    return ret;
}


/*
 * "net-uuid" command
 */
static vshCmdInfo info_network_uuid[] = {
    {"syntax", "net-uuid <network>"},
    {"help", gettext_noop("convert a network name to network UUID")},
    {NULL, NULL}
};

static vshCmdOptDef opts_network_uuid[] = {
    {"network", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network name")},
    {NULL, 0, 0, NULL}
};

static int
cmdNetworkUuid(vshControl * ctl, vshCmd * cmd)
{
    virNetworkPtr network;
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(network = vshCommandOptNetworkBy(ctl, cmd, "network", NULL,
					   VSH_BYNAME)))
        return FALSE;

    if (virNetworkGetUUIDString(network, uuid) != -1)
        vshPrint(ctl, "%s\n", uuid);
    else
        vshError(ctl, FALSE, "%s", _("failed to get network UUID"));

    return TRUE;
}













/*
 * "pool-autostart" command
 */
static vshCmdInfo info_pool_autostart[] = {
    {"syntax", "pool-autostart [--disable] <pool>"},
    {"help", gettext_noop("autostart a pool")},
    {"desc",
     gettext_noop("Configure a pool to be automatically started at boot.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_autostart[] = {
    {"pool",  VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {"disable", VSH_OT_BOOL, 0, gettext_noop("disable autostarting")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolAutostart(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    char *name;
    int autostart;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return FALSE;

    autostart = !vshCommandOptBool(cmd, "disable");

    if (virStoragePoolSetAutostart(pool, autostart) < 0) {
        if (autostart)
	    vshError(ctl, FALSE, _("failed to mark pool %s as autostarted"),
                                   name);
	else
	    vshError(ctl, FALSE,_("failed to unmark pool %s as autostarted"),
                                   name);
        virStoragePoolFree(pool);
        return FALSE;
    }

    if (autostart)
	vshPrint(ctl, _("Pool %s marked as autostarted\n"), name);
    else
	vshPrint(ctl, _("Pool %s unmarked as autostarted\n"), name);

    return TRUE;
}

/*
 * "pool-create" command
 */
static vshCmdInfo info_pool_create[] = {
    {"syntax", "create a pool from an XML <file>"},
    {"help", gettext_noop("create a pool from an XML file")},
    {"desc", gettext_noop("Create a pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_create[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML pool description")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolCreate(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0)
        return FALSE;

    pool = virStoragePoolCreateXML(ctl->conn, buffer, 0);
    free (buffer);

    if (pool != NULL) {
        vshPrint(ctl, _("Pool %s created from %s\n"),
                 virStoragePoolGetName(pool), from);
    } else {
        vshError(ctl, FALSE, _("Failed to create pool from %s"), from);
        ret = FALSE;
    }
    return ret;
}

/*
 * "pool-create-as" command
 */
static vshCmdInfo info_pool_create_as[] = {
    {"syntax", "pool-create-as <name> <type>"},
    {"help", gettext_noop("create a pool from a set of args")},
    {"desc", gettext_noop("Create a pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_create_as[] = {
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("name of the pool")},
    {"type", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("type of the pool")},
    {"source-host", VSH_OT_DATA, 0, gettext_noop("source-host for underlying storage")},
    {"source-path", VSH_OT_DATA, 0, gettext_noop("source path for underlying storage")},
    {"source-dev", VSH_OT_DATA, 0, gettext_noop("source device for underlying storage")},
    {"target", VSH_OT_DATA, 0, gettext_noop("target for underlying storage")},
    {NULL, 0, 0, NULL}
};


static int
cmdPoolCreateAs(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int found;
    char *name, *type, *srcHost, *srcPath, *srcDev, *target;
    virBuffer buf;

    memset(&buf, 0, sizeof(buf));

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    name = vshCommandOptString(cmd, "name", &found);
    if (!found)
        goto cleanup;
    type = vshCommandOptString(cmd, "type", &found);
    if (!found)
        goto cleanup;

    srcHost = vshCommandOptString(cmd, "source-host", &found);
    srcPath = vshCommandOptString(cmd, "source-path", &found);
    srcDev = vshCommandOptString(cmd, "source-dev", &found);
    target = vshCommandOptString(cmd, "target", &found);

    if (virBufferVSprintf(&buf, "<pool type='%s'>\n", type) < 0)
        goto cleanup;
    if (virBufferVSprintf(&buf, "  <name>%s</name>\n", name) < 0)
        goto cleanup;
    if (srcHost || srcPath || srcDev) {
        if (virBufferAddLit(&buf, "  <source>\n") < 0)
            goto cleanup;
        if (srcHost &&
            virBufferVSprintf(&buf, "    <host name='%s'>\n", srcHost) < 0)
            goto cleanup;
        if (srcPath &&
            virBufferVSprintf(&buf, "    <dir path='%s'/>\n", srcPath) < 0)
            goto cleanup;
        if (srcDev &&
            virBufferVSprintf(&buf, "    <device path='%s'/>\n", srcDev) < 0)
            goto cleanup;

        if (virBufferAddLit(&buf, "  </source>\n") < 0)
            goto cleanup;
    }
    if (target) {
        if (virBufferAddLit(&buf, "  <target>\n") < 0)
            goto cleanup;
        if (virBufferVSprintf(&buf, "    <path>%s</path>\n", target) < 0)
            goto cleanup;
        if (virBufferAddLit(&buf, "  </target>\n") < 0)
            goto cleanup;
    }
    if (virBufferAddLit(&buf, "</pool>\n") < 0)
        goto cleanup;

    pool = virStoragePoolCreateXML(ctl->conn, buf.content, 0);
    free (buf.content);

    if (pool != NULL) {
        vshPrint(ctl, _("Pool %s created\n"), name);
        virStoragePoolFree(pool);
        return TRUE;
    } else {
        vshError(ctl, FALSE, _("Failed to create pool %s"), name);
        return FALSE;
    }

 cleanup:
    free(buf.content);
    return FALSE;
}


/*
 * "pool-define" command
 */
static vshCmdInfo info_pool_define[] = {
    {"syntax", "define a pool from an XML <file>"},
    {"help", gettext_noop("define (but don't start) a pool from an XML file")},
    {"desc", gettext_noop("Define a pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_define[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML pool description")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolDefine(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found)
        return FALSE;

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0)
        return FALSE;

    pool = virStoragePoolDefineXML(ctl->conn, buffer, 0);
    free (buffer);

    if (pool != NULL) {
        vshPrint(ctl, _("Pool %s defined from %s\n"),
                 virStoragePoolGetName(pool), from);
    } else {
        vshError(ctl, FALSE, _("Failed to define pool from %s"), from);
        ret = FALSE;
    }
    return ret;
}


/*
 * "pool-define-as" command
 */
static vshCmdInfo info_pool_define_as[] = {
    {"syntax", "pool-define-as <name> <type>"},
    {"help", gettext_noop("define a pool from a set of args")},
    {"desc", gettext_noop("Define a pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_define_as[] = {
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("name of the pool")},
    {"type", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("type of the pool")},
    {"source-host", VSH_OT_DATA, 0, gettext_noop("source-host for underlying storage")},
    {"source-path", VSH_OT_DATA, 0, gettext_noop("source path for underlying storage")},
    {"source-dev", VSH_OT_DATA, 0, gettext_noop("source device for underlying storage")},
    {"target", VSH_OT_DATA, 0, gettext_noop("target for underlying storage")},
    {NULL, 0, 0, NULL}
};


static int
cmdPoolDefineAs(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int found;
    char *name, *type, *srcHost, *srcPath, *srcDev, *target;
    virBuffer buf;

    memset(&buf, 0, sizeof(buf));

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    name = vshCommandOptString(cmd, "name", &found);
    if (!found)
        goto cleanup;
    type = vshCommandOptString(cmd, "type", &found);
    if (!found)
        goto cleanup;

    srcHost = vshCommandOptString(cmd, "source-host", &found);
    srcPath = vshCommandOptString(cmd, "source-path", &found);
    srcDev = vshCommandOptString(cmd, "source-dev", &found);
    target = vshCommandOptString(cmd, "target", &found);

    if (virBufferVSprintf(&buf, "<pool type='%s'>\n", type) < 0)
        goto cleanup;
    if (virBufferVSprintf(&buf, "  <name>%s</name>\n", name) < 0)
        goto cleanup;
    if (srcHost || srcPath || srcDev) {
        if (virBufferAddLit(&buf, "  <source>\n") < 0)
            goto cleanup;
        if (srcHost &&
            virBufferVSprintf(&buf, "    <host>%s</host>\n", srcHost) < 0)
            goto cleanup;
        if (srcPath &&
            virBufferVSprintf(&buf, "    <path>%s</path>\n", srcPath) < 0)
            goto cleanup;
        if (srcDev &&
            virBufferVSprintf(&buf, "    <device>%s</device>\n", srcDev) < 0)
            goto cleanup;

        if (virBufferAddLit(&buf, "  </source>\n") < 0)
            goto cleanup;
    }
    if (target) {
        if (virBufferAddLit(&buf, "  <target>\n") < 0)
            goto cleanup;
        if (virBufferVSprintf(&buf, "    <path>%s</path>\n", target) < 0)
            goto cleanup;
        if (virBufferAddLit(&buf, "  </target>\n") < 0)
            goto cleanup;
    }
    if (virBufferAddLit(&buf, "</pool>\n") < 0)
        goto cleanup;

    pool = virStoragePoolDefineXML(ctl->conn, buf.content, 0);
    free (buf.content);

    if (pool != NULL) {
        vshPrint(ctl, _("Pool %s defined\n"), name);
        virStoragePoolFree(pool);
        return TRUE;
    } else {
        vshError(ctl, FALSE, _("Failed to define pool %s"), name);
        return FALSE;
    }

 cleanup:
    free(buf.content);
    return FALSE;
}


/*
 * "pool-build" command
 */
static vshCmdInfo info_pool_build[] = {
    {"syntax", "pool-build <pool>"},
    {"help", gettext_noop("build a pool")},
    {"desc", gettext_noop("Build a given pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_build[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolBuild(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return FALSE;

    if (virStoragePoolBuild(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s builded\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to build pool %s"), name);
        ret = FALSE;
        virStoragePoolFree(pool);
    }

    return ret;
}


/*
 * "pool-destroy" command
 */
static vshCmdInfo info_pool_destroy[] = {
    {"syntax", "pool-destroy <pool>"},
    {"help", gettext_noop("destroy a pool")},
    {"desc", gettext_noop("Destroy a given pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_destroy[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolDestroy(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return FALSE;

    if (virStoragePoolDestroy(pool) == 0) {
        vshPrint(ctl, _("Pool %s destroyed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to destroy pool %s"), name);
        ret = FALSE;
        virStoragePoolFree(pool);
    }

    return ret;
}


/*
 * "pool-delete" command
 */
static vshCmdInfo info_pool_delete[] = {
    {"syntax", "pool-delete <pool>"},
    {"help", gettext_noop("delete a pool")},
    {"desc", gettext_noop("Delete a given pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_delete[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolDelete(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return FALSE;

    if (virStoragePoolDelete(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s deleteed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to delete pool %s"), name);
        ret = FALSE;
        virStoragePoolFree(pool);
    }

    return ret;
}


/*
 * "pool-refresh" command
 */
static vshCmdInfo info_pool_refresh[] = {
    {"syntax", "pool-refresh <pool>"},
    {"help", gettext_noop("refresh a pool")},
    {"desc", gettext_noop("Refresh a given pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_refresh[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolRefresh(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return FALSE;

    if (virStoragePoolRefresh(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s refreshed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to refresh pool %s"), name);
        ret = FALSE;
    }
    virStoragePoolFree(pool);

    return ret;
}


/*
 * "pool-dumpxml" command
 */
static vshCmdInfo info_pool_dumpxml[] = {
    {"syntax", "pool-dumpxml <pool>"},
    {"help", gettext_noop("pool information in XML")},
    {"desc", gettext_noop("Output the pool information as an XML dump to stdout.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_dumpxml[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolDumpXML(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;
    char *dump;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", NULL)))
        return FALSE;

    dump = virStoragePoolGetXMLDesc(pool, 0);
    if (dump != NULL) {
        printf("%s", dump);
        free(dump);
    } else {
        ret = FALSE;
    }

    virStoragePoolFree(pool);
    return ret;
}


/*
 * "pool-list" command
 */
static vshCmdInfo info_pool_list[] = {
    {"syntax", "pool-list [ --inactive | --all ]"},
    {"help", gettext_noop("list pools")},
    {"desc", gettext_noop("Returns list of pools.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_list[] = {
    {"inactive", VSH_OT_BOOL, 0, gettext_noop("list inactive pools")},
    {"all", VSH_OT_BOOL, 0, gettext_noop("list inactive & active pools")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolList(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    int inactive = vshCommandOptBool(cmd, "inactive");
    int all = vshCommandOptBool(cmd, "all");
    int active = !inactive || all ? 1 : 0;
    int maxactive = 0, maxinactive = 0, i;
    char **activeNames = NULL, **inactiveNames = NULL;
    inactive |= all;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (active) {
        maxactive = virConnectNumOfStoragePools(ctl->conn);
        if (maxactive < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list active pools"));
            return FALSE;
        }
        if (maxactive) {
            activeNames = vshMalloc(ctl, sizeof(char *) * maxactive);

            if ((maxactive = virConnectListStoragePools(ctl->conn, activeNames,
                                                        maxactive)) < 0) {
                vshError(ctl, FALSE, "%s", _("Failed to list active pools"));
                free(activeNames);
                return FALSE;
            }

            qsort(&activeNames[0], maxactive, sizeof(char *), namesorter);
        }
    }
    if (inactive) {
        maxinactive = virConnectNumOfDefinedStoragePools(ctl->conn);
        if (maxinactive < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list inactive pools"));
            free(activeNames);
            return FALSE;
        }
        if (maxinactive) {
            inactiveNames = vshMalloc(ctl, sizeof(char *) * maxinactive);

            if ((maxinactive = virConnectListDefinedStoragePools(ctl->conn, inactiveNames, maxinactive)) < 0) {
                vshError(ctl, FALSE, "%s", _("Failed to list inactive pools"));
                free(activeNames);
                free(inactiveNames);
                return FALSE;
            }

            qsort(&inactiveNames[0], maxinactive, sizeof(char*), namesorter);
        }
    }
    vshPrintExtra(ctl, "%-20s %-10s %-10s\n", _("Name"), _("State"), _("Autostart"));
    vshPrintExtra(ctl, "-----------------------------------------\n");

    for (i = 0; i < maxactive; i++) {
        virStoragePoolPtr pool = virStoragePoolLookupByName(ctl->conn, activeNames[i]);
        const char *autostartStr;
        int autostart = 0;

        /* this kind of work with pools is not atomic operation */
        if (!pool) {
            free(activeNames[i]);
            continue;
        }

        if (virStoragePoolGetAutostart(pool, &autostart) < 0)
            autostartStr = _("no autostart");
        else
            autostartStr = autostart ? "yes" : "no";

        vshPrint(ctl, "%-20s %-10s %-10s\n",
                 virStoragePoolGetName(pool),
                 _("active"),
                 autostartStr);
        virStoragePoolFree(pool);
        free(activeNames[i]);
    }
    for (i = 0; i < maxinactive; i++) {
        virStoragePoolPtr pool = virStoragePoolLookupByName(ctl->conn, inactiveNames[i]);
        const char *autostartStr;
        int autostart = 0;

        /* this kind of work with pools is not atomic operation */
        if (!pool) {
            free(inactiveNames[i]);
            continue;
        }

        if (virStoragePoolGetAutostart(pool, &autostart) < 0)
            autostartStr = _("no autostart");
        else
            autostartStr = autostart ? "yes" : "no";

        vshPrint(ctl, "%-20s %-10s %-10s\n",
                 inactiveNames[i],
                 _("inactive"),
                 autostartStr);

        virStoragePoolFree(pool);
        free(inactiveNames[i]);
    }
    free(activeNames);
    free(inactiveNames);
    return TRUE;
}

static double
prettyCapacity(unsigned long long val,
               const char **unit) {
    if (val < 1024) {
        *unit = "";
        return (double)val;
    } else if (val < (1024.0l * 1024.0l)) {
        *unit = "KB";
        return (((double)val / 1024.0l));
    } else if (val < (1024.0l * 1024.0l * 1024.0l)) {
        *unit = "MB";
        return ((double)val / (1024.0l * 1024.0l));
    } else if (val < (1024.0l * 1024.0l * 1024.0l * 1024.0l)) {
        *unit = "GB";
        return ((double)val / (1024.0l * 1024.0l * 1024.0l));
    } else {
        *unit = "TB";
        return ((double)val / (1024.0l * 1024.0l * 1024.0l * 1024.0l));
    }
}

/*
 * "pool-info" command
 */
static vshCmdInfo info_pool_info[] = {
    {"syntax", "pool-info <pool>"},
    {"help", gettext_noop("storage pool information")},
    {"desc", gettext_noop("Returns basic information about the storage pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_info[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolInfo(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolInfo info;
    virStoragePoolPtr pool;
    int ret = TRUE;
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", NULL)))
        return FALSE;

    vshPrint(ctl, "%-15s %s\n", _("Name:"), virStoragePoolGetName(pool));

    if (virStoragePoolGetUUIDString(pool, &uuid[0])==0)
        vshPrint(ctl, "%-15s %s\n", _("UUID:"), uuid);

    if (virStoragePoolGetInfo(pool, &info) == 0) {
        double val;
        const char *unit;
        switch (info.state) {
        case VIR_STORAGE_POOL_INACTIVE:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("inactive"));
            break;
        case VIR_STORAGE_POOL_BUILDING:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("building"));
            break;
        case VIR_STORAGE_POOL_RUNNING:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("running"));
            break;
        case VIR_STORAGE_POOL_DEGRADED:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("degraded"));
            break;
        }

        if (info.state == VIR_STORAGE_POOL_RUNNING ||
            info.state == VIR_STORAGE_POOL_DEGRADED) {
            val = prettyCapacity(info.capacity, &unit);
            vshPrint(ctl, "%-15s %2.2lf %s\n", _("Capacity:"), val, unit);

            val = prettyCapacity(info.allocation, &unit);
            vshPrint(ctl, "%-15s %2.2lf %s\n", _("Allocation:"), val, unit);

            val = prettyCapacity(info.available, &unit);
            vshPrint(ctl, "%-15s %2.2lf %s\n", _("Available:"), val, unit);
        }
    } else {
        ret = FALSE;
    }

    virStoragePoolFree(pool);
    return ret;
}


/*
 * "pool-name" command
 */
static vshCmdInfo info_pool_name[] = {
    {"syntax", "pool-name <pool>"},
    {"help", gettext_noop("convert a pool UUID to pool name")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_name[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolName(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;
    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "pool", NULL,
					   VSH_BYUUID)))
        return FALSE;

    vshPrint(ctl, "%s\n", virStoragePoolGetName(pool));
    virStoragePoolFree(pool);
    return TRUE;
}


/*
 * "pool-start" command
 */
static vshCmdInfo info_pool_start[] = {
    {"syntax", "start <pool>"},
    {"help", gettext_noop("start a (previously defined) inactive pool")},
    {"desc", gettext_noop("Start a pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_start[] = {
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("name of the inactive pool")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolStart(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "name", NULL, VSH_BYNAME)))
         return FALSE;

    if (virStoragePoolCreate(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s started\n"),
                 virStoragePoolGetName(pool));
    } else {
        vshError(ctl, FALSE, _("Failed to start pool %s"),
                 virStoragePoolGetName(pool));
        ret = FALSE;
    }
    return ret;
}


/*
 * "vol-create-as" command
 */
static vshCmdInfo info_vol_create_as[] = {
    {"syntax", "create-as <pool> <name> <capacity>"},
    {"help", gettext_noop("create a vol from a set of as")},
    {"desc", gettext_noop("Create a vol.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_create_as[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name")},
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("name of the vol")},
    {"capacity", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("size of the vol with optional k,M,G,T suffix")},
    {"allocation", VSH_OT_DATA, 0, gettext_noop("initial allocation size with optional k,M,G,T suffix")},
    {"format", VSH_OT_DATA, 0, gettext_noop("file format type raw,bochs,qcow,qcow2,vmdk")},
    {NULL, 0, 0, NULL}
};

static int cmdVolSize(const char *data, unsigned long long *val)
{
    char *end;
    if (virStrToLong_ull(data, &end, 10, val) < 0)
        return -1;

    if (end && *end) {
        /* Delibrate fallthrough cases here :-) */
        switch (*end) {
        case 'T':
            *val *= 1024;
        case 'G':
            *val *= 1024;
        case 'M':
            *val *= 1024;
        case 'k':
            *val *= 1024;
            break;
        default:
            return -1;
        }
        end++;
        if (*end)
            return -1;
    }
    return 0;
}

static int
cmdVolCreateAs(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    virStorageVolPtr vol;
    int found;
    char *name, *capacityStr, *allocationStr, *format;
    unsigned long long capacity, allocation = 0;
    virBuffer buf;

    memset(&buf, 0, sizeof(buf));

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "pool", NULL,
                                     VSH_BYNAME)))
        return FALSE;

    name = vshCommandOptString(cmd, "name", &found);
    if (!found)
        goto cleanup;

    capacityStr = vshCommandOptString(cmd, "capacity", &found);
    if (!found)
        goto cleanup;
    if (cmdVolSize(capacityStr, &capacity) < 0)
        vshError(ctl, FALSE, _("Malformed size %s"), capacityStr);

    allocationStr = vshCommandOptString(cmd, "allocation", &found);
    if (allocationStr &&
        cmdVolSize(allocationStr, &allocation) < 0)
        vshError(ctl, FALSE, _("Malformed size %s"), allocationStr);

    format = vshCommandOptString(cmd, "format", &found);

    if (virBufferAddLit(&buf, "<volume>\n") < 0)
        goto cleanup;
    if (virBufferVSprintf(&buf, "  <name>%s</name>\n", name) < 0)
        goto cleanup;
    if (virBufferVSprintf(&buf, "  <capacity>%llu</capacity>\n", capacity) < 0)
        goto cleanup;
    if (allocationStr &&
        virBufferVSprintf(&buf, "  <allocation>%llu</allocation>\n", allocation) < 0)
        goto cleanup;

    if (format) {
        if (virBufferAddLit(&buf, "  <target>\n") < 0)
            goto cleanup;
        if (format)
            if (virBufferVSprintf(&buf, "    <format type='%s'/>\n",format) < 0)
                goto cleanup;
        if (virBufferAddLit(&buf, "  </target>\n") < 0)
            goto cleanup;
    }
    if (virBufferAddLit(&buf, "</volume>\n") < 0)
        goto cleanup;

    vol = virStorageVolCreateXML(pool, buf.content, 0);
    free (buf.content);
    virStoragePoolFree(pool);

    if (vol != NULL) {
        vshPrint(ctl, _("Vol %s created\n"), name);
        virStorageVolFree(vol);
        return TRUE;
    } else {
        vshError(ctl, FALSE, _("Failed to create vol %s"), name);
        return FALSE;
    }

 cleanup:
    free(buf.content);
    virStoragePoolFree(pool);
    return FALSE;
}


/*
 * "pool-undefine" command
 */
static vshCmdInfo info_pool_undefine[] = {
    {"syntax", "pool-undefine <pool>"},
    {"help", gettext_noop("undefine an inactive pool")},
    {"desc", gettext_noop("Undefine the configuration for an inactive pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_undefine[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolUndefine(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return FALSE;

    if (virStoragePoolUndefine(pool) == 0) {
        vshPrint(ctl, _("Pool %s has been undefined\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to undefine pool %s"), name);
        ret = FALSE;
    }

    return ret;
}


/*
 * "pool-uuid" command
 */
static vshCmdInfo info_pool_uuid[] = {
    {"syntax", "pool-uuid <pool>"},
    {"help", gettext_noop("convert a pool name to pool UUID")},
    {NULL, NULL}
};

static vshCmdOptDef opts_pool_uuid[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name")},
    {NULL, 0, 0, NULL}
};

static int
cmdPoolUuid(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "pool", NULL,
					   VSH_BYNAME)))
        return FALSE;

    if (virStoragePoolGetUUIDString(pool, uuid) != -1)
        vshPrint(ctl, "%s\n", uuid);
    else
        vshError(ctl, FALSE, "%s", _("failed to get pool UUID"));

    return TRUE;
}




/*
 * "vol-create" command
 */
static vshCmdInfo info_vol_create[] = {
    {"syntax", "create <file>"},
    {"help", gettext_noop("create a vol from an XML file")},
    {"desc", gettext_noop("Create a vol.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_create[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name")},
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("file containing an XML vol description")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolCreate(vshControl * ctl, vshCmd * cmd)
{
    virStoragePoolPtr pool;
    virStorageVolPtr vol;
    char *from;
    int found;
    int ret = TRUE;
    char *buffer;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "pool", NULL,
					   VSH_BYNAME)))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found) {
        virStoragePoolFree(pool);
        return FALSE;
    }

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0) {
        virStoragePoolFree(pool);
        return FALSE;
    }

    vol = virStorageVolCreateXML(pool, buffer, 0);
    free (buffer);
    virStoragePoolFree(pool);

    if (vol != NULL) {
        vshPrint(ctl, _("Vol %s created from %s\n"),
                 virStorageVolGetName(vol), from);
        virStorageVolFree(vol);
    } else {
        vshError(ctl, FALSE, _("Failed to create vol from %s"), from);
        ret = FALSE;
    }
    return ret;
}

/*
 * "vol-delete" command
 */
static vshCmdInfo info_vol_delete[] = {
    {"syntax", "vol-delete <vol>"},
    {"help", gettext_noop("delete a vol")},
    {"desc", gettext_noop("Delete a given vol.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_delete[] = {
    {"pool", VSH_OT_STRING, 0, gettext_noop("pool name or uuid")},
    {"vol", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vol name, key or path")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolDelete(vshControl * ctl, vshCmd * cmd)
{
    virStorageVolPtr vol;
    int ret = TRUE;
    char *name;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(vol = vshCommandOptVol(ctl, cmd, "vol", "pool", &name))) {
        return FALSE;
    }

    if (virStorageVolDelete(vol, 0) == 0) {
        vshPrint(ctl, _("Vol %s deleteed\n"), name);
    } else {
        vshError(ctl, FALSE, _("Failed to delete vol %s"), name);
        ret = FALSE;
        virStorageVolFree(vol);
    }

    return ret;
}


/*
 * "vol-info" command
 */
static vshCmdInfo info_vol_info[] = {
    {"syntax", "vol-info <vol>"},
    {"help", gettext_noop("storage vol information")},
    {"desc", gettext_noop("Returns basic information about the storage vol.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_info[] = {
    {"pool", VSH_OT_STRING, 0, gettext_noop("pool name or uuid")},
    {"vol", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vol name, key or path")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolInfo(vshControl * ctl, vshCmd * cmd)
{
    virStorageVolInfo info;
    virStorageVolPtr vol;
    int ret = TRUE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(vol = vshCommandOptVol(ctl, cmd, "vol", "pool", NULL)))
        return FALSE;

    vshPrint(ctl, "%-15s %s\n", _("Name:"), virStorageVolGetName(vol));

    if (virStorageVolGetInfo(vol, &info) == 0) {
        double val;
        const char *unit;
        vshPrint(ctl, "%-15s %s\n", _("Type:"),
                 info.type == VIR_STORAGE_VOL_FILE ?
                 _("file") : _("block"));

        val = prettyCapacity(info.capacity, &unit);
        vshPrint(ctl, "%-15s %2.2lf %s\n", _("Capacity:"), val, unit);

        val = prettyCapacity(info.allocation, &unit);
        vshPrint(ctl, "%-15s %2.2lf %s\n", _("Allocation:"), val, unit);
    } else {
        ret = FALSE;
    }

    virStorageVolFree(vol);
    return ret;
}


/*
 * "vol-dumpxml" command
 */
static vshCmdInfo info_vol_dumpxml[] = {
    {"syntax", "vol-dumpxml <vol>"},
    {"help", gettext_noop("vol information in XML")},
    {"desc", gettext_noop("Output the vol information as an XML dump to stdout.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_dumpxml[] = {
    {"pool", VSH_OT_STRING, 0, gettext_noop("pool name or uuid")},
    {"vol", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vol name, key or path")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolDumpXML(vshControl * ctl, vshCmd * cmd)
{
    virStorageVolPtr vol;
    int ret = TRUE;
    char *dump;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(vol = vshCommandOptVol(ctl, cmd, "vol", "pool", NULL)))
        return FALSE;

    dump = virStorageVolGetXMLDesc(vol, 0);
    if (dump != NULL) {
        printf("%s", dump);
        free(dump);
    } else {
        ret = FALSE;
    }

    virStorageVolFree(vol);
    return ret;
}


/*
 * "vol-list" command
 */
static vshCmdInfo info_vol_list[] = {
    {"syntax", "vol-list <pool>"},
    {"help", gettext_noop("list vols")},
    {"desc", gettext_noop("Returns list of vols by pool.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_list[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolList(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    virStoragePoolPtr pool;
    int maxactive = 0, i;
    char **activeNames = NULL;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", NULL)))
        return FALSE;

    maxactive = virStoragePoolNumOfVolumes(pool);
    if (maxactive < 0) {
        virStoragePoolFree(pool);
        vshError(ctl, FALSE, "%s", _("Failed to list active vols"));
        return FALSE;
    }
    if (maxactive) {
        activeNames = vshMalloc(ctl, sizeof(char *) * maxactive);

        if ((maxactive = virStoragePoolListVolumes(pool, activeNames,
                                                   maxactive)) < 0) {
            vshError(ctl, FALSE, "%s", _("Failed to list active vols"));
            free(activeNames);
            virStoragePoolFree(pool);
            return FALSE;
        }

        qsort(&activeNames[0], maxactive, sizeof(char *), namesorter);
    }
    vshPrintExtra(ctl, "%-20s %-40s\n", _("Name"), _("Path"));
    vshPrintExtra(ctl, "-----------------------------------------\n");

    for (i = 0; i < maxactive; i++) {
        virStorageVolPtr vol = virStorageVolLookupByName(pool, activeNames[i]);
        char *path;

        /* this kind of work with vols is not atomic operation */
        if (!vol) {
            free(activeNames[i]);
            continue;
        }

        if ((path = virStorageVolGetPath(vol)) == NULL) {
            virStorageVolFree(vol);
            continue;
        }


        vshPrint(ctl, "%-20s %-40s\n",
                 virStorageVolGetName(vol),
                 path);
        free(path);
        virStorageVolFree(vol);
        free(activeNames[i]);
    }
    free(activeNames);
    virStoragePoolFree(pool);
    return TRUE;
}


/*
 * "vol-name" command
 */
static vshCmdInfo info_vol_name[] = {
    {"syntax", "vol-name <vol>"},
    {"help", gettext_noop("convert a vol UUID to vol name")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_name[] = {
    {"vol", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vol key or path")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolName(vshControl * ctl, vshCmd * cmd)
{
    virStorageVolPtr vol;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(vol = vshCommandOptVolBy(ctl, cmd, "vol", "pool", NULL,
                                   VSH_BYUUID)))
        return FALSE;

    vshPrint(ctl, "%s\n", virStorageVolGetName(vol));
    virStorageVolFree(vol);
    return TRUE;
}



/*
 * "vol-key" command
 */
static vshCmdInfo info_vol_key[] = {
    {"syntax", "vol-key <vol>"},
    {"help", gettext_noop("convert a vol UUID to vol key")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_key[] = {
    {"vol", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vol uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolKey(vshControl * ctl, vshCmd * cmd)
{
    virStorageVolPtr vol;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(vol = vshCommandOptVolBy(ctl, cmd, "vol", NULL, NULL,
                                   VSH_BYUUID)))
        return FALSE;

    vshPrint(ctl, "%s\n", virStorageVolGetKey(vol));
    virStorageVolFree(vol);
    return TRUE;
}



/*
 * "vol-path" command
 */
static vshCmdInfo info_vol_path[] = {
    {"syntax", "vol-path <pool> <vol>"},
    {"help", gettext_noop("convert a vol UUID to vol path")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vol_path[] = {
    {"pool", VSH_OT_STRING, 0, gettext_noop("pool name or uuid")},
    {"vol", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("vol name or key")},
    {NULL, 0, 0, NULL}
};

static int
cmdVolPath(vshControl * ctl, vshCmd * cmd)
{
    virStorageVolPtr vol;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;
    if (!(vol = vshCommandOptVolBy(ctl, cmd, "vol", "pool", NULL,
                                   VSH_BYUUID)))
        return FALSE;

    vshPrint(ctl, "%s\n", virStorageVolGetPath(vol));
    virStorageVolFree(vol);
    return TRUE;
}







/*
 * "version" command
 */
static vshCmdInfo info_version[] = {
    {"syntax", "version"},
    {"help", gettext_noop("show version")},
    {"desc", gettext_noop("Display the system version information.")},
    {NULL, NULL}
};


static int
cmdVersion(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    unsigned long hvVersion;
    const char *hvType;
    unsigned long libVersion;
    unsigned long includeVersion;
    unsigned long apiVersion;
    int ret;
    unsigned int major;
    unsigned int minor;
    unsigned int rel;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    hvType = virConnectGetType(ctl->conn);
    if (hvType == NULL) {
        vshError(ctl, FALSE, "%s", _("failed to get hypervisor type"));
        return FALSE;
    }

    includeVersion = LIBVIR_VERSION_NUMBER;
    major = includeVersion / 1000000;
    includeVersion %= 1000000;
    minor = includeVersion / 1000;
    rel = includeVersion % 1000;
    vshPrint(ctl, _("Compiled against library: libvir %d.%d.%d\n"),
             major, minor, rel);

    ret = virGetVersion(&libVersion, hvType, &apiVersion);
    if (ret < 0) {
        vshError(ctl, FALSE, "%s", _("failed to get the library version"));
        return FALSE;
    }
    major = libVersion / 1000000;
    libVersion %= 1000000;
    minor = libVersion / 1000;
    rel = libVersion % 1000;
    vshPrint(ctl, _("Using library: libvir %d.%d.%d\n"),
             major, minor, rel);

    major = apiVersion / 1000000;
    apiVersion %= 1000000;
    minor = apiVersion / 1000;
    rel = apiVersion % 1000;
    vshPrint(ctl, _("Using API: %s %d.%d.%d\n"), hvType,
             major, minor, rel);

    ret = virConnectGetVersion(ctl->conn, &hvVersion);
    if (ret < 0) {
        vshError(ctl, FALSE, "%s", _("failed to get the hypervisor version"));
        return FALSE;
    }
    if (hvVersion == 0) {
        vshPrint(ctl,
                 _("Cannot extract running %s hypervisor version\n"), hvType);
    } else {
        major = hvVersion / 1000000;
        hvVersion %= 1000000;
        minor = hvVersion / 1000;
        rel = hvVersion % 1000;

        vshPrint(ctl, _("Running hypervisor: %s %d.%d.%d\n"),
                 hvType, major, minor, rel);
    }
    return TRUE;
}

/*
 * "hostkey" command
 */
static vshCmdInfo info_hostname[] = {
    {"syntax", "hostname"},
    {"help", gettext_noop("print the hypervisor hostname")},
    {NULL, NULL}
};

static int
cmdHostname (vshControl *ctl, vshCmd *cmd ATTRIBUTE_UNUSED)
{
    char *hostname;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    hostname = virConnectGetHostname (ctl->conn);
    if (hostname == NULL) {
        vshError(ctl, FALSE, "%s", _("failed to get hostname"));
        return FALSE;
    }

    vshPrint (ctl, "%s\n", hostname);
    free (hostname);

    return TRUE;
}

/*
 * "uri" command
 */
static vshCmdInfo info_uri[] = {
    {"syntax", "uri"},
    {"help", gettext_noop("print the hypervisor canonical URI")},
    {NULL, NULL}
};

static int
cmdURI (vshControl *ctl, vshCmd *cmd ATTRIBUTE_UNUSED)
{
    char *uri;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    uri = virConnectGetURI (ctl->conn);
    if (uri == NULL) {
        vshError(ctl, FALSE, "%s", _("failed to get URI"));
        return FALSE;
    }

    vshPrint (ctl, "%s\n", uri);
    free (uri);

    return TRUE;
}

/*
 * "vncdisplay" command
 */
static vshCmdInfo info_vncdisplay[] = {
    {"syntax", "vncdisplay <domain>"},
    {"help", gettext_noop("vnc display")},
    {"desc", gettext_noop("Output the IP address and port number for the VNC display.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_vncdisplay[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdVNCDisplay(vshControl * ctl, vshCmd * cmd)
{
    xmlDocPtr xml = NULL;
    xmlXPathObjectPtr obj = NULL;
    xmlXPathContextPtr ctxt = NULL;
    virDomainPtr dom;
    int ret = FALSE;
    int port = 0;
    char *doc;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    doc = virDomainGetXMLDesc(dom, 0);
    if (!doc)
        goto cleanup;

    xml = xmlReadDoc((const xmlChar *) doc, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOWARNING);
    free(doc);
    if (!xml)
        goto cleanup;
    ctxt = xmlXPathNewContext(xml);
    if (!ctxt)
        goto cleanup;

    obj = xmlXPathEval(BAD_CAST "string(/domain/devices/graphics[@type='vnc']/@port)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        goto cleanup;
    }
    if (virStrToLong_i((const char *)obj->stringval, NULL, 10, &port) || port < 0)
        goto cleanup;
    xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "string(/domain/devices/graphics[@type='vnc']/@listen)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0) ||
        !strcmp((const char*)obj->stringval, "0.0.0.0")) {
        vshPrint(ctl, ":%d\n", port-5900);
    } else {
        vshPrint(ctl, "%s:%d\n", (const char *)obj->stringval, port-5900);
    }
    xmlXPathFreeObject(obj);
    obj = NULL;
    ret = TRUE;

 cleanup:
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (xml)
        xmlFreeDoc(xml);
    virDomainFree(dom);
    return ret;
}

/*
 * "ttyconsole" command
 */
static vshCmdInfo info_ttyconsole[] = {
    {"syntax", "ttyconsole <domain>"},
    {"help", gettext_noop("tty console")},
    {"desc", gettext_noop("Output the device for the TTY console.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_ttyconsole[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {NULL, 0, 0, NULL}
};

static int
cmdTTYConsole(vshControl * ctl, vshCmd * cmd)
{
    xmlDocPtr xml = NULL;
    xmlXPathObjectPtr obj = NULL;
    xmlXPathContextPtr ctxt = NULL;
    virDomainPtr dom;
    int ret = FALSE;
    char *doc;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    doc = virDomainGetXMLDesc(dom, 0);
    if (!doc)
        goto cleanup;

    xml = xmlReadDoc((const xmlChar *) doc, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOWARNING);
    free(doc);
    if (!xml)
        goto cleanup;
    ctxt = xmlXPathNewContext(xml);
    if (!ctxt)
        goto cleanup;

    obj = xmlXPathEval(BAD_CAST "string(/domain/devices/console/@tty)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        goto cleanup;
    }
    vshPrint(ctl, "%s\n", (const char *)obj->stringval);

 cleanup:
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (xml)
        xmlFreeDoc(xml);
    virDomainFree(dom);
    return ret;
}

/*
 * "attach-device" command
 */
static vshCmdInfo info_attach_device[] = {
    {"syntax", "attach-device <domain> <file> "},
    {"help", gettext_noop("attach device from an XML file")},
    {"desc", gettext_noop("Attach device from an XML <file>.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_attach_device[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"file",   VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("XML file")},
    {NULL, 0, 0, NULL}
};

static int
cmdAttachDevice(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *from;
    char *buffer;
    int ret = TRUE;
    int found;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found) {
        virDomainFree(dom);
        return FALSE;
    }

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0) {
        virDomainFree(dom);
        return FALSE;
    }

    ret = virDomainAttachDevice(dom, buffer);
    free (buffer);

    if (ret < 0) {
        vshError(ctl, FALSE, _("Failed to attach device from %s"), from);
        virDomainFree(dom);
        return FALSE;
    }

    virDomainFree(dom);
    return TRUE;
}


/*
 * "detach-device" command
 */
static vshCmdInfo info_detach_device[] = {
    {"syntax", "detach-device <domain> <file> "},
    {"help", gettext_noop("detach device from an XML file")},
    {"desc", gettext_noop("Detach device from an XML <file>")},
    {NULL, NULL}
};

static vshCmdOptDef opts_detach_device[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"file",   VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("XML file")},
    {NULL, 0, 0, NULL}
};

static int
cmdDetachDevice(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom;
    char *from;
    char *buffer;
    int ret = TRUE;
    int found;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        return FALSE;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        return FALSE;

    from = vshCommandOptString(cmd, "file", &found);
    if (!found) {
        virDomainFree(dom);
        return FALSE;
    }

    if (virFileReadAll(from, VIRSH_MAX_XML_FILE, &buffer) < 0) {
        virDomainFree(dom);
        return FALSE;
    }

    ret = virDomainDetachDevice(dom, buffer);
    free (buffer);

    if (ret < 0) {
        vshError(ctl, FALSE, _("Failed to detach device from %s"), from);
        virDomainFree(dom);
        return FALSE;
    }

    virDomainFree(dom);
    return TRUE;
}


/*
 * "attach-interface" command
 */
static vshCmdInfo info_attach_interface[] = {
    {"syntax", "attach-interface <domain> <type> <source> [--target <target>] [--mac <mac>] [--script <script>] "},
    {"help", gettext_noop("attach network interface")},
    {"desc", gettext_noop("Attach new network interface.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_attach_interface[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"type",   VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network interface type")},
    {"source", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("source of network interface")},
    {"target", VSH_OT_DATA, 0, gettext_noop("target network name")},
    {"mac",    VSH_OT_DATA, 0, gettext_noop("MAC adress")},
    {"script", VSH_OT_DATA, 0, gettext_noop("script used to bridge network interface")},
    {NULL, 0, 0, NULL}
};

static int
cmdAttachInterface(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom = NULL;
    char *mac, *target, *script, *type, *source;
    int typ, ret = FALSE;
    char *buf = NULL, *tmp = NULL;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        goto cleanup;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        goto cleanup;

    if (!(type = vshCommandOptString(cmd, "type", NULL)))
        goto cleanup;

    source = vshCommandOptString(cmd, "source", NULL);
    target = vshCommandOptString(cmd, "target", NULL);
    mac = vshCommandOptString(cmd, "mac", NULL);
    script = vshCommandOptString(cmd, "script", NULL);

    /* check interface type */
    if (strcmp(type, "network") == 0) {
        typ = 1;
    } else if (strcmp(type, "bridge") == 0) {
        typ = 2;
    } else {
        vshError(ctl, FALSE, _("No support %s in command 'attach-interface'"), type);
        goto cleanup;
    }

    /* Make XML of interface */
    tmp = vshMalloc(ctl, 1);
    if (!tmp) goto cleanup;
    buf = vshMalloc(ctl, strlen(type) + 25);
    if (!buf) goto cleanup;
    sprintf(buf, "    <interface type='%s'>\n" , type);

    tmp = vshRealloc(ctl, tmp, strlen(source) + 28);
    if (!tmp) goto cleanup;
    if (typ == 1) {
        sprintf(tmp, "      <source network='%s'/>\n", source);
    } else if (typ == 2) {
        sprintf(tmp, "      <source bridge='%s'/>\n", source);
    }
    buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
    if (!buf) goto cleanup;
    strcat(buf, tmp);

    if (target != NULL) {
        tmp = vshRealloc(ctl, tmp, strlen(target) + 24);
        if (!tmp) goto cleanup;
        sprintf(tmp, "      <target dev='%s'/>\n", target);
        buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
        if (!buf) goto cleanup;
        strcat(buf, tmp);
    }

    if (mac != NULL) {
        tmp = vshRealloc(ctl, tmp, strlen(mac) + 25);
        if (!tmp) goto cleanup;
        sprintf(tmp, "      <mac address='%s'/>\n", mac);
        buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
        if (!buf) goto cleanup;
        strcat(buf, tmp);
    }

    if (script != NULL) {
        tmp = vshRealloc(ctl, tmp, strlen(script) + 25);
        if (!tmp) goto cleanup;
        sprintf(tmp, "      <script path='%s'/>\n", script);
        buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
        if (!buf) goto cleanup;
        strcat(buf, tmp);
    }

    buf = vshRealloc(ctl, buf, strlen(buf) + 19);
    if (!buf) goto cleanup;
    strcat(buf, "    </interface>\n");

    if (virDomainAttachDevice(dom, buf))
        goto cleanup;

    ret = TRUE;

 cleanup:
    if (dom)
        virDomainFree(dom);
    free(buf);
    free(tmp);
    return ret;
}

/*
 * "detach-interface" command
 */
static vshCmdInfo info_detach_interface[] = {
    {"syntax", "detach-interface <domain> <type> [--mac <mac>] "},
    {"help", gettext_noop("detach network interface")},
    {"desc", gettext_noop("Detach network interface.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_detach_interface[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"type",   VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("network interface type")},
    {"mac",    VSH_OT_DATA, 0, gettext_noop("MAC adress")},
    {NULL, 0, 0, NULL}
};

static int
cmdDetachInterface(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom = NULL;
    xmlDocPtr xml = NULL;
    xmlXPathObjectPtr obj=NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlNodePtr cur = NULL;
    xmlChar *tmp_mac = NULL;
    xmlBufferPtr xml_buf = NULL;
    char *doc, *mac =NULL, *type;
    char buf[64];
    int i = 0, diff_mac, ret = FALSE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        goto cleanup;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        goto cleanup;

    if (!(type = vshCommandOptString(cmd, "type", NULL)))
        goto cleanup;

    mac = vshCommandOptString(cmd, "mac", NULL);

    doc = virDomainGetXMLDesc(dom, 0);
    if (!doc)
        goto cleanup;

    xml = xmlReadDoc((const xmlChar *) doc, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOWARNING);
    free(doc);
    if (!xml) {
        vshError(ctl, FALSE, "%s", _("Failed to get interface information"));
        goto cleanup;
    }
    ctxt = xmlXPathNewContext(xml);
    if (!ctxt) {
        vshError(ctl, FALSE, "%s", _("Failed to get interface information"));
        goto cleanup;
    }

    sprintf(buf, "/domain/devices/interface[@type='%s']", type);
    obj = xmlXPathEval(BAD_CAST buf, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_NODESET) ||
        (obj->nodesetval == NULL) || (obj->nodesetval->nodeNr == 0)) {
        vshError(ctl, FALSE, _("No found interface whose type is %s"), type);
        goto cleanup;
    }

    if (!mac)
        goto hit;

    /* search mac */
    for (; i < obj->nodesetval->nodeNr; i++) {
        cur = obj->nodesetval->nodeTab[i]->children;
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE && xmlStrEqual(cur->name, BAD_CAST "mac")) {
                tmp_mac = xmlGetProp(cur, BAD_CAST "address");
                diff_mac = virMacAddrCompare ((char *) tmp_mac, mac);
                xmlFree(tmp_mac);
                if (!diff_mac) {
                    goto hit;
                }
            }
            cur = cur->next;
        }
    }
    vshError(ctl, FALSE, _("No found interface whose MAC address is %s"), mac);
    goto cleanup;

 hit:
    xml_buf = xmlBufferCreate();
    if (!xml_buf) {
        vshError(ctl, FALSE, "%s", _("Failed to allocate memory"));
        goto cleanup;
    }

    if(xmlNodeDump(xml_buf, xml, obj->nodesetval->nodeTab[i], 0, 0) < 0){
        vshError(ctl, FALSE, "%s", _("Failed to create XML"));
        goto cleanup;
    }

    ret = virDomainDetachDevice(dom, (char *)xmlBufferContent(xml_buf));
    if (ret != 0)
        ret = FALSE;
    else
        ret = TRUE;

 cleanup:
    if (dom)
        virDomainFree(dom);
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (xml)
        xmlFreeDoc(xml);
    if (xml_buf)
        xmlBufferFree(xml_buf);
    return ret;
}

/*
 * "attach-disk" command
 */
static vshCmdInfo info_attach_disk[] = {
    {"syntax", "attach-disk <domain> <source> <target> [--driver <driver>] [--subdriver <subdriver>] [--type <type>] [--mode <mode>] "},
    {"help", gettext_noop("attach disk device")},
    {"desc", gettext_noop("Attach new disk device.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_attach_disk[] = {
    {"domain",  VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"source",  VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("source of disk device")},
    {"target",  VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("target of disk device")},
    {"driver",    VSH_OT_DATA, 0, gettext_noop("driver of disk device")},
    {"subdriver", VSH_OT_DATA, 0, gettext_noop("subdriver of disk device")},
    {"type",    VSH_OT_DATA, 0, gettext_noop("target device type")},
    {"mode",    VSH_OT_DATA, 0, gettext_noop("mode of device reading and writing")},
    {NULL, 0, 0, NULL}
};

static int
cmdAttachDisk(vshControl * ctl, vshCmd * cmd)
{
    virDomainPtr dom = NULL;
    char *source, *target, *driver, *subdriver, *type, *mode;
    int isFile = 0, ret = FALSE;
    char *buf = NULL, *tmp = NULL;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        goto cleanup;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        goto cleanup;

    if (!(source = vshCommandOptString(cmd, "source", NULL)))
        goto cleanup;

    if (!(target = vshCommandOptString(cmd, "target", NULL)))
        goto cleanup;

    driver = vshCommandOptString(cmd, "driver", NULL);
    subdriver = vshCommandOptString(cmd, "subdriver", NULL);
    type = vshCommandOptString(cmd, "type", NULL);
    mode = vshCommandOptString(cmd, "mode", NULL);

    if (type) {
        if (strcmp(type, "cdrom") && strcmp(type, "disk")) {
            vshError(ctl, FALSE, _("No support %s in command 'attach-disk'"), type);
            goto cleanup;
        }
    }

    if (driver) {
        if (!strcmp(driver, "file") || !strcmp(driver, "tap")) {
            isFile = 1;
        } else if (strcmp(driver, "phy")) {
            vshError(ctl, FALSE, _("No support %s in command 'attach-disk'"), driver);
            goto cleanup;
        }
    }

    if (mode) {
        if (strcmp(mode, "readonly") && strcmp(mode, "shareable")) {
            vshError(ctl, FALSE, _("No support %s in command 'attach-disk'"), mode);
            goto cleanup;
        }
    }

    /* Make XML of disk */
    tmp = vshMalloc(ctl, 1);
    if (!tmp) goto cleanup;
    buf = vshMalloc(ctl, 23);
    if (!buf) goto cleanup;
    if (isFile) {
        sprintf(buf, "    <disk type='file'");
    } else {
        sprintf(buf, "    <disk type='block'");
    }

    if (type) {
        tmp = vshRealloc(ctl, tmp, strlen(type) + 13);
        if (!tmp) goto cleanup;
        sprintf(tmp, " device='%s'>\n", type);
    } else {
        tmp = vshRealloc(ctl, tmp, 3);
        if (!tmp) goto cleanup;
        sprintf(tmp, ">\n");
    }
    buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
    if (!buf) goto cleanup;
    strcat(buf, tmp);

    if (driver) {
        tmp = vshRealloc(ctl, tmp, strlen(driver) + 22);
        if (!tmp) goto cleanup;
        sprintf(tmp, "      <driver name='%s'", driver);
    } else {
        tmp = vshRealloc(ctl, tmp, 25);
        if (!tmp) goto cleanup;
        sprintf(tmp, "      <driver name='phy'");
    }
    buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
    if (!buf) goto cleanup;
    strcat(buf, tmp);

    if (subdriver) {
        tmp = vshRealloc(ctl, tmp, strlen(subdriver) + 12);
        if (!tmp) goto cleanup;
        sprintf(tmp, " type='%s'/>\n", subdriver);
    } else {
        tmp = vshRealloc(ctl, tmp, 4);
        if (!tmp) goto cleanup;
        sprintf(tmp, "/>\n");
    }
    buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
    if (!buf) goto cleanup;
    strcat(buf, tmp);

    tmp = vshRealloc(ctl, tmp, strlen(source) + 25);
    if (!tmp) goto cleanup;
    if (isFile) {
        sprintf(tmp, "      <source file='%s'/>\n", source);
    } else {
        sprintf(tmp, "      <source dev='%s'/>\n", source);
    }
    buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
    if (!buf) goto cleanup;
    strcat(buf, tmp);

    tmp = vshRealloc(ctl, tmp, strlen(target) + 24);
    if (!tmp) goto cleanup;
    sprintf(tmp, "      <target dev='%s'/>\n", target);
    buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
    if (!buf) goto cleanup;
    strcat(buf, tmp);

    if (mode != NULL) {
        tmp = vshRealloc(ctl, tmp, strlen(mode) + 11);
        if (!tmp) goto cleanup;
        sprintf(tmp, "      <%s/>\n", mode);
        buf = vshRealloc(ctl, buf, strlen(buf) + strlen(tmp) + 1);
        if (!buf) goto cleanup;
        strcat(buf, tmp);
    }

    buf = vshRealloc(ctl, buf, strlen(buf) + 13);
    if (!buf) goto cleanup;
    strcat(buf, "    </disk>\n");

    if (virDomainAttachDevice(dom, buf))
        goto cleanup;

    ret = TRUE;

 cleanup:
    if (dom)
        virDomainFree(dom);
    free(buf);
    free(tmp);
    return ret;
}

/*
 * "detach-disk" command
 */
static vshCmdInfo info_detach_disk[] = {
    {"syntax", "detach-disk <domain> <target> "},
    {"help", gettext_noop("detach disk device")},
    {"desc", gettext_noop("Detach disk device.")},
    {NULL, NULL}
};

static vshCmdOptDef opts_detach_disk[] = {
    {"domain", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("domain name, id or uuid")},
    {"target", VSH_OT_DATA, VSH_OFLAG_REQ, gettext_noop("target of disk device")},
    {NULL, 0, 0, NULL}
};

static int
cmdDetachDisk(vshControl * ctl, vshCmd * cmd)
{
    xmlDocPtr xml = NULL;
    xmlXPathObjectPtr obj=NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlNodePtr cur = NULL;
    xmlChar *tmp_tgt = NULL;
    xmlBufferPtr xml_buf = NULL;
    virDomainPtr dom = NULL;
    char *doc, *target;
    int i = 0, diff_tgt, ret = FALSE;

    if (!vshConnectionUsability(ctl, ctl->conn, TRUE))
        goto cleanup;

    if (!(dom = vshCommandOptDomain(ctl, cmd, "domain", NULL)))
        goto cleanup;

    if (!(target = vshCommandOptString(cmd, "target", NULL)))
        goto cleanup;

    doc = virDomainGetXMLDesc(dom, 0);
    if (!doc)
        goto cleanup;

    xml = xmlReadDoc((const xmlChar *) doc, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOWARNING);
    free(doc);
    if (!xml) {
        vshError(ctl, FALSE, "%s", _("Failed to get disk information"));
        goto cleanup;
    }
    ctxt = xmlXPathNewContext(xml);
    if (!ctxt) {
        vshError(ctl, FALSE, "%s", _("Failed to get disk information"));
        goto cleanup;
    }

    obj = xmlXPathEval(BAD_CAST "/domain/devices/disk", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_NODESET) ||
        (obj->nodesetval == NULL) || (obj->nodesetval->nodeNr == 0)) {
        vshError(ctl, FALSE, "%s", _("Failed to get disk information"));
        goto cleanup;
    }

    /* search target */
    for (; i < obj->nodesetval->nodeNr; i++) {
        cur = obj->nodesetval->nodeTab[i]->children;
        while (cur != NULL) {
            if (cur->type == XML_ELEMENT_NODE && xmlStrEqual(cur->name, BAD_CAST "target")) {
                tmp_tgt = xmlGetProp(cur, BAD_CAST "dev");
                diff_tgt = xmlStrEqual(tmp_tgt, BAD_CAST target);
                xmlFree(tmp_tgt);
                if (diff_tgt) {
                    goto hit;
                }
            }
            cur = cur->next;
        }
    }
    vshError(ctl, FALSE, _("No found disk whose target is %s"), target);
    goto cleanup;

 hit:
    xml_buf = xmlBufferCreate();
    if (!xml_buf) {
        vshError(ctl, FALSE, "%s", _("Failed to allocate memory"));
        goto cleanup;
    }

    if(xmlNodeDump(xml_buf, xml, obj->nodesetval->nodeTab[i], 0, 0) < 0){
        vshError(ctl, FALSE, "%s", _("Failed to create XML"));
        goto cleanup;
    }

    ret = virDomainDetachDevice(dom, (char *)xmlBufferContent(xml_buf));
    if (ret != 0)
        ret = FALSE;
    else
        ret = TRUE;

 cleanup:
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (xml)
        xmlFreeDoc(xml);
    if (xml_buf)
        xmlBufferFree(xml_buf);
    if (dom)
        virDomainFree(dom);
    return ret;
}

/*
 * "quit" command
 */
static vshCmdInfo info_quit[] = {
    {"syntax", "quit"},
    {"help", gettext_noop("quit this interactive terminal")},
    {NULL, NULL}
};

static int
cmdQuit(vshControl * ctl, vshCmd * cmd ATTRIBUTE_UNUSED)
{
    ctl->imode = FALSE;
    return TRUE;
}

/*
 * Commands
 */
static vshCmdDef commands[] = {
    {"help", cmdHelp, opts_help, info_help},
    {"attach-device", cmdAttachDevice, opts_attach_device, info_attach_device},
    {"attach-disk", cmdAttachDisk, opts_attach_disk, info_attach_disk},
    {"attach-interface", cmdAttachInterface, opts_attach_interface, info_attach_interface},
    {"autostart", cmdAutostart, opts_autostart, info_autostart},
    {"capabilities", cmdCapabilities, NULL, info_capabilities},
    {"connect", cmdConnect, opts_connect, info_connect},
    {"console", cmdConsole, opts_console, info_console},
    {"create", cmdCreate, opts_create, info_create},
    {"start", cmdStart, opts_start, info_start},
    {"destroy", cmdDestroy, opts_destroy, info_destroy},
    {"detach-device", cmdDetachDevice, opts_detach_device, info_detach_device},
    {"detach-disk", cmdDetachDisk, opts_detach_disk, info_detach_disk},
    {"detach-interface", cmdDetachInterface, opts_detach_interface, info_detach_interface},
    {"define", cmdDefine, opts_define, info_define},
    {"domid", cmdDomid, opts_domid, info_domid},
    {"domuuid", cmdDomuuid, opts_domuuid, info_domuuid},
    {"dominfo", cmdDominfo, opts_dominfo, info_dominfo},
    {"domname", cmdDomname, opts_domname, info_domname},
    {"domstate", cmdDomstate, opts_domstate, info_domstate},
    {"domblkstat", cmdDomblkstat, opts_domblkstat, info_domblkstat},
    {"domifstat", cmdDomIfstat, opts_domifstat, info_domifstat},
    {"dumpxml", cmdDumpXML, opts_dumpxml, info_dumpxml},
    {"freecell", cmdFreecell, opts_freecell, info_freecell},
    {"hostname", cmdHostname, NULL, info_hostname},
    {"list", cmdList, opts_list, info_list},
    {"migrate", cmdMigrate, opts_migrate, info_migrate},

    {"net-autostart", cmdNetworkAutostart, opts_network_autostart, info_network_autostart},
    {"net-create", cmdNetworkCreate, opts_network_create, info_network_create},
    {"net-define", cmdNetworkDefine, opts_network_define, info_network_define},
    {"net-destroy", cmdNetworkDestroy, opts_network_destroy, info_network_destroy},
    {"net-dumpxml", cmdNetworkDumpXML, opts_network_dumpxml, info_network_dumpxml},
    {"net-list", cmdNetworkList, opts_network_list, info_network_list},
    {"net-name", cmdNetworkName, opts_network_name, info_network_name},
    {"net-start", cmdNetworkStart, opts_network_start, info_network_start},
    {"net-undefine", cmdNetworkUndefine, opts_network_undefine, info_network_undefine},
    {"net-uuid", cmdNetworkUuid, opts_network_uuid, info_network_uuid},
    {"nodeinfo", cmdNodeinfo, NULL, info_nodeinfo},

    {"pool-autostart", cmdPoolAutostart, opts_pool_autostart, info_pool_autostart},
    {"pool-build", cmdPoolBuild, opts_pool_build, info_pool_build},
    {"pool-create", cmdPoolCreate, opts_pool_create, info_pool_create},
    {"pool-create-as", cmdPoolCreateAs, opts_pool_create_as, info_pool_create_as},
    {"pool-define", cmdPoolDefine, opts_pool_define, info_pool_define},
    {"pool-define-as", cmdPoolDefineAs, opts_pool_define_as, info_pool_define_as},
    {"pool-destroy", cmdPoolDestroy, opts_pool_destroy, info_pool_destroy},
    {"pool-delete", cmdPoolDelete, opts_pool_delete, info_pool_delete},
    {"pool-dumpxml", cmdPoolDumpXML, opts_pool_dumpxml, info_pool_dumpxml},
    {"pool-info", cmdPoolInfo, opts_pool_info, info_pool_info},
    {"pool-list", cmdPoolList, opts_pool_list, info_pool_list},
    {"pool-name", cmdPoolName, opts_pool_name, info_pool_name},
    {"pool-refresh", cmdPoolRefresh, opts_pool_refresh, info_pool_refresh},
    {"pool-start", cmdPoolStart, opts_pool_start, info_pool_start},
    {"pool-undefine", cmdPoolUndefine, opts_pool_undefine, info_pool_undefine},
    {"pool-uuid", cmdPoolUuid, opts_pool_uuid, info_pool_uuid},

    {"quit", cmdQuit, NULL, info_quit},
    {"reboot", cmdReboot, opts_reboot, info_reboot},
    {"restore", cmdRestore, opts_restore, info_restore},
    {"resume", cmdResume, opts_resume, info_resume},
    {"save", cmdSave, opts_save, info_save},
    {"schedinfo", cmdSchedinfo, opts_schedinfo, info_schedinfo},
    {"dump", cmdDump, opts_dump, info_dump},
    {"shutdown", cmdShutdown, opts_shutdown, info_shutdown},
    {"setmem", cmdSetmem, opts_setmem, info_setmem},
    {"setmaxmem", cmdSetmaxmem, opts_setmaxmem, info_setmaxmem},
    {"setvcpus", cmdSetvcpus, opts_setvcpus, info_setvcpus},
    {"suspend", cmdSuspend, opts_suspend, info_suspend},
    {"ttyconsole", cmdTTYConsole, opts_ttyconsole, info_ttyconsole},
    {"undefine", cmdUndefine, opts_undefine, info_undefine},
    {"uri", cmdURI, NULL, info_uri},

    {"vol-create", cmdVolCreate, opts_vol_create, info_vol_create},
    {"vol-create-as", cmdVolCreateAs, opts_vol_create_as, info_vol_create_as},
    {"vol-delete", cmdVolDelete, opts_vol_delete, info_vol_delete},
    {"vol-dumpxml", cmdVolDumpXML, opts_vol_dumpxml, info_vol_dumpxml},
    {"vol-info", cmdVolInfo, opts_vol_info, info_vol_info},
    {"vol-list", cmdVolList, opts_vol_list, info_vol_list},
    {"vol-path", cmdVolPath, opts_vol_path, info_vol_path},
    {"vol-name", cmdVolName, opts_vol_name, info_vol_name},
    {"vol-key", cmdVolKey, opts_vol_key, info_vol_key},

    {"vcpuinfo", cmdVcpuinfo, opts_vcpuinfo, info_vcpuinfo},
    {"vcpupin", cmdVcpupin, opts_vcpupin, info_vcpupin},
    {"version", cmdVersion, NULL, info_version},
    {"vncdisplay", cmdVNCDisplay, opts_vncdisplay, info_vncdisplay},
    {NULL, NULL, NULL, NULL}
};

/* ---------------
 * Utils for work with command definition
 * ---------------
 */
static const char *
vshCmddefGetInfo(vshCmdDef * cmd, const char *name)
{
    vshCmdInfo *info;

    for (info = cmd->info; info && info->name; info++) {
        if (strcmp(info->name, name) == 0)
            return info->data;
    }
    return NULL;
}

static vshCmdOptDef *
vshCmddefGetOption(vshCmdDef * cmd, const char *name)
{
    vshCmdOptDef *opt;

    for (opt = cmd->opts; opt && opt->name; opt++)
        if (strcmp(opt->name, name) == 0)
            return opt;
    return NULL;
}

static vshCmdOptDef *
vshCmddefGetData(vshCmdDef * cmd, int data_ct)
{
    vshCmdOptDef *opt;

    for (opt = cmd->opts; opt && opt->name; opt++) {
        if (opt->type == VSH_OT_DATA) {
            if (data_ct == 0)
                return opt;
            else
                data_ct--;
        }
    }
    return NULL;
}

/*
 * Checks for required options
 */
static int
vshCommandCheckOpts(vshControl * ctl, vshCmd * cmd)
{
    vshCmdDef *def = cmd->def;
    vshCmdOptDef *d;
    int err = 0;

    for (d = def->opts; d && d->name; d++) {
        if (d->flag & VSH_OFLAG_REQ) {
            vshCmdOpt *o = cmd->opts;
            int ok = 0;

            while (o && ok == 0) {
                if (o->def == d)
                    ok = 1;
                o = o->next;
            }
            if (!ok) {
                vshError(ctl, FALSE,
                         d->type == VSH_OT_DATA ?
                         _("command '%s' requires <%s> option") :
                         _("command '%s' requires --%s option"),
                         def->name, d->name);
                err = 1;
            }

        }
    }
    return !err;
}

static vshCmdDef *
vshCmddefSearch(const char *cmdname)
{
    vshCmdDef *c;

    for (c = commands; c->name; c++)
        if (strcmp(c->name, cmdname) == 0)
            return c;
    return NULL;
}

static int
vshCmddefHelp(vshControl * ctl, const char *cmdname, int withprog)
{
    vshCmdDef *def = vshCmddefSearch(cmdname);

    if (!def) {
        vshError(ctl, FALSE, _("command '%s' doesn't exist"), cmdname);
        return FALSE;
    } else {
        vshCmdOptDef *opt;
        const char *desc = N_(vshCmddefGetInfo(def, "desc"));
        const char *help = N_(vshCmddefGetInfo(def, "help"));
        const char *syntax = vshCmddefGetInfo(def, "syntax");

        fputs(_("  NAME\n"), stdout);
        fprintf(stdout, "    %s - %s\n", def->name, help);

        if (syntax) {
            fputs(_("\n  SYNOPSIS\n"), stdout);
            if (!withprog)
                fprintf(stdout, "    %s\n", syntax);
            else
                fprintf(stdout, "    %s %s\n", progname, syntax);
        }
        if (desc) {
            fputs(_("\n  DESCRIPTION\n"), stdout);
            fprintf(stdout, "    %s\n", desc);
        }
        if (def->opts) {
            fputs(_("\n  OPTIONS\n"), stdout);
            for (opt = def->opts; opt->name; opt++) {
                char buf[256];

                if (opt->type == VSH_OT_BOOL)
                    snprintf(buf, sizeof(buf), "--%s", opt->name);
                else if (opt->type == VSH_OT_INT)
                    snprintf(buf, sizeof(buf), _("--%s <number>"), opt->name);
                else if (opt->type == VSH_OT_STRING)
                    snprintf(buf, sizeof(buf), _("--%s <string>"), opt->name);
                else if (opt->type == VSH_OT_DATA)
                    snprintf(buf, sizeof(buf), "<%s>", opt->name);

                fprintf(stdout, "    %-15s  %s\n", buf, N_(opt->help));
            }
        }
        fputc('\n', stdout);
    }
    return TRUE;
}

/* ---------------
 * Utils for work with runtime commands data
 * ---------------
 */
static void
vshCommandOptFree(vshCmdOpt * arg)
{
    vshCmdOpt *a = arg;

    while (a) {
        vshCmdOpt *tmp = a;

        a = a->next;

        free(tmp->data);
        free(tmp);
    }
}

static void
vshCommandFree(vshCmd * cmd)
{
    vshCmd *c = cmd;

    while (c) {
        vshCmd *tmp = c;

        c = c->next;

        if (tmp->opts)
            vshCommandOptFree(tmp->opts);
        free(tmp);
    }
}

/*
 * Returns option by name
 */
static vshCmdOpt *
vshCommandOpt(vshCmd * cmd, const char *name)
{
    vshCmdOpt *opt = cmd->opts;

    while (opt) {
        if (opt->def && strcmp(opt->def->name, name) == 0)
            return opt;
        opt = opt->next;
    }
    return NULL;
}

/*
 * Returns option as INT
 */
static int
vshCommandOptInt(vshCmd * cmd, const char *name, int *found)
{
    vshCmdOpt *arg = vshCommandOpt(cmd, name);
    int res = 0, num_found = FALSE;
    char *end_p = NULL;

    if ((arg != NULL) && (arg->data != NULL)) {
        res = strtol(arg->data, &end_p, 10);
        if ((arg->data == end_p) || (*end_p!= 0))
            num_found = FALSE;
        else
            num_found = TRUE;
    }
    if (found)
        *found = num_found;
    return res;
}

/*
 * Returns option as STRING
 */
static char *
vshCommandOptString(vshCmd * cmd, const char *name, int *found)
{
    vshCmdOpt *arg = vshCommandOpt(cmd, name);

    if (found)
        *found = arg ? TRUE : FALSE;

    return arg && arg->data && *arg->data ? arg->data : NULL;
}

#if 0
static int
vshCommandOptStringList(vshCmd * cmd, const char *name, char ***data)
{
    vshCmdOpt *arg = cmd->opts;
    char **val = NULL;
    int nval = 0;

    while (arg) {
        if (arg->def && STREQ(arg->def->name, name)) {
            char **tmp = realloc(val, sizeof(*tmp) * (nval+1));
            if (!tmp) {
                free(val);
                return -1;
            }
            val = tmp;
            val[nval++] = arg->data;
        }
        arg = arg->next;
    }

    *data = val;
    return nval;
}
#endif

/*
 * Returns TRUE/FALSE if the option exists
 */
static int
vshCommandOptBool(vshCmd * cmd, const char *name)
{
    return vshCommandOpt(cmd, name) ? TRUE : FALSE;
}


static virDomainPtr
vshCommandOptDomainBy(vshControl * ctl, vshCmd * cmd, const char *optname,
                      char **name, int flag)
{
    virDomainPtr dom = NULL;
    char *n;
    int id;

    if (!(n = vshCommandOptString(cmd, optname, NULL))) {
        vshError(ctl, FALSE, "%s", _("undefined domain name or id"));
        return NULL;
    }

    vshDebug(ctl, 5, "%s: found option <%s>: %s\n",
             cmd->def->name, optname, n);

    if (name)
        *name = n;

    /* try it by ID */
    if (flag & VSH_BYID) {
        if (virStrToLong_i(n, NULL, 10, &id) == 0 && id >= 0) {
            vshDebug(ctl, 5, "%s: <%s> seems like domain ID\n",
                     cmd->def->name, optname);
            dom = virDomainLookupByID(ctl->conn, id);
        }
    }
    /* try it by UUID */
    if (dom==NULL && (flag & VSH_BYUUID) && strlen(n)==VIR_UUID_STRING_BUFLEN-1) {
        vshDebug(ctl, 5, "%s: <%s> tring as domain UUID\n",
                 cmd->def->name, optname);
        dom = virDomainLookupByUUIDString(ctl->conn, n);
    }
    /* try it by NAME */
    if (dom==NULL && (flag & VSH_BYNAME)) {
        vshDebug(ctl, 5, "%s: <%s> tring as domain NAME\n",
                 cmd->def->name, optname);
        dom = virDomainLookupByName(ctl->conn, n);
    }

    if (!dom)
        vshError(ctl, FALSE, _("failed to get domain '%s'"), n);

    return dom;
}

static virNetworkPtr
vshCommandOptNetworkBy(vshControl * ctl, vshCmd * cmd, const char *optname,
		       char **name, int flag)
{
    virNetworkPtr network = NULL;
    char *n;

    if (!(n = vshCommandOptString(cmd, optname, NULL))) {
        vshError(ctl, FALSE, "%s", _("undefined network name"));
        return NULL;
    }

    vshDebug(ctl, 5, "%s: found option <%s>: %s\n",
             cmd->def->name, optname, n);

    if (name)
        *name = n;

    /* try it by UUID */
    if (network==NULL && (flag & VSH_BYUUID) && strlen(n)==VIR_UUID_STRING_BUFLEN-1) {
        vshDebug(ctl, 5, "%s: <%s> tring as network UUID\n",
		 cmd->def->name, optname);
        network = virNetworkLookupByUUIDString(ctl->conn, n);
    }
    /* try it by NAME */
    if (network==NULL && (flag & VSH_BYNAME)) {
        vshDebug(ctl, 5, "%s: <%s> tring as network NAME\n",
                 cmd->def->name, optname);
        network = virNetworkLookupByName(ctl->conn, n);
    }

    if (!network)
        vshError(ctl, FALSE, _("failed to get network '%s'"), n);

    return network;
}

static virStoragePoolPtr
vshCommandOptPoolBy(vshControl * ctl, vshCmd * cmd, const char *optname,
                    char **name, int flag)
{
    virStoragePoolPtr pool = NULL;
    char *n;

    if (!(n = vshCommandOptString(cmd, optname, NULL))) {
        vshError(ctl, FALSE, "%s", _("undefined pool name"));
        return NULL;
    }

    vshDebug(ctl, 5, "%s: found option <%s>: %s\n",
             cmd->def->name, optname, n);

    if (name)
        *name = n;

    /* try it by UUID */
    if (pool==NULL && (flag & VSH_BYUUID) && strlen(n)==VIR_UUID_STRING_BUFLEN-1) {
        vshDebug(ctl, 5, "%s: <%s> trying as pool UUID\n",
		 cmd->def->name, optname);
        pool = virStoragePoolLookupByUUIDString(ctl->conn, n);
    }
    /* try it by NAME */
    if (pool==NULL && (flag & VSH_BYNAME)) {
        vshDebug(ctl, 5, "%s: <%s> trying as pool NAME\n",
                 cmd->def->name, optname);
        pool = virStoragePoolLookupByName(ctl->conn, n);
    }

    if (!pool)
        vshError(ctl, FALSE, _("failed to get pool '%s'"), n);

    return pool;
}

static virStorageVolPtr
vshCommandOptVolBy(vshControl * ctl, vshCmd * cmd,
                   const char *optname,
                   const char *pooloptname,
                   char **name, int flag)
{
    virStorageVolPtr vol = NULL;
    virStoragePoolPtr pool = NULL;
    char *n, *p;
    int found;

    if (!(n = vshCommandOptString(cmd, optname, NULL))) {
        vshError(ctl, FALSE, "%s", _("undefined vol name"));
        return NULL;
    }

    if (!(p = vshCommandOptString(cmd, pooloptname, &found)) && found) {
        vshError(ctl, FALSE, "%s", _("undefined pool name"));
        return NULL;
    }

    if (p)
        pool = vshCommandOptPoolBy(ctl, cmd, pooloptname, name, flag);

    vshDebug(ctl, 5, "%s: found option <%s>: %s\n",
             cmd->def->name, optname, n);

    if (name)
        *name = n;

    /* try it by PATH */
    if (pool && (flag & VSH_BYNAME)) {
        vshDebug(ctl, 5, "%s: <%s> trying as vol UUID\n",
                 cmd->def->name, optname);
        vol = virStorageVolLookupByName(pool, n);
    }
    if (vol == NULL && (flag & VSH_BYUUID)) {
        vshDebug(ctl, 5, "%s: <%s> trying as vol key\n",
                 cmd->def->name, optname);
        vol = virStorageVolLookupByKey(ctl->conn, n);
    }
    if (vol == NULL && (flag & VSH_BYUUID)) {
        vshDebug(ctl, 5, "%s: <%s> trying as vol path\n",
                 cmd->def->name, optname);
        vol = virStorageVolLookupByPath(ctl->conn, n);
    }

    if (!vol)
        vshError(ctl, FALSE, _("failed to get vol '%s'"), n);

    if (pool)
        virStoragePoolFree(pool);

    return vol;
}

/*
 * Executes command(s) and returns return code from last command
 */
static int
vshCommandRun(vshControl * ctl, vshCmd * cmd)
{
    int ret = TRUE;

    while (cmd) {
        struct timeval before, after;

        if (ctl->timing)
            GETTIMEOFDAY(&before);

        ret = cmd->def->handler(ctl, cmd);

        if (ctl->timing)
            GETTIMEOFDAY(&after);

        if (strcmp(cmd->def->name, "quit") == 0)        /* hack ... */
            return ret;

        if (ctl->timing)
            vshPrint(ctl, _("\n(Time: %.3f ms)\n\n"),
                     DIFF_MSEC(&after, &before));
        else
            vshPrintExtra(ctl, "\n");
        cmd = cmd->next;
    }
    return ret;
}

/* ---------------
 * Command string parsing
 * ---------------
 */
#define VSH_TK_ERROR    -1
#define VSH_TK_NONE    0
#define VSH_TK_OPTION    1
#define VSH_TK_DATA    2
#define VSH_TK_END    3

static int
vshCommandGetToken(vshControl * ctl, char *str, char **end, char **res)
{
    int tk = VSH_TK_NONE;
    int quote = FALSE;
    int sz = 0;
    char *p = str;
    char *tkstr = NULL;

    *end = NULL;

    while (p && *p && (*p == ' ' || *p == '\t'))
        p++;

    if (p == NULL || *p == '\0')
        return VSH_TK_END;
    if (*p == ';') {
        *end = ++p;             /* = \0 or begi of next command */
        return VSH_TK_END;
    }
    while (*p) {
        /* end of token is blank space or ';' */
        if ((quote == FALSE && (*p == ' ' || *p == '\t')) || *p == ';')
            break;

        /* end of option name could be '=' */
        if (tk == VSH_TK_OPTION && *p == '=') {
            p++;                /* skip '=' */
            break;
        }

        if (tk == VSH_TK_NONE) {
            if (*p == '-' && *(p + 1) == '-' && *(p + 2)
                && isalnum((unsigned char) *(p + 2))) {
                tk = VSH_TK_OPTION;
                p += 2;
            } else {
                tk = VSH_TK_DATA;
                if (*p == '"') {
                    quote = TRUE;
                    p++;
                } else {
                    quote = FALSE;
                }
            }
            tkstr = p;          /* begin of token */
        } else if (quote && *p == '"') {
            quote = FALSE;
            p++;
            break;              /* end of "..." token */
        }
        p++;
        sz++;
    }
    if (quote) {
        vshError(ctl, FALSE, "%s", _("missing \""));
        return VSH_TK_ERROR;
    }
    if (tkstr == NULL || *tkstr == '\0' || p == NULL)
        return VSH_TK_END;
    if (sz == 0)
        return VSH_TK_END;

    *res = vshMalloc(ctl, sz + 1);
    memcpy(*res, tkstr, sz);
    *(*res + sz) = '\0';

    *end = p;
    return tk;
}

static int
vshCommandParse(vshControl * ctl, char *cmdstr)
{
    char *str;
    char *tkdata = NULL;
    vshCmd *clast = NULL;
    vshCmdOpt *first = NULL;

    if (ctl->cmd) {
        vshCommandFree(ctl->cmd);
        ctl->cmd = NULL;
    }

    if (cmdstr == NULL || *cmdstr == '\0')
        return FALSE;

    str = cmdstr;
    while (str && *str) {
        vshCmdOpt *last = NULL;
        vshCmdDef *cmd = NULL;
        int tk = VSH_TK_NONE;
        int data_ct = 0;

        first = NULL;

        while (tk != VSH_TK_END) {
            char *end = NULL;
            vshCmdOptDef *opt = NULL;

            tkdata = NULL;

            /* get token */
            tk = vshCommandGetToken(ctl, str, &end, &tkdata);

            str = end;

            if (tk == VSH_TK_END)
                break;
            if (tk == VSH_TK_ERROR)
                goto syntaxError;

            if (cmd == NULL) {
                /* first token must be command name */
                if (tk != VSH_TK_DATA) {
                    vshError(ctl, FALSE,
                             _("unexpected token (command name): '%s'"),
                             tkdata);
                    goto syntaxError;
                }
                if (!(cmd = vshCmddefSearch(tkdata))) {
                    vshError(ctl, FALSE, _("unknown command: '%s'"), tkdata);
                    goto syntaxError;   /* ... or ignore this command only? */
                }
                free(tkdata);
            } else if (tk == VSH_TK_OPTION) {
                if (!(opt = vshCmddefGetOption(cmd, tkdata))) {
                    vshError(ctl, FALSE,
                             _("command '%s' doesn't support option --%s"),
                             cmd->name, tkdata);
                    goto syntaxError;
                }
                free(tkdata);   /* option name */
                tkdata = NULL;

                if (opt->type != VSH_OT_BOOL) {
                    /* option data */
                    tk = vshCommandGetToken(ctl, str, &end, &tkdata);
                    str = end;
                    if (tk == VSH_TK_ERROR)
                        goto syntaxError;
                    if (tk != VSH_TK_DATA) {
                        vshError(ctl, FALSE,
                                 _("expected syntax: --%s <%s>"),
                                 opt->name,
                                 opt->type ==
                                 VSH_OT_INT ? _("number") : _("string"));
                        goto syntaxError;
                    }
                }
            } else if (tk == VSH_TK_DATA) {
                if (!(opt = vshCmddefGetData(cmd, data_ct++))) {
                    vshError(ctl, FALSE, _("unexpected data '%s'"), tkdata);
                    goto syntaxError;
                }
            }
            if (opt) {
                /* save option */
                vshCmdOpt *arg = vshMalloc(ctl, sizeof(vshCmdOpt));

                arg->def = opt;
                arg->data = tkdata;
                arg->next = NULL;
                tkdata = NULL;

                if (!first)
                    first = arg;
                if (last)
                    last->next = arg;
                last = arg;

                vshDebug(ctl, 4, "%s: %s(%s): %s\n",
                         cmd->name,
                         opt->name,
                         tk == VSH_TK_OPTION ? _("OPTION") : _("DATA"),
                         arg->data);
            }
            if (!str)
                break;
        }

        /* commad parsed -- allocate new struct for the command */
        if (cmd) {
            vshCmd *c = vshMalloc(ctl, sizeof(vshCmd));

            c->opts = first;
            c->def = cmd;
            c->next = NULL;

            if (!vshCommandCheckOpts(ctl, c)) {
                free(c);
                goto syntaxError;
            }

            if (!ctl->cmd)
                ctl->cmd = c;
            if (clast)
                clast->next = c;
            clast = c;
        }
    }

    return TRUE;

 syntaxError:
    if (ctl->cmd)
        vshCommandFree(ctl->cmd);
    if (first)
        vshCommandOptFree(first);
    free(tkdata);
    return FALSE;
}


/* ---------------
 * Misc utils
 * ---------------
 */
static const char *
vshDomainStateToString(int state)
{
    switch (state) {
    case VIR_DOMAIN_RUNNING:
        return gettext_noop("running");
    case VIR_DOMAIN_BLOCKED:
        return gettext_noop("blocked");
    case VIR_DOMAIN_PAUSED:
        return gettext_noop("paused");
    case VIR_DOMAIN_SHUTDOWN:
        return gettext_noop("in shutdown");
    case VIR_DOMAIN_SHUTOFF:
        return gettext_noop("shut off");
    case VIR_DOMAIN_CRASHED:
        return gettext_noop("crashed");
    default:
        ;/*FALLTHROUGH*/
    }
    return gettext_noop("no state");  /* = dom0 state */
}

static const char *
vshDomainVcpuStateToString(int state)
{
    switch (state) {
    case VIR_VCPU_OFFLINE:
        return gettext_noop("offline");
    case VIR_VCPU_BLOCKED:
        return gettext_noop("blocked");
    case VIR_VCPU_RUNNING:
        return gettext_noop("running");
    default:
        ;/*FALLTHROUGH*/
    }
    return gettext_noop("no state");
}

static int
vshConnectionUsability(vshControl * ctl, virConnectPtr conn, int showerror)
{
    /* TODO: use something like virConnectionState() to
     *       check usability of the connection
     */
    if (!conn) {
        if (showerror)
            vshError(ctl, FALSE, "%s", _("no valid connection"));
        return FALSE;
    }
    return TRUE;
}

static void
vshDebug(vshControl * ctl, int level, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vshOutputLogFile(ctl, VSH_ERR_DEBUG, format, ap);
    va_end(ap);

    if (level > ctl->debug)
        return;

    va_start(ap, format);
    vfprintf(stdout, format, ap);
    va_end(ap);
}

static void
vshPrintExtra(vshControl * ctl, const char *format, ...)
{
    va_list ap;

    if (ctl->quiet == TRUE)
        return;

    va_start(ap, format);
    vfprintf(stdout, format, ap);
    va_end(ap);
}


static void
vshError(vshControl * ctl, int doexit, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vshOutputLogFile(ctl, VSH_ERR_ERROR, format, ap);
    va_end(ap);

    if (doexit)
        fprintf(stderr, _("%s: error: "), progname);
    else
        fputs(_("error: "), stderr);

    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);

    fputc('\n', stderr);

    if (doexit) {
        if (ctl)
            vshDeinit(ctl);
        exit(EXIT_FAILURE);
    }
}

static void *
_vshMalloc(vshControl * ctl, size_t size, const char *filename, int line)
{
    void *x;

    if ((x = malloc(size)))
        return x;
    vshError(ctl, TRUE, _("%s: %d: failed to allocate %d bytes"),
             filename, line, (int) size);
    return NULL;
}

static void *
_vshCalloc(vshControl * ctl, size_t nmemb, size_t size, const char *filename, int line)
{
    void *x;

    if ((x = calloc(nmemb, size)))
        return x;
    vshError(ctl, TRUE, _("%s: %d: failed to allocate %d bytes"),
             filename, line, (int) (size*nmemb));
    return NULL;
}

static void *
_vshRealloc(vshControl * ctl, void *ptr, size_t size, const char *filename, int line)
{
    void *x;

    if ((x = realloc(ptr, size)))
        return x;
    free(ptr);
    vshError(ctl, TRUE, _("%s: %d: failed to allocate %d bytes"),
             filename, line, (int) size);
    return NULL;
}

static char *
_vshStrdup(vshControl * ctl, const char *s, const char *filename, int line)
{
    char *x;

    if (s == NULL)
        return(NULL);
    if ((x = strdup(s)))
        return x;
    vshError(ctl, TRUE, _("%s: %d: failed to allocate %lu bytes"),
             filename, line, (unsigned long)strlen(s));
    return NULL;
}

/*
 * Initialize connection.
 */
static int
vshInit(vshControl * ctl)
{
    if (ctl->conn)
        return FALSE;

#ifndef __MINGW32__
    ctl->uid = getuid();
#endif

    vshOpenLogFile(ctl);

    /* set up the library error handler */
    virSetErrorFunc(NULL, virshErrorHandler);

#ifndef __MINGW32__
    /* Force a non-root, Xen connection to readonly */
    if ((ctl->name == NULL ||
         !strcasecmp(ctl->name, "xen")) && ctl->uid != 0)
         ctl->readonly = 1;
#endif

    ctl->conn = virConnectOpenAuth(ctl->name,
                                   virConnectAuthPtrDefault,
                                   ctl->readonly ? VIR_CONNECT_RO : 0);


    /* This is not necessarily fatal.  All the individual commands check
     * vshConnectionUsability, except ones which don't need a connection
     * such as "help".
     */
    if (!ctl->conn) {
        vshError(ctl, FALSE, "%s", _("failed to connect to the hypervisor"));
        return FALSE;
    }

    return TRUE;
}

#ifndef O_SYNC
#define O_SYNC 0
#endif
#define LOGFILE_FLAGS (O_WRONLY | O_APPEND | O_CREAT | O_SYNC)

/**
 * vshOpenLogFile:
 *
 * Open log file.
 */
static void
vshOpenLogFile(vshControl *ctl)
{
    struct stat st;

    if (ctl->logfile == NULL)
        return;

    /* check log file */
    if (stat(ctl->logfile, &st) == -1) {
        switch (errno) {
            case ENOENT:
                break;
            default:
                vshError(ctl, TRUE, "%s",
                         _("failed to get the log file information"));
                break;
        }
    } else {
        if (!S_ISREG(st.st_mode)) {
            vshError(ctl, TRUE, "%s", _("the log path is not a file"));
        }
    }

    /* log file open */
    if ((ctl->log_fd = open(ctl->logfile, LOGFILE_FLAGS, FILE_MODE)) < 0) {
        vshError(ctl, TRUE, "%s",
                 _("failed to open the log file. check the log file path"));
    }
}

/**
 * vshOutputLogFile:
 *
 * Outputting an error to log file.
 */
static void
vshOutputLogFile(vshControl *ctl, int log_level, const char *msg_format, va_list ap)
{
    char msg_buf[MSG_BUFFER];
    const char *lvl = "";
    struct timeval stTimeval;
    struct tm *stTm;

    if (ctl->log_fd == -1)
        return;

    /**
     * create log format
     *
     * [YYYY.MM.DD HH:MM:SS SIGNATURE PID] LOG_LEVEL message
    */
    gettimeofday(&stTimeval, NULL);
    stTm = localtime(&stTimeval.tv_sec);
    snprintf(msg_buf, sizeof(msg_buf),
             "[%d.%02d.%02d %02d:%02d:%02d ",
             (1900 + stTm->tm_year),
             (1 + stTm->tm_mon),
             (stTm->tm_mday),
             (stTm->tm_hour),
             (stTm->tm_min),
             (stTm->tm_sec));
    snprintf(msg_buf + strlen(msg_buf), sizeof(msg_buf) - strlen(msg_buf),
             "%s] ", SIGN_NAME);
    switch (log_level) {
        case VSH_ERR_DEBUG:
            lvl = LVL_DEBUG;
            break;
        case VSH_ERR_INFO:
            lvl = LVL_INFO;
            break;
        case VSH_ERR_NOTICE:
            lvl = LVL_INFO;
            break;
        case VSH_ERR_WARNING:
            lvl = LVL_WARNING;
            break;
        case VSH_ERR_ERROR:
            lvl = LVL_ERROR;
            break;
        default:
            lvl = LVL_DEBUG;
            break;
    }
    snprintf(msg_buf + strlen(msg_buf), sizeof(msg_buf) - strlen(msg_buf),
             "%s ", lvl);
    vsnprintf(msg_buf + strlen(msg_buf), sizeof(msg_buf) - strlen(msg_buf),
              msg_format, ap);

    if (msg_buf[strlen(msg_buf) - 1] != '\n')
        snprintf(msg_buf + strlen(msg_buf), sizeof(msg_buf) - strlen(msg_buf), "\n");

    /* write log */
    if (safewrite(ctl->log_fd, msg_buf, strlen(msg_buf)) < 0) {
        vshCloseLogFile(ctl);
        vshError(ctl, FALSE, "%s", _("failed to write the log file"));
    }
}

/**
 * vshCloseLogFile:
 *
 * Close log file.
 */
static void
vshCloseLogFile(vshControl *ctl)
{
    /* log file close */
    if (ctl->log_fd >= 0) {
        if (close(ctl->log_fd) < 0)
            vshError(ctl, FALSE, _("%s: failed to write log file: %s"),
                     ctl->logfile ? ctl->logfile : "?", strerror (errno));
        ctl->log_fd = -1;
    }

    if (ctl->logfile) {
        free(ctl->logfile);
        ctl->logfile = NULL;
    }
}

#ifdef USE_READLINE

/* -----------------
 * Readline stuff
 * -----------------
 */

/*
 * Generator function for command completion.  STATE lets us
 * know whether to start from scratch; without any state
 * (i.e. STATE == 0), then we start at the top of the list.
 */
static char *
vshReadlineCommandGenerator(const char *text, int state)
{
    static int list_index, len;
    const char *name;

    /* If this is a new word to complete, initialize now.  This
     * includes saving the length of TEXT for efficiency, and
     * initializing the index variable to 0.
     */
    if (!state) {
        list_index = 0;
        len = strlen(text);
    }

    /* Return the next name which partially matches from the
     * command list.
     */
    while ((name = commands[list_index].name)) {
        list_index++;
        if (strncmp(name, text, len) == 0)
            return vshStrdup(NULL, name);
    }

    /* If no names matched, then return NULL. */
    return NULL;
}

static char *
vshReadlineOptionsGenerator(const char *text, int state)
{
    static int list_index, len;
    static vshCmdDef *cmd = NULL;
    const char *name;

    if (!state) {
        /* determine command name */
        char *p;
        char *cmdname;

        if (!(p = strchr(rl_line_buffer, ' ')))
            return NULL;

        cmdname = vshCalloc(NULL, (p - rl_line_buffer) + 1, 1);
        memcpy(cmdname, rl_line_buffer, p - rl_line_buffer);

        cmd = vshCmddefSearch(cmdname);
        list_index = 0;
        len = strlen(text);
        free(cmdname);
    }

    if (!cmd)
        return NULL;

    if (!cmd->opts)
        return NULL;

    while ((name = cmd->opts[list_index].name)) {
        vshCmdOptDef *opt = &cmd->opts[list_index];
        char *res;

        list_index++;

        if (opt->type == VSH_OT_DATA)
            /* ignore non --option */
            continue;

        if (len > 2) {
            if (strncmp(name, text + 2, len - 2))
                continue;
        }
        res = vshMalloc(NULL, strlen(name) + 3);
        snprintf(res, strlen(name) + 3,  "--%s", name);
        return res;
    }

    /* If no names matched, then return NULL. */
    return NULL;
}

static char **
vshReadlineCompletion(const char *text, int start,
                      int end ATTRIBUTE_UNUSED)
{
    char **matches = (char **) NULL;

    if (start == 0)
        /* command name generator */
        matches = rl_completion_matches(text, vshReadlineCommandGenerator);
    else
        /* commands options */
        matches = rl_completion_matches(text, vshReadlineOptionsGenerator);
    return matches;
}


static void
vshReadlineInit(void)
{
    /* Allow conditional parsing of the ~/.inputrc file. */
    rl_readline_name = "virsh";

    /* Tell the completer that we want a crack first. */
    rl_attempted_completion_function = vshReadlineCompletion;
}

static char *
vshReadline (vshControl *ctl ATTRIBUTE_UNUSED, const char *prompt)
{
    return readline (prompt);
}

#else /* !USE_READLINE */

static void
vshReadlineInit (void)
{
    /* empty */
}

static char *
vshReadline (vshControl *ctl, const char *prompt)
{
    char line[1024];
    char *r;
    int len;

    fputs (prompt, stdout);
    r = fgets (line, sizeof line, stdin);
    if (r == NULL) return NULL; /* EOF */

    /* Chomp trailing \n */
    len = strlen (r);
    if (len > 0 && r[len-1] == '\n')
        r[len-1] = '\0';

    return vshStrdup (ctl, r);
}

#endif /* !USE_READLINE */

/*
 * Deinitialize virsh
 */
static int
vshDeinit(vshControl * ctl)
{
    vshCloseLogFile(ctl);
    free(ctl->name);
    if (ctl->conn) {
        if (virConnectClose(ctl->conn) != 0) {
            ctl->conn = NULL;   /* prevent recursive call from vshError() */
            vshError(ctl, TRUE, "%s",
                     _("failed to disconnect from the hypervisor"));
        }
    }
    virResetLastError();

    return TRUE;
}

/*
 * Print usage
 */
static void
vshUsage(vshControl * ctl, const char *cmdname)
{
    vshCmdDef *cmd;

    /* global help */
    if (!cmdname) {
        fprintf(stdout, _("\n%s [options] [commands]\n\n"
                          "  options:\n"
                          "    -c | --connect <uri>    hypervisor connection URI\n"
                          "    -r | --readonly         connect readonly\n"
                          "    -d | --debug <num>      debug level [0-5]\n"
                          "    -h | --help             this help\n"
                          "    -q | --quiet            quiet mode\n"
                          "    -t | --timing           print timing information\n"
                          "    -l | --log <file>       output logging to file\n"
                          "    -v | --version          program version\n\n"
                          "  commands (non interactive mode):\n"), progname);

        for (cmd = commands; cmd->name; cmd++)
            fprintf(stdout,
                    "    %-15s %s\n", cmd->name, N_(vshCmddefGetInfo(cmd,
                                                                     "help")));

        fprintf(stdout, "%s",
                _("\n  (specify help <command> for details about the command)\n\n"));
        return;
    }
    if (!vshCmddefHelp(ctl, cmdname, TRUE))
        exit(EXIT_FAILURE);
}

/*
 * argv[]:  virsh [options] [command]
 *
 */
static int
vshParseArgv(vshControl * ctl, int argc, char **argv)
{
    char *last = NULL;
    int i, end = 0, help = 0;
    int arg, idx = 0;
    struct option opt[] = {
        {"debug", 1, 0, 'd'},
        {"help", 0, 0, 'h'},
        {"quiet", 0, 0, 'q'},
        {"timing", 0, 0, 't'},
        {"version", 0, 0, 'v'},
        {"connect", 1, 0, 'c'},
        {"readonly", 0, 0, 'r'},
        {"log", 1, 0, 'l'},
        {0, 0, 0, 0}
    };


    if (argc < 2)
        return TRUE;

    /* look for begin of the command, for example:
     *   ./virsh --debug 5 -q command --cmdoption
     *                  <--- ^ --->
     *        getopt() stuff | command suff
     */
    for (i = 1; i < argc; i++) {
        if (*argv[i] != '-') {
            int valid = FALSE;

            /* non "--option" argv, is it command? */
            if (last) {
                struct option *o;
                int sz = strlen(last);

                for (o = opt; o->name; o++) {
                    if (o->has_arg == 1){
                        if (sz == 2 && *(last + 1) == o->val)
                            /* valid virsh short option */
                            valid = TRUE;
                        else if (sz > 2 && strcmp(o->name, last + 2) == 0)
                            /* valid virsh long option */
                            valid = TRUE;
                    }
                }
            }
            if (!valid) {
                end = i;
                break;
            }
        }
        last = argv[i];
    }
    end = end ? end : argc;

    /* standard (non-command) options */
    while ((arg = getopt_long(end, argv, "d:hqtc:vrl:", opt, &idx)) != -1) {
        switch (arg) {
        case 'd':
            ctl->debug = atoi(optarg);
            break;
        case 'h':
            help = 1;
            break;
        case 'q':
            ctl->quiet = TRUE;
            break;
        case 't':
            ctl->timing = TRUE;
            break;
        case 'c':
            ctl->name = vshStrdup(ctl, optarg);
            break;
        case 'v':
            fprintf(stdout, "%s\n", VERSION);
            exit(EXIT_SUCCESS);
        case 'r':
            ctl->readonly = TRUE;
            break;
        case 'l':
            ctl->logfile = vshStrdup(ctl, optarg);
            break;
        default:
            vshError(ctl, TRUE,
                     _("unsupported option '-%c'. See --help."), arg);
            break;
        }
    }

    if (help) {
        /* global or command specific help */
        vshUsage(ctl, argc > end ? argv[end] : NULL);
        exit(EXIT_SUCCESS);
    }

    if (argc > end) {
        /* parse command */
        char *cmdstr;
        int sz = 0, ret;

        ctl->imode = FALSE;

        for (i = end; i < argc; i++)
            sz += strlen(argv[i]) + 1;  /* +1 is for blank space between items */

        cmdstr = vshCalloc(ctl, sz + 1, 1);

        for (i = end; i < argc; i++) {
            strncat(cmdstr, argv[i], sz);
            sz -= strlen(argv[i]);
            strncat(cmdstr, " ", sz--);
        }
        vshDebug(ctl, 2, "command: \"%s\"\n", cmdstr);
        ret = vshCommandParse(ctl, cmdstr);

        free(cmdstr);
        return ret;
    }
    return TRUE;
}

int
main(int argc, char **argv)
{
    vshControl _ctl, *ctl = &_ctl;
    char *defaultConn;
    int ret = TRUE;

    if (!setlocale(LC_ALL, "")) {
        perror("setlocale");
        return -1;
    }
    if (!bindtextdomain(GETTEXT_PACKAGE, LOCALEBASEDIR)) {
        perror("bindtextdomain");
        return -1;
    }
    if (!textdomain(GETTEXT_PACKAGE)) {
        perror("textdomain");
        return -1;
    }

    if (!(progname = strrchr(argv[0], '/')))
        progname = argv[0];
    else
        progname++;

    memset(ctl, 0, sizeof(vshControl));
    ctl->imode = TRUE;          /* default is interactive mode */
    ctl->log_fd = -1;           /* Initialize log file descriptor */

    if ((defaultConn = getenv("VIRSH_DEFAULT_CONNECT_URI"))) {
        ctl->name = strdup(defaultConn);
    }

    if (!vshParseArgv(ctl, argc, argv)) {
        vshDeinit(ctl);
        exit(EXIT_FAILURE);
    }

    if (!vshInit(ctl)) {
        vshDeinit(ctl);
        exit(EXIT_FAILURE);
    }

    if (!ctl->imode) {
        ret = vshCommandRun(ctl, ctl->cmd);
    } else {
        /* interactive mode */
        if (!ctl->quiet) {
            vshPrint(ctl,
                     _("Welcome to %s, the virtualization interactive terminal.\n\n"),
                     progname);
            vshPrint(ctl, "%s",
                     _("Type:  'help' for help with commands\n"
                       "       'quit' to quit\n\n"));
        }
        vshReadlineInit();
        do {
            const char *prompt = ctl->readonly ? VSH_PROMPT_RO : VSH_PROMPT_RW;
            ctl->cmdstr =
                vshReadline(ctl, prompt);
            if (ctl->cmdstr == NULL)
                break;          /* EOF */
            if (*ctl->cmdstr) {
#if USE_READLINE
                add_history(ctl->cmdstr);
#endif
                if (vshCommandParse(ctl, ctl->cmdstr))
                    vshCommandRun(ctl, ctl->cmd);
            }
            free(ctl->cmdstr);
            ctl->cmdstr = NULL;
        } while (ctl->imode);

        if (ctl->cmdstr == NULL)
            fputc('\n', stdout);        /* line break after alone prompt */
    }

    vshDeinit(ctl);
    exit(ret ? EXIT_SUCCESS : EXIT_FAILURE);
}

/*
 * vim: set tabstop=4:
 * vim: set shiftwidth=4:
 * vim: set expandtab:
 */
/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
