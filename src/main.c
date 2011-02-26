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

static int context_setup();

struct loopback {
    uint32_t source_idx;
    pa_stream *source;
    pa_stream *sink;
    char *description;
    struct list_node list;
};

static pa_mainloop_api *api;
static pa_context *context;
static LIST_HEAD(loops);

static struct loopback* loopback_get(uint32_t source_idx)
{
    struct loopback *l;

    list_for_each(&loops, l, list) {
        if (l->source_idx == source_idx)
            return l;
    }

    return NULL;
}

static void loopback_stop(struct loopback* l)
{
    fprintf(stderr, "Removed A2DP Source: %s\n", l->description);
    list_del(&l->list);
    pa_stream_disconnect(l->source);
    pa_stream_disconnect(l->sink);
    pa_stream_unref(l->source);
    pa_stream_unref(l->sink);
    free(l->description);
    free(l);
}

static void loopback_stop_all()
{
    struct loopback *l, *n;

    list_for_each_safe(&loops, l, n, list)
        loopback_stop(l);
}

static void loopback_read(pa_stream *s, size_t rlen, void *data)
{
    struct loopback *l = (struct loopback*)data;
    const void *buffer;

    assert(s == l->source);
    pa_stream_peek(s, &buffer, &rlen);
    assert(buffer && rlen);
    pa_stream_write(l->sink, buffer, rlen, NULL, 0, 0);
    pa_stream_drop(s);
}

static void loopback_state(pa_stream *s, void *data)
{
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_TERMINATED:
        case PA_STREAM_READY:
            break;

        case PA_STREAM_FAILED:
            fprintf(stderr, "Stream failure: %s\n",
                    pa_strerror(pa_context_errno(context)));
            loopback_stop((struct loopback*)data);
            break;
    }
}

static void loopback_start(pa_context *c, const pa_source_info *i)
{
    struct loopback *l;

    assert(!loopback_get(i->index));
    fprintf(stderr, "New A2DP Source: %s\n", i->description);

    l = malloc(sizeof(*l));
    l->source_idx = i->index;
    l->description = strdup(i->description);

    /* source stream */
    l->source = pa_stream_new(c, l->description, &i->sample_spec, NULL);
    pa_stream_set_state_callback(l->source, loopback_state, l);
    pa_stream_set_read_callback(l->source, loopback_read, l);
    pa_stream_connect_record(l->source, i->name, NULL, PA_STREAM_DONT_MOVE);

    /* sink stream */
    l->sink = pa_stream_new(c, l->description, &i->sample_spec, NULL);
    pa_stream_set_state_callback(l->sink, loopback_state, l);
    pa_stream_connect_playback(l->sink, NULL, NULL, 0, NULL, NULL);

    list_add(&loops, &l->list);
}

static void source_info(pa_context *c,
        const pa_source_info *i, int eol, void *data)
{
    const char *proto;

    if (eol)
        return;

    proto = pa_proplist_gets(i->proplist, "bluetooth.protocol");
    if (proto == NULL)
        return;

    if (strcmp("a2dp_source", proto))
        return;

    if (loopback_get(i->index) != NULL)
        return;

    loopback_start(c, i);
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
                struct loopback *l = loopback_get(idx);
                if (l != NULL)
                    loopback_stop(l);
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
            loopback_stop_all();

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
        int sig, void *data)
{
    if (context) {
        pao(pa_context_subscribe(context,
                    PA_SUBSCRIPTION_MASK_NULL, NULL, NULL));
        loopback_stop_all();
    }
    api->quit(api, 0);
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
