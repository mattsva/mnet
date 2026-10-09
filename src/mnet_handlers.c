/* src/mnet_handlers.c - Built-in handlers: the dev-mode welcome page and
 * static file serving. */

#define _GNU_SOURCE
#include "mnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

mnet_response_t welcome_response(mnet_app_t *app)
{
    /* Welcome page shown when dev mode is enabled and no routes are defined. */
    static const char *welcome_html =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>mnet Server</title></head>\n"
        "<body>\n"
        "<h1>Congratulations!</h1>\n"
        "<p>You successfully started the mnet server.</p>\n"
        "<p>Go to <a href=\"https://github.com/mnet-web/mnet/blob/main/README.md\">\n"
        "README.md</a> for more information about MNET.</p>\n"
        "<p>Go to <a href=\"https://github.com/mnet-web/mnet/blob/main/docs/clib.md\">\n"
        "clib.md</a> for further setup.</p>\n"
        "<p>Development state: %s</p>\n"
        "<p>Reported https state: %s (display only, TLS is not implemented)</p>\n"
        "<p>Listening port: %d</p>\n"
        "<p>Debug mode: %s</p>\n"
        "<p>Worker count: %d</p>\n"
        "<p>Max connections: %d</p>\n"
        "<p>Timeout: %d seconds</p>\n"
        "<p>Keep-alive timeout: %d seconds</p>\n"
        "<p>Max body size: %zu bytes</p>\n"
        "<p>Routes defined: %zu</p>\n"
        "<p>Build time: %s</p>\n"
        "<p>Git commit: <code>%12s</code></p>\n"
        "</body>\n"
        "</html>";

    char dev_state[8];
    snprintf(dev_state, sizeof(dev_state), "%s", app->dev ? "true" : "false");

    char https_state[8];
    snprintf(https_state, sizeof(https_state), "%s",
        app->https ? "https" : "http");

    /* Build a simple HTML page with all the info.
       The body is allocated with malloc() and must be freed by the caller. */
    size_t total_len = strlen(welcome_html) + strlen(dev_state) +
                       strlen(https_state) + 512;
    char *body = malloc(total_len);
    if (body == NULL) {
        return mnet_error(500, "internal server error");
    }

    /* Get current time. localtime() returns a pointer to a process-wide
       static struct tm, which is not safe to use from several worker
       threads at once; use the reentrant variant instead. */
    time_t now = time(NULL);
    char time_str[64] = "unknown";
    struct tm tmbuf;
#ifdef _WIN32
    if (localtime_s(&tmbuf, &now) == 0)
#else
    if (localtime_r(&now, &tmbuf) != NULL)
#endif
    {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &tmbuf);
    }

    /* Get git commit hash */
    char commit_hash[13] = "unknown";
    FILE *fp = fopen(".git/HEAD", "r");
    if (fp != NULL) {
        char line[256];
        if (fgets(line, sizeof(line), fp) != NULL) {
            /* Remove newline */
            line[strcspn(line, "\n")] = 0;
            /* Extract hash from ref: ref: refs/heads/main */
            if (strncmp(line, "ref:", 4) == 0) {
                char *hash = line + 4;
                while (*hash == ' ' || *hash == '\t') hash++;
                strncpy(commit_hash, hash, 12);
                commit_hash[12] = '\0';
            } else {
                /* Direct hash (unborn branch) */
                strncpy(commit_hash, line, 12);
                commit_hash[12] = '\0';
            }
        }
        fclose(fp);
    }

    int written = snprintf(body, total_len, welcome_html,
        dev_state, https_state, app->port,
        app->debug ? "true" : "false",
        app->workers, app->max_connections,
        app->timeout_seconds, app->keep_alive_timeout,
        app->max_body_size, app->route_count,
        time_str, commit_hash);
    /* snprintf reports the would-be length on truncation: the buffer has
       512 bytes of slack over the template, but clamp anyway so a future
       template edit cannot turn truncation into an over-read. */
    if (written < 0) written = 0;
    if ((size_t)written >= total_len) written = (int)total_len - 1;

    mnet_response_t r;
    r.status = 200;
    r.content_type = "text/html";
    r.body = body;
    r.body_length = (size_t)written;
    r.chunked = 0;

    return r;
}

static const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    dot++;
    if (strcasecmp(dot, "html") == 0 || strcasecmp(dot, "htm") == 0) return "text/html";
    if (strcasecmp(dot, "css") == 0) return "text/css";
    if (strcasecmp(dot, "js") == 0) return "application/javascript";
    if (strcasecmp(dot, "json") == 0) return "application/json";
    if (strcasecmp(dot, "png") == 0) return "image/png";
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, "gif") == 0) return "image/gif";
    if (strcasecmp(dot, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, "ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, "txt") == 0) return "text/plain";
    if (strcasecmp(dot, "xml") == 0) return "application/xml";
    if (strcasecmp(dot, "pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

mnet_response_t static_handler(mnet_request_t *req)
{
    static_config_t *cfg = (static_config_t *)req->user_data;
    if (cfg == NULL) return mnet_error(500, "internal server error");

    const char *url_path = req->path;
    const char *rel = url_path + strlen(cfg->url_prefix);
    if (*rel == '/') rel++;

    char fs_path[4096];
    int written = snprintf(fs_path, sizeof(fs_path), "%s/%s", cfg->fs_path, rel);
    if (written < 0 || (size_t)written >= sizeof(fs_path)) {
        return mnet_error(404, "not found");
    }

    char *real = mnet_realpath(fs_path);
    if (real == NULL) {
        return mnet_error(404, "not found");
    }
    char *base_real = mnet_realpath(cfg->fs_path);
    if (base_real == NULL) {
        free(real);
        return mnet_error(404, "not found");
    }

    /* The resolved path must be the root itself or lie underneath it. A plain
       prefix comparison would also accept a sibling such as /var/www2 when the
       root is /var/www, so check the separator boundary too. */
    size_t base_len = strlen(base_real);
    int inside = (strncmp(real, base_real, base_len) == 0) &&
                 (real[base_len] == '\0' ||
                  real[base_len] == '/' ||
                  base_real[base_len - 1] == '/');

    if (!inside) {
        mnet_app_log(cfg->app, MNET_LOG_WARN,
            "blocked path traversal attempt: '%s' resolves outside the root",
            req->path);
        free(real);
        free(base_real);
        return mnet_error(403, "forbidden");
    }
    free(base_real);

    FILE *fp = fopen(real, "rb");
    if (fp == NULL) {
        free(real);
        return mnet_error(404, "not found");
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        free(real);
        return mnet_error(404, "not found");
    }

    long file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        free(real);
        return mnet_error(404, "not found");
    }

    rewind(fp);

    char *data = malloc((size_t)file_size + 1);
    if (data == NULL) {
        fclose(fp);
        free(real);
        return mnet_error(500, "internal server error");
    }

    size_t total = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);
    free(real);

    mnet_response_t r = {
        .status = 200,
        .content_type = mime_type(fs_path),
        .body = data,
        .body_length = total,
    };
    return r;
}
