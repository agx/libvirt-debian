#!/usr/bin/env python3
#
# This is the API builder, it parses the C sources and build the
# API formal description in XML.
#
# See Copyright for the status of this software.
#
# daniel@veillard.com
#

import os
import sys
import glob
import re

quiet = True
warnings = 0
debug = False
debugsym = None

#
# C parser analysis code
#
included_files = {
    "libvirt-common.h": "header with general libvirt API definitions",
    "libvirt-domain.h": "header with general libvirt API definitions",
    "libvirt-domain-checkpoint.h": "header with general libvirt API definitions",
    "libvirt-domain-snapshot.h": "header with general libvirt API definitions",
    "libvirt-event.h": "header with general libvirt API definitions",
    "libvirt-host.h": "header with general libvirt API definitions",
    "libvirt-interface.h": "header with general libvirt API definitions",
    "libvirt-network.h": "header with general libvirt API definitions",
    "libvirt-nodedev.h": "header with general libvirt API definitions",
    "libvirt-nwfilter.h": "header with general libvirt API definitions",
    "libvirt-secret.h": "header with general libvirt API definitions",
    "libvirt-storage.h": "header with general libvirt API definitions",
    "libvirt-stream.h": "header with general libvirt API definitions",
    "virterror.h": "header with error specific API definitions",
    "libvirt.c": "Main interfaces for the libvirt library",
    "libvirt-domain.c": "Domain interfaces for the libvirt library",
    "libvirt-domain-checkpoint.c": "Domain checkpoint interfaces for the libvirt library",
    "libvirt-domain-snapshot.c": "Domain snapshot interfaces for the libvirt library",
    "libvirt-host.c": "Host interfaces for the libvirt library",
    "libvirt-interface.c": "Interface interfaces for the libvirt library",
    "libvirt-network.c": "Network interfaces for the libvirt library",
    "libvirt-nodedev.c": "Node device interfaces for the libvirt library",
    "libvirt-nwfilter.c": "NWFilter interfaces for the libvirt library",
    "libvirt-secret.c": "Secret interfaces for the libvirt library",
    "libvirt-storage.c": "Storage interfaces for the libvirt library",
    "libvirt-stream.c": "Stream interfaces for the libvirt library",
    "virerror.c": "implements error handling and reporting code for libvirt",
    "virevent.c": "event loop for monitoring file handles",
    "virtypedparam-public.c": "virTypedParameters APIs",
}

qemu_included_files = {
    "libvirt-qemu.h": "header with QEMU specific API definitions",
    "libvirt-qemu.c": "Implementations for the QEMU specific APIs",
}

lxc_included_files = {
    "libvirt-lxc.h": "header with LXC specific API definitions",
    "libvirt-lxc.c": "Implementations for the LXC specific APIs",
}

admin_included_files = {
    "libvirt-admin.h": "header with admin specific API definitions",
    "admin/libvirt-admin.c": "Implementations for the admin specific APIs",
}

ignored_words = {
    "G_GNUC_UNUSED": (0, "macro keyword"),
    "G_GNUC_NULL_TERMINATED": (0, "macro keyword"),
    "VIR_DEPRECATED": (0, "macro keyword"),
    "VIR_EXPORT_VAR": (0, "macro keyword"),
    "WINAPI": (0, "Windows keyword"),
    "__declspec": (3, "Windows keyword"),
    "__stdcall": (0, "Windows keyword"),
}

ignored_functions = {
    "virConnectSupportsFeature": "private function for remote access",
    "virDomainMigrateFinish": "private function for migration",
    "virDomainMigrateFinish2": "private function for migration",
    "virDomainMigratePerform": "private function for migration",
    "virDomainMigratePrepare": "private function for migration",
    "virDomainMigratePrepare2": "private function for migration",
    "virDomainMigratePrepareTunnel": "private function for tunnelled migration",
    "virDomainMigrateBegin3": "private function for migration",
    "virDomainMigrateFinish3": "private function for migration",
    "virDomainMigratePerform3": "private function for migration",
    "virDomainMigratePrepare3": "private function for migration",
    "virDomainMigrateConfirm3": "private function for migration",
    "virDomainMigratePrepareTunnel3": "private function for tunnelled migration",
    "DllMain": "specific function for Win32",
    "virTypedParamsValidate": "internal function in virtypedparam.c",
    "virTypedParameterValidateSet": "internal function in virtypedparam.c",
    "virTypedParameterAssign": "internal function in virtypedparam.c",
    "virTypedParameterAssignFromStr": "internal function in virtypedparam.c",
    "virTypedParameterToString": "internal function in virtypedparam.c",
    "virTypedParamsCheck": "internal function in virtypedparam.c",
    "virTypedParamsCopy": "internal function in virtypedparam.c",
    "virDomainMigrateBegin3Params": "private function for migration",
    "virDomainMigrateFinish3Params": "private function for migration",
    "virDomainMigratePerform3Params": "private function for migration",
    "virDomainMigratePrepare3Params": "private function for migration",
    "virDomainMigrateConfirm3Params": "private function for migration",
    "virDomainMigratePrepareTunnel3Params": "private function for tunnelled migration",
    "virErrorCopyNew": "private",
}

ignored_macros = {
    "_virSchedParameter": "backward compatibility macro for virTypedParameter",
    "_virBlkioParameter": "backward compatibility macro for virTypedParameter",
    "_virMemoryParameter": "backward compatibility macro for virTypedParameter",
}

# macros that should be completely skipped
hidden_macros = {
    "VIR_DEPRECATED": "internal macro to mark deprecated apis",
    "VIR_EXPORT_VAR": "internal macro to mark exported vars",
}


def escape(raw):
    raw = raw.replace('&', '&amp;')
    raw = raw.replace('<', '&lt;')
    raw = raw.replace('>', '&gt;')
    raw = raw.replace("'", '&apos;')
    raw = raw.replace('"', '&quot;')
    return raw


def uniq(items):
    return sorted(set(items))


class identifier:
    def __init__(self, name, header=None, module=None, type=None, lineno=0,
                 info=None, extra=None, conditionals=None):
        self.name = name
        self.header = header
        self.module = module
        self.type = type
        self.info = info
        self.extra = extra
        self.lineno = lineno
        self.static = 0
        if conditionals is None or len(conditionals) == 0:
            self.conditionals = None
        else:
            self.conditionals = conditionals[:]
        if self.name == debugsym and not quiet:
            print("=> define %s : %s" % (debugsym, (module, type, info,
                                         extra, conditionals)))

    def __repr__(self):
        r = "%s %s:" % (self.type, self.name)
        if self.static:
            r = r + " static"
        if self.module is not None:
            r = r + " from %s" % self.module
        if self.info is not None:
            r = r + " " + repr(self.info)
        if self.extra is not None:
            r = r + " " + repr(self.extra)
        if self.conditionals is not None:
            r = r + " " + repr(self.conditionals)
        return r

    def set_header(self, header):
        self.header = header

    def set_module(self, module):
        self.module = module

    def set_type(self, type):
        self.type = type

    def set_info(self, info):
        self.info = info

    def set_extra(self, extra):
        self.extra = extra

    def set_lineno(self, lineno):
        self.lineno = lineno

    def set_static(self, static):
        self.static = static

    def set_conditionals(self, conditionals):
        if conditionals is None or len(conditionals) == 0:
            self.conditionals = None
        else:
            self.conditionals = conditionals[:]

    def get_name(self):
        return self.name

    def get_header(self):
        return self.module

    def get_module(self):
        return self.module

    def get_type(self):
        return self.type

    def get_info(self):
        return self.info

    def get_lineno(self):
        return self.lineno

    def get_extra(self):
        return self.extra

    def get_static(self):
        return self.static

    def get_conditionals(self):
        return self.conditionals

    def update(self, header, module, type=None, info=None, extra=None,
               conditionals=None):
        if self.name == debugsym and not quiet:
            print("=> update %s : %s" % (debugsym, (module, type, info,
                                         extra, conditionals)))
        if header is not None and self.header is None:
            self.set_header(module)
        if module is not None and (self.module is None or self.header == self.module):
            self.set_module(module)
        if type is not None and self.type is None:
            self.set_type(type)
        if info is not None:
            self.set_info(info)
        if extra is not None:
            self.set_extra(extra)
        if conditionals is not None:
            self.set_conditionals(conditionals)


class index:
    def __init__(self, name="noname"):
        self.name = name
        self.identifiers = {}
        self.functions = {}
        self.variables = {}
        self.includes = {}
        self.structs = {}
        self.unions = {}
        self.enums = {}
        self.typedefs = {}
        self.macros = {}
        self.references = {}
        self.info = {}

    def warning(self, msg):
        global warnings
        warnings = warnings + 1
        print(msg)

    def add_ref(self, name, header, module, static, type, lineno, info=None, extra=None, conditionals=None):
        if name[0:2] == '__':
            return None
        d = None
        try:
            d = self.identifiers[name]
            d.update(header, module, type, lineno, info, extra, conditionals)
        except Exception:
            d = identifier(name, header, module, type, lineno, info, extra,
                           conditionals)
            self.identifiers[name] = d

        if d is not None and static == 1:
            d.set_static(1)

        if d is not None and name is not None and type is not None:
            self.references[name] = d

        if name == debugsym and not quiet:
            print("New ref: %s" % (d))

        return d

    def add(self, name, header, module, static, type, lineno, info=None,
            extra=None, conditionals=None):
        if name[0:2] == '__':
            return None
        d = None
        try:
            d = self.identifiers[name]
            d.update(header, module, type, lineno, info, extra, conditionals)
        except Exception:
            d = identifier(name, header, module, type, lineno, info, extra,
                           conditionals)
            self.identifiers[name] = d

        if d is not None and static == 1:
            d.set_static(1)

        if d is not None and name is not None and type is not None:
            type_map = {
                "function": self.functions,
                "functype": self.functions,
                "variable": self.variables,
                "include": self.includes,
                "struct": self.structs,
                "union": self.unions,
                "enum": self.enums,
                "typedef": self.typedefs,
                "macro": self.macros
            }
            if type in type_map:
                type_map[type][name] = d
            else:
                self.warning("Unable to register type ", type)

        if name == debugsym and not quiet:
            print("New symbol: %s" % (d))

        return d

    def merge(self, idx):
        for id in idx.functions.keys():
            #
            # macro might be used to override functions or variables
            # definitions
            #
            if id in self.macros:
                del self.macros[id]
            if id in self.functions:
                self.warning("function %s from %s redeclared in %s" % (
                    id, self.functions[id].header, idx.functions[id].header))
            else:
                self.functions[id] = idx.functions[id]
                self.identifiers[id] = idx.functions[id]
        for id in idx.variables.keys():
            #
            # macro might be used to override functions or variables
            # definitions
            #
            if id in self.macros:
                del self.macros[id]
            if id in self.variables:
                self.warning("variable %s from %s redeclared in %s" % (
                    id, self.variables[id].header, idx.variables[id].header))
            else:
                self.variables[id] = idx.variables[id]
                self.identifiers[id] = idx.variables[id]
        for id in idx.structs.keys():
            if id in self.structs:
                self.warning("struct %s from %s redeclared in %s" % (
                    id, self.structs[id].header, idx.structs[id].header))
            else:
                self.structs[id] = idx.structs[id]
                self.identifiers[id] = idx.structs[id]
        for id in idx.unions.keys():
            if id in self.unions:
                print("union %s from %s redeclared in %s" % (
                    id, self.unions[id].header, idx.unions[id].header))
            else:
                self.unions[id] = idx.unions[id]
                self.identifiers[id] = idx.unions[id]
        for id in idx.typedefs.keys():
            if id in self.typedefs:
                self.warning("typedef %s from %s redeclared in %s" % (
                    id, self.typedefs[id].header, idx.typedefs[id].header))
            else:
                self.typedefs[id] = idx.typedefs[id]
                self.identifiers[id] = idx.typedefs[id]
        for id in idx.macros.keys():
            #
            # macro might be used to override functions or variables
            # definitions
            #
            if id in self.variables:
                continue
            if id in self.functions:
                continue
            if id in self.enums:
                continue
            if id in self.macros:
                self.warning("macro %s from %s redeclared in %s" % (
                    id, self.macros[id].header, idx.macros[id].header))
            else:
                self.macros[id] = idx.macros[id]
                self.identifiers[id] = idx.macros[id]
        for id in idx.enums.keys():
            if id in self.enums:
                self.warning("enum %s from %s redeclared in %s" % (
                    id, self.enums[id].header, idx.enums[id].header))
            else:
                self.enums[id] = idx.enums[id]
                self.identifiers[id] = idx.enums[id]

    def merge_public(self, idx):
        for id in idx.functions.keys():
            if id in self.functions:
                up = idx.functions[id]
                # check that function condition agrees with header
                if up.conditionals != self.functions[id].conditionals:
                    self.warning("Header condition differs from Function"
                                 " for %s:" % id)
                    self.warning("  H: %s" % self.functions[id].conditionals)
                    self.warning("  C: %s" % up.conditionals)
                self.functions[id].update(None, up.module, up.type, up.info,
                                          up.extra)
        #     else:
        #         print("Function %s from %s is not declared in headers" % (
        #               id, idx.functions[id].module))
        # TODO: do the same for variables.

    def analyze_dict(self, type, dict):
        count = 0
        public = 0
        for name in dict.keys():
            id = dict[name]
            count = count + 1
            if id.static == 0:
                public = public + 1
        if count != public:
            print("  %d %s , %d public" % (count, type, public))
        elif count != 0:
            print("  %d public %s" % (count, type))

    def analyze(self):
        if not quiet:
            self.analyze_dict("functions", self.functions)
            self.analyze_dict("variables", self.variables)
            self.analyze_dict("structs", self.structs)
            self.analyze_dict("unions", self.unions)
            self.analyze_dict("typedefs", self.typedefs)
            self.analyze_dict("macros", self.macros)


class CLexer:
    """A lexer for the C language, tokenize the input by reading and
       analyzing it line by line"""
    def __init__(self, input):
        self.input = input
        self.tokens = []
        self.line = ""
        self.lineno = 0

    def getline(self):
        line = ''
        while line == '':
            line = self.input.readline()
            if not line:
                return None
            self.lineno += 1
            line = line.strip()
            if line == '':
                continue
            while line[-1] == '\\':
                line = line[:-1]
                n = self.input.readline().strip()
                self.lineno += 1
                if not n:
                    break
                line += n
        return line

    def getlineno(self):
        return self.lineno

    def push(self, token):
        self.tokens.insert(0, token)

    def debug(self):
        print("Last token: ", self.last)
        print("Token queue: ", self.tokens)
        print("Line %d end: " % self.lineno, self.line)

    def token(self):
        while self.tokens == []:
            if self.line == "":
                line = self.getline()
            else:
                line = self.line
                self.line = ""
            if line is None:
                return None

            if line[0] == '#':
                self.tokens = [('preproc', word) for word in line.split()]

                # We might have whitespace between the '#' and preproc
                # macro name, so instead of having a single token element
                # of '#define' we might end up with '#' and 'define'. This
                # merges them back together
                if self.tokens[0][1] == "#":
                    self.tokens[0] = ('preproc', "#" + self.tokens[1][1])
                    del self.tokens[1]
                break
            nline = len(line)
            if line[0] == '"' or line[0] == "'":
                quote = line[0]
                i = 1
                while quote not in line[i:]:
                    i = len(line)
                    nextline = self.getline()
                    if nextline is None:
                        return None
                    line += nextline

                tok, self.line = line[1:].split(quote, 1)
                self.last = ('string', tok)
                return self.last

            if line.startswith("/*"):
                line = line[2:]
                found = 0
                tok = ""
                while found == 0:
                    i = 0
                    nline = len(line)
                    while i < nline:
                        if line[i] == '*' and i + 1 < nline and line[i + 1] == '/':
                            self.line = line[i + 2:]
                            line = line[:i - 1]
                            nline = i
                            found = 1
                            break
                        i = i + 1
                    if tok != "":
                        tok = tok + "\n"
                    tok = tok + line
                    if found == 0:
                        line = self.getline()
                        if line is None:
                            return None
                self.last = ('comment', tok)
                return self.last
            if line.startswith("//"):
                line = line[2:]
                self.last = ('comment', line)
                return self.last
            i = 0
            while i < nline:
                if line[i] == '/' and i + 1 < nline and line[i + 1] == '/':
                    self.line = line[i:]
                    line = line[:i]
                    break
                if line[i] == '/' and i + 1 < nline and line[i + 1] == '*':
                    self.line = line[i:]
                    line = line[:i]
                    break
                if line[i] == '"' or line[i] == "'":
                    self.line = line[i:]
                    line = line[:i]
                    break
                i = i + 1
            nline = len(line)
            i = 0
            while i < nline:
                if line[i] == ' ' or line[i] == '\t':
                    i = i + 1
                    continue
                if line[i].isalnum():
                    s = i
                    while i < nline:
                        if line[i] not in " \t(){}:;,+-*/%&!|[]=><":
                            i = i + 1
                        else:
                            break
                    self.tokens.append(('name', line[s:i]))
                    continue
                if line[i] in "(){}:;,[]":
                    self.tokens.append(('sep', line[i]))
                    i = i + 1
                    continue
                if line[i] in "+-*><=/%&!|.":
                    if line[i] == '.' and i + 2 < nline and \
                       line[i + 1] == '.' and line[i + 2] == '.':
                        self.tokens.append(('name', '...'))
                        i = i + 3
                        continue

                    j = i + 1
                    if j < nline and line[j] in "+-*><=/%&!|":
                        self.tokens.append(('op', line[i:j + 1]))
                        i = j + 1
                    else:
                        self.tokens.append(('op', line[i]))
                        i = i + 1
                    continue
                s = i
                while i < nline:
                    if line[i] not in " \t(){}:;,+-*/%&!|[]=><":
                        i = i + 1
                    else:
                        break
                self.tokens.append(('name', line[s:i]))

        tok = self.tokens[0]
        self.tokens = self.tokens[1:]
        self.last = tok
        return tok


class CParser:
    """The C module parser"""
    def __init__(self, filename, idx=None):
        self.filename = filename
        if len(filename) > 2 and filename[-2:] == '.h':
            self.is_header = 1
        else:
            self.is_header = 0
        self.input = open(filename)
        self.lexer = CLexer(self.input)
        if idx is None:
            self.index = index()
        else:
            self.index = idx
        self.top_comment = ""
        self.last_comment = ""
        self.comment = None
        self.collect_ref = 0
        self.no_error = 0
        self.conditionals = []
        self.defines = []

    def collect_references(self):
        self.collect_ref = 1

    def stop_error(self):
        self.no_error = 1

    def start_error(self):
        self.no_error = 0

    def lineno(self):
        return self.lexer.getlineno()

    def index_add(self, name, module, static, type, info=None, extra=None):
        if self.is_header == 1:
            self.index.add(name, module, module, static, type, self.lineno(),
                           info, extra, self.conditionals)
        else:
            self.index.add(name, None, module, static, type, self.lineno(),
                           info, extra, self.conditionals)

    def index_add_ref(self, name, module, static, type, info=None,
                      extra=None):
        if self.is_header == 1:
            self.index.add_ref(name, module, module, static, type,
                               self.lineno(), info, extra, self.conditionals)
        else:
            self.index.add_ref(name, None, module, static, type, self.lineno(),
                               info, extra, self.conditionals)

    def warning(self, msg):
        global warnings
        warnings = warnings + 1
        if self.no_error:
            return
        print(msg)

    def error(self, msg, token=-1):
        if self.no_error:
            return

        print("Parse Error: " + msg)
        if token != -1:
            print("Got token ", token)
        self.lexer.debug()
        sys.exit(1)

    def debug(self, msg, token=-1):
        print("Debug: " + msg)
        if token != -1:
            print("Got token ", token)
        self.lexer.debug()

    def parseTopComment(self, comment):
        res = {}
        lines = comment.split("\n")
        item = None
        for line in lines:
            line = line.lstrip().lstrip('*').lstrip()

            m = re.match(r'([_.a-zA-Z0-9]+):(.*)', line)
            if m:
                item = m.group(1)
                line = m.group(2).lstrip()

            if item:
                if item in res:
                    res[item] = res[item] + " " + line
                else:
                    res[item] = line
        self.index.info = res

    def strip_lead_star(self, line):
        if line.lstrip().startswith('*'):
            line = line.replace('*', '', 1)
        return line

    def cleanupComment(self):
        if not isinstance(self.comment, str):
            return
        # remove the leading * on multi-line comments
        lines = self.comment.splitlines(True)
        com = ""
        for line in lines:
            com = com + self.strip_lead_star(line)
        self.comment = com.strip()

    def parseComment(self, token):
        com = token[1]
        if self.top_comment == "":
            self.top_comment = com
        if self.comment is None or com[0] == '*':
            self.comment = com
        else:
            self.comment = self.comment + com
        token = self.lexer.token()

        if self.comment.find("DOC_DISABLE") != -1:
            self.stop_error()

        if self.comment.find("DOC_ENABLE") != -1:
            self.start_error()

        return token

    #
    # Parse a comment block associate to a typedef
    #
    def parseTypeComment(self, name, quiet=False):
        if name[0:2] == '__':
            quiet = True

        if self.comment is None:
            if not quiet:
                self.warning("Missing comment for type %s" % name)
            return None
        if not self.comment.startswith('*'):
            if not quiet:
                self.warning("Missing * in type comment for %s" % name)
            return None

        lines = self.comment.split('\n')
        # Remove lines that contain only single asterisk
        lines[:] = [line for line in lines if line.strip() != '*']

        if lines[0] != "* %s:" % name:
            if not quiet:
                self.warning("Misformatted type comment for %s" % name)
                self.warning("  Expecting '* %s:' got '%s'" % (name, lines[0]))
            return None
        del lines[0]

        # Concatenate all remaining lines by striping leading asterisks
        desc = " ".join([line.lstrip("*").strip() for line in lines]).strip()

        if not (quiet or desc):
            self.warning("Type comment for %s lack description of the macro"
                         % name)

        return desc

    #
    # Parse a comment block associate to a macro
    #
    def parseMacroComment(self, name, quiet=0):
        global ignored_macros

        if name[0:2] == '__':
            quiet = 1
        if name in ignored_macros:
            quiet = 1

        args = []
        desc = ""

        if self.comment is None:
            if not quiet:
                self.warning("Missing comment for macro %s" % name)
            return args, desc
        if self.comment[0] != '*':
            if not quiet:
                self.warning("Missing * in macro comment for %s" % name)
            return args, desc
        lines = self.comment.split('\n')
        if lines[0] == '*':
            del lines[0]
        if lines[0] != "* %s:" % name:
            if not quiet:
                self.warning("Misformatted macro comment for %s" % name)
                self.warning("  Expecting '* %s:' got '%s'" % (name, lines[0]))
            return args, desc
        del lines[0]
        while lines[0] == '*':
            del lines[0]
        while len(lines) > 0 and lines[0][0:3] == '* @':
            prefix = lines[0][3:]
            try:
                arg, desc = prefix.split(':', 1)
                desc = desc.strip()
                arg = arg.strip()
            except Exception:
                if not quiet:
                    self.warning("Misformatted macro comment for %s" % name)
                    self.warning("  problem with '%s'" % lines[0])
                del lines[0]
                continue
            del lines[0]
            line = lines[0].strip()
            while len(line) > 2 and line[0:3] != '* @':
                while line[0] == '*':
                    line = line[1:]
                desc = desc + ' ' + line.strip()
                del lines[0]
                if len(lines) == 0:
                    break
                line = lines[0]
            args.append((arg, desc))
        while len(lines) > 0 and lines[0] == '*':
            del lines[0]
        desc = ""
        while len(lines) > 0:
            line = lines[0]
            while len(line) > 0 and line[0] == '*':
                line = line[1:]
            line = line.strip()
            desc = desc + " " + line
            del lines[0]

        desc = desc.strip()

        if quiet == 0:
            if desc == "":
                self.warning("Macro comment for %s lack description of the macro" % name)

        return args, desc

    #
    # Parse a comment block and merge the information found in the
    # parameters descriptions, finally returns a block as complete
    # as possible
    #
    def mergeFunctionComment(self, name, description, quiet=0):
        global ignored_functions

        if name == 'main':
            quiet = 1
        if name[0:2] == '__':
            quiet = 1
        if name in ignored_functions:
            quiet = 1

        ret, args = description
        desc = ""
        retdesc = ""

        if self.comment is None:
            if not quiet:
                self.warning("Missing comment for function %s" % name)
            return (ret[0], retdesc), args, desc
        if self.comment[0] != '*':
            if not quiet:
                self.warning("Missing * in function comment for %s" % name)
            return (ret[0], retdesc), args, desc
        lines = self.comment.split('\n')
        if lines[0] == '*':
            del lines[0]
        if lines[0] != "* %s:" % name:
            if not quiet:
                self.warning("Misformatted function comment for %s" % name)
                self.warning("  Expecting '* %s:' got '%s'" % (name, lines[0]))
            return (ret[0], retdesc), args, desc
        del lines[0]
        while lines[0] == '*':
            del lines[0]
        nbargs = len(args)
        while len(lines) > 0 and lines[0][0:3] == '* @':
            prefix = lines[0][3:]
            try:
                arg, desc = prefix.split(':', 1)
                desc = desc.strip()
                arg = arg.strip()
            except Exception:
                if not quiet:
                    self.warning("Misformatted function comment for %s" % name)
                    self.warning("  problem with '%s'" % lines[0])
                del lines[0]
                continue
            del lines[0]
            line = lines[0].strip()
            while len(line) > 2 and line[0:3] != '* @':
                while line[0] == '*':
                    line = line[1:]
                desc = desc + ' ' + line.strip()
                del lines[0]
                if len(lines) == 0:
                    break
                line = lines[0]
            i = 0
            while i < nbargs:
                if args[i][1] == arg:
                    args[i] = (args[i][0], arg, desc)
                    break
                i = i + 1
            if i >= nbargs:
                if not quiet:
                    self.warning("Unable to find arg %s from function comment for %s" %
                                 (arg, name))
        while len(lines) > 0 and lines[0] == '*':
            del lines[0]
        desc = None
        while len(lines) > 0:
            line = lines[0]
            i = 0
            # Remove all leading '*', followed by at most one ' ' character
            # since we need to preserve correct indentation of code examples
            while i < len(line) and line[i] == '*':
                i = i + 1
            if i > 0:
                if i < len(line) and line[i] == ' ':
                    i = i + 1
                line = line[i:]
            if len(line) >= 6 and line[0:7] == "Returns":
                try:
                    line = line.split(' ', 1)[1]
                except Exception:
                    line = ""
                retdesc = line.strip()
                del lines[0]
                while len(lines) > 0:
                    line = lines[0]
                    while len(line) > 0 and line[0] == '*':
                        line = line[1:]
                    line = line.strip()
                    retdesc = retdesc + " " + line
                    del lines[0]
            else:
                if desc is not None:
                    desc = desc + "\n" + line
                else:
                    desc = line
                del lines[0]

        if desc is None:
            desc = ""
        retdesc = retdesc.strip()
        desc = desc.strip()

        if quiet == 0:
            #
            # report missing comments
            #
            i = 0
            while i < nbargs:
                if args[i][2] is None and args[i][0] != "void" and args[i][1] is not None:
                    self.warning("Function comment for %s lacks description of arg %s" % (name, args[i][1]))
                i = i + 1
            if retdesc == "" and ret[0] != "void":
                self.warning("Function comment for %s lacks description of return value" % name)
            if desc == "":
                self.warning("Function comment for %s lacks description of the function" % name)

        return (ret[0], retdesc), args, desc

    def parsePreproc(self, token):
        if debug:
            print("=> preproc ", token, self.lexer.tokens)
        name = token[1]
        if name == "#include":
            token = self.lexer.token()
            if token is None:
                return None
            if token[0] == 'preproc':
                self.index_add(token[1], self.filename, not self.is_header,
                               "include")
                return self.lexer.token()
            return token
        if name == "#define":
            token = self.lexer.token()
            if token is None:
                return None
            if token[0] == 'preproc':
                # TODO macros with arguments
                name = token[1]
                lst = []
                token = self.lexer.token()
                while (token is not None and token[0] == 'preproc' and
                       token[1][0] != '#'):
                    lst.append(token[1])
                    token = self.lexer.token()
                try:
                    name = name.split('(')[0]
                except Exception:
                    pass

                # skip hidden macros
                if name in hidden_macros:
                    return token
                if name[-2:] == "_H" or name[-8:] == "_H_ALLOW":
                    return token

                strValue = None
                if len(lst) == 1 and lst[0][0] == '"' and lst[0][-1] == '"':
                    strValue = lst[0][1:-1]
                (args, desc) = self.parseMacroComment(name, not self.is_header)
                self.index_add(name, self.filename, not self.is_header,
                               "macro", (args, desc, strValue))
                return token

        #
        # Processing of conditionals modified by Bill 1/1/05
        #
        # We process conditionals (i.e. tokens from #ifdef, #ifndef,
        # #if, #else and #endif) for headers and mainline code,
        # store the ones from the header in libxml2-api.xml, and later
        # (in the routine merge_public) verify that the two (header and
        # mainline code) agree.
        #
        # There is a small problem with processing the headers. Some of
        # the variables are not concerned with enabling / disabling of
        # library functions (e.g. '__XML_PARSER_H__'), and we don't want
        # them to be included in libxml2-api.xml, or involved in
        # the check between the header and the mainline code.  To
        # accomplish this, we ignore any conditional which doesn't include
        # the string 'ENABLED'
        #
        if name == "#ifdef":
            apstr = self.lexer.tokens[0][1]
            try:
                self.defines.append(apstr)
                if apstr.find('ENABLED') != -1:
                    self.conditionals.append("defined(%s)" % apstr)
            except Exception:
                pass
        elif name == "#ifndef":
            apstr = self.lexer.tokens[0][1]
            try:
                self.defines.append(apstr)
                if apstr.find('ENABLED') != -1:
                    self.conditionals.append("!defined(%s)" % apstr)
            except Exception:
                pass
        elif name == "#if":
            apstr = ""
            for tok in self.lexer.tokens:
                if apstr != "":
                    apstr = apstr + " "
                apstr = apstr + tok[1]
            try:
                self.defines.append(apstr)
                if apstr.find('ENABLED') != -1:
                    self.conditionals.append(apstr)
            except Exception:
                pass
        elif name == "#else":
            if (self.conditionals != [] and
                    self.defines[-1].find('ENABLED') != -1):
                self.conditionals[-1] = "!(%s)" % self.conditionals[-1]
        elif name == "#endif":
            if (self.conditionals != [] and
                    self.defines[-1].find('ENABLED') != -1):
                self.conditionals = self.conditionals[:-1]
            self.defines = self.defines[:-1]
        token = self.lexer.token()
        while (token is not None and token[0] == 'preproc' and
               token[1][0] != '#'):
            token = self.lexer.token()
        return token

    #
    # token acquisition on top of the lexer, it handle internally
    # preprocessor and comments since they are logically not part of
    # the program structure.
    #
    def push(self, tok):
        self.lexer.push(tok)

    def token(self):
        global ignored_words

        token = self.lexer.token()
        while token is not None:
            if token[0] == 'comment':
                token = self.parseComment(token)
                continue
            elif token[0] == 'preproc':
                token = self.parsePreproc(token)
                continue
            elif token[0] == "name" and token[1] == "__const":
                token = ("name", "const")
                return token
            elif token[0] == "name" and token[1] == "__attribute":
                token = self.lexer.token()
                while token is not None and token[1] != ";":
                    token = self.lexer.token()
                return token
            elif token[0] == "name" and token[1] in ignored_words:
                (n, info) = ignored_words[token[1]]
                i = 0
                while i < n:
                    token = self.lexer.token()
                    i = i + 1
                token = self.lexer.token()
                continue
            else:
                if debug:
                    print("=> ", token)
                return token
        return None

    #
    # Parse a typedef, it records the type and its name.
    #
    def parseTypedef(self, token):
        if token is None:
            return None
        token = self.parseType(token)
        if token is None:
            self.error("parsing typedef")
            return None
        base_type = self.type
        type = base_type
        # self.debug("end typedef type", token)
        while token is not None:
            if token[0] == "name":
                name = token[1]
                signature = self.signature
                if signature is not None:
                    type = type.split('(')[0]
                    d = self.mergeFunctionComment(name,
                                                  ((type, None), signature), 1)
                    self.index_add(name, self.filename, not self.is_header,
                                   "functype", d)
                else:
                    if base_type == "struct":
                        self.index_add(name, self.filename, not self.is_header,
                                       "struct", type)
                        base_type = "struct " + name
                    else:
                        # TODO report missing or misformatted comments
                        info = self.parseTypeComment(name, 1)
                        self.index_add(name, self.filename, not self.is_header,
                                       "typedef", type, info)
                token = self.token()
            else:
                self.error("parsing typedef: expecting a name")
                return token
            # self.debug("end typedef", token)
            if token is not None and token[0] == 'sep' and token[1] == ',':
                type = base_type
                token = self.token()
                while token is not None and token[0] == "op":
                    type = type + token[1]
                    token = self.token()
            elif token is not None and token[0] == 'sep' and token[1] == ';':
                break
            elif token is not None and token[0] == 'name':
                type = base_type
                continue
            else:
                self.error("parsing typedef: expecting ';'", token)
                return token
        token = self.token()
        return token

    #
    # Parse a C code block, used for functions it parse till
    # the balancing } included
    #
    def parseBlock(self, token):
        while token is not None:
            if token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseBlock(token)
            elif token[0] == "sep" and token[1] == "}":
                self.comment = None
                token = self.token()
                return token
            else:
                if self.collect_ref == 1:
                    oldtok = token
                    token = self.token()
                    if oldtok[0] == "name" and oldtok[1][0:3] == "vir":
                        if token[0] == "sep" and token[1] == "(":
                            self.index_add_ref(oldtok[1], self.filename,
                                               0, "function")
                            token = self.token()
                        elif token[0] == "name":
                            token = self.token()
                            if token[0] == "sep" and (token[1] == ";" or
                               token[1] == "," or token[1] == "="):
                                self.index_add_ref(oldtok[1], self.filename,
                                                   0, "type")
                    elif oldtok[0] == "name" and oldtok[1][0:4] == "XEN_":
                        self.index_add_ref(oldtok[1], self.filename,
                                           0, "typedef")
                    elif oldtok[0] == "name" and oldtok[1][0:7] == "LIBXEN_":
                        self.index_add_ref(oldtok[1], self.filename,
                                           0, "typedef")

                else:
                    token = self.token()
        return token

    #
    # Parse a C struct definition till the balancing }
    #
    def parseStruct(self, token):
        fields = []
        # self.debug("start parseStruct", token)
        while token is not None:
            if token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseTypeBlock(token)
            elif token[0] == "sep" and token[1] == "}":
                self.struct_fields = fields
                # self.debug("end parseStruct", token)
                # print(fields)
                token = self.token()
                return token
            else:
                base_type = self.type
                # self.debug("before parseType", token)
                token = self.parseType(token)
                # self.debug("after parseType", token)
                if token is not None and token[0] == "name":
                    fname = token[1]
                    token = self.token()
                    if token[0] == "sep" and token[1] == ";":
                        self.comment = None
                        token = self.token()
                        self.cleanupComment()
                        if self.type == "union":
                            fields.append((self.type, fname, self.comment,
                                           self.union_fields))
                            self.union_fields = []
                        else:
                            fields.append((self.type, fname, self.comment))
                        self.comment = None
                    else:
                        self.error("parseStruct: expecting ;", token)
                elif token is not None and token[0] == "sep" and token[1] == "{":
                    token = self.token()
                    token = self.parseTypeBlock(token)
                    if token is not None and token[0] == "name":
                        token = self.token()
                    if token is not None and token[0] == "sep" and token[1] == ";":
                        token = self.token()
                    else:
                        self.error("parseStruct: expecting ;", token)
                else:
                    self.error("parseStruct: name", token)
                    token = self.token()
                self.type = base_type
        self.struct_fields = fields
        # self.debug("end parseStruct", token)
        # print(fields)
        return token

    #
    # Parse a C union definition till the balancing }
    #
    def parseUnion(self, token):
        fields = []
        # self.debug("start parseUnion", token)
        while token is not None:
            if token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseTypeBlock(token)
            elif token[0] == "sep" and token[1] == "}":
                self.union_fields = fields
                # self.debug("end parseUnion", token)
                # print(fields)
                token = self.token()
                return token
            else:
                base_type = self.type
                # self.debug("before parseType", token)
                token = self.parseType(token)
                # self.debug("after parseType", token)
                if token is not None and token[0] == "name":
                    fname = token[1]
                    token = self.token()
                    if token[0] == "sep" and token[1] == ";":
                        self.comment = None
                        token = self.token()
                        self.cleanupComment()
                        fields.append((self.type, fname, self.comment))
                        self.comment = None
                    else:
                        self.error("parseUnion: expecting ;", token)
                elif token is not None and token[0] == "sep" and token[1] == "{":
                    token = self.token()
                    token = self.parseTypeBlock(token)
                    if token is not None and token[0] == "name":
                        token = self.token()
                    if token is not None and token[0] == "sep" and token[1] == ";":
                        token = self.token()
                    else:
                        self.error("parseUnion: expecting ;", token)
                else:
                    self.error("parseUnion: name", token)
                    token = self.token()
                self.type = base_type
        self.union_fields = fields
        # self.debug("end parseUnion", token)
        # print(fields)
        return token

    #
    # Parse a C enum block, parse till the balancing }
    #
    def parseEnumBlock(self, token):
        self.enums = []
        name = None
        comment = ""
        value = "-1"
        commentsBeforeVal = self.comment is not None
        while token is not None:
            if token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseTypeBlock(token)
            elif token[0] == "sep" and token[1] == "}":
                if name is not None:
                    self.cleanupComment()
                    if self.comment is not None:
                        comment = self.comment
                        self.comment = None
                    self.enums.append((name, value, comment))
                token = self.token()
                return token
            elif token[0] == "name":
                self.cleanupComment()
                if name is not None:
                    if self.comment is not None:
                        comment = self.comment.strip()
                        self.comment = None
                    self.enums.append((name, value, comment))
                name = token[1]
                comment = ""
                token = self.token()
                if token[0] == "op" and token[1][0] == "=":
                    value = ""
                    if len(token[1]) > 1:
                        value = token[1][1:]
                    token = self.token()
                    while token[0] != "sep" or (token[1] != ',' and
                                                token[1] != '}'):
                        # We might be dealing with '1U << 12' here
                        value = value + re.sub(r"^(\d+)U$", "\\1", token[1])
                        token = self.token()
                else:
                    try:
                        value = "%d" % (int(value) + 1)
                    except Exception:
                        self.warning("Failed to compute value of enum %s" % name)
                        value = ""
                if token[0] == "sep" and token[1] == ",":
                    if commentsBeforeVal:
                        self.cleanupComment()
                        self.enums.append((name, value, self.comment))
                        name = comment = self.comment = None
                    token = self.token()
            else:
                token = self.token()
        return token

    def parseVirEnumDecl(self, token):
        if token[0] != "name":
            self.error("parsing VIR_ENUM_DECL: expecting name", token)

        token = self.token()

        if token[0] != "sep":
            self.error("parsing VIR_ENUM_DECL: expecting ')'", token)

        if token[1] != ')':
            self.error("parsing VIR_ENUM_DECL: expecting ')'", token)

        token = self.token()
        if token[0] == "sep" and token[1] == ';':
            token = self.token()

        return token

    def parseVirEnumImpl(self, token):
        # First the type name
        if token[0] != "name":
            self.error("parsing VIR_ENUM_IMPL: expecting name", token)

        token = self.token()

        if token[0] != "sep":
            self.error("parsing VIR_ENUM_IMPL: expecting ','", token)

        if token[1] != ',':
            self.error("parsing VIR_ENUM_IMPL: expecting ','", token)
        token = self.token()

        # Now the sentinel name
        if token[0] != "name":
            self.error("parsing VIR_ENUM_IMPL: expecting name", token)

        token = self.token()

        if token[0] != "sep":
            self.error("parsing VIR_ENUM_IMPL: expecting ','", token)

        if token[1] != ',':
            self.error("parsing VIR_ENUM_IMPL: expecting ','", token)

        token = self.token()

        # Now a list of strings (optional comments)
        while token is not None:
            isGettext = False
            # First a string, optionally with N_(...)
            if token[0] == 'name':
                if token[1] != 'N_':
                    self.error("parsing VIR_ENUM_IMPL: expecting 'N_'", token)
                token = self.token()
                if token[0] != "sep" or token[1] != '(':
                    self.error("parsing VIR_ENUM_IMPL: expecting '('", token)
                token = self.token()
                isGettext = True

                if token[0] != "string":
                    self.error("parsing VIR_ENUM_IMPL: expecting a string", token)
                token = self.token()
            elif token[0] == "string":
                token = self.token()
            else:
                self.error("parsing VIR_ENUM_IMPL: expecting a string", token)

            # Then a separator
            if token[0] == "sep":
                if isGettext and token[1] == ')':
                    token = self.token()

                if token[1] == ',':
                    token = self.token()

                if token[1] == ')':
                    token = self.token()
                    break

            # Then an optional comment
            if token[0] == "comment":
                token = self.token()

        if token[0] == "sep" and token[1] == ';':
            token = self.token()

        return token

    def parseVirLogInit(self, token):
        if token[0] != "string":
            self.error("parsing VIR_LOG_INIT: expecting string", token)

        token = self.token()

        if token[0] != "sep":
            self.error("parsing VIR_LOG_INIT: expecting ')'", token)

        if token[1] != ')':
            self.error("parsing VIR_LOG_INIT: expecting ')'", token)

        token = self.token()
        if token[0] == "sep" and token[1] == ';':
            token = self.token()

        return token

    #
    # Parse a C definition block, used for structs or unions it parse till
    # the balancing }
    #
    def parseTypeBlock(self, token):
        while token is not None:
            if token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseTypeBlock(token)
            elif token[0] == "sep" and token[1] == "}":
                token = self.token()
                return token
            else:
                token = self.token()
        return token

    #
    # Parse a type: the fact that the type name can either occur after
    #    the definition or within the definition makes it a little harder
    #    if inside, the name token is pushed back before returning
    #
    def parseType(self, token):
        self.type = ""
        self.struct_fields = []
        self.union_fields = []
        self.signature = None
        if token is None:
            return token

        while (token[0] == "name" and
               token[1] in ["const", "unsigned", "signed"]):
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]
            token = self.token()

        if token[0] == "name" and token[1] == "long":
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]

            # some read ahead for long long
            oldtmp = token
            token = self.token()
            if token[0] == "name" and token[1] == "long":
                self.type = self.type + " " + token[1]
            else:
                self.push(token)
                token = oldtmp

            oldtmp = token
            token = self.token()
            if token[0] == "name" and token[1] == "int":
                self.type = self.type + " " + token[1]
            else:
                self.push(token)
                token = oldtmp

        elif token[0] == "name" and token[1] == "short":
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]

        elif token[0] == "name" and token[1] == "struct":
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]
            token = self.token()
            nametok = None
            if token[0] == "name":
                nametok = token
                token = self.token()
            if token is not None and token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseStruct(token)
            elif token is not None and token[0] == "op" and token[1] == "*":
                self.type = self.type + " " + nametok[1] + " *"
                token = self.token()
                while token is not None and token[0] == "op" and token[1] == "*":
                    self.type = self.type + " *"
                    token = self.token()
                if token[0] == "name":
                    nametok = token
                    token = self.token()
                else:
                    self.error("struct : expecting name", token)
                    return token
            elif token is not None and token[0] == "name" and nametok is not None:
                self.type = self.type + " " + nametok[1]
                return token

            if nametok is not None:
                self.lexer.push(token)
                token = nametok
            return token

        elif token[0] == "name" and token[1] == "union":
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]
            token = self.token()
            nametok = None
            if token[0] == "name":
                nametok = token
                token = self.token()
            if token is not None and token[0] == "sep" and token[1] == "{":
                token = self.token()
                token = self.parseUnion(token)
            elif token is not None and token[0] == "name" and nametok is not None:
                self.type = self.type + " " + nametok[1]
                return token

            if nametok is not None:
                self.lexer.push(token)
                token = nametok
            return token

        elif token[0] == "name" and token[1] == "enum":
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]
            self.enums = []
            token = self.token()
            if token is not None and token[0] == "sep" and token[1] == "{":
                # drop comments before the enum block
                self.comment = None
                token = self.token()
                token = self.parseEnumBlock(token)
            else:
                self.error("parsing enum: expecting '{'", token)
            enum_type = None
            if token is not None and token[0] != "name":
                self.lexer.push(token)
                token = ("name", "enum")
            else:
                enum_type = token[1]
            for enum in self.enums:
                self.index_add(enum[0], self.filename,
                               not self.is_header, "enum",
                               (enum[1], enum[2], enum_type))
            return token
        elif token[0] == "name" and token[1] == "VIR_ENUM_DECL":
            token = self.token()
            if token is not None and token[0] == "sep" and token[1] == "(":
                token = self.token()
                token = self.parseVirEnumDecl(token)
            else:
                self.error("parsing VIR_ENUM_DECL: expecting '('", token)
            if token is not None:
                self.lexer.push(token)
                token = ("name", "virenumdecl")
            return token

        elif token[0] == "name" and token[1] == "VIR_ENUM_IMPL":
            token = self.token()
            if token is not None and token[0] == "sep" and token[1] == "(":
                token = self.token()
                token = self.parseVirEnumImpl(token)
            else:
                self.error("parsing VIR_ENUM_IMPL: expecting '('", token)
            if token is not None:
                self.lexer.push(token)
                token = ("name", "virenumimpl")
            return token

        elif token[0] == "name" and token[1] == "VIR_LOG_INIT":
            token = self.token()
            if token is not None and token[0] == "sep" and token[1] == "(":
                token = self.token()
                token = self.parseVirLogInit(token)
            else:
                self.error("parsing VIR_LOG_INIT: expecting '('", token)
            if token is not None:
                self.lexer.push(token)
                token = ("name", "virloginit")
            return token

        elif token[0] == "name" and token[1] == "G_STATIC_ASSERT":
            # skip whole line
            while token is not None and not (token[0] == "sep" and
                                             token[1] == ";"):
                token = self.token()
            return self.token()

        elif token[0] == "name":
            if self.type == "":
                self.type = token[1]
            else:
                self.type = self.type + " " + token[1]
        else:
            self.error("parsing type %s: expecting a name" % (self.type),
                       token)
            return token
        token = self.token()
        while token is not None and (token[0] == "op" or
                                     token[0] == "name" and
                                     token[1] == "const"):
            self.type = self.type + " " + token[1]
            token = self.token()

        #
        # if there is a parenthesis here, this means a function type
        #
        if token is not None and token[0] == "sep" and token[1] == '(':
            self.type = self.type + token[1]
            token = self.token()
            while token is not None and token[0] == "op" and token[1] == '*':
                self.type = self.type + token[1]
                token = self.token()
            if token is None or token[0] != "name":
                self.error("parsing function type, name expected", token)
                return token
            self.type = self.type + token[1]
            nametok = token
            token = self.token()
            if token is not None and token[0] == "sep" and token[1] == ')':
                self.type = self.type + token[1]
                token = self.token()
                if token is not None and token[0] == "sep" and token[1] == '(':
                    token = self.token()
                    type = self.type
                    token = self.parseSignature(token)
                    self.type = type
                else:
                    self.error("parsing function type, '(' expected", token)
                    return token
            else:
                self.error("parsing function type, ')' expected", token)
                return token
            self.lexer.push(token)
            token = nametok
            return token

        #
        # do some lookahead for arrays
        #
        if token is not None and token[0] == "name":
            nametok = token
            token = self.token()
            if token is not None and token[0] == "sep" and token[1] == '[':
                self.type = self.type + " " + nametok[1]
                while token is not None and token[0] == "sep" and token[1] == '[':
                    self.type = self.type + token[1]
                    token = self.token()
                    while (token is not None and token[0] != 'sep' and
                           token[1] != ']' and token[1] != ';'):
                        self.type = self.type + token[1]
                        token = self.token()
                if token is not None and token[0] == 'sep' and token[1] == ']':
                    self.type = self.type + token[1]
                    token = self.token()
                else:
                    self.error("parsing array type, ']' expected", token)
                    return token
            elif token is not None and token[0] == "sep" and token[1] == ':':
                # remove :12 in case it's a limited int size
                token = self.token()
                token = self.token()
            self.lexer.push(token)
            token = nametok

        return token

    #
    # Parse a signature: '(' has been parsed and we scan the type definition
    #    up to the ')' included
    def parseSignature(self, token):
        signature = []
        if token is not None and token[0] == "sep" and token[1] == ')':
            self.signature = []
            token = self.token()
            return token
        while token is not None:
            token = self.parseType(token)
            if token is not None and token[0] == "name":
                signature.append((self.type, token[1], None))
                token = self.token()
            elif token is not None and token[0] == "sep" and token[1] == ',':
                token = self.token()
                continue
            elif token is not None and token[0] == "sep" and token[1] == ')':
                # only the type was provided
                if self.type == "...":
                    signature.append((self.type, "...", None))
                else:
                    signature.append((self.type, None, None))
            if token is not None and token[0] == "sep":
                if token[1] == ',':
                    token = self.token()
                    continue
                elif token[1] == ')':
                    token = self.token()
                    break
        self.signature = signature
        return token

    # this dict contains the functions that are allowed to use [unsigned]
    # long for legacy reasons in their signature and return type. this list is
    # fixed. new procedures and public APIs have to use [unsigned] long long
    long_legacy_functions = {
        "virGetVersion": (False, ("libVer", "typeVer")),
        "virConnectGetLibVersion": (False, ("libVer")),
        "virConnectGetVersion": (False, ("hvVer")),
        "virDomainGetMaxMemory": (True, ()),
        "virDomainMigrate": (False, ("flags", "bandwidth")),
        "virDomainMigrate2": (False, ("flags", "bandwidth")),
        "virDomainMigrateBegin3": (False, ("flags", "bandwidth")),
        "virDomainMigrateConfirm3": (False, ("flags", "bandwidth")),
        "virDomainMigrateDirect": (False, ("flags", "bandwidth")),
        "virDomainMigrateFinish": (False, ("flags")),
        "virDomainMigrateFinish2": (False, ("flags")),
        "virDomainMigrateFinish3": (False, ("flags")),
        "virDomainMigratePeer2Peer": (False, ("flags", "bandwidth")),
        "virDomainMigratePerform": (False, ("flags", "bandwidth")),
        "virDomainMigratePerform3": (False, ("flags", "bandwidth")),
        "virDomainMigratePrepare": (False, ("flags", "bandwidth")),
        "virDomainMigratePrepare2": (False, ("flags", "bandwidth")),
        "virDomainMigratePrepare3": (False, ("flags", "bandwidth")),
        "virDomainMigratePrepareTunnel": (False, ("flags", "bandwidth")),
        "virDomainMigratePrepareTunnel3": (False, ("flags", "bandwidth")),
        "virDomainMigrateToURI": (False, ("flags", "bandwidth")),
        "virDomainMigrateToURI2": (False, ("flags", "bandwidth")),
        "virDomainMigrateVersion1": (False, ("flags", "bandwidth")),
        "virDomainMigrateVersion2": (False, ("flags", "bandwidth")),
        "virDomainMigrateVersion3": (False, ("flags", "bandwidth")),
        "virDomainMigrateSetMaxSpeed": (False, ("bandwidth")),
        "virDomainSetMaxMemory": (False, ("memory")),
        "virDomainSetMemory": (False, ("memory")),
        "virDomainSetMemoryFlags": (False, ("memory")),
        "virDomainBlockCommit": (False, ("bandwidth")),
        "virDomainBlockJobSetSpeed": (False, ("bandwidth")),
        "virDomainBlockPull": (False, ("bandwidth")),
        "virDomainBlockRebase": (False, ("bandwidth")),
        "virDomainMigrateGetMaxSpeed": (False, ("bandwidth"))
    }

    def checkLongLegacyFunction(self, name, return_type, signature):
        if "long" in return_type and "long long" not in return_type:
            try:
                if not CParser.long_legacy_functions[name][0]:
                    raise Exception()
            except Exception:
                self.error(("function '%s' is not allowed to return long, "
                            "use long long instead") % name)

        for param in signature:
            if "long" in param[0] and "long long" not in param[0]:
                try:
                    if param[1] not in CParser.long_legacy_functions[name][1]:
                        raise Exception()
                except Exception:
                    self.error(("function '%s' is not allowed to take long "
                                "parameter '%s', use long long instead")
                               % (name, param[1]))

    # this dict contains the structs that are allowed to use [unsigned]
    # long for legacy reasons. this list is fixed. new structs have to use
    # [unsigned] long long
    long_legacy_struct_fields = {
        "_virDomainInfo": ("maxMem", "memory"),
        "_virNodeInfo": ("memory"),
        "_virDomainBlockJobInfo": ("bandwidth")
    }

    def checkLongLegacyStruct(self, name, fields):
        for field in fields:
            if "long" in field[0] and "long long" not in field[0]:
                try:
                    if field[1] not in CParser.long_legacy_struct_fields[name]:
                        raise Exception()
                except Exception:
                    self.error(("struct '%s' is not allowed to contain long "
                                "field '%s', use long long instead")
                               % (name, field[1]))

    #
    # Parse a global definition, be it a type, variable or function
    # the extern "C" blocks are a bit nasty and require it to recurse.
    #
    def parseGlobal(self, token):
        static = 0
        if token[1] == 'extern':
            token = self.token()
            if token is None:
                return token
            if token[0] == 'string':
                if token[1] == 'C':
                    token = self.token()
                    if token is None:
                        return token
                    if token[0] == 'sep' and token[1] == "{":
                        token = self.token()
#                        print('Entering extern "C line ', self.lineno())
                        while token is not None and (token[0] != 'sep' or
                                                     token[1] != "}"):
                            if token[0] == 'name':
                                token = self.parseGlobal(token)
                            else:
                                self.error(("token %s %s unexpected at the "
                                            "top level") %
                                           (token[0], token[1]))
                                token = self.parseGlobal(token)
#                        print('Exiting extern "C" line', self.lineno())
                        token = self.token()
                        return token
                else:
                    return token
        elif token[1] == 'static':
            static = 1
            token = self.token()
            if token is None or token[0] != 'name':
                return token

        if token[1] == 'typedef':
            token = self.token()
            return self.parseTypedef(token)
        else:
            token = self.parseType(token)
            type_orig = self.type
        if token is None or token[0] != "name":
            return token
        type = type_orig
        self.name = token[1]
        token = self.token()
        while token is not None and (token[0] == "sep" or token[0] == "op"):
            if token[0] == "sep":
                if token[1] == "[":
                    type = type + token[1]
                    token = self.token()
                    while token is not None and (token[0] != "sep" or
                                                 token[1] != ";"):
                        type = type + token[1]
                        token = self.token()

            if token is not None and token[0] == "op" and token[1] == "=":
                #
                # Skip the initialization of the variable
                #
                token = self.token()
                if token[0] == 'sep' and token[1] == '{':
                    token = self.token()
                    token = self.parseBlock(token)
                else:
                    self.comment = None
                    while token is not None and (token[0] != "sep" or
                                                 token[1] not in ',;'):
                        token = self.token()
                self.comment = None
                if token is None or token[0] != "sep" or (token[1] != ';' and
                   token[1] != ','):
                    self.error("missing ';' or ',' after value")

            if token is not None and token[0] == "sep":
                if token[1] == ";":
                    self.comment = None
                    token = self.token()
                    if type == "struct":
                        self.checkLongLegacyStruct(self.name, self.struct_fields)
                        self.index_add(self.name, self.filename,
                                       not self.is_header, "struct",
                                       self.struct_fields)
                    else:
                        self.index_add(self.name, self.filename,
                                       not self.is_header, "variable", type)
                    break
                elif token[1] == "(":
                    token = self.token()
                    token = self.parseSignature(token)
                    if token is None:
                        return None
                    if token[0] == "sep" and token[1] == ";":
                        self.checkLongLegacyFunction(self.name, type, self.signature)
                        d = self.mergeFunctionComment(self.name,
                                                      ((type, None),
                                                       self.signature), 1)
                        self.index_add(self.name, self.filename, static,
                                       "function", d)
                        token = self.token()
                    elif token[0] == "sep" and token[1] == "{":
                        self.checkLongLegacyFunction(self.name, type, self.signature)
                        d = self.mergeFunctionComment(self.name,
                                                      ((type, None),
                                                       self.signature), static)
                        self.index_add(self.name, self.filename, static,
                                       "function", d)
                        token = self.token()
                        token = self.parseBlock(token)
                elif token[1] == ',':
                    self.comment = None
                    self.index_add(self.name, self.filename, static,
                                   "variable", type)
                    type = type_orig
                    token = self.token()
                    while token is not None and token[0] == "sep":
                        type = type + token[1]
                        token = self.token()
                    if token is not None and token[0] == "name":
                        self.name = token[1]
                        token = self.token()
                else:
                    break

        return token

    def parse(self):
        if not quiet:
            print("Parsing %s" % (self.filename))
        token = self.token()
        while token is not None:
            if token[0] == 'name':
                token = self.parseGlobal(token)
            else:
                self.error("token %s %s unexpected at the top level" % (
                    token[0], token[1]))
                token = self.parseGlobal(token)
                return
        self.parseTopComment(self.top_comment)
        return self.index


class docBuilder:
    """A documentation builder"""
    def __init__(self, name, path='.', directories=['.'], includes=[]):
        self.name = name
        self.path = path
        self.directories = directories
        if name == "libvirt":
            self.includes = includes + list(included_files.keys())
        elif name == "libvirt-qemu":
            self.includes = includes + list(qemu_included_files.keys())
        elif name == "libvirt-lxc":
            self.includes = includes + list(lxc_included_files.keys())
        elif name == "libvirt-admin":
            self.includes = includes + list(admin_included_files.keys())
        self.modules = {}
        self.headers = {}
        self.idx = index()
        self.xref = {}
        self.index = {}
        self.basename = name
        self.errors = 0

    def warning(self, msg):
        global warnings
        warnings = warnings + 1
        print(msg)

    def error(self, msg):
        self.errors += 1
        print("Error:", msg, file=sys.stderr)

    def indexString(self, id, str):
        if str is None:
            return
        str = str.replace("'", ' ')
        str = str.replace('"', ' ')
        str = str.replace("/", ' ')
        str = str.replace('*', ' ')
        str = str.replace("[", ' ')
        str = str.replace("]", ' ')
        str = str.replace("(", ' ')
        str = str.replace(")", ' ')
        str = str.replace("<", ' ')
        str = str.replace('>', ' ')
        str = str.replace("&", ' ')
        str = str.replace('#', ' ')
        str = str.replace(",", ' ')
        str = str.replace('.', ' ')
        str = str.replace(';', ' ')
        tokens = str.split()
        for token in tokens:
            c = token[0]
            if not re.match(r"[a-zA-Z]", c):
                pass
            elif len(token) < 3:
                pass
            else:
                lower = token.lower()
                # TODO: generalize this a bit
                if lower == 'and' or lower == 'the':
                    pass
                elif token in self.xref:
                    self.xref[token].append(id)
                else:
                    self.xref[token] = [id]

    def analyze(self):
        if not quiet:
            print("Project %s : %d headers, %d modules" % (self.name, len(self.headers.keys()), len(self.modules.keys())))
        self.idx.analyze()

    def scanHeaders(self):
        for header in self.headers.keys():
            parser = CParser(header)
            idx = parser.parse()
            self.headers[header] = idx
            self.idx.merge(idx)

    def scanModules(self):
        for module in self.modules.keys():
            parser = CParser(module)
            idx = parser.parse()
            # idx.analyze()
            self.modules[module] = idx
            self.idx.merge_public(idx)

    def scan(self):
        for directory in self.directories:
            files = glob.glob(directory + "/*.c")
            for file in files:
                skip = 1
                for incl in self.includes:
                    if file.find(incl) != -1:
                        skip = 0
                        break
                if skip == 0:
                    self.modules[file] = None
            files = glob.glob(directory + "/*.h")
            for file in files:
                skip = 1
                for incl in self.includes:
                    if file.find(incl) != -1:
                        skip = 0
                        break
                if skip == 0:
                    self.headers[file] = None
        self.scanHeaders()
        self.scanModules()

    def modulename_file(self, file):
        module = os.path.basename(file)
        if module[-2:] == '.h':
            module = module[:-2]
        elif module[-2:] == '.c':
            module = module[:-2]
        return module

    def serialize_enum(self, output, name):
        id = self.idx.enums[name]
        output.write("    <enum name='%s' file='%s'" % (name,
                     self.modulename_file(id.header)))
        if id.info is not None:
            info = id.info
            valhex = ""
            if info[0] is not None and info[0] != '':
                try:
                    val = eval(info[0])
                    valhex = hex(val)
                except Exception:
                    val = info[0]
                output.write(" value='%s'" % (val))

                if valhex != "":
                    output.write(" value_hex='%s'" % (valhex))

                m = re.match(r"\(?1<<(\d+)\)?", info[0])
                if m:
                    output.write(" value_bitshift='%s'" % (m.group(1)))

            if info[2] is not None and info[2] != '':
                output.write(" type='%s'" % info[2])
            if info[1] is not None and info[1] != '':
                output.write(" info='%s'" % escape(info[1]))
        output.write("/>\n")

    def serialize_macro(self, output, name):
        id = self.idx.macros[name]
        output.write("    <macro name='%s' file='%s'" % (name,
                     self.modulename_file(id.header)))
        if id.info is None:
            args = []
            desc = None
            strValue = None
        else:
            (args, desc, strValue) = id.info

        if strValue is not None:
            output.write(" string='%s'" % strValue)
        output.write(">\n")

        if desc is not None and desc != "":
            output.write("      <info><![CDATA[%s]]></info>\n" % (desc))
            self.indexString(name, desc)
        for arg in args:
            (name, desc) = arg
            if desc is not None and desc != "":
                output.write("      <arg name='%s' info='%s'/>\n" % (
                             name, escape(desc)))
                self.indexString(name, desc)
            else:
                output.write("      <arg name='%s'/>\n" % name)
        output.write("    </macro>\n")

    def serialize_union(self, output, field, desc):
        output.write("      <field name='%s' type='union' info='%s'>\n" % (field[1], desc))
        output.write("        <union>\n")
        for f in field[3]:
            desc = f[2]
            if desc is None:
                desc = ''
            else:
                desc = escape(desc)
            output.write("          <field name='%s' type='%s' info='%s'/>\n" % (f[1], f[0], desc))

        output.write("        </union>\n")
        output.write("      </field>\n")

    def serialize_typedef(self, output, name):
        id = self.idx.typedefs[name]
        if id.info[0:7] == 'struct ':
            output.write("    <struct name='%s' file='%s' type='%s'" % (
                name, self.modulename_file(id.header), id.info))
            name = id.info[7:]
            if (name in self.idx.structs and
                    isinstance(self.idx.structs[name].info, (list, tuple))):
                output.write(">\n")
                try:
                    for field in self.idx.structs[name].info:
                        desc = field[2]
                        self.indexString(name, desc)
                        if desc is None:
                            desc = ''
                        else:
                            desc = escape(desc)
                        if field[0] == "union":
                            self.serialize_union(output, field, desc)
                        else:
                            output.write("      <field name='%s' type='%s' info='%s'/>\n" % (field[1], field[0], desc))
                except Exception:
                    self.warning("Failed to serialize struct %s" % name)
                output.write("    </struct>\n")
            else:
                output.write("/>\n")
        else:
            output.write("    <typedef name='%s' file='%s' type='%s'" % (
                         name, self.modulename_file(id.header), id.info))
            try:
                desc = id.extra
                if desc is not None and desc != "":
                    output.write(">\n      <info><![CDATA[%s]]></info>\n" % (desc))
                    output.write("    </typedef>\n")
                else:
                    output.write("/>\n")
            except Exception:
                output.write("/>\n")

    def serialize_variable(self, output, name):
        id = self.idx.variables[name]
        if id.info is not None:
            output.write("    <variable name='%s' file='%s' type='%s'/>\n" % (
                name, self.modulename_file(id.header), id.info))
        else:
            output.write("    <variable name='%s' file='%s'/>\n" % (
                name, self.modulename_file(id.header)))

    def serialize_function(self, output, name):
        id = self.idx.functions[name]
        if name == debugsym and not quiet:
            print("=>", id)

        # NB: this is consumed by a regex in 'getAPIFilenames' in hvsupport.pl
        output.write("    <%s name='%s' file='%s' module='%s'>\n" % (id.type,
                     name, self.modulename_file(id.header),
                     self.modulename_file(id.module)))
        #
        # Processing of conditionals modified by Bill 1/1/05
        #
        if id.conditionals is not None:
            apstr = ""
            for cond in id.conditionals:
                if apstr != "":
                    apstr = apstr + " &amp;&amp; "
                apstr = apstr + cond
            output.write("      <cond>%s</cond>\n" % (apstr))
        try:
            (ret, params, desc) = id.info
            output.write("      <info><![CDATA[%s]]></info>\n" % (desc))
            self.indexString(name, desc)
            if ret[0] is not None:
                if ret[0] == "void":
                    output.write("      <return type='void'/>\n")
                elif (ret[1] is None or ret[1] == '') and name not in ignored_functions:
                    self.error("Missing documentation for return of function `%s'" % name)
                else:
                    output.write("      <return type='%s' info='%s'/>\n" % (
                        ret[0], escape(ret[1])))
                    self.indexString(name, ret[1])
            for param in params:
                if param[0] == 'void':
                    continue
                if (param[2] is None or param[2] == ''):
                    if name in ignored_functions:
                        output.write("      <arg name='%s' type='%s' info=''/>\n" % (param[1], param[0]))
                    else:
                        self.error("Missing documentation for arg `%s' of function `%s'" % (param[1], name))
                else:
                    output.write("      <arg name='%s' type='%s' info='%s'/>\n" % (param[1], param[0], escape(param[2])))
                    self.indexString(name, param[2])
        except Exception:
            print("Exception:", sys.exc_info()[1], file=sys.stderr)
            self.warning("Failed to save function %s info: %s" % (name, repr(id.info)))
        output.write("    </%s>\n" % (id.type))

    def serialize_exports(self, output, file):
        module = self.modulename_file(file)
        output.write("    <file name='%s'>\n" % (module))
        dict = self.headers[file]
        if dict.info is not None:
            for data in ('Summary', 'Description'):
                try:
                    output.write("     <%s>%s</%s>\n" % (
                                 data.lower(),
                                 escape(dict.info[data]),
                                 data.lower()))
                except KeyError:
                    self.warning("Header %s lacks a %s description" % (module, data))
            if 'Description' in dict.info:
                desc = dict.info['Description']
                if desc.find("DEPRECATED") != -1:
                    output.write("     <deprecated/>\n")

        for id in uniq(dict.macros.keys()):
            # Macros are sometime used to masquerade other types.
            if id in dict.functions:
                continue
            if id in dict.variables:
                continue
            if id in dict.typedefs:
                continue
            if id in dict.structs:
                continue
            if id in dict.unions:
                continue
            if id in dict.enums:
                continue
            output.write("     <exports symbol='%s' type='macro'/>\n" % (id))
        for id in uniq(dict.enums.keys()):
            output.write("     <exports symbol='%s' type='enum'/>\n" % (id))
        for id in uniq(dict.typedefs.keys()):
            output.write("     <exports symbol='%s' type='typedef'/>\n" % (id))
        for id in uniq(dict.structs.keys()):
            output.write("     <exports symbol='%s' type='struct'/>\n" % (id))
        for id in uniq(dict.variables.keys()):
            output.write("     <exports symbol='%s' type='variable'/>\n" % (id))
        for id in uniq(dict.functions.keys()):
            output.write("     <exports symbol='%s' type='function'/>\n" % (id))
        output.write("    </file>\n")

    def serialize(self):
        filename = "%s/%s-api.xml" % (self.path, self.name)
        if not quiet:
            print("Saving XML description %s" % (filename))
        output = open(filename, "w")
        output.write('<?xml version="1.0" encoding="ISO-8859-1"?>\n')
        output.write("<api name='%s'>\n" % self.name)
        output.write("  <files>\n")
        headers = sorted(self.headers.keys())
        for file in headers:
            self.serialize_exports(output, file)
        output.write("  </files>\n")
        output.write("  <symbols>\n")
        macros = sorted(self.idx.macros.keys())
        for macro in macros:
            self.serialize_macro(output, macro)
        enums = sorted(self.idx.enums.keys())
        for enum in enums:
            self.serialize_enum(output, enum)
        typedefs = sorted(self.idx.typedefs.keys())
        for typedef in typedefs:
            self.serialize_typedef(output, typedef)
        variables = sorted(self.idx.variables.keys())
        for variable in variables:
            self.serialize_variable(output, variable)
        functions = sorted(self.idx.functions.keys())
        for function in functions:
            self.serialize_function(output, function)
        output.write("  </symbols>\n")
        output.write("</api>\n")
        output.close()

        if self.errors > 0:
            print("apibuild.py: %d error(s) encountered during generation" % self.errors, file=sys.stderr)
            sys.exit(3)


class app:
    def warning(self, msg):
        global warnings
        warnings = warnings + 1
        print(msg)

    def rebuild(self, name):
        if name not in ["libvirt", "libvirt-qemu", "libvirt-lxc", "libvirt-admin"]:
            self.warning("rebuild() failed, unknown module %s" % name)
            return None
        builder = None
        srcdir = os.path.abspath((os.environ["srcdir"]))
        builddir = os.path.abspath((os.environ["builddir"]))
        if glob.glob(srcdir + "/../src/libvirt.c") != []:
            if not quiet:
                print("Rebuilding API description for %s" % name)
            dirs = [srcdir + "/../src",
                    srcdir + "/../src/admin",
                    srcdir + "/../src/util",
                    srcdir + "/../include/libvirt",
                    builddir + "/../include/libvirt"]
            builder = docBuilder(name, builddir, dirs, [])
        else:
            self.warning("rebuild() failed, unable to guess the module")
            return None
        builder.scan()
        builder.analyze()
        builder.serialize()
        return builder

    #
    # for debugging the parser
    #
    def parse(self, filename):
        parser = CParser(filename)
        idx = parser.parse()
        return idx


if __name__ == "__main__":
    app = app()
    if len(sys.argv) > 1:
        debug = 1
        app.parse(sys.argv[1])
    else:
        app.rebuild("libvirt")
        app.rebuild("libvirt-qemu")
        app.rebuild("libvirt-lxc")
        app.rebuild("libvirt-admin")
    if warnings > 0:
        sys.exit(2)
    else:
        sys.exit(0)
