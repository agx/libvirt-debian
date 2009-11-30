
/*
 * esx_vi.c: client for the VMware VI API 2.5 to manage ESX hosts
 *
 * Copyright (C) 2009 Matthias Bolte <matthias.bolte@googlemail.com>
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
 */

#include <config.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <libxml/parser.h>
#include <libxml/xpathInternals.h>

#include "buf.h"
#include "memory.h"
#include "logging.h"
#include "util.h"
#include "uuid.h"
#include "virterror_internal.h"
#include "xml.h"
#include "esx_vi.h"
#include "esx_vi_methods.h"
#include "esx_util.h"

#define VIR_FROM_THIS VIR_FROM_ESX

#define ESX_VI_ERROR(conn, code, fmt...)                                      \
    virReportErrorHelper(conn, VIR_FROM_ESX, code, __FILE__, __FUNCTION__,    \
                         __LINE__, fmt)

#define ESX_VI__SOAP__REQUEST_HEADER                                          \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                            \
    "<soapenv:Envelope "                                                      \
      "xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "          \
      "xmlns:soapenc=\"http://schemas.xmlsoap.org/soap/encoding/\" "          \
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "              \
      "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">"                       \
    "<soapenv:Body>"

#define ESX_VI__SOAP__REQUEST_FOOTER                                          \
    "</soapenv:Body>"                                                         \
    "</soapenv:Envelope>"

#define ESX_VI__SOAP__RESPONSE_XPATH(_type)                                   \
    ((char *)"/soapenv:Envelope/soapenv:Body/"                                \
               "vim:"_type"Response/vim:returnval")

#define ESV_VI__XML_TAG__OPEN(_buffer, _element, _type)                       \
    do {                                                                      \
        virBufferAddLit(_buffer, "<");                                        \
        virBufferAdd(_buffer, _element, -1);                                  \
        virBufferAddLit(_buffer, " xmlns=\"urn:vim25\" xsi:type=\"");         \
        virBufferAdd(_buffer, _type, -1);                                     \
        virBufferAddLit(_buffer, "\">");                                      \
    } while (0)

#define ESV_VI__XML_TAG__CLOSE(_buffer, _element)                             \
    do {                                                                      \
        virBufferAddLit(_buffer, "</");                                       \
        virBufferAdd(_buffer, _element, -1);                                  \
        virBufferAddLit(_buffer, ">");                                        \
    } while (0)

#define ESX_VI__TEMPLATE__ALLOC(_type)                                        \
    int                                                                       \
    esxVI_##_type##_Alloc(virConnectPtr conn, esxVI_##_type **ptrptr)         \
    {                                                                         \
        return esxVI_Alloc(conn, (void **)ptrptr, sizeof(esxVI_##_type));     \
    }

#define ESX_VI__TEMPLATE__FREE(_type, _body)                                  \
    void                                                                      \
    esxVI_##_type##_Free(esxVI_##_type **ptrptr)                              \
    {                                                                         \
        esxVI_##_type *item = NULL;                                           \
                                                                              \
        if (ptrptr == NULL || *ptrptr == NULL) {                              \
            return;                                                           \
        }                                                                     \
                                                                              \
        item = *ptrptr;                                                       \
                                                                              \
        _body                                                                 \
                                                                              \
        VIR_FREE(*ptrptr);                                                    \
    }



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Context
 */

/* esxVI_Context_Alloc */
ESX_VI__TEMPLATE__ALLOC(Context);

/* esxVI_Context_Free */
ESX_VI__TEMPLATE__FREE(Context,
{
    VIR_FREE(item->url);
    VIR_FREE(item->ipAddress);

    if (item->curl_handle != NULL) {
        curl_easy_cleanup(item->curl_handle);
    }

    if (item->curl_headers != NULL) {
        curl_slist_free_all(item->curl_headers);
    }

    virMutexDestroy(&item->curl_lock);

    VIR_FREE(item->username);
    VIR_FREE(item->password);
    esxVI_ServiceContent_Free(&item->service);
    esxVI_UserSession_Free(&item->session);
    esxVI_ManagedObjectReference_Free(&item->datacenter);
    esxVI_ManagedObjectReference_Free(&item->vmFolder);
    esxVI_ManagedObjectReference_Free(&item->hostFolder);
    esxVI_SelectionSpec_Free(&item->fullTraversalSpecList);
});

static size_t
esxVI_CURL_ReadString(char *data, size_t size, size_t nmemb, void *ptrptr)
{
    const char *content = *(const char **)ptrptr;
    size_t available = 0;
    size_t requested = size * nmemb;

    if (content == NULL) {
        return 0;
    }

    available = strlen(content);

    if (available == 0) {
        return 0;
    }

    if (requested > available) {
        requested = available;
    }

    memcpy(data, content, requested);

    *(const char **)ptrptr = content + requested;

    return requested;
}

static size_t
esxVI_CURL_WriteBuffer(char *data, size_t size, size_t nmemb, void *buffer)
{
    if (buffer != NULL) {
        virBufferAdd((virBufferPtr) buffer, data, size * nmemb);

        return size * nmemb;
    }

    return 0;
}

#define ESX_VI__CURL__ENABLE_DEBUG_OUTPUT 0

#if ESX_VI__CURL__ENABLE_DEBUG_OUTPUT
static int
esxVI_CURL_Debug(CURL *curl ATTRIBUTE_UNUSED, curl_infotype type,
                 char *info, size_t size, void *data ATTRIBUTE_UNUSED)
{
    switch (type) {
      case CURLINFO_TEXT:
        VIR_DEBUG0("CURLINFO_TEXT");
        fwrite(info, 1, size, stderr);
        printf("\n\n");
        break;

      case CURLINFO_HEADER_IN:
        VIR_DEBUG0("CURLINFO_HEADER_IN");
        break;

      case CURLINFO_HEADER_OUT:
        VIR_DEBUG0("CURLINFO_HEADER_OUT");
        break;

      case CURLINFO_DATA_IN:
        VIR_DEBUG0("CURLINFO_DATA_IN");
        break;

      case CURLINFO_DATA_OUT:
        VIR_DEBUG0("CURLINFO_DATA_OUT");
        break;

      default:
        VIR_DEBUG0("unknown");
        break;
    }

    return 0;
}
#endif

static int
esxVI_CURL_Perform(virConnectPtr conn, esxVI_Context *ctx, const char *url)
{
    CURLcode errorCode;
    long responseCode = 0;
#if LIBCURL_VERSION_NUM >= 0x071202 /* 7.18.2 */
    const char *redirectUrl = NULL;
#endif

    errorCode = curl_easy_perform(ctx->curl_handle);

    if (errorCode != CURLE_OK) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "curl_easy_perform() returned an error: %s (%d)",
                     curl_easy_strerror(errorCode), errorCode);
        return -1;
    }

    errorCode = curl_easy_getinfo(ctx->curl_handle, CURLINFO_RESPONSE_CODE,
                                  &responseCode);

    if (errorCode != CURLE_OK) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "curl_easy_getinfo(CURLINFO_RESPONSE_CODE) returned an "
                     "error: %s (%d)", curl_easy_strerror(errorCode),
                     errorCode);
        return -1;
    }

    if (responseCode < 0) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "curl_easy_getinfo(CURLINFO_RESPONSE_CODE) returned a "
                     "negative response code");
        return -1;
    }

    if (responseCode == 301) {
#if LIBCURL_VERSION_NUM >= 0x071202 /* 7.18.2 */
        errorCode = curl_easy_getinfo(ctx->curl_handle, CURLINFO_REDIRECT_URL,
                                      &redirectUrl);

        if (errorCode != CURLE_OK) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "curl_easy_getinfo(CURLINFO_REDIRECT_URL) returned "
                         "an error: %s (%d)", curl_easy_strerror(errorCode),
                         errorCode);
        } else {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "The server redirects from '%s' to '%s'", url,
                         redirectUrl);
        }
#else
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "The server redirects from '%s'", url);
#endif

        return -1;
    }

    return responseCode;
}

int
esxVI_Context_Connect(virConnectPtr conn, esxVI_Context *ctx, const char *url,
                      const char *ipAddress, const char *username,
                      const char *password, int noVerify)
{
    int result = 0;
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *datacenterList = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;

    if (ctx == NULL || url == NULL || ipAddress == NULL || username == NULL ||
        password == NULL || ctx->url != NULL || ctx->service != NULL ||
        ctx->curl_handle != NULL || ctx->curl_headers != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        goto failure;
    }

    if (esxVI_String_DeepCopyValue(conn, &ctx->url, url) < 0 ||
        esxVI_String_DeepCopyValue(conn, &ctx->ipAddress, ipAddress) < 0) {
        goto failure;
    }

    ctx->curl_handle = curl_easy_init();

    if (ctx->curl_handle == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not initialize CURL");
        goto failure;
    }

    ctx->curl_headers = curl_slist_append(ctx->curl_headers, "Content-Type: "
                                          "text/xml; charset=UTF-8");

    /*
     * Add a dummy expect header to stop CURL from waiting for a response code
     * 100 (Continue) from the server before continuing the POST operation.
     * Waiting for this response would slowdown each communication with the
     * server by approx. 2 sec, because the server doesn't send the expected
     * 100 (Continue) response and the wait times out resulting in wasting
     * approx. 2 sec per POST operation.
     */
    ctx->curl_headers = curl_slist_append(ctx->curl_headers,
                                          "Expect: nothing");

    if (ctx->curl_headers == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not build CURL header list");
        goto failure;
    }

    curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, ctx->url);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_USERAGENT, "libvirt-esx");
    curl_easy_setopt(ctx->curl_handle, CURLOPT_HEADER, 0);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_FOLLOWLOCATION, 0);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_SSL_VERIFYPEER, noVerify ? 0 : 1);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_SSL_VERIFYHOST, noVerify ? 0 : 2);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(ctx->curl_handle, CURLOPT_HTTPHEADER, ctx->curl_headers);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_READFUNCTION,
                     esxVI_CURL_ReadString);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION,
                     esxVI_CURL_WriteBuffer);
#if ESX_VI__CURL__ENABLE_DEBUG_OUTPUT
    curl_easy_setopt(ctx->curl_handle, CURLOPT_DEBUGFUNCTION,
                     esxVI_CURL_Debug);
#endif

    if (virMutexInit(&ctx->curl_lock) < 0) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not initialize CURL mutex");
        goto failure;
    }

    ctx->username = strdup(username);
    ctx->password = strdup(password);

    if (ctx->username == NULL || ctx->password == NULL) {
        virReportOOMError(conn);
        goto failure;
    }

    if (esxVI_RetrieveServiceContent(conn, ctx, &ctx->service) < 0) {
        goto failure;
    }

    if (STREQ(ctx->service->about->apiType, "HostAgent") ||
        STREQ(ctx->service->about->apiType, "VirtualCenter")) {
        if (STRPREFIX(ctx->service->about->apiVersion, "2.5")) {
            ctx->apiVersion = esxVI_APIVersion_25;
        } else if (STRPREFIX(ctx->service->about->apiVersion, "4.0")) {
            ctx->apiVersion = esxVI_APIVersion_40;
        } else {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Expecting VI API major/minor version '2.5' or '4.0' "
                         "but found '%s'", ctx->service->about->apiVersion);
            goto failure;
        }

        if (STREQ(ctx->service->about->productLineId, "gsx")) {
            if (STRPREFIX(ctx->service->about->version, "2.0")) {
                ctx->productVersion = esxVI_ProductVersion_GSX20;
            } else {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "Expecting GSX major/minor version '2.0' but "
                             "found '%s'", ctx->service->about->version);
                goto failure;
            }
        } else if (STREQ(ctx->service->about->productLineId, "esx") ||
                   STREQ(ctx->service->about->productLineId, "embeddedEsx")) {
            if (STRPREFIX(ctx->service->about->version, "3.5")) {
                ctx->productVersion = esxVI_ProductVersion_ESX35;
            } else if (STRPREFIX(ctx->service->about->version, "4.0")) {
                ctx->productVersion = esxVI_ProductVersion_ESX40;
            } else {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "Expecting ESX major/minor version '3.5' or "
                             "'4.0' but found '%s'",
                             ctx->service->about->version);
                goto failure;
            }
        } else if (STREQ(ctx->service->about->productLineId, "vpx")) {
            if (STRPREFIX(ctx->service->about->version, "2.5")) {
                ctx->productVersion = esxVI_ProductVersion_VPX25;
            } else if (STRPREFIX(ctx->service->about->version, "4.0")) {
                ctx->productVersion = esxVI_ProductVersion_VPX40;
            } else {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "Expecting VPX major/minor version '2.5' or '4.0' "
                             "but found '%s'", ctx->service->about->version);
                goto failure;
            }
        } else {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Expecting product 'gsx' or 'esx' or 'embeddedEsx' "
                         "or 'vpx' but found '%s'",
                         ctx->service->about->productLineId);
            goto failure;
        }
    } else {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Expecting VI API type 'HostAgent' or 'VirtualCenter' "
                     "but found '%s'", ctx->service->about->apiType);
        goto failure;
    }

    if (esxVI_Login(conn, ctx, username, password, &ctx->session) < 0) {
        goto failure;
    }

    esxVI_BuildFullTraversalSpecList(conn, &ctx->fullTraversalSpecList);

    if (esxVI_String_AppendValueListToList(conn, &propertyNameList,
                                           "vmFolder\0"
                                           "hostFolder\0") < 0) {
        goto failure;
    }

    /* Get pointer to Datacenter for later use */
    if (esxVI_LookupObjectContentByType(conn, ctx, ctx->service->rootFolder,
                                        "Datacenter", propertyNameList,
                                        esxVI_Boolean_True,
                                        &datacenterList) < 0) {
        goto failure;
    }

    if (datacenterList == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not retrieve the 'datacenter' object from the VI "
                     "host/center");
        goto failure;
    }

    ctx->datacenter = datacenterList->obj;
    datacenterList->obj = NULL;

    /* Get pointer to vmFolder and hostFolder for later use */
    for (dynamicProperty = datacenterList->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "vmFolder")) {
            if (esxVI_ManagedObjectReference_CastFromAnyType
                  (conn, dynamicProperty->val, &ctx->vmFolder, "Folder")) {
                goto failure;
            }
        } else if (STREQ(dynamicProperty->name, "hostFolder")) {
            if (esxVI_ManagedObjectReference_CastFromAnyType
                  (conn, dynamicProperty->val, &ctx->hostFolder, "Folder")) {
                goto failure;
            }
        } else {
            VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
        }
    }

    if (ctx->vmFolder == NULL || ctx->hostFolder == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "The 'datacenter' object is missing the "
                     "'vmFolder'/'hostFolder' property");
        goto failure;
    }

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datacenterList);

    return result;

  failure:
    result = -1;

    goto cleanup;
}

int
esxVI_Context_DownloadFile(virConnectPtr conn, esxVI_Context *ctx,
                           const char *url, char **content)
{
    virBuffer buffer = VIR_BUFFER_INITIALIZER;
    int responseCode = 0;

    if (content == NULL || *content != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        goto failure;
    }

    virMutexLock(&ctx->curl_lock);

    curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_UPLOAD, 0);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_HTTPGET, 1);

    responseCode = esxVI_CURL_Perform(conn, ctx, url);

    virMutexUnlock(&ctx->curl_lock);

    if (responseCode < 0) {
        goto failure;
    } else if (responseCode != 200) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "HTTP response code %d while trying to download '%s'",
                     responseCode, url);
        goto failure;
    }

    if (virBufferError(&buffer)) {
        virReportOOMError(conn);
        goto failure;
    }

    *content = virBufferContentAndReset(&buffer);

    return 0;

  failure:
    free(virBufferContentAndReset(&buffer));

    return -1;
}

int
esxVI_Context_UploadFile(virConnectPtr conn, esxVI_Context *ctx,
                         const char *url, const char *content)
{
    int responseCode = 0;

    if (content == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    virMutexLock(&ctx->curl_lock);

    curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_READDATA, &content);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_UPLOAD, 1);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_INFILESIZE, strlen(content));

    responseCode = esxVI_CURL_Perform(conn, ctx, url);

    virMutexUnlock(&ctx->curl_lock);

    if (responseCode < 0) {
        return -1;
    } else if (responseCode != 200 && responseCode != 201) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "HTTP response code %d while trying to upload to '%s'",
                     responseCode, url);
        return -1;
    }

    return 0;
}

int
esxVI_Context_Execute(virConnectPtr conn, esxVI_Context *ctx,
                      const char *request, const char *xpathExpression,
                      esxVI_Response **response, esxVI_Boolean expectList)
{
    virBuffer buffer = VIR_BUFFER_INITIALIZER;
    esxVI_Fault *fault = NULL;

    if (request == NULL || response == NULL || *response != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        goto failure;
    }

    if (esxVI_Response_Alloc(conn, response) < 0) {
        goto failure;
    }

    virMutexLock(&ctx->curl_lock);

    curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, ctx->url);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_UPLOAD, 0);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDS, request);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDSIZE, strlen(request));

    (*response)->responseCode = esxVI_CURL_Perform(conn, ctx, ctx->url);

    virMutexUnlock(&ctx->curl_lock);

    if ((*response)->responseCode < 0) {
        goto failure;
    }

    if (virBufferError(&buffer)) {
        virReportOOMError(conn);
        goto failure;
    }

    (*response)->content = virBufferContentAndReset(&buffer);

    if ((*response)->responseCode == 500 ||
        (xpathExpression != NULL && (*response)->responseCode == 200)) {
        (*response)->document = xmlReadDoc(BAD_CAST (*response)->content, "",
                                           NULL, XML_PARSE_NONET);

        if ((*response)->document == NULL) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not parse XML response");
            goto failure;
        }

        if (xmlDocGetRootElement((*response)->document) == NULL) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "XML response is an empty document");
            goto failure;
        }

        (*response)->xpathContext = xmlXPathNewContext((*response)->document);

        if ((*response)->xpathContext == NULL) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not create XPath context");
            goto failure;
        }

        xmlXPathRegisterNs((*response)->xpathContext, BAD_CAST "soapenv",
                           BAD_CAST "http://schemas.xmlsoap.org/soap/envelope/");
        xmlXPathRegisterNs((*response)->xpathContext, BAD_CAST "vim",
                           BAD_CAST "urn:vim25");

        if ((*response)->responseCode == 500) {
            (*response)->node =
              virXPathNode(conn, "/soapenv:Envelope/soapenv:Body/soapenv:Fault",
                           (*response)->xpathContext);

            if ((*response)->node == NULL) {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "HTTP response code %d. VI Fault is unknown, "
                             "XPath evaluation failed",
                             (int)(*response)->responseCode);
                goto failure;
            }

            if (esxVI_Fault_Deserialize(conn, (*response)->node, &fault) < 0) {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "HTTP response code %d. VI Fault is unknown, "
                             "deserialization failed",
                             (int)(*response)->responseCode);
                goto failure;
            }

            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "HTTP response code %d. VI Fault: %s - %s",
                         (int)(*response)->responseCode,
                         fault->faultcode, fault->faultstring);

            goto failure;
        } else if (expectList == esxVI_Boolean_True) {
            xmlNodePtr *nodeSet = NULL;
            int nodeSet_size;

            nodeSet_size = virXPathNodeSet(conn, xpathExpression,
                                           (*response)->xpathContext,
                                           &nodeSet);

            if (nodeSet_size < 0) {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "XPath evaluation of '%s' failed",
                             xpathExpression);
                goto failure;
            } else if (nodeSet_size == 0) {
                (*response)->node = NULL;
            } else {
                (*response)->node = nodeSet[0];
            }

            VIR_FREE(nodeSet);
        } else {
            (*response)->node = virXPathNode(conn, xpathExpression,
                                             (*response)->xpathContext);

            if ((*response)->node == NULL) {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "XPath evaluation of '%s' failed",
                             xpathExpression);
                goto failure;
            }
        }
    } else if ((*response)->responseCode != 200) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "HTTP response code %d", (*response)->responseCode);
        goto failure;
    }

    return 0;

  failure:
    free(virBufferContentAndReset(&buffer));
    esxVI_Response_Free(response);
    esxVI_Fault_Free(&fault);

    return -1;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Response
 */

/* esxVI_Response_Alloc */
ESX_VI__TEMPLATE__ALLOC(Response);

/* esxVI_Response_Free */
ESX_VI__TEMPLATE__FREE(Response,
{
    VIR_FREE(item->content);

    xmlXPathFreeContext(item->xpathContext);

    if (item->document != NULL) {
        xmlFreeDoc(item->document);
    }
});



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Enumeration
 */

int
esxVI_Enumeration_CastFromAnyType(virConnectPtr conn,
                                  const esxVI_Enumeration *enumeration,
                                  esxVI_AnyType *anyType, int *value)
{
    int i;

    if (anyType == NULL || value == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    *value = 0; /* undefined */

    if (STRNEQ(anyType->other, enumeration->type)) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Expecting type '%s' but found '%s'", enumeration->type,
                     anyType->other);
        return -1;
    }

    for (i = 0; enumeration->values[i].name != NULL; ++i) {
        if (STREQ(anyType->value, enumeration->values[i].name)) {
            *value = enumeration->values[i].value;
            return 0;
        }
    }

    ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                 "Unknown value '%s' for %s", anyType->value,
                 enumeration->type);

    return -1;
}

int
esxVI_Enumeration_Serialize(virConnectPtr conn,
                            const esxVI_Enumeration *enumeration,
                            int value, const char *element,
                            virBufferPtr output, esxVI_Boolean required)
{
    int i;
    const char *name = NULL;

    if (element == NULL || output == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (value == 0) { /* undefined */
        return esxVI_CheckSerializationNecessity(conn, element, required);
    }

    for (i = 0; enumeration->values[i].name != NULL; ++i) {
        if (value == enumeration->values[i].value) {
            name = enumeration->values[i].name;
            break;
        }
    }

    if (name == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    ESV_VI__XML_TAG__OPEN(output, element, enumeration->type);

    virBufferAdd(output, name, -1);

    ESV_VI__XML_TAG__CLOSE(output, element);

    return 0;
}

int
esxVI_Enumeration_Deserialize(virConnectPtr conn,
                              const esxVI_Enumeration *enumeration,
                              xmlNodePtr node, int *value)
{
    int i;
    int result = 0;
    char *name = NULL;

    if (value == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        goto failure;
    }

    *value = 0; /* undefined */

    if (esxVI_String_DeserializeValue(conn, node, &name) < 0) {
        goto failure;
    }

    for (i = 0; enumeration->values[i].name != NULL; ++i) {
        if (STREQ(name, enumeration->values[i].name)) {
            *value = enumeration->values[i].value;
            goto cleanup;
        }
    }

    ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Unknown value '%s' for %s",
                 name, enumeration->type);

  cleanup:
    VIR_FREE(name);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * List
 */

int
esxVI_List_Append(virConnectPtr conn, esxVI_List **list, esxVI_List *item)
{
    esxVI_List *next = NULL;

    if (list == NULL || item == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (*list == NULL) {
        *list = item;
        return 0;
    }

    next = *list;

    while (next->_next != NULL) {
        next = next->_next;
    }

    next->_next = item;

    return 0;
}

int
esxVI_List_DeepCopy(virConnectPtr conn, esxVI_List **destList,
                    esxVI_List *srcList,
                    esxVI_List_DeepCopyFunc deepCopyFunc,
                    esxVI_List_FreeFunc freeFunc)
{
    esxVI_List *dest = NULL;
    esxVI_List *src = NULL;

    if (destList == NULL || *destList != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        goto failure;
    }

    for (src = srcList; src != NULL; src = src->_next) {
        if (deepCopyFunc(conn, &dest, src) < 0 ||
            esxVI_List_Append(conn, destList, dest) < 0) {
            goto failure;
        }

        dest = NULL;
    }

    return 0;

  failure:
    freeFunc(&dest);
    freeFunc(destList);

    return -1;
}

int
esxVI_List_CastFromAnyType(virConnectPtr conn, esxVI_AnyType *anyType,
                           esxVI_List **list,
                           esxVI_List_CastFromAnyTypeFunc castFromAnyTypeFunc,
                           esxVI_List_FreeFunc freeFunc)
{
    int result = 0;
    xmlNodePtr childNode = NULL;
    esxVI_AnyType *childAnyType = NULL;
    esxVI_List *item = NULL;

    if (list == NULL || *list != NULL ||
        castFromAnyTypeFunc == NULL || freeFunc == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        goto failure;
    }

    if (anyType == NULL) {
        return 0;
    }

    if (! STRPREFIX(anyType->other, "ArrayOf")) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Expecting type to begin with 'ArrayOf' but found '%s'",
                     anyType->other);
        goto failure;
    }

    for (childNode = anyType->_node->xmlChildrenNode; childNode != NULL;
         childNode = childNode->next) {
        if (childNode->type != XML_ELEMENT_NODE) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Wrong XML element type %d", childNode->type);
            goto failure;
        }

        esxVI_AnyType_Free(&childAnyType);

        if (esxVI_AnyType_Deserialize(conn, childNode, &childAnyType) < 0 ||
            castFromAnyTypeFunc(conn, childAnyType, &item) < 0 ||
            esxVI_List_Append(conn, list, item) < 0) {
            goto failure;
        }

        item = NULL;
    }

  cleanup:
    esxVI_AnyType_Free(&childAnyType);

    return result;

  failure:
    freeFunc(&item);
    freeFunc(list);

    result = -1;

    goto cleanup;
}


int
esxVI_List_Serialize(virConnectPtr conn, esxVI_List *list, const char *element,
                     virBufferPtr output, esxVI_Boolean required,
                     esxVI_List_SerializeFunc serializeFunc)
{
    esxVI_List *item = NULL;

    if (element == NULL || output == NULL || serializeFunc == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (list == NULL) {
        return esxVI_CheckSerializationNecessity(conn, element, required);
    }

    for (item = list; item != NULL; item = item->_next) {
        if (serializeFunc(conn, item, element, output,
                          esxVI_Boolean_True) < 0) {
            return -1;
        }
    }

    return 0;
}

int
esxVI_List_Deserialize(virConnectPtr conn, xmlNodePtr node, esxVI_List **list,
                       esxVI_List_DeserializeFunc deserializeFunc,
                       esxVI_List_FreeFunc freeFunc)
{
    esxVI_List *item = NULL;

    if (list == NULL || *list != NULL ||
        deserializeFunc == NULL || freeFunc == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (node == NULL) {
        return 0;
    }

    for (; node != NULL; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Wrong XML element type %d", node->type);
            goto failure;
        }

        if (deserializeFunc(conn, node, &item) < 0 ||
            esxVI_List_Append(conn, list, item) < 0) {
            goto failure;
        }

        item = NULL;
    }

    return 0;

  failure:
    freeFunc(&item);
    freeFunc(list);

    return -1;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Utility and Convenience Functions
 *
 * Function naming scheme:
 *  - 'lookup' functions query the ESX or vCenter for information
 *  - 'get' functions get information from a local object
 */

int
esxVI_Alloc(virConnectPtr conn, void **ptrptr, size_t size)
{
    if (ptrptr == NULL || *ptrptr != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (virAllocN(ptrptr, size, 1) < 0) {
        virReportOOMError(conn);
        return -1;
    }

    return 0;
}

int
esxVI_CheckSerializationNecessity(virConnectPtr conn, const char *element,
                                  esxVI_Boolean required)
{
    if (element == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (required == esxVI_Boolean_True) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Required property missing while trying to serialize "
                     "'%s'", element);
        return -1;
    } else {
        return 0;
    }
}



int
esxVI_BuildFullTraversalSpecItem(virConnectPtr conn,
                                 esxVI_SelectionSpec **fullTraversalSpecList,
                                 const char *name, const char *type,
                                 const char *path, const char *selectSetNames)
{
    esxVI_TraversalSpec *traversalSpec = NULL;
    esxVI_SelectionSpec *selectionSpec = NULL;
    const char *currentSelectSetName = NULL;

    if (fullTraversalSpecList == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (esxVI_TraversalSpec_Alloc(conn, &traversalSpec) < 0 ||
        esxVI_String_DeepCopyValue(conn, &traversalSpec->_base->name,
                                   name) < 0 ||
        esxVI_String_DeepCopyValue(conn, &traversalSpec->type, type) < 0 ||
        esxVI_String_DeepCopyValue(conn, &traversalSpec->path, path) < 0) {
        goto failure;
    }

    traversalSpec->skip = esxVI_Boolean_False;

    if (selectSetNames != NULL) {
        currentSelectSetName = selectSetNames;

        while (currentSelectSetName != NULL && *currentSelectSetName != '\0') {
            selectionSpec = NULL;

            if (esxVI_SelectionSpec_Alloc(conn, &selectionSpec) < 0 ||
                esxVI_String_DeepCopyValue(conn, &selectionSpec->name,
                                           currentSelectSetName) < 0 ||
                esxVI_SelectionSpec_AppendToList(conn,
                                                 &traversalSpec->selectSet,
                                                 selectionSpec) < 0) {
                goto failure;
            }

            currentSelectSetName += strlen(currentSelectSetName) + 1;
        }
    }

    if (esxVI_SelectionSpec_AppendToList(conn, fullTraversalSpecList,
                                         traversalSpec->_base) < 0) {
        goto failure;
    }

    return 0;

  failure:
    esxVI_TraversalSpec_Free(&traversalSpec);

    return -1;
}



int
esxVI_BuildFullTraversalSpecList(virConnectPtr conn,
                                 esxVI_SelectionSpec **fullTraversalSpecList)
{
    if (fullTraversalSpecList == NULL || *fullTraversalSpecList != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "visitFolders",
                                         "Folder", "childEntity",
                                         "visitFolders\0"
                                         "datacenterToDatastore\0"
                                         "datacenterToVmFolder\0"
                                         "datacenterToHostFolder\0"
                                         "computeResourceToHost\0"
                                         "computeResourceToResourcePool\0"
                                         "hostSystemToVm\0"
                                         "resourcePoolToVm\0") < 0) {
        goto failure;
    }

    /* Traversal through datastore branch */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "datacenterToDatastore",
                                         "Datacenter", "datastore",
                                         NULL) < 0) {
        goto failure;
    }

    /* Traversal through vmFolder branch */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "datacenterToVmFolder",
                                         "Datacenter", "vmFolder",
                                         "visitFolders\0") < 0) {
        goto failure;
    }

    /* Traversal through hostFolder branch  */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "datacenterToHostFolder",
                                         "Datacenter", "hostFolder",
                                         "visitFolders\0") < 0) {
        goto failure;
    }

    /* Traversal through host branch  */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "computeResourceToHost",
                                         "ComputeResource", "host",
                                         NULL) < 0) {
        goto failure;
    }

    /* Traversal through resourcePool branch */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "computeResourceToResourcePool",
                                         "ComputeResource", "resourcePool",
                                         "resourcePoolToResourcePool\0"
                                         "resourcePoolToVm\0") < 0) {
        goto failure;
    }

    /* Recurse through all resource pools */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "resourcePoolToResourcePool",
                                         "ResourcePool", "resourcePool",
                                         "resourcePoolToResourcePool\0"
                                         "resourcePoolToVm\0") < 0) {
        goto failure;
    }

    /* Recurse through all hosts */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "hostSystemToVm",
                                         "HostSystem", "vm",
                                         "visitFolders\0") < 0) {
        goto failure;
    }

    /* Recurse through all resource pools */
    if (esxVI_BuildFullTraversalSpecItem(conn, fullTraversalSpecList,
                                         "resourcePoolToVm",
                                         "ResourcePool", "vm", NULL) < 0) {
        goto failure;
    }

    return 0;

  failure:
    esxVI_SelectionSpec_Free(fullTraversalSpecList);

    return -1;
}



/*
 * Can't use the SessionIsActive() function here, because at least
 * 'ESX Server 3.5.0 build-64607' returns an 'method not implemented' fault if
 * you try to call it. Query the session manager for the current session of
 * this connection instead and re-login if there is no current session for this
 * connection.
 */
#define ESX_VI_USE_SESSION_IS_ACTIVE 0

int
esxVI_EnsureSession(virConnectPtr conn, esxVI_Context *ctx)
{
    int result = 0;
#if ESX_VI_USE_SESSION_IS_ACTIVE
    esxVI_Boolean active = esxVI_Boolean_Undefined;
#else
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *sessionManager = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_UserSession *currentSession = NULL;
#endif

    if (ctx->session == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid call");
        return -1;
    }

#if ESX_VI_USE_SESSION_IS_ACTIVE
    if (esxVI_SessionIsActive(conn, ctx, ctx->session->key,
                              ctx->session->userName, &active) < 0) {
        return -1;
    }

    if (active != esxVI_Boolean_True) {
        esxVI_UserSession_Free(&ctx->session);

        if (esxVI_Login(conn, ctx, ctx->username, ctx->password,
                        &ctx->session) < 0) {
            return -1;
        }
    }
#else
    if (esxVI_String_AppendValueToList(conn, &propertyNameList,
                                       "currentSession") < 0 ||
        esxVI_LookupObjectContentByType(conn, ctx,
                                        ctx->service->sessionManager,
                                        "SessionManager", propertyNameList,
                                        esxVI_Boolean_False,
                                        &sessionManager) < 0) {
        goto failure;
    }

    for (dynamicProperty = sessionManager->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "currentSession")) {
            if (esxVI_UserSession_CastFromAnyType(conn, dynamicProperty->val,
                                                  &currentSession) < 0) {
                goto failure;
            }

            break;
        } else {
            VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
        }
    }

    if (currentSession == NULL) {
        esxVI_UserSession_Free(&ctx->session);

        if (esxVI_Login(conn, ctx, ctx->username, ctx->password,
                        &ctx->session) < 0) {
            goto failure;
        }
    } else if (STRNEQ(ctx->session->key, currentSession->key)) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Key of the current session differs from the key at "
                     "last login");
        goto failure;
    }
#endif

  cleanup:
#if ESX_VI_USE_SESSION_IS_ACTIVE
    /* nothing */
#else
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&sessionManager);
    esxVI_UserSession_Free(&currentSession);
#endif

    return result;

  failure:
    result = -1;

    goto cleanup;

    return 0;
}



int
esxVI_LookupObjectContentByType(virConnectPtr conn, esxVI_Context *ctx,
                                esxVI_ManagedObjectReference *root,
                                const char *type,
                                esxVI_String *propertyNameList,
                                esxVI_Boolean recurse,
                                esxVI_ObjectContent **objectContentList)
{
    int result = 0;
    esxVI_ObjectSpec *objectSpec = NULL;
    esxVI_PropertySpec *propertySpec = NULL;
    esxVI_PropertyFilterSpec *propertyFilterSpec = NULL;

    if (ctx->fullTraversalSpecList == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid call");
        return -1;
    }

    if (esxVI_ObjectSpec_Alloc(conn, &objectSpec) < 0) {
        goto failure;
    }

    objectSpec->obj = root;
    objectSpec->skip = esxVI_Boolean_False;

    if (recurse == esxVI_Boolean_True) {
        objectSpec->selectSet = ctx->fullTraversalSpecList;
    }

    if (esxVI_PropertySpec_Alloc(conn, &propertySpec) < 0) {
        goto failure;
    }

    propertySpec->type = (char *)type;
    propertySpec->pathSet = propertyNameList;

    if (esxVI_PropertyFilterSpec_Alloc(conn, &propertyFilterSpec) < 0 ||
        esxVI_PropertySpec_AppendToList(conn, &propertyFilterSpec->propSet,
                                        propertySpec) < 0 ||
        esxVI_ObjectSpec_AppendToList(conn, &propertyFilterSpec->objectSet,
                                      objectSpec) < 0) {
        goto failure;
    }

    result = esxVI_RetrieveProperties(conn, ctx, propertyFilterSpec,
                                      objectContentList);

  cleanup:
    /*
     * Remove values given by the caller from the data structures to prevent
     * them from being freed by the call to esxVI_PropertyFilterSpec_Free().
     */
    if (objectSpec != NULL) {
        objectSpec->obj = NULL;
        objectSpec->selectSet = NULL;
    }

    if (propertySpec != NULL) {
        propertySpec->type = NULL;
        propertySpec->pathSet = NULL;
    }

    esxVI_PropertyFilterSpec_Free(&propertyFilterSpec);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



int
esxVI_GetManagedEntityStatus(virConnectPtr conn,
                             esxVI_ObjectContent *objectContent,
                             const char *propertyName,
                             esxVI_ManagedEntityStatus *managedEntityStatus)
{
    esxVI_DynamicProperty *dynamicProperty;

    for (dynamicProperty = objectContent->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, propertyName)) {
            return esxVI_ManagedEntityStatus_CastFromAnyType
                     (conn, dynamicProperty->val, managedEntityStatus);
        }
    }

    ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                 "Missing '%s' property while looking for ManagedEntityStatus",
                 propertyName);

    return -1;
}



int
esxVI_GetVirtualMachinePowerState(virConnectPtr conn,
                                  esxVI_ObjectContent *virtualMachine,
                                  esxVI_VirtualMachinePowerState *powerState)
{
    esxVI_DynamicProperty *dynamicProperty;

    for (dynamicProperty = virtualMachine->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "runtime.powerState")) {
            return esxVI_VirtualMachinePowerState_CastFromAnyType
                     (conn, dynamicProperty->val, powerState);
        }
    }

    ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                 "Missing 'runtime.powerState' property");

    return -1;
}



int
esxVI_LookupNumberOfDomainsByPowerState(virConnectPtr conn, esxVI_Context *ctx,
                                        esxVI_VirtualMachinePowerState powerState,
                                        esxVI_Boolean inverse)
{
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *virtualMachineList = NULL;
    esxVI_ObjectContent *virtualMachine = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_VirtualMachinePowerState powerState_;
    int numberOfDomains = 0;

    if (esxVI_String_AppendValueToList(conn, &propertyNameList,
                                       "runtime.powerState") < 0 ||
        esxVI_LookupObjectContentByType(conn, ctx, ctx->vmFolder,
                                        "VirtualMachine", propertyNameList,
                                        esxVI_Boolean_True,
                                        &virtualMachineList) < 0) {
        goto failure;
    }

    for (virtualMachine = virtualMachineList; virtualMachine != NULL;
         virtualMachine = virtualMachine->_next) {
        for (dynamicProperty = virtualMachine->propSet;
             dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "runtime.powerState")) {
                if (esxVI_VirtualMachinePowerState_CastFromAnyType
                      (conn, dynamicProperty->val, &powerState_) < 0) {
                    goto failure;
                }

                if ((inverse != esxVI_Boolean_True &&
                     powerState_ == powerState) ||
                    (inverse == esxVI_Boolean_True &&
                     powerState_ != powerState)) {
                    numberOfDomains++;
                }
            } else {
                VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
            }
        }
    }

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&virtualMachineList);

    return numberOfDomains;

  failure:
    numberOfDomains = -1;

    goto cleanup;
}



int
esxVI_GetVirtualMachineIdentity(virConnectPtr conn,
                                esxVI_ObjectContent *virtualMachine,
                                int *id, char **name, unsigned char *uuid)
{
    const char *uuid_string = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_ManagedEntityStatus configStatus = esxVI_ManagedEntityStatus_Undefined;

    if (STRNEQ(virtualMachine->obj->type, "VirtualMachine")) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "ObjectContent does not reference a virtual machine");
        return -1;
    }

    if (id != NULL) {
        if (esxUtil_ParseVirtualMachineIDString
              (virtualMachine->obj->value, id) < 0 || *id <= 0) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not parse positive integer from '%s'",
                         virtualMachine->obj->value);
            goto failure;
        }
    }

    if (name != NULL) {
        if (*name != NULL) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
            goto failure;
        }

        for (dynamicProperty = virtualMachine->propSet;
             dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "name")) {
                if (esxVI_AnyType_ExpectType(conn, dynamicProperty->val,
                                             esxVI_Type_String) < 0) {
                    goto failure;
                }

                *name = strdup(dynamicProperty->val->string);

                if (*name == NULL) {
                    virReportOOMError(conn);
                    goto failure;
                }

                break;
            }
        }

        if (*name == NULL) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not get name of virtual machine");
            goto failure;
        }
    }

    if (uuid != NULL) {
        if (esxVI_GetManagedEntityStatus(conn, virtualMachine, "configStatus",
                                         &configStatus) < 0) {
            goto failure;
        }

        if (configStatus == esxVI_ManagedEntityStatus_Green) {
            for (dynamicProperty = virtualMachine->propSet;
                 dynamicProperty != NULL;
                 dynamicProperty = dynamicProperty->_next) {
                if (STREQ(dynamicProperty->name, "config.uuid")) {
                    if (esxVI_AnyType_ExpectType(conn, dynamicProperty->val,
                                                 esxVI_Type_String) < 0) {
                        goto failure;
                    }

                    uuid_string = dynamicProperty->val->string;
                    break;
                }
            }

            if (uuid_string == NULL) {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "Could not get UUID of virtual machine");
                goto failure;
            }

            if (virUUIDParse(uuid_string, uuid) < 0) {
                ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                             "Could not parse UUID from string '%s'",
                             uuid_string);
                goto failure;
            }
        } else {
            memset(uuid, 0, VIR_UUID_BUFLEN);

            VIR_WARN0("Cannot access UUID, because 'configStatus' property "
                      "indicates a config problem");
        }
    }

    return 0;

  failure:
    if (name != NULL) {
        VIR_FREE(*name);
    }

    return -1;
}



int
esxVI_LookupResourcePoolByHostSystem
  (virConnectPtr conn, esxVI_Context *ctx, esxVI_ObjectContent *hostSystem,
   esxVI_ManagedObjectReference **resourcePool)
{
    int result = 0;
    esxVI_String *propertyNameList = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_ManagedObjectReference *managedObjectReference = NULL;
    esxVI_ObjectContent *computeResource = NULL;

    if (resourcePool == NULL || *resourcePool != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    for (dynamicProperty = hostSystem->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "parent")) {
            if (esxVI_ManagedObjectReference_CastFromAnyType
                  (conn, dynamicProperty->val, &managedObjectReference,
                   "ComputeResource") < 0) {
                goto failure;
            }

            break;
        } else {
            VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
        }
    }

    if (managedObjectReference == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not retrieve compute resource of host system");
        goto failure;
    }

    if (esxVI_String_AppendValueToList(conn, &propertyNameList,
                                       "resourcePool") < 0 ||
        esxVI_LookupObjectContentByType(conn, ctx, managedObjectReference,
                                        "ComputeResource", propertyNameList,
                                        esxVI_Boolean_False,
                                        &computeResource) < 0) {
        goto failure;
    }

    if (computeResource == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not retrieve compute resource of host system");
        goto failure;
    }

    for (dynamicProperty = computeResource->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "resourcePool")) {
            if (esxVI_ManagedObjectReference_CastFromAnyType
                  (conn, dynamicProperty->val, resourcePool,
                   "ResourcePool") < 0) {
                goto failure;
            }

            break;
        } else {
            VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
        }
    }

    if ((*resourcePool) == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                     "Could not retrieve resource pool of compute resource");
        goto failure;
    }

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ManagedObjectReference_Free(&managedObjectReference);
    esxVI_ObjectContent_Free(&computeResource);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



int
esxVI_LookupHostSystemByIp(virConnectPtr conn, esxVI_Context *ctx,
                           const char *ipAddress,
                           esxVI_String *propertyNameList,
                           esxVI_ObjectContent **hostSystem)
{
    int result = 0;
    esxVI_ManagedObjectReference *managedObjectReference = NULL;

    if (hostSystem == NULL || *hostSystem != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (esxVI_FindByIp(conn, ctx, ctx->datacenter, ipAddress,
                       esxVI_Boolean_False, &managedObjectReference) < 0) {
        goto failure;
    }

    if (esxVI_LookupObjectContentByType(conn, ctx, managedObjectReference,
                                        "HostSystem", propertyNameList,
                                        esxVI_Boolean_False, hostSystem) < 0) {
        goto failure;
    }

  cleanup:
    esxVI_ManagedObjectReference_Free(&managedObjectReference);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



int
esxVI_LookupVirtualMachineByUuid(virConnectPtr conn, esxVI_Context *ctx,
                                 const unsigned char *uuid,
                                 esxVI_String *propertyNameList,
                                 esxVI_ObjectContent **virtualMachine,
                                 esxVI_Occurence occurence)
{
    int result = 0;
    esxVI_ManagedObjectReference *managedObjectReference = NULL;
    char uuid_string[VIR_UUID_STRING_BUFLEN] = "";

    if (virtualMachine == NULL || *virtualMachine != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    if (esxVI_FindByUuid(conn, ctx, ctx->datacenter, uuid, esxVI_Boolean_True,
                         &managedObjectReference) < 0) {
        goto failure;
    }

    if (managedObjectReference == NULL) {
        if (occurence == esxVI_Occurence_OptionalItem) {
            return 0;
        } else {
            virUUIDFormat(uuid, uuid_string);

            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not find domain with UUID '%s'", uuid_string);
            goto failure;
        }
    }

    if (esxVI_LookupObjectContentByType(conn, ctx, managedObjectReference,
                                        "VirtualMachine", propertyNameList,
                                        esxVI_Boolean_False,
                                        virtualMachine) < 0) {
        goto failure;
    }

  cleanup:
    esxVI_ManagedObjectReference_Free(&managedObjectReference);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



int
esxVI_LookupDatastoreByName(virConnectPtr conn, esxVI_Context *ctx,
                            const char *name, esxVI_String *propertyNameList,
                            esxVI_ObjectContent **datastore,
                            esxVI_Occurence occurence)
{
    int result = 0;
    esxVI_String *completePropertyNameList = NULL;
    esxVI_ObjectContent *datastoreList = NULL;
    esxVI_ObjectContent *candidate = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_Boolean accessible = esxVI_Boolean_Undefined;
    size_t offset = strlen("/vmfs/volumes/");
    int numInaccessibleDatastores = 0;

    if (datastore == NULL || *datastore != NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    /* Get all datastores */
    if (esxVI_String_DeepCopyList(conn, &completePropertyNameList,
                                  propertyNameList) < 0 ||
        esxVI_String_AppendValueListToList(conn, &completePropertyNameList,
                                           "summary.accessible\0"
                                           "summary.name\0"
                                           "summary.url\0") < 0) {
        goto failure;
    }

    if (esxVI_LookupObjectContentByType(conn, ctx, ctx->datacenter,
                                        "Datastore", completePropertyNameList,
                                        esxVI_Boolean_True,
                                        &datastoreList) < 0) {
        goto failure;
    }

    if (datastoreList == NULL) {
        if (occurence == esxVI_Occurence_OptionalItem) {
            goto cleanup;
        } else {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "No datastores available");
            goto failure;
        }
    }

    /* Search for a matching datastore */
    for (candidate = datastoreList; candidate != NULL;
         candidate = candidate->_next) {
        accessible = esxVI_Boolean_Undefined;

        for (dynamicProperty = candidate->propSet; dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "summary.accessible")) {
                if (esxVI_AnyType_ExpectType(conn, dynamicProperty->val,
                                             esxVI_Type_Boolean) < 0) {
                    goto failure;
                }

                accessible = dynamicProperty->val->boolean;
                break;
            }
        }

        if (accessible == esxVI_Boolean_Undefined) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Got incomplete response while querying for the "
                         "datastore 'summary.accessible' property");
            goto failure;
        }

        if (accessible == esxVI_Boolean_False) {
            ++numInaccessibleDatastores;
        }

        for (dynamicProperty = candidate->propSet; dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "summary.accessible")) {
                /* Ignore it */
            } else if (STREQ(dynamicProperty->name, "summary.name")) {
                if (esxVI_AnyType_ExpectType(conn, dynamicProperty->val,
                                             esxVI_Type_String) < 0) {
                    goto failure;
                }

                if (STREQ(dynamicProperty->val->string, name)) {
                    if (esxVI_ObjectContent_DeepCopy(conn, datastore,
                                                     candidate) < 0) {
                        goto failure;
                    }

                    /* Found datastore with matching name */
                    goto cleanup;
                }
            } else if (STREQ(dynamicProperty->name, "summary.url")) {
                if (accessible == esxVI_Boolean_False) {
                    /*
                     * The 'summary.url' property of an inaccessible datastore
                     * is invalid and cannot be used to identify the datastore.
                     */
                    continue;
                }

                if (esxVI_AnyType_ExpectType(conn, dynamicProperty->val,
                                             esxVI_Type_String) < 0) {
                    goto failure;
                }

                if (! STRPREFIX(dynamicProperty->val->string,
                                "/vmfs/volumes/")) {
                    ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                                 "Datastore URL '%s' has unexpected prefix, "
                                 "expecting '/vmfs/volumes/' prefix",
                                 dynamicProperty->val->string);
                    goto failure;
                }

                if (STREQ(dynamicProperty->val->string + offset, name)) {
                    if (esxVI_ObjectContent_DeepCopy(conn, datastore,
                                                     candidate) < 0) {
                        goto failure;
                    }

                    /* Found datastore with matching URL suffix */
                    goto cleanup;
                }
            } else {
                VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
            }
        }
    }

    if (occurence != esxVI_Occurence_OptionalItem) {
        if (numInaccessibleDatastores > 0) {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not find datastore '%s', maybe it's "
                         "inaccessible", name);
        } else {
            ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR,
                         "Could not find datastore '%s'", name);
        }

        goto failure;
    }

  cleanup:
    esxVI_String_Free(&completePropertyNameList);
    esxVI_ObjectContent_Free(&datastoreList);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



int
esxVI_StartVirtualMachineTask(virConnectPtr conn, esxVI_Context *ctx,
                              const char *name, const char *request,
                              esxVI_ManagedObjectReference **task)
{
    int result = 0;
    char *xpathExpression = NULL;
    esxVI_Response *response = NULL;

    if (virAsprintf(&xpathExpression,
                    ESX_VI__SOAP__RESPONSE_XPATH("%s_Task"), name) < 0) {
        virReportOOMError(conn);
        goto failure;
    }

    if (esxVI_Context_Execute(conn, ctx, request, xpathExpression, &response,
                              esxVI_Boolean_False) < 0 ||
        esxVI_ManagedObjectReference_Deserialize(conn, response->node, task,
                                                 "Task") < 0) {
        goto failure;
    }

  cleanup:
    VIR_FREE(xpathExpression);
    esxVI_Response_Free(&response);

    return result;

  failure:
    result = -1;

    goto cleanup;
}



int
esxVI_StartSimpleVirtualMachineTask
  (virConnectPtr conn, esxVI_Context *ctx, const char *name,
   esxVI_ManagedObjectReference *virtualMachine,
   esxVI_ManagedObjectReference **task)
{
    int result = 0;
    virBuffer buffer = VIR_BUFFER_INITIALIZER;
    char *request = NULL;

    virBufferAddLit(&buffer, ESX_VI__SOAP__REQUEST_HEADER);
    virBufferAddLit(&buffer, "<");
    virBufferAdd(&buffer, name, -1);
    virBufferAddLit(&buffer, "_Task xmlns=\"urn:vim25\">");

    if (esxVI_ManagedObjectReference_Serialize(conn, virtualMachine, "_this",
                                               &buffer,
                                               esxVI_Boolean_True) < 0) {
        goto failure;
    }

    virBufferAddLit(&buffer, "</");
    virBufferAdd(&buffer, name, -1);
    virBufferAddLit(&buffer, "_Task>");
    virBufferAddLit(&buffer, ESX_VI__SOAP__REQUEST_FOOTER);

    if (virBufferError(&buffer)) {
        virReportOOMError(conn);
        goto failure;
    }

    request = virBufferContentAndReset(&buffer);

    if (esxVI_StartVirtualMachineTask(conn, ctx, name, request, task) < 0) {
        goto failure;
    }

  cleanup:
    VIR_FREE(request);

    return result;

  failure:
    free(virBufferContentAndReset(&buffer));

    result = -1;

    goto cleanup;
}



int
esxVI_SimpleVirtualMachineMethod(virConnectPtr conn, esxVI_Context *ctx,
                                 const char *name,
                                 esxVI_ManagedObjectReference *virtualMachine)
{
    int result = 0;
    virBuffer buffer = VIR_BUFFER_INITIALIZER;
    char *request = NULL;
    esxVI_Response *response = NULL;

    if (ctx->service == NULL) {
        ESX_VI_ERROR(conn, VIR_ERR_INTERNAL_ERROR, "Invalid argument");
        return -1;
    }

    virBufferAddLit(&buffer, ESX_VI__SOAP__REQUEST_HEADER);
    virBufferAddLit(&buffer, "<");
    virBufferAdd(&buffer, name, -1);
    virBufferAddLit(&buffer, " xmlns=\"urn:vim25\">");

    if (esxVI_ManagedObjectReference_Serialize(conn, virtualMachine, "_this",
                                               &buffer,
                                               esxVI_Boolean_True) < 0) {
        goto failure;
    }

    virBufferAddLit(&buffer, "</");
    virBufferAdd(&buffer, name, -1);
    virBufferAddLit(&buffer, ">");
    virBufferAddLit(&buffer, ESX_VI__SOAP__REQUEST_FOOTER);

    if (virBufferError(&buffer)) {
        virReportOOMError(conn);
        goto failure;
    }

    request = virBufferContentAndReset(&buffer);

    if (esxVI_Context_Execute(conn, ctx, request, NULL, &response,
                              esxVI_Boolean_False) < 0) {
        goto failure;
    }

  cleanup:
    VIR_FREE(request);
    esxVI_Response_Free(&response);

    return result;

  failure:
    if (request == NULL) {
        request = virBufferContentAndReset(&buffer);
    }

    result = -1;

    goto cleanup;
}



int
esxVI_WaitForTaskCompletion(virConnectPtr conn, esxVI_Context *ctx,
                            esxVI_ManagedObjectReference *task,
                            esxVI_TaskInfoState *finalState)
{
    int result = 0;
    esxVI_ObjectSpec *objectSpec = NULL;
    esxVI_PropertySpec *propertySpec = NULL;
    esxVI_PropertyFilterSpec *propertyFilterSpec = NULL;
    esxVI_ManagedObjectReference *propertyFilter = NULL;
    char *version = NULL;
    esxVI_UpdateSet *updateSet = NULL;
    esxVI_PropertyFilterUpdate *propertyFilterUpdate = NULL;
    esxVI_ObjectUpdate *objectUpdate = NULL;
    esxVI_PropertyChange *propertyChange = NULL;
    esxVI_AnyType *propertyValue = NULL;
    esxVI_TaskInfoState state = esxVI_TaskInfoState_Undefined;

    version = strdup("");

    if (version == NULL) {
        virReportOOMError(conn);
        goto failure;
    }

    if (esxVI_ObjectSpec_Alloc(conn, &objectSpec) < 0) {
        goto failure;
    }

    objectSpec->obj = task;
    objectSpec->skip = esxVI_Boolean_False;

    if (esxVI_PropertySpec_Alloc(conn, &propertySpec) < 0) {
        goto failure;
    }

    propertySpec->type = task->type;

    if (esxVI_String_AppendValueToList(conn, &propertySpec->pathSet,
                                       "info.state") < 0 ||
        esxVI_PropertyFilterSpec_Alloc(conn, &propertyFilterSpec) < 0 ||
        esxVI_PropertySpec_AppendToList(conn, &propertyFilterSpec->propSet,
                                        propertySpec) < 0 ||
        esxVI_ObjectSpec_AppendToList(conn, &propertyFilterSpec->objectSet,
                                      objectSpec) < 0 ||
        esxVI_CreateFilter(conn, ctx, propertyFilterSpec, esxVI_Boolean_True,
                           &propertyFilter) < 0) {
        goto failure;
    }

    while (state != esxVI_TaskInfoState_Success &&
           state != esxVI_TaskInfoState_Error) {
        esxVI_UpdateSet_Free(&updateSet);

        if (esxVI_WaitForUpdates(conn, ctx, version, &updateSet) < 0) {
            goto failure;
        }

        VIR_FREE(version);
        version = strdup(updateSet->version);

        if (version == NULL) {
            virReportOOMError(conn);
            goto failure;
        }

        if (updateSet->filterSet == NULL) {
            continue;
        }

        for (propertyFilterUpdate = updateSet->filterSet;
             propertyFilterUpdate != NULL;
             propertyFilterUpdate = propertyFilterUpdate->_next) {
            for (objectUpdate = propertyFilterUpdate->objectSet;
                 objectUpdate != NULL; objectUpdate = objectUpdate->_next) {
                for (propertyChange = objectUpdate->changeSet;
                     propertyChange != NULL;
                     propertyChange = propertyChange->_next) {
                    if (STREQ(propertyChange->name, "info.state")) {
                        if (propertyChange->op == esxVI_PropertyChangeOp_Add ||
                            propertyChange->op == esxVI_PropertyChangeOp_Assign) {
                            propertyValue = propertyChange->val;
                        } else {
                            propertyValue = NULL;
                        }
                    }
                }
            }
        }

        if (propertyValue == NULL) {
            continue;
        }

        if (esxVI_TaskInfoState_CastFromAnyType(conn, propertyValue,
                                                &state) < 0) {
            goto failure;
        }
    }

    if (esxVI_DestroyPropertyFilter(conn, ctx, propertyFilter) < 0) {
        VIR_DEBUG0("DestroyPropertyFilter failed");
    }

    if (esxVI_TaskInfoState_CastFromAnyType(conn, propertyValue,
                                            finalState) < 0) {
        goto failure;
    }

  cleanup:
    /*
     * Remove values given by the caller from the data structures to prevent
     * them from being freed by the call to esxVI_PropertyFilterSpec_Free().
     */
    if (objectSpec != NULL) {
        objectSpec->obj = NULL;
    }

    if (propertySpec != NULL) {
        propertySpec->type = NULL;
    }

    esxVI_PropertyFilterSpec_Free(&propertyFilterSpec);
    esxVI_ManagedObjectReference_Free(&propertyFilter);
    VIR_FREE(version);
    esxVI_UpdateSet_Free(&updateSet);

    return result;

  failure:
    result = -1;

    goto cleanup;
}