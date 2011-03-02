#include <glib.h>
#include <signal.h>
#include <glib-object.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#include "bluepulse.h"

static GMainLoop *mainloop;
static pa_glib_mainloop *pulse_mainloop;
static pa_mainloop_api *pulse_api;
static int returncode = 1;

void quit(int retval)
{
    returncode = retval;
    pulse_quit();
    g_main_loop_quit(mainloop);
}

static void signal_quit(pa_mainloop_api *api,
                        pa_signal_event *e,
                        int sig, void *data)
{
    quit(0);
}

int main(int argc, char *argv[])
{
    g_type_init();

    mainloop = g_main_loop_new(NULL, FALSE);
    pulse_mainloop = pa_glib_mainloop_new(NULL);
    pulse_api = pa_glib_mainloop_get_api(pulse_mainloop);

    pa_signal_init(pulse_api);
    pa_signal_new(SIGINT, signal_quit, NULL);
    pa_signal_new(SIGTERM, signal_quit, NULL);

    if (pulse_init(pulse_api))
        goto finish;

    g_main_loop_run(mainloop);

finish:
    pa_signal_done();

    pa_glib_mainloop_free(pulse_mainloop);
    g_main_loop_unref(mainloop);

    return returncode;
}
