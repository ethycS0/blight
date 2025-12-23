#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <libportal/portal.h>
#include <stdio.h>

static XdpSession *g_session;
static GstElement *g_pipeline;

static GstFlowReturn on_new_sample(GstElement *sink, gpointer data) {
        GstSample *sample;
        GstBuffer *buffer;
        GstMapInfo map;

        sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample) {
                return GST_FLOW_ERROR;
        }

        buffer = gst_sample_get_buffer(sample);
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {

                printf("\033[2J\033[H");

                // Print frame header
                printf("Frame: %zu bytes (256x144 RGB)\n", map.size);
                printf("====================================\n");

                // Print all pixels
                for (int y = 0; y < 144; y++) {
                        for (int x = 0; x < 256; x++) {
                                int offset = (y * 256 + x) * 3;
                                unsigned char r = map.data[offset + 0];
                                unsigned char g = map.data[offset + 1];
                                unsigned char b = map.data[offset + 2];

                                // Print RGB values with ANSI color background
                                printf("\033[48;2;%d;%d;%dm  \033[0m", r, g, b);
                        }
                        printf("\n");
                }

                gst_buffer_unmap(buffer, &map);
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
}

static void start_gstreamer(int fd, int node) {
        GstElement *pipewiresrc, *appsink, *videorate, *videoscale, *videoconvert;
        GstCaps *caps;
        gchar *path;

        gst_init(NULL, NULL);

        g_pipeline = gst_pipeline_new("capture");
        if (!g_pipeline) {
                g_printerr("Failed to create pipeline\n");
                return;
        }

        pipewiresrc = gst_element_factory_make("pipewiresrc", NULL);
        if (!pipewiresrc) {
                g_printerr("Failed to create pipewiresrc\n");
                return;
        }

        videoconvert = gst_element_factory_make("videoconvert", NULL);
        if (!videoconvert) {
                g_printerr("Failed to create videoconvert\n");
                return;
        }

        videoscale = gst_element_factory_make("videoscale", NULL);
        if (!videoscale) {
                g_printerr("Failed to create videoscale\n");
                return;
        }

        videorate = gst_element_factory_make("videorate", NULL);
        if (!videorate) {
                g_printerr("Failed to create videorate\n");
                return;
        }

        appsink = gst_element_factory_make("appsink", NULL);
        if (!appsink) {
                g_printerr("Failed to create appsink\n");
                return;
        }

        g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, "max-buffers", 1, "drop", TRUE,
                     NULL);
        g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

        path = g_strdup_printf("%u", node);
        g_object_set(pipewiresrc, "fd", fd, "path", path, NULL);
        g_free(path);

        gst_bin_add_many(GST_BIN(g_pipeline), pipewiresrc, videoconvert, videoscale, videorate,
                         appsink, NULL);

        if (!gst_element_link_many(pipewiresrc, videoconvert, videoscale, videorate, NULL)) {
                g_printerr("Failed to link elements\n");
                return;
        }

        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                   G_TYPE_INT, 256, "height", G_TYPE_INT, 144, "framerate",
                                   GST_TYPE_FRACTION, 1, 1, NULL);

        if (!gst_element_link_filtered(videorate, appsink, caps)) {
                g_printerr("Failed to link with caps filter\n");
                gst_caps_unref(caps);
                return;
        }

        gst_caps_unref(caps);

        gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
        g_print("Pipeline running, capturing frames...\n");
}

static void start_session_cb(GObject *source, GAsyncResult *res, gpointer data) {
        XdpSession *session = XDP_SESSION(source);
        GError *error = NULL;

        if (!xdp_session_start_finish(session, res, &error)) {
                g_printerr("Start session failed: %s\n", error->message);
                g_error_free(error);
                return;
        }

        GVariant *streams = xdp_session_get_streams(session);
        int node = 0;

        if (streams) {
                GVariantIter iter;
                g_variant_iter_init(&iter, streams);

                GVariant *options;

                g_variant_iter_next(&iter, "(u@a{sv})", &node, &options);
                g_print("PipeWire Node ID: %d\n", node);
                g_variant_unref(options);
        }

        int fd = xdp_session_open_pipewire_remote(session);
        g_print("PipeWire FD: %d\n", fd);

        start_gstreamer(fd, node);
}

static void create_session_cb(GObject *source, GAsyncResult *res, gpointer data) {
        XdpPortal *portal = XDP_PORTAL(source);
        GError *error = NULL;
        XdpSession *session = xdp_portal_create_screencast_session_finish(portal, res, &error);

        if (error) {
                g_printerr("Create session failed: %s\n", error->message);
                g_error_free(error);
                return;
        }

        g_print("Session created: %p\n", session);
        g_session = session;

        xdp_session_start(session, NULL, NULL, start_session_cb, NULL);
}

int main() {
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        XdpPortal *portal = xdp_portal_new();

        xdp_portal_create_screencast_session(portal, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
                                             XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_TRANSIENT,
                                             NULL, NULL, create_session_cb, NULL);

        g_main_loop_run(loop);
        return 0;
}
