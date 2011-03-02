#include <pulse/pulseaudio.h>
#include <ccan/list/list.h>
#include <gio/gio.h>

/* Used to identify duplicate processes */
#define APPLICATION_NAME "BluePulse"

/* Info for each loopback stream */
struct loopback {
    /* PulseAudio data */
    uint32_t source_idx;
    pa_stream *source;
    pa_stream *sink;
    char *description;

    /* shared data */
    char *device_path;

    /* BlueZ DBus data */
    char *device_state;
    GCancellable *pending;
    GDBusProxy *proxy;

    struct list_node list;
};

void quit(int retval);

int pulse_init(pa_mainloop_api *api);
void pulse_quit();

void blue_free(struct loopback *l);
void blue_connect(struct loopback *l);
