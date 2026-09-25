#include <mnet.h>
#include <stdio.h>
#include <string.h>

MNET_HANDLER(home)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html>"
        "<head><title>mnet</title></head>"
        "<body>"
        "<h1>Hello World!</h1>"
        "<p>Try GET /api/users/42</p>"
        "<p>Try POST /api/echo</p>"
        "<p>Try GET /nonexistent (custom 404)</p>"
        "</body>"
        "</html>"
    );
}

MNET_HANDLER(get_user)
{
    const char *id = MNET_PARAM(req, "id");

    if (id == NULL) {
        return mnet_error(400, "missing id");
    }

    return mnet_jsonf(
        "{\"id\":\"%s\",\"name\":\"User %s\"}",
        id,
        id
    );
}

MNET_HANDLER(echo)
{
    const char *body = MNET_BODY(req);

    if (body == NULL) {
        return mnet_error(400, "empty body");
    }

    return mnet_jsonf(
        "{\"received\":\"%s\"}",
        body
    );
}

/* Custom 404 handler */
MNET_HANDLER(custom_404)
{
    fprintf(stderr, "DEBUG: custom_404 called\n");
    (void)req;
    mnet_response_t r = mnet_html(
        "<!doctype html>"
        "<html><head><title>404</title></head>"
        "<body>"
        "<h1>404 - Page Not Found</h1>"
        "<p>The page is not found.</p>"
        "<p><a href=\"/\">Go home</a></p>"
        "</body></html>"
    );
    r.status = 404;
    return r;
}

int main(void)
{
    mnet_app_t *app = mnet_create();

    if (app == NULL) {
        return 1;
    }

    mnet_set_debug(app, 1);
    mnet_set_not_found_handler(app, custom_404);

    MNET_GET(app, "/", home);
    MNET_GET(app, "/api/users/:id", get_user);
    MNET_POST(app, "/api/echo", echo);

    int rc = mnet_run(app, 8080);

    mnet_destroy(app);

    return rc;
}
