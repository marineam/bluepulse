#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <pulse/pulseaudio.h>
#include <ccan/list/list.h>

/* Alias this because it is used constantly */
#define pao(o) pa_operation_unref(o)

/* Used to identify duplicate processes */
#define APPLICATION_NAME "BluePulse"

/* Arguments for PulseAudio's loopback module.
 * A high latency is required to make resampling less aggressive. The
 * loopback module manages latency by changing the playback sample Hz
 * which causes pitch bending. Hopefully the larger the buffer the
 * less this will happen, it is pretty damn annoying.
 * (future note: try adjust_time when that arg starts working)
 */
#define MODULE_ARGS "latency_msec=1500"

static int context_setup();

struct source {
    uint32_t index;
    uint32_t loopback;
    char *description;
    struct list_node list;
};

static pa_mainloop_api *api;
static pa_context *context;
static int stopping = 0;
static LIST_HEAD(sources);

static struct source* get_source(uint32_t index)
{
    struct source *s;

    list_for_each(&sources, s, list) {
        if (s->index == index)
            return s;
    }

    return NULL;
}

static void free_source(struct source* s)
{
    free(s->description);
    free(s);
}

static void finish_load(pa_context *c, uint32_t idx, void *data)
{
    struct source *s = (struct source*)data;

    s->loopback = idx;
    list_add(&sources, &s->list);
}

static void finish_unload(pa_context *c, int success, void *data)
{
    struct source *s = (struct source*)data;

    free_source(s);

    if (stopping && --stopping <= 0)
        api->quit(api, 0);
}

static void source_info(pa_context *c,
        const pa_source_info *i, int eol, void *data)
{
    const char *proto;
    struct source *s;
    char *arg;

    if (eol)
        return;

    proto = pa_proplist_gets(i->proplist, "bluetooth.protocol");
    if (proto == NULL)
        return;

    if (strcmp("a2dp_source", proto))
        return;

    if (get_source(i->index) != NULL)
        return;

    fprintf(stderr, "New A2DP Source: %s\n", i->description);

    s = malloc(sizeof(*s));
    assert(s);
    assert(asprintf(&arg, "source=%s %s", i->name, MODULE_ARGS) > 0);

    s->index = i->index;
    s->description = strdup(i->description);
    pao(pa_context_load_module(c, "module-loopback", arg, finish_load, s));
    free(arg);
}

static void source_cleanup(pa_context *c, struct source *s)
{
    fprintf(stderr, "Removed A2DP Source: %s\n", s->description);

    list_del(&s->list);
    pao(pa_context_unload_module(c, s->loopback, finish_unload, s));
}

static void context_event(pa_context *c,
        pa_subscription_event_type_t t, uint32_t idx, void *data)
{
    int facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    int type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    switch (facility) {
        case PA_SUBSCRIPTION_EVENT_SOURCE:
            if (type == PA_SUBSCRIPTION_EVENT_NEW) {
                pao(pa_context_get_source_info_by_index(c,
                            idx, source_info, NULL));
            }
            else if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
                struct source *s = get_source(idx);
                if (s != NULL)
                    source_cleanup(c, s);
            }
            break;

        default:
            break;
    }
}

static void client_info(pa_context *c,
        const pa_client_info *i, int eol, void *data)
{
    const char *name, *pid;

    if (eol) {
        /* Conflicting client check done, start the real work! */
        pao(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SOURCE, NULL, NULL));
        pao(pa_context_get_source_info_list(c, source_info, NULL));
        return;
    }

    name = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
    pid = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_ID);
    if (name == NULL || pid == NULL)
        return;

    if (atoi(pid) == getpid())
        return;

    if (strcmp(name, APPLICATION_NAME))
        return;

    /* Uh oh... two copies of this daemon are connected! */
    fprintf(stderr, "ERROR: Another instance of %s is already connected.\n",
            APPLICATION_NAME);
    api->quit(api, 1);
}

static void context_change(pa_context *c, void *data)
{
    struct source *s, *n;

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_SETTING_NAME:
        case PA_CONTEXT_TERMINATED:
            break;

        case PA_CONTEXT_READY:
            fprintf(stderr, "Connected\n");
            pao(pa_context_get_client_info_list(c, client_info, NULL));
            break;

        case PA_CONTEXT_FAILED:
            fprintf(stderr, "Connection failure: %s\n",
                    pa_strerror(pa_context_errno(c)));

            /* Not sure what to do about the source list as it is
             * invalid if we reconnect to a fresh process but is valid
             * if we get to reconnect to the same old one. */
            list_for_each_safe(&sources, s, n, list) {
                list_del(&s->list);
                free_source(s);
            }

            /* Attempt to reconnect */
            if (context != c || context_setup(c))
                api->quit(api, 1);
            break;
    }
}

static int context_setup() {
    time_t timeout = time(NULL) + 30;

    if (context)
        pa_context_unref(context);

    context = pa_context_new(api, APPLICATION_NAME);
    assert(context);

    pa_context_set_state_callback(context, context_change, NULL);
    pa_context_set_subscribe_callback(context, context_event, NULL);

    do {
        if (pa_context_connect(context, NULL, 0, NULL)) {
            fprintf(stderr, "Connection failure: %s\n",
                pa_strerror(pa_context_errno(context)));
            sleep(1);
        }
        else
            return 0;
    } while (time(NULL) <= timeout);

    pa_context_unref(context);
    context = NULL;

    return 1;
}

static void signal_exit(pa_mainloop_api *api, pa_signal_event *e,
        int sig, void *data) {
    struct source *s, *n;

    if (list_empty(&sources))
        api->quit(api, 0);
    else if (context) {
        pao(pa_context_subscribe(context,
                    PA_SUBSCRIPTION_MASK_NULL, NULL, NULL));
        list_for_each_safe(&sources, s, n, list) {
            stopping++;
            source_cleanup(context, s);
        }
    }
    else {
        list_for_each_safe(&sources, s, n, list) {
            list_del(&s->list);
            free_source(s);
        }
    }
}

int main(int argc, char * argv[])
{
    pa_mainloop *mainloop = NULL;
    int ret = 1;

    /* Start up the PulseAudio connection */
    mainloop = pa_mainloop_new();
    assert(mainloop);
    api = pa_mainloop_get_api(mainloop);

    /* Register some signal handlers */
    pa_signal_init(api);
    pa_signal_new(SIGINT, signal_exit, NULL);
    pa_signal_new(SIGTERM, signal_exit, NULL);

    if (context_setup())
        goto finish;

    pa_mainloop_run(mainloop, &ret);

finish:
    pa_signal_done();
    if (context)
        pa_context_disconnect(context);
        pa_context_unref(context);
    pa_mainloop_free(mainloop);
    api = NULL;

    return ret;
}
