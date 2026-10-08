#include <mnet/mnet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "example_common.h"

MNET_HANDLER(home)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html>"
        "<head><title>mnet HTTP Server</title></head>"
        "<body>"
        "<h1>Welcome to mnet!</h1>"
        "<p>This is a basic HTTP server example.</p>"
        "<ul>"
        "  <li><a href=\"/about\">About</a></li>"
        "  <li><a href=\"/contact\">Contact</a></li>"
        "  <li><a href=\"/time\">Current Time</a></li>"
        "</ul>"
        "</body></html>"
    );
}

MNET_HANDLER(about)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>About</title></head>"
        "<body>"
        "<h1>About mnet</h1>"
        "<p>mnet is a simple, ergonomic web framework for C.</p>"
        "<p><a href=\"/\">Back home</a></p>"
        "</body></html>"
    );
}

MNET_HANDLER(contact)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>Contact</title></head>"
        "<body>"
        "<h1>Contact</h1>"
        "<p>Email: example@example.com</p>"
        "<p><a href=\"/\">Back home</a></p>"
        "</body></html>"
    );
}

MNET_HANDLER(current_time)
{
    (void)req;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char html[256];
    snprintf(html, sizeof(html),
        "<!doctype html>"
        "<html><head><title>Current Time</title></head>"
        "<body>"
        "<h1>Current Time</h1>"
        "<p>%s</p>"
        "<p><a href=\"/\">Back home</a></p>"
        "</body></html>",
        time_str
    );

    return mnet_html(html);
}

MNET_HANDLER(not_found)
{
    (void)req;
    return mnet_html(
        "<!doctype html>"
        "<html><head><title>404</title></head>"
        "<body>"
        "<h1>404 - Page Not Found</h1>"
        "<p>The page you're looking for doesn't exist.</p>"
        "<p><a href=\"/\">Go home</a></p>"
        "</body></html>"
    );
}

int main(void)
{
    mnet_app_t *app = example_create();
    if (app == NULL) return 1;

    mnet_set_not_found_handler(app, not_found);

    MNET_GET(app, "/", home);
    MNET_GET(app, "/about", about);
    MNET_GET(app, "/contact", contact);
    MNET_GET(app, "/time", current_time);

    return example_serve(app, 8080,
        "Starting HTTP server on http://localhost:8080\n");
}
