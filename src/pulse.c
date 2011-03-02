#include <glib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include <ccan/list/list.h>

#include "bluepulse.h"

/* Alias this because it is used constantly */
#define pao(o) pa_operation_unref(o)

struct loopback {
    uint32_t source_idx;
    pa_stream *source;
    pa_stream *sink;
    char *description;
    struct list_node list;
};

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
    g_message("Removed A2DP Source: %s", l->description);
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

    g_assert(s == l->source);
    pa_stream_peek(s, &buffer, &rlen);
    g_assert(buffer && rlen);
    pa_stream_write(l->sink, buffer, rlen, NULL, 0, 0);
    pa_stream_drop(s);
}

static void loopback_state(pa_stream *s, void *data)
{
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_TERMINATED:
            break;

        case PA_STREAM_READY:
            pao(pa_stream_flush(s, NULL, NULL));
            break;

        case PA_STREAM_FAILED:
            g_warning("Stream failure: %s",
                    pa_strerror(pa_context_errno(context)));
            loopback_stop((struct loopback*)data);
            break;
    }
}

static void loopback_start(pa_context *c, const pa_source_info *i)
{
    struct loopback *l;
    pa_buffer_attr max_latency = {-1, -1, -1, -1, -1};

    g_assert(!loopback_get(i->index));
    g_message("New A2DP Source: %s", i->description);

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
    pa_stream_connect_playback(l->sink, NULL, &max_latency,
            PA_STREAM_ADJUST_LATENCY, NULL, NULL);

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
    g_critical("Another instance of %s is already connected.",
            APPLICATION_NAME);
    quit(1);
}

static void context_change(pa_context *c, void *data)
{
    pa_mainloop_api *api = (pa_mainloop_api*)data;

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_SETTING_NAME:
        case PA_CONTEXT_TERMINATED:
            break;

        case PA_CONTEXT_READY:
            pao(pa_context_get_client_info_list(c, client_info, NULL));
            break;

        case PA_CONTEXT_FAILED:
            g_warning("Connection failure: %s",
                    pa_strerror(pa_context_errno(c)));
            loopback_stop_all();

            /* Attempt to reconnect */
            if (context != c || pulse_init(api))
                quit(1);
            break;
    }
}

int pulse_init(pa_mainloop_api *api)
{
    time_t timeout = time(NULL) + 30;

    if (context)
        pa_context_unref(context);

    context = pa_context_new(api, APPLICATION_NAME);
    g_assert(context);

    pa_context_set_state_callback(context, context_change, api);
    pa_context_set_subscribe_callback(context, context_event, NULL);

    do {
        if (pa_context_connect(context, NULL, 0, NULL)) {
            g_warning("Connection failure: %s",
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

void pulse_quit()
{
    if (context) {
        pao(pa_context_subscribe(context,
                    PA_SUBSCRIPTION_MASK_NULL, NULL, NULL));
        loopback_stop_all();
        pa_context_disconnect(context);
        pa_context_unref(context);
        context = NULL;
    }
}
