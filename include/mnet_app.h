#ifndef MNET_APP_H
#define MNET_APP_H

#include <stdint.h>

#include "mnet_router.h"

typedef struct mnet_app mnet_app_t;

mnet_app_t *mnet_app_create(void);

void mnet_app_destroy(mnet_app_t *app);

int mnet_app_run(
    mnet_app_t *app,
    uint16_t port
);

int mnet_app_stop(
    mnet_app_t *app
);

int mnet_get(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler
);

int mnet_post(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler
);

int mnet_put(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler
);

int mnet_delete(
    mnet_app_t *app,
    const char *path,
    mnet_handler_t handler
);

#endif
