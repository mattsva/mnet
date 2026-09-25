#define _GNU_SOURCE
#include "mnet_router.h"
#include <string.h>
#include <stdlib.h>

int mnet_route_match(const mnet_route_t *route,
                     const char *request_path,
                     const char **out_values,
                     size_t max_values)
{
    const char *pattern = route->path;
    if (pattern == NULL || request_path == NULL) return 0;

    if (pattern[0] != '/' || request_path[0] != '/') return 0;

    size_t ppos = 0, rpos = 0, n = 0;

    while (pattern[ppos] && request_path[rpos]) {
        if (pattern[ppos] == ':') {
            const char *seg_start = request_path + rpos;
            while (request_path[rpos] && request_path[rpos] != '/')
                rpos++;

            if (n < max_values) {
                size_t seg_len = (size_t)(request_path + rpos - seg_start);
                char *value = malloc(seg_len + 1);
                if (value == NULL) return -1;
                memcpy(value, seg_start, seg_len);
                value[seg_len] = '\0';
                out_values[n] = value;
            }
            n++;

            /* Advance past the parameter name in the pattern */
            ppos++; /* skip ':' */
            while (pattern[ppos] && pattern[ppos] != '/')
                ppos++;
            if (pattern[ppos] == '/') ppos++;

            /* Consume the '/' in the request path if present */
            if (request_path[rpos] == '/') rpos++;
        } else if (pattern[ppos] != request_path[rpos]) {
            return 0;
        } else {
            ppos++;
            rpos++;
        }
    }

    if (pattern[ppos] != '\0' || request_path[rpos] != '\0')
        return 0;

    return (int)n;
}

void mnet_match_params_free(const char **values, int count)
{
    for (int i = 0; i < count; i++)
        free((void *)values[i]);
}
