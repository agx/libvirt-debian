/*
 * cpu.h: internal functions for CPU manipulation
 *
 * Copyright (C) 2009-2010, 2013 Red Hat, Inc.
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
 * Authors:
 *      Jiri Denemark <jdenemar@redhat.com>
 */

#ifndef __VIR_CPU_H__
# define __VIR_CPU_H__

# include "virerror.h"
# include "datatypes.h"
# include "virarch.h"
# include "conf/cpu_conf.h"
# include "cpu_x86_data.h"
# include "cpu_ppc64_data.h"


typedef struct _virCPUData virCPUData;
typedef virCPUData *virCPUDataPtr;
struct _virCPUData {
    virArch arch;
    union {
        virCPUx86Data x86;
        virCPUppc64Data ppc64;
        /* generic driver needs no data */
    } data;
};


typedef virCPUCompareResult
(*virCPUArchCompare)(virCPUDefPtr host,
                     virCPUDefPtr cpu,
                     bool failIncompatible);

typedef int
(*cpuArchDecode)    (virCPUDefPtr cpu,
                     const virCPUData *data,
                     const char **models,
                     unsigned int nmodels,
                     const char *preferred,
                     unsigned int flags);

typedef int
(*cpuArchEncode)    (virArch arch,
                     const virCPUDef *cpu,
                     virCPUDataPtr *forced,
                     virCPUDataPtr *required,
                     virCPUDataPtr *optional,
                     virCPUDataPtr *disabled,
                     virCPUDataPtr *forbidden,
                     virCPUDataPtr *vendor);

typedef void
(*cpuArchDataFree)  (virCPUDataPtr data);

typedef virCPUDataPtr
(*cpuArchNodeData)  (virArch arch);

typedef virCPUCompareResult
(*cpuArchGuestData) (virCPUDefPtr host,
                     virCPUDefPtr guest,
                     virCPUDataPtr *data,
                     char **message);

typedef virCPUDefPtr
(*cpuArchBaseline)  (virCPUDefPtr *cpus,
                     unsigned int ncpus,
                     const char **models,
                     unsigned int nmodels,
                     unsigned int flags);

typedef int
(*virCPUArchUpdate)(virCPUDefPtr guest,
                    const virCPUDef *host);

typedef int
(*virCPUArchCheckFeature)(const virCPUDef *cpu,
                          const char *feature);

typedef int
(*virCPUArchDataCheckFeature)(const virCPUData *data,
                              const char *feature);

typedef char *
(*cpuArchDataFormat)(const virCPUData *data);

typedef virCPUDataPtr
(*cpuArchDataParse) (xmlXPathContextPtr ctxt);

typedef int
(*cpuArchGetModels) (char ***models);

typedef int
(*virCPUArchTranslate)(virCPUDefPtr cpu,
                       const char **models,
                       unsigned int nmodels);

struct cpuArchDriver {
    const char *name;
    const virArch *arch;
    unsigned int narch;
    virCPUArchCompare   compare;
    cpuArchDecode       decode;
    cpuArchEncode       encode;
    cpuArchDataFree     free;
    cpuArchNodeData     nodeData;
    cpuArchGuestData    guestData;
    cpuArchBaseline     baseline;
    virCPUArchUpdate    update;
    virCPUArchCheckFeature checkFeature;
    virCPUArchDataCheckFeature dataCheckFeature;
    cpuArchDataFormat   dataFormat;
    cpuArchDataParse    dataParse;
    cpuArchGetModels    getModels;
    virCPUArchTranslate translate;
};


virCPUCompareResult
virCPUCompareXML(virArch arch,
                 virCPUDefPtr host,
                 const char *xml,
                 bool failIncompatible);

virCPUCompareResult
virCPUCompare(virArch arch,
              virCPUDefPtr host,
              virCPUDefPtr cpu,
              bool failIncompatible)
    ATTRIBUTE_NONNULL(3);

int
cpuDecode   (virCPUDefPtr cpu,
             const virCPUData *data,
             const char **models,
             unsigned int nmodels,
             const char *preferred)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);

int
cpuEncode   (virArch arch,
             const virCPUDef *cpu,
             virCPUDataPtr *forced,
             virCPUDataPtr *required,
             virCPUDataPtr *optional,
             virCPUDataPtr *disabled,
             virCPUDataPtr *forbidden,
             virCPUDataPtr *vendor)
    ATTRIBUTE_NONNULL(2);

void
cpuDataFree (virCPUDataPtr data);

virCPUDataPtr
cpuNodeData (virArch arch);

virCPUCompareResult
cpuGuestData(virCPUDefPtr host,
             virCPUDefPtr guest,
             virCPUDataPtr *data,
             char **msg)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);

char *
cpuBaselineXML(const char **xmlCPUs,
               unsigned int ncpus,
               const char **models,
               unsigned int nmodels,
               unsigned int flags);

virCPUDefPtr
cpuBaseline (virCPUDefPtr *cpus,
             unsigned int ncpus,
             const char **models,
             unsigned int nmodels,
             unsigned int flags)
    ATTRIBUTE_NONNULL(1);

int
virCPUUpdate(virArch arch,
             virCPUDefPtr guest,
             const virCPUDef *host)
    ATTRIBUTE_NONNULL(2);


int
virCPUCheckFeature(virArch arch,
                   const virCPUDef *cpu,
                   const char *feature)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);


int
virCPUDataCheckFeature(const virCPUData *data,
                       const char *feature)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);


bool
cpuModelIsAllowed(const char *model,
                  const char **models,
                  unsigned int nmodels)
    ATTRIBUTE_NONNULL(1);

int
cpuGetModels(virArch arch, char ***models);

int
virCPUTranslate(virArch arch,
                virCPUDefPtr cpu,
                char **models,
                unsigned int nmodels)
    ATTRIBUTE_NONNULL(2);


/* cpuDataFormat and cpuDataParse are implemented for unit tests only and
 * have no real-life usage
 */
char *cpuDataFormat(const virCPUData *data)
    ATTRIBUTE_NONNULL(1);
virCPUDataPtr cpuDataParse(const char *xmlStr)
    ATTRIBUTE_NONNULL(1);

#endif /* __VIR_CPU_H__ */
