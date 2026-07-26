#include "relay/app.h"

#include <string.h>

/** Create, run, and release the Relay application. */
int main(void)
{
    Relay_App app;
    int exit_code;

    (void)memset(&app, 0, sizeof(app));
    exit_code = relay_app_init(&app) ? relay_app_run(&app) : 1;
    relay_app_shutdown(&app);
    return exit_code;
}
