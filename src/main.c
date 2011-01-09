#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <pulse/pulseaudio.h>
#include <ccan/list/list.h>

/* Alias this because it is used constantly */
#define pao(o) pa_operation_unref(o)

struct source {
    uint32_t index;
    uint32_t loopback;
    char *description;
    struct list_node list;
};

static pa_mainloop_api *api;
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

static void finish_load(pa_context *c, uint32_t idx, void *data)
{
    struct source *s = (struct source*)data;

    s->loopback = idx;
    list_add(&sources, &s->list);
}

static void finish_unload(pa_context *c, int success, void *data)
{
    struct source *s = (struct source*)data;

    free(s->description);
    free(s);

    if (stopping && --stopping <= 0)
        api->quit(api, 0);
}

static void source_info(pa_context *c,
        const pa_source_info *i, int eol, void *data)
{
    const void *value;
    size_t length;

    if (eol)
        return;

    if (pa_proplist_get(i->proplist, "bluetooth.protocol", &value, &length))
        return;

    if (strcmp("a2dp_source", (char*)value))
        return;

    if (get_source(i->index) != NULL)
        return;

    fprintf(stderr, "New A2DP Source: %s\n", i->description);

    struct source *s = malloc(sizeof(*s));
    assert(s);
    char *arg;
    assert(asprintf(&arg, "source=%s", i->name) > 0);

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

static void context_change(pa_context *c, void *data)
{
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            pao(pa_context_subscribe(c,
                        PA_SUBSCRIPTION_MASK_SOURCE, NULL, NULL));
            pao(pa_context_get_source_info_list(c, source_info, NULL));
            break;

        case PA_CONTEXT_TERMINATED:
            fprintf(stderr, "Unexpected connection close.\n");
            api->quit(api, 1);
            break;

        case PA_CONTEXT_FAILED:
            fprintf(stderr, "Connection failure: %s\n",
                    pa_strerror(pa_context_errno(c)));
            api->quit(api, 1);
            break;
    }
}

static void signal_exit(pa_mainloop_api *api, pa_signal_event *e,
        int sig, void *data) {
    pa_context *c = (pa_context*)data;
    struct source *s, *n;

    if (list_empty(&sources))
        api->quit(api, 0);
    else {
        list_for_each_safe(&sources, s, n, list) {
            stopping++;
            source_cleanup(c, s);
        }
    }
}

int main(int argc, char * argv[])
{
    pa_mainloop *mainloop = NULL;
    pa_context *context = NULL;
    int ret = 1;

    list_head_init(&sources);

    /* Start up the PulseAudio connection */
    mainloop = pa_mainloop_new();
    assert(mainloop);
    api = pa_mainloop_get_api(mainloop);
    context = pa_context_new(api, argv[0]);
    assert(context);

    /* Register some signal handlers */
    pa_signal_init(api);
    pa_signal_new(SIGINT, signal_exit, context);
    pa_signal_new(SIGTERM, signal_exit, context);

    pa_context_set_state_callback(context, context_change, NULL);
    pa_context_set_subscribe_callback(context, context_event, NULL);

    if (pa_context_connect(context, NULL, 0, NULL)) {
        fprintf(stderr, "Connection failure: %s\n",
                pa_strerror(pa_context_errno(context)));
        goto finish;
    }

    pa_mainloop_run(mainloop, &ret);

finish:
    pa_signal_done();
    pa_context_unref(context);
    pa_mainloop_free(mainloop);

    return ret;
}
