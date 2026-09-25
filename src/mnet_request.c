#define _GNU_SOURCE
#include "mnet_request.h"
#include <string.h>
#include <strings.h>

const char *mnet_request_method(const mnet_request_t *request)
{
    if (request == NULL) return NULL;
    return request->method;
}

const char *mnet_request_path(const mnet_request_t *request)
{
    if (request == NULL) return NULL;
    return request->path;
}

const char *mnet_request_body(const mnet_request_t *request)
{
    if (request == NULL) return NULL;
    return request->body;
}

size_t mnet_request_body_length(const mnet_request_t *request)
{
    if (request == NULL) return 0;
    return request->body_length;
}

const char *mnet_request_param(const mnet_request_t *request, const char *name)
{
    if (request == NULL || name == NULL) return NULL;
    if (request->path_param_names == NULL) return NULL;

    for (int i = 0; i < request->path_param_count; i++) {
        if (strcmp(request->path_param_names[i], name) == 0)
            return request->path_param_values[i];
    }
    return NULL;
}

const char *mnet_request_query(const mnet_request_t *request, const char *name)
{
    if (request == NULL || name == NULL) return NULL;
    if (request->query_names == NULL) return NULL;

    for (int i = 0; i < request->query_count; i++) {
        if (strcmp(request->query_names[i], name) == 0)
            return request->query_values[i];
    }
    return NULL;
}

const char *mnet_request_header(const mnet_request_t *request, const char *name)
{
    if (request == NULL || name == NULL) return NULL;
    if (request->header_names == NULL) return NULL;

    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->header_names[i], name) == 0)
            return request->header_values[i];
    }
    return NULL;
}
