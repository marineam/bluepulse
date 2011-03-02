#include <string.h>
#include <gio/gio.h>

#include "bluepulse.h"

static void device_event(GDBusProxy *proxy,
                       gchar *sender_name,
                       gchar *signal_name,
                       GVariant *parameters,
                       gpointer data)
{
    struct loopback *l = (struct loopback*)data;
    const gchar *key;
    GVariant *value;
    char *new_state;

    if (strcmp(signal_name, "PropertyChanged"))
        return;

    g_variant_get(parameters, "(&sv)", &key, &value);
    if (strcmp(key, "State"))
        return;

    new_state = g_variant_dup_string(value, NULL);
    g_message("New device state for %s: %s", l->description, new_state);

    if (l->device_state) {
        /* XXX This was the idea... but it is useless...
        if (!strcmp(l->device_state, "playing") &&
                strcmp(new_state, "playing"))
            loopback_flush(l);
        */

        g_free(l->device_state);
    }

    l->device_state = new_state;
}

static void device_state(GObject *src, GAsyncResult *res, gpointer data)
{
    struct loopback *l = (struct loopback*)data;
    GError *err = NULL;
    GVariant *result;

    result = g_dbus_proxy_call_finish(l->proxy, res, &err);
    if (err) {
        g_warning("DBus Failure: %s", err->message);
        g_error_free(err);
        return;
    }

    if (l->device_state) {
        g_free(l->device_state);
        l->device_state = NULL;
    }

    /* pull out the State property */
    GVariantIter *iter;
    const gchar *key;
    GVariant *value;
    g_variant_get(result, "(a{sv})", &iter);
    while (g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
        if (strcmp(key, "State"))
            continue;
        l->device_state = g_variant_dup_string(value, NULL);
    }
    g_variant_iter_free(iter);
    g_variant_unref(result);

    if (!l->device_state)
        g_error("BlueZ property 'State' not found.");
}

static void connected(GObject *src, GAsyncResult *res, gpointer data)
{
    struct loopback *l = (struct loopback*)data;
    GError *err = NULL;

    l->proxy = g_dbus_proxy_new_for_bus_finish(res, &err);
    if (err) {
        g_warning("DBus Failure: %s", err->message);
        g_error_free(err);
        return;
    }

    g_dbus_proxy_call(l->proxy, "GetProperties", NULL,
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      device_state, l);
    g_signal_connect(l->proxy, "g-signal", G_CALLBACK(device_event), l);
}

void blue_free(struct loopback *l)
{
    if (l->proxy)
        g_object_unref(l->proxy);
    g_free(l->device_state);
}

void blue_connect(struct loopback *l) {
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                             G_DBUS_PROXY_FLAGS_NONE,
                             NULL, /* GDBusInterfaceInfo */
                             "org.bluez",
                             l->device_path,
                             "org.bluez.AudioSource",
                             NULL, /* GCancellable */
                             connected, l);
}
