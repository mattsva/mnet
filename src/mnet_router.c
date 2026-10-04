#define _GNU_SOURCE
#include "mnet_router.h"
#include "mnet_response.h"
#include <string.h>
#include <stdlib.h>

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mnet_route_match(const mnet_route_t *route,
                     const char *request_path,
                     const char **out_values,
                     size_t max_values)
{
    const char *pattern = route->path;
    if (pattern == NULL || request_path == NULL) return -1;

    if (pattern[0] != '/' || request_path[0] != '/') return -1;

    size_t ppos = 0, rpos = 0, n = 0;

    while (pattern[ppos] && request_path[rpos]) {
        if (pattern[ppos] == '*') {
            /* Wildcard: match everything remaining. Do NOT decode — the
               caller may use the raw path for filesystem operations. */
            if (n < max_values) {
                size_t seg_len = strlen(request_path + rpos);
                char *value = malloc(seg_len + 1);
                if (value == NULL) {
                    out_values[n] = NULL;
                    n++;
                    mnet_match_params_free(out_values, (int)n);
                    return -1;
                }
                memcpy(value, request_path + rpos, seg_len);
                value[seg_len] = '\0';
                out_values[n] = value;
            }
            n++;
            ppos++;
            rpos = strlen(request_path);
        } else if (pattern[ppos] == ':') {
            const char *seg_start = request_path + rpos;
            while (request_path[rpos] && request_path[rpos] != '/')
                rpos++;

            if (n < max_values) {
                size_t seg_len = (size_t)(request_path + rpos - seg_start);
                char *value = malloc(seg_len + 1);
                if (value == NULL) {
                    out_values[n] = NULL;
                    n++;
                    mnet_match_params_free(out_values, (int)n);
                    return -1;
                }
                memcpy(value, seg_start, seg_len);
                value[seg_len] = '\0';
                /* Decode path params, but do NOT decode '+' as space —
                   that is a query-string convention, not a path convention. */
                {
                    char decoded[4096];
                    size_t di = 0;
                    for (size_t si = 0; si < seg_len && di < sizeof(decoded) - 1; si++) {
                        if (value[si] == '%' && si + 2 < seg_len) {
                            int hi = hex_val(value[si + 1]);
                            int lo = hex_val(value[si + 2]);
                            if (hi >= 0 && lo >= 0) {
                                decoded[di++] = (char)((hi << 4) | lo);
                                si += 2;
                            } else {
                                decoded[di++] = value[si];
                            }
                        } else {
                            decoded[di++] = value[si];
                        }
                    }
                    decoded[di] = '\0';
                    memcpy(value, decoded, di + 1);
                }
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
            mnet_match_params_free(out_values, (int)n);
            return -1;
        } else {
            ppos++;
            rpos++;
        }
    }

    if (pattern[ppos] != '\0' || request_path[rpos] != '\0') {
        mnet_match_params_free(out_values, (int)n);
        return -1;
    }

    return (int)n;
}

void mnet_match_params_free(const char **values, int count)
{
    for (int i = 0; i < count; i++)
        free((void *)values[i]);
}
