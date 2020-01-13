/*
 * virtypedparam.c: utility functions for dealing with virTypedParameters
 *
 * Copyright (C) 2011-2014 Red Hat, Inc.
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
#include "virtypedparam.h"

#include <stdarg.h>

#include "viralloc.h"
#include "virutil.h"
#include "virerror.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_ENUM_IMPL(virTypedParameter,
              VIR_TYPED_PARAM_LAST,
              "unknown",
              "int",
              "uint",
              "llong",
              "ullong",
              "double",
              "boolean",
              "string",
);

static int
virTypedParamsSortName(const void *left, const void *right)
{
    const virTypedParameter *param_left = left, *param_right = right;
    return strcmp(param_left->field, param_right->field);
}

/* Validate that PARAMS contains only recognized parameter names with
 * correct types, and with no duplicates except for parameters
 * specified with VIR_TYPED_PARAM_MULTIPLE flag in type.
 * Pass in as many name/type pairs as appropriate, and pass NULL to end
 * the list of accepted parameters.  Return 0 on success, -1 on failure
 * with error message already issued.  */
int
virTypedParamsValidate(virTypedParameterPtr params, int nparams, ...)
{
    va_list ap;
    int ret = -1;
    size_t i, j;
    const char *name, *last_name = NULL;
    int type;
    size_t nkeys = 0, nkeysalloc = 0;
    virTypedParameterPtr sorted = NULL, keys = NULL;

    va_start(ap, nparams);

    if (VIR_ALLOC_N(sorted, nparams) < 0)
        goto cleanup;

    /* Here we intentionally don't copy values */
    memcpy(sorted, params, sizeof(*params) * nparams);
    qsort(sorted, nparams, sizeof(*sorted), virTypedParamsSortName);

    name = va_arg(ap, const char *);
    while (name) {
        type = va_arg(ap, int);
        if (VIR_RESIZE_N(keys, nkeysalloc, nkeys, 1) < 0)
            goto cleanup;

        if (virStrcpyStatic(keys[nkeys].field, name) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Field name '%s' too long"), name);
            goto cleanup;
        }

        keys[nkeys].type = type & ~VIR_TYPED_PARAM_MULTIPLE;
        /* Value is not used anyway */
        keys[nkeys].value.i = type & VIR_TYPED_PARAM_MULTIPLE;

        nkeys++;
        name = va_arg(ap, const char *);
    }

    qsort(keys, nkeys, sizeof(*keys), virTypedParamsSortName);

    for (i = 0, j = 0; i < nparams && j < nkeys;) {
        if (STRNEQ(sorted[i].field, keys[j].field)) {
            j++;
        } else {
            if (STREQ_NULLABLE(last_name, sorted[i].field) &&
                !(keys[j].value.i & VIR_TYPED_PARAM_MULTIPLE)) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("parameter '%s' occurs multiple times"),
                               sorted[i].field);
                goto cleanup;
            }
            if (sorted[i].type != keys[j].type) {
                const char *badtype;

                badtype = virTypedParameterTypeToString(sorted[i].type);
                if (!badtype)
                    badtype = virTypedParameterTypeToString(0);
                virReportError(VIR_ERR_INVALID_ARG,
                               _("invalid type '%s' for parameter '%s', "
                                 "expected '%s'"),
                               badtype, sorted[i].field,
                               virTypedParameterTypeToString(keys[j].type));
                goto cleanup;
            }
            last_name = sorted[i].field;
            i++;
        }
    }

    if (j == nkeys && i != nparams) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                       _("parameter '%s' not supported"),
                       sorted[i].field);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    va_end(ap);
    VIR_FREE(sorted);
    VIR_FREE(keys);
    return ret;
}


/* Check if params contains only specified parameter names. Return true if
 * only specified names are present in params, false if params contains any
 * unspecified parameter name. */
bool
virTypedParamsCheck(virTypedParameterPtr params,
                    int nparams,
                    const char **names,
                    int nnames)
{
    size_t i, j;

    for (i = 0; i < nparams; i++) {
        bool found = false;
        for (j = 0; j < nnames; j++) {
            if (STREQ(params[i].field, names[j])) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }

    return true;
}

char *
virTypedParameterToString(virTypedParameterPtr param)
{
    char *value = NULL;

    switch (param->type) {
    case VIR_TYPED_PARAM_INT:
        value = g_strdup_printf("%d", param->value.i);
        break;
    case VIR_TYPED_PARAM_UINT:
        value = g_strdup_printf("%u", param->value.ui);
        break;
    case VIR_TYPED_PARAM_LLONG:
        value = g_strdup_printf("%lld", param->value.l);
        break;
    case VIR_TYPED_PARAM_ULLONG:
        value = g_strdup_printf("%llu", param->value.ul);
        break;
    case VIR_TYPED_PARAM_DOUBLE:
        value = g_strdup_printf("%g", param->value.d);
        break;
    case VIR_TYPED_PARAM_BOOLEAN:
        value = g_strdup_printf("%d", param->value.b);
        break;
    case VIR_TYPED_PARAM_STRING:
        value = g_strdup(param->value.s);
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected type %d for field %s"),
                       param->type, param->field);
    }

    return value;
}


static int
virTypedParameterAssignValueVArgs(virTypedParameterPtr param,
                                  int type,
                                  va_list ap,
                                  bool copystr)
{
    param->type = type;
    switch (type) {
    case VIR_TYPED_PARAM_INT:
        param->value.i = va_arg(ap, int);
        break;
    case VIR_TYPED_PARAM_UINT:
        param->value.ui = va_arg(ap, unsigned int);
        break;
    case VIR_TYPED_PARAM_LLONG:
        param->value.l = va_arg(ap, long long int);
        break;
    case VIR_TYPED_PARAM_ULLONG:
        param->value.ul = va_arg(ap, unsigned long long int);
        break;
    case VIR_TYPED_PARAM_DOUBLE:
        param->value.d = va_arg(ap, double);
        break;
    case VIR_TYPED_PARAM_BOOLEAN:
        param->value.b = !!va_arg(ap, int);
        break;
    case VIR_TYPED_PARAM_STRING:
        if (copystr) {
            param->value.s = g_strdup(va_arg(ap, char *));
        } else {
            param->value.s = va_arg(ap, char *);
        }

        if (!param->value.s)
            param->value.s = g_strdup("");
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected type %d for field %s"), type,
                       NULLSTR(param->field));
        return -1;
    }

    return 0;
}


static int
virTypedParameterAssignValue(virTypedParameterPtr param,
                             bool copystr,
                             int type,
                             ...)
{
    int ret;
    va_list ap;

    va_start(ap, type);
    ret = virTypedParameterAssignValueVArgs(param, type, ap, copystr);
    va_end(ap);

    return ret;
}


/* Assign name, type, and the appropriately typed arg to param; in the
 * case of a string, the caller is assumed to have malloc'd a string,
 * or can pass NULL to have this function malloc an empty string.
 * Return 0 on success, -1 after an error message on failure.  */
int
virTypedParameterAssign(virTypedParameterPtr param, const char *name,
                        int type, ...)
{
    va_list ap;
    int ret = -1;

    if (virStrcpyStatic(param->field, name) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Field name '%s' too long"),
                       name);
        return -1;
    }

    va_start(ap, type);
    ret = virTypedParameterAssignValueVArgs(param, type, ap, false);
    va_end(ap);

    return ret;
}


/**
 * virTypedParamsReplaceString:
 * @params: pointer to the array of typed parameters
 * @nparams: number of parameters in the @params array
 * @name: name of the parameter to set
 * @value: the value to store into the parameter
 *
 * Sets new value @value to parameter called @name with char * type. If the
 * parameter does not exist yet in @params, it is automatically created and
 * @naprams is incremented by one. Otherwise current value of the parameter
 * is freed on success. The function creates its own copy of @value string,
 * which needs to be freed using virTypedParamsFree or virTypedParamsClear.
 *
 * Returns 0 on success, -1 on error.
 */
int
virTypedParamsReplaceString(virTypedParameterPtr *params,
                            int *nparams,
                            const char *name,
                            const char *value)
{
    char *str = NULL;
    char *old = NULL;
    size_t n = *nparams;
    virTypedParameterPtr param;

    param = virTypedParamsGet(*params, n, name);
    if (param) {
        if (param->type != VIR_TYPED_PARAM_STRING) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("Parameter '%s' is not a string"),
                           param->field);
            return -1;
        }
        old = param->value.s;
    } else {
        if (VIR_EXPAND_N(*params, n, 1) < 0)
            return -1;
        param = *params + n - 1;
    }

    str = g_strdup(value);

    if (virTypedParameterAssign(param, name,
                                VIR_TYPED_PARAM_STRING, str) < 0) {
        param->value.s = old;
        VIR_FREE(str);
        return -1;
    }
    VIR_FREE(old);

    *nparams = n;
    return 0;
}


int
virTypedParamsCopy(virTypedParameterPtr *dst,
                   virTypedParameterPtr src,
                   int nparams)
{
    size_t i;

    *dst = NULL;
    if (!src || nparams <= 0)
        return 0;

    if (VIR_ALLOC_N(*dst, nparams) < 0)
        return -1;

    for (i = 0; i < nparams; i++) {
        ignore_value(virStrcpyStatic((*dst)[i].field, src[i].field));
        (*dst)[i].type = src[i].type;
        if (src[i].type == VIR_TYPED_PARAM_STRING) {
            (*dst)[i].value.s = g_strdup(src[i].value.s);
        } else {
            (*dst)[i].value = src[i].value;
        }
    }

    return 0;
}


/**
 * virTypedParamsFilter:
 * @params: array of typed parameters
 * @nparams: number of parameters in the @params array
 * @name: name of the parameter to find
 * @ret: pointer to the returned array
 *
 * Filters @params retaining only the parameters named @name in the
 * resulting array @ret. Caller should free the @ret array but not
 * the items since they are pointing to the @params elements.
 *
 * Returns amount of elements in @ret on success, -1 on error.
 */
int
virTypedParamsFilter(virTypedParameterPtr params,
                     int nparams,
                     const char *name,
                     virTypedParameterPtr **ret)
{
    size_t i, n = 0;

    if (VIR_ALLOC_N(*ret, nparams) < 0)
        return -1;

    for (i = 0; i < nparams; i++) {
        if (STREQ(params[i].field, name)) {
            (*ret)[n] = &params[i];
            n++;
        }
    }

    return n;
}


/**
 * virTypedParamsGetStringList:
 * @params: array of typed parameters
 * @nparams: number of parameters in the @params array
 * @name: name of the parameter to find
 * @values: array of returned values
 *
 * Finds all parameters with desired @name within @params and
 * store their values into @values. The @values array is self
 * allocated and its length is stored into @picked. When no
 * longer needed, caller should free the returned array, but not
 * the items since they are taken from @params array.
 *
 * Returns amount of strings in @values array on success,
 * -1 otherwise.
 */
int
virTypedParamsGetStringList(virTypedParameterPtr params,
                            int nparams,
                            const char *name,
                            const char ***values)
{
    size_t i, n;
    int nfiltered;
    virTypedParameterPtr *filtered = NULL;

    virCheckNonNullArgGoto(values, error);
    *values = NULL;

    nfiltered = virTypedParamsFilter(params, nparams, name, &filtered);

    if (nfiltered < 0)
        goto error;

    if (nfiltered &&
        VIR_ALLOC_N(*values, nfiltered) < 0)
        goto error;

    for (n = 0, i = 0; i < nfiltered; i++) {
        if (filtered[i]->type == VIR_TYPED_PARAM_STRING)
            (*values)[n++] = filtered[i]->value.s;
    }

    VIR_FREE(filtered);
    return n;

 error:
    if (values)
        VIR_FREE(*values);
    VIR_FREE(filtered);
    return -1;
}


/**
 * virTypedParamsRemoteFree:
 * @remote_params_val: array of typed parameters as specified by
 *                     (remote|admin)_protocol.h
 * @remote_params_len: number of parameters in @remote_params_val
 *
 * Frees memory used by string representations of parameter identificators,
 * memory used by string values of parameters and the memory occupied by
 * @remote_params_val itself.
 *
 * Returns nothing.
 */
void
virTypedParamsRemoteFree(virTypedParameterRemotePtr remote_params_val,
                         unsigned int remote_params_len)
{
    size_t i;

    if (!remote_params_val)
        return;

    for (i = 0; i < remote_params_len; i++) {
        VIR_FREE(remote_params_val[i].field);
        if (remote_params_val[i].value.type == VIR_TYPED_PARAM_STRING)
            VIR_FREE(remote_params_val[i].value.remote_typed_param_value.s);
    }
    VIR_FREE(remote_params_val);
}


/**
 * virTypedParamsDeserialize:
 * @remote_params: protocol data to be deserialized (obtained from remote side)
 * @remote_params_len: number of parameters returned in @remote_params
 * @limit: user specified maximum limit to @remote_params_len
 * @params: pointer which will hold the deserialized @remote_params data
 * @nparams: number of entries in @params
 *
 * This function will attempt to deserialize protocol-encoded data obtained
 * from remote side. Two modes of operation are supported, depending on the
 * caller's design:
 * 1) Older APIs do not rely on deserializer allocating memory for @params,
 *    thus calling the deserializer twice, once to find out the actual number of
 *    parameters for @params to hold, followed by an allocation of @params and
 *    a second call to the deserializer to actually retrieve the data.
 * 2) Newer APIs rely completely on the deserializer to allocate the right
 *    amount of memory for @params to hold all the data obtained in
 *    @remote_params.
 *
 * If used with model 1, two checks are performed, first one comparing the user
 * specified limit to the actual size of remote data and the second one
 * verifying the user allocated amount of memory is indeed capable of holding
 * remote data @remote_params.
 * With model 2, only the first check against @limit is performed.
 *
 * Returns 0 on success or -1 in case of an error.
 */
int
virTypedParamsDeserialize(virTypedParameterRemotePtr remote_params,
                          unsigned int remote_params_len,
                          int limit,
                          virTypedParameterPtr *params,
                          int *nparams)
{
    size_t i = 0;
    int rv = -1;
    bool userAllocated = *params != NULL;

    if (limit && remote_params_len > limit) {
        virReportError(VIR_ERR_RPC,
                       _("too many parameters '%u' for limit '%d'"),
                       remote_params_len, limit);
        goto cleanup;
    }

    if (userAllocated) {
        /* Check the length of the returned list carefully. */
        if (remote_params_len > *nparams) {
            virReportError(VIR_ERR_RPC,
                           _("too many parameters '%u' for nparams '%d'"),
                           remote_params_len, *nparams);
            goto cleanup;
        }
    } else {
        if (VIR_ALLOC_N(*params, remote_params_len) < 0)
            goto cleanup;
    }
    *nparams = remote_params_len;

    /* Deserialize the result. */
    for (i = 0; i < remote_params_len; ++i) {
        virTypedParameterPtr param = *params + i;
        virTypedParameterRemotePtr remote_param = remote_params + i;

        if (virStrcpyStatic(param->field,
                            remote_param->field) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("parameter %s too big for destination"),
                           remote_param->field);
            goto cleanup;
        }

        param->type = remote_param->value.type;
        switch (param->type) {
        case VIR_TYPED_PARAM_INT:
            param->value.i =
                remote_param->value.remote_typed_param_value.i;
            break;
        case VIR_TYPED_PARAM_UINT:
            param->value.ui =
                remote_param->value.remote_typed_param_value.ui;
            break;
        case VIR_TYPED_PARAM_LLONG:
            param->value.l =
                remote_param->value.remote_typed_param_value.l;
            break;
        case VIR_TYPED_PARAM_ULLONG:
            param->value.ul =
                remote_param->value.remote_typed_param_value.ul;
            break;
        case VIR_TYPED_PARAM_DOUBLE:
            param->value.d =
                remote_param->value.remote_typed_param_value.d;
            break;
        case VIR_TYPED_PARAM_BOOLEAN:
            param->value.b =
                remote_param->value.remote_typed_param_value.b;
            break;
        case VIR_TYPED_PARAM_STRING:
            param->value.s = g_strdup(remote_param->value.remote_typed_param_value.s);
            break;
        default:
            virReportError(VIR_ERR_RPC, _("unknown parameter type: %d"),
                           param->type);
            goto cleanup;
        }
    }

    rv = 0;

 cleanup:
    if (rv < 0) {
        if (userAllocated) {
            virTypedParamsClear(*params, i);
        } else {
            virTypedParamsFree(*params, i);
            *params = NULL;
            *nparams = 0;
        }
    }
    return rv;
}


/**
 * virTypedParamsSerialize:
 * @params: array of parameters to be serialized and later sent to remote side
 * @nparams: number of elements in @params
 * @limit: user specified maximum limit to @remote_params_len
 * @remote_params_val: protocol independent remote representation of @params
 * @remote_params_len: the final number of elements in @remote_params_val
 * @flags: bitwise-OR of virTypedParameterFlags
 *
 * This method serializes typed parameters provided by @params into
 * @remote_params_val which is the representation actually being sent.
 * It also checks, if the @limit imposed by RPC on the maximum number of
 * parameters is not exceeded.
 *
 * Server side using this method also filters out any string parameters that
 * must not be returned to older clients and handles possibly sparse arrays
 * returned by some APIs.
 *
 * Returns 0 on success, -1 on error.
 */
int
virTypedParamsSerialize(virTypedParameterPtr params,
                        int nparams,
                        int limit,
                        virTypedParameterRemotePtr *remote_params_val,
                        unsigned int *remote_params_len,
                        unsigned int flags)
{
    size_t i;
    size_t j;
    int rv = -1;
    virTypedParameterRemotePtr params_val = NULL;
    int params_len = nparams;

    if (nparams > limit) {
        virReportError(VIR_ERR_RPC,
                       _("too many parameters '%d' for limit '%d'"),
                       nparams, limit);
        goto cleanup;
    }

    if (VIR_ALLOC_N(params_val, nparams) < 0)
        goto cleanup;

    for (i = 0, j = 0; i < nparams; ++i) {
        virTypedParameterPtr param = params + i;
        virTypedParameterRemotePtr val = params_val + j;
        /* NOTE: Following snippet is relevant to server only, because
         * virDomainGetCPUStats can return a sparse array; also, we can't pass
         * back strings to older clients. */
        if (!param->type ||
            (!(flags & VIR_TYPED_PARAM_STRING_OKAY) &&
             param->type == VIR_TYPED_PARAM_STRING)) {
            --params_len;
            continue;
        }

        /* This will be either freed by virNetServerDispatchCall or call(),
         * depending on the calling side, i.e. server or client */
        val->field = g_strdup(param->field);
        val->value.type = param->type;
        switch (param->type) {
        case VIR_TYPED_PARAM_INT:
            val->value.remote_typed_param_value.i = param->value.i;
            break;
        case VIR_TYPED_PARAM_UINT:
            val->value.remote_typed_param_value.ui = param->value.ui;
            break;
        case VIR_TYPED_PARAM_LLONG:
            val->value.remote_typed_param_value.l = param->value.l;
            break;
        case VIR_TYPED_PARAM_ULLONG:
            val->value.remote_typed_param_value.ul = param->value.ul;
            break;
        case VIR_TYPED_PARAM_DOUBLE:
            val->value.remote_typed_param_value.d = param->value.d;
            break;
        case VIR_TYPED_PARAM_BOOLEAN:
            val->value.remote_typed_param_value.b = param->value.b;
            break;
        case VIR_TYPED_PARAM_STRING:
            val->value.remote_typed_param_value.s = g_strdup(param->value.s);
            break;
        default:
            virReportError(VIR_ERR_RPC, _("unknown parameter type: %d"),
                           param->type);
            goto cleanup;
        }
        j++;
    }

    *remote_params_val = params_val;
    *remote_params_len = params_len;
    params_val = NULL;
    rv = 0;

 cleanup:
    virTypedParamsRemoteFree(params_val, nparams);
    return rv;
}


void
virTypedParamListFree(virTypedParamListPtr list)
{
    if (!list)
        return;

    virTypedParamsFree(list->par, list->npar);
    VIR_FREE(list);
}


size_t
virTypedParamListStealParams(virTypedParamListPtr list,
                             virTypedParameterPtr *params)
{
    size_t ret = list->npar;

    *params = g_steal_pointer(&list->par);
    list->npar = 0;
    list->par_alloc = 0;

    return ret;
}


static int G_GNUC_PRINTF(2, 0)
virTypedParamSetNameVPrintf(virTypedParameterPtr par,
                            const char *fmt,
                            va_list ap)
{
    if (g_vsnprintf(par->field, VIR_TYPED_PARAM_FIELD_LENGTH, fmt, ap) > VIR_TYPED_PARAM_FIELD_LENGTH) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Field name too long"));
        return -1;
    }

    return 0;
}


static virTypedParameterPtr
virTypedParamListExtend(virTypedParamListPtr list)
{
    if (VIR_RESIZE_N(list->par, list->par_alloc, list->npar, 1) < 0)
        return NULL;

    list->npar++;

    return list->par + (list->npar - 1);
}


int
virTypedParamListAddInt(virTypedParamListPtr list,
                        int value,
                        const char *namefmt,
                        ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_INT, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}


int
virTypedParamListAddUInt(virTypedParamListPtr list,
                         unsigned int value,
                         const char *namefmt,
                         ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_UINT, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}


int
virTypedParamListAddLLong(virTypedParamListPtr list,
                          long long value,
                          const char *namefmt,
                          ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_LLONG, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}


int
virTypedParamListAddULLong(virTypedParamListPtr list,
                           unsigned long long value,
                           const char *namefmt,
                           ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_ULLONG, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}


int
virTypedParamListAddString(virTypedParamListPtr list,
                           const char *value,
                           const char *namefmt,
                           ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_STRING, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}


int
virTypedParamListAddBoolean(virTypedParamListPtr list,
                            bool value,
                            const char *namefmt,
                            ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_BOOLEAN, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}


int
virTypedParamListAddDouble(virTypedParamListPtr list,
                           double value,
                           const char *namefmt,
                           ...)
{
    virTypedParameterPtr par;
    va_list ap;
    int ret;

    if (!(par = virTypedParamListExtend(list)) ||
        virTypedParameterAssignValue(par, true, VIR_TYPED_PARAM_DOUBLE, value) < 0)
        return -1;

    va_start(ap, namefmt);
    ret = virTypedParamSetNameVPrintf(par, namefmt, ap);
    va_end(ap);

    return ret;
}
