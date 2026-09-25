#ifndef MNET_REQUEST_H
#define MNET_REQUEST_H

#include <stddef.h>

typedef struct mnet_request {
    const char *method;
    const char *path;
    const char *query_string;

    const char *body;
    size_t body_length;

    int         path_param_count;
    const char **path_param_names;
    const char **path_param_values;

    int         query_count;
    const char **query_names;
    const char **query_values;

    int         header_count;
    const char **header_names;
    const char **header_values;
} mnet_request_t;

const char *mnet_request_method(const mnet_request_t *request);
const char *mnet_request_path(const mnet_request_t *request);
const char *mnet_request_body(const mnet_request_t *request);
size_t mnet_request_body_length(const mnet_request_t *request);

#define MNET_PARAM(req, name)        mnet_request_param(req, name)
#define MNET_QUERY(req, name)        mnet_request_query(req, name)
#define MNET_HEADER(req, name)       mnet_request_header(req, name)
#define MNET_BODY(req)               mnet_request_body(req)
#define MNET_BODY_LEN(req)           mnet_request_body_length(req)

const char *mnet_request_param(const mnet_request_t *request, const char *name);
const char *mnet_request_query(const mnet_request_t *request, const char *name);
const char *mnet_request_header(const mnet_request_t *request, const char *name);

#endif
