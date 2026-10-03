#ifndef EXAMPLE_COMMON_H
#define EXAMPLE_COMMON_H

/*
 * Shared setup for the mnet examples.
 *
 * Each example is really just a set of handlers plus a route table; the
 * create/configure/run/destroy lifecycle around it was identical in all of
 * them. These two helpers hold that boilerplate so the examples can stay
 * focused on what they are demonstrating.
 *
 * The file is a plain header with static functions so each example remains a
 * single translation unit that can be compiled on its own, for instance
 * "gcc -Iinclude example/example_api_server.c src/mnet.c ... -o myapp".
 *
 * Nothing here is part of the library: it is example scaffolding only.
 */

#include <mnet.h>
#include <stdio.h>

/*
 * Create an application with debug logging enabled.
 * Returns NULL if the application could not be allocated.
 */
static mnet_app_t *example_create(void)
{
    mnet_app_t *app = mnet_create();

    if (app == NULL) {
        fprintf(stderr, "mnet: failed to create the application\n");
        return NULL;
    }

    mnet_set_debug(app, 1);
    return app;
}

/*
 * Print the banner (if any), run the server until it is stopped, and release
 * the application. Returns the value the example's main() should return.
 */
static int example_serve(mnet_app_t *app, uint16_t port, const char *banner)
{
    if (banner != NULL) {
        fputs(banner, stdout);
    }
    fputs("Press Ctrl+C to stop\n", stdout);
    fflush(stdout);

    int rc = mnet_run(app, port);
    mnet_destroy(app);
    return rc;
}

#endif /* EXAMPLE_COMMON_H */
