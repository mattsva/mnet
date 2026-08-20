#include <mnet.h>

static void hello(
    mnet_request_t *request,
    mnet_response_t *response)
{
    (void)request;

    mnet_response_text(response, "Hello World!");
}

int main(void)
{
    mnet_app_t *app = mnet_app_create();

    if (app == NULL) {
        return 1;
    }

    mnet_get(app, "/", hello);

    mnet_app_run(app, 8080);

    mnet_app_destroy(app);

    return 0;
}
l