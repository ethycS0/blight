#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <libportal/portal.h>
#include <stdio.h>
#include <unistd.h>

#include "serial.h"

#define CAPTURE_WIDTH 256
#define CAPTURE_HEIGHT 144
#define CAPTURE_DEPTH 10
#define CAPTURE_FPS 24
#define BRIGHTNESS 120
#define SATURATION 1

typedef struct {
        unsigned char r, g, b;
} RGB;

static RGB g_final_buffer[((CAPTURE_HEIGHT - CAPTURE_DEPTH) / CAPTURE_DEPTH + 1) +
                          ((CAPTURE_WIDTH + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH) +
                          ((CAPTURE_HEIGHT + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH)];

static XdpSession *g_session;
static GstElement *g_pipeline;

static void average_pixel_box(const unsigned char *data, int start_x, int start_y, int box_size,
                              RGB *result) {
        int total_r = 0, total_g = 0, total_b = 0;
        int count = 0;

        for (int dy = 0; dy < box_size; dy++) {
                for (int dx = 0; dx < box_size; dx++) {
                        int x = start_x + dx;
                        int y = start_y + dy;

                        if (x >= 0 && x < CAPTURE_WIDTH && y >= 0 && y < CAPTURE_HEIGHT) {
                                int offset = (y * CAPTURE_WIDTH + x) * 3;
                                total_r += data[offset + 0];
                                total_g += data[offset + 1];
                                total_b += data[offset + 2];
                                count++;
                        }
                }
        }

        result->r = count > 0 ? total_r / count : 0;
        result->g = count > 0 ? total_g / count : 0;
        result->b = count > 0 ? total_b / count : 0;
}

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
                memset(g_final_buffer, 0, sizeof(g_final_buffer));

                int buffer_index = 0;

                // 1. LEFT EDGE: Bottom to top
                for (int y = CAPTURE_HEIGHT - CAPTURE_DEPTH; y >= 0; y -= CAPTURE_DEPTH) {
                        int x = 0;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                // 2. TOP EDGE: Left to right
                for (int x = 0; x < CAPTURE_WIDTH; x += CAPTURE_DEPTH) {
                        int y = 0;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                // 3. RIGHT EDGE: Top to bottom
                for (int y = 0; y < CAPTURE_HEIGHT; y += CAPTURE_DEPTH) {
                        int x = CAPTURE_WIDTH - CAPTURE_DEPTH;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                ssize_t result = serial_tx((uint8_t *)g_final_buffer, sizeof(g_final_buffer));

                if (result < 0) {
                        printf("Serial error\n");
                        exit(1);
                }

                gst_buffer_unmap(buffer, &map);
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
}

static void start_gstreamer(int fd, int node) {
        GstElement *pipewiresrc, *appsink, *videorate, *videoscale, *videoconvert, *queue;
        GstCaps *caps;
        gchar *path;

        gst_init(NULL, NULL);

        g_pipeline = gst_pipeline_new("capture");
        if (!g_pipeline) {
                g_printerr("Failed to create pipeline\n");
                return;
        }

        pipewiresrc = gst_element_factory_make("pipewiresrc", NULL);
        videoconvert = gst_element_factory_make("videoconvert", NULL);
        videoscale = gst_element_factory_make("videoscale", NULL);
        videorate = gst_element_factory_make("videorate", NULL);
        queue = gst_element_factory_make("queue", NULL);
        appsink = gst_element_factory_make("appsink", NULL);

        if (!pipewiresrc || !videoconvert || !videoscale || !videorate || !queue || !appsink) {
                g_printerr("Failed to create elements\n");
                return;
        }

        g_object_set(queue, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", 0, "leaky",
                     2, NULL);

        g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, "max-buffers", 1, "drop", TRUE,
                     NULL);
        g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

        path = g_strdup_printf("%u", node);
        g_object_set(pipewiresrc, "fd", fd, "path", path, NULL);
        g_free(path);

        gst_bin_add_many(GST_BIN(g_pipeline), pipewiresrc, videoconvert, videoscale, videorate,
                         queue, appsink, NULL);

        if (!gst_element_link_many(pipewiresrc, videoconvert, videoscale, videorate, queue, NULL)) {
                g_printerr("Failed to link elements\n");
                return;
        }

        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                   G_TYPE_INT, CAPTURE_WIDTH, "height", G_TYPE_INT, CAPTURE_HEIGHT,
                                   "framerate", GST_TYPE_FRACTION, CAPTURE_FPS, 1, NULL);

        if (!gst_element_link_filtered(queue, appsink, caps)) {
                g_printerr("Failed to link with caps filter\n");
                gst_caps_unref(caps);
                return;
        }

        gst_caps_unref(caps);

        gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
        g_print("Pipeline running\n");
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
                g_print("PipeWire Node: %d\n", node);
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

        g_print("Session created\n");
        g_session = session;

        xdp_session_start(session, NULL, NULL, start_session_cb, NULL);
}

static int send_config(uint8_t brightness, uint8_t saturation_boost) {
        uint8_t config_packet[4] = {
            0xFF,            // Magic byte
            0xAA,            // Config identifier
            brightness,      // 0-255
            saturation_boost // 0-100
        };

        ssize_t result = serial_tx(config_packet, sizeof(config_packet));
        if (result < 0) {
                printf("Config send failed\n");
                return -1;
        }

        usleep(600000);
        return 0;
}

int main(int argc, const char *argv[]) {
        uint8_t brightness = BRIGHTNESS;
        uint8_t saturation = SATURATION;

        if (argc >= 2) {
                brightness = atoi(argv[1]);
        }

        if (argc >= 3) {
                saturation = atoi(argv[2]);
        }

        if (serial_init("/dev/ttyUSB0", 921600, 1000) == -1) {
                printf("No device found\n");
                return -1;
        }

        if (send_config(brightness, saturation) == -1) {
                return -1;
        }

        printf("Config sent: brightness=%d, saturation=%d\n", brightness, saturation);

        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        XdpPortal *portal = xdp_portal_new();

        xdp_portal_create_screencast_session(portal, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
                                             XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_TRANSIENT,
                                             NULL, NULL, create_session_cb, NULL);

        g_main_loop_run(loop);
        return 0;
}
