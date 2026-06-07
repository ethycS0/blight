#include <libportal/portal.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/video/format-utils.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#if defined(WIFI)
#include "wifi.h"
#endif

struct PipeWireCtx {
        struct pw_thread_loop *thread_loop;
        struct pw_context *context;
        struct pw_core *core;
        struct pw_stream *stream;
        struct spa_hook stream_listener;

        // Dynamic Resolution Trackers
        uint32_t real_width;
        uint32_t real_height;
        uint32_t real_stride;
} g_pw;

static void on_stream_process(void *data);
static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param);

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error) {
        if (error) {
                g_printerr("Stream error: %s\n", error);
        }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_stream_process,
    .param_changed = on_stream_param_changed,
};

#define CAPTURE_WIDTH 160
#define CAPTURE_HEIGHT 90
#define CAPTURE_DEPTH 10

typedef struct {
        unsigned char r, g, b;
} RGB;

typedef struct {
        float r, g, b;
} ColorFloat;

static RGB g_final_buffer[((CAPTURE_HEIGHT - CAPTURE_DEPTH) / CAPTURE_DEPTH + 1) +
                          ((CAPTURE_WIDTH + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH) +
                          ((CAPTURE_HEIGHT + CAPTURE_DEPTH - 1) / CAPTURE_DEPTH)];

static XdpSession *g_session;

static int g_brightness = 150;
static float g_saturation = 1.0f;
static float g_smoothing = 1.0f;

static struct {
        bool is_bgr;
} g_format_info = {true};

#define MAX_CACHED_BUFFERS 64
static struct {
        int fd;
        uint8_t *ptr;
        size_t size;
} mmap_cache[MAX_CACHED_BUFFERS];

static void clear_mmap_cache() {
        for (int i = 0; i < MAX_CACHED_BUFFERS; i++) {
                if (mmap_cache[i].fd > 0 && mmap_cache[i].ptr != NULL) {
                        munmap(mmap_cache[i].ptr, mmap_cache[i].size);
                        mmap_cache[i].fd = 0;
                        mmap_cache[i].ptr = NULL;
                }
        }
}

static uint8_t *get_cached_mmap(int fd, size_t size, uint32_t offset) {
        for (int i = 0; i < MAX_CACHED_BUFFERS; i++) {
                if (mmap_cache[i].fd == fd) {
                        return mmap_cache[i].ptr;
                }
        }
        uint8_t *ptr = mmap(NULL, size + offset, PROT_READ, MAP_SHARED, fd, 0);
        if (ptr != MAP_FAILED) {
                for (int i = 0; i < MAX_CACHED_BUFFERS; i++) {
                        if (mmap_cache[i].fd == 0) {
                                mmap_cache[i].fd = fd;
                                mmap_cache[i].ptr = ptr;
                                mmap_cache[i].size = size + offset;
                                return ptr;
                        }
                }
                return ptr;
        }
        return NULL;
}

static uint64_t get_time_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t last_frame_time = 0;

static int send_config(uint8_t brightness) {
        uint8_t config_packet[12];
        config_packet[0] = 0xFF;
        config_packet[1] = 0xAA;
        config_packet[2] = brightness;
        config_packet[3] = 0x00; // padding

        // Pass the floating point tuning parameters to ESP32
        memcpy(&config_packet[4], &g_saturation, sizeof(float));
        memcpy(&config_packet[8], &g_smoothing, sizeof(float));

        ssize_t result = 0;

#if defined(WIFI)
        result = wifi_tx(config_packet, sizeof(config_packet));
#endif

        if (result < 0) {
#ifdef DEBUG
                printf("Config send failed\n");
#endif
                return -1;
        }

        usleep(600000);
        return 0;
}

static void average_pixel_box(const unsigned char *data, size_t max_mapped_size, int start_x,
                              int start_y, int box_w, int box_h, int stride, RGB *result) {
        unsigned int total_r = 0, total_g = 0, total_b = 0;
        int count = 0;

        if (g_format_info.is_bgr) {
                for (int dy = 0; dy < box_h; dy += CAPTURE_DEPTH) {
                        int current_y = start_y + dy;
                        if ((size_t)(current_y * stride) >= max_mapped_size)
                                break;

                        const unsigned char *row = data + (current_y * stride);

                        for (int dx = 0; dx < box_w; dx += CAPTURE_DEPTH) {
                                size_t byte_offset = (start_x + dx) * 4;
                                if ((size_t)(current_y * stride) + byte_offset + 3 >=
                                    max_mapped_size)
                                        break;

                                const unsigned char *px = row + byte_offset;
                                total_b += px[0];
                                total_g += px[1];
                                total_r += px[2];
                                count++;
                        }
                }
        } else {
                for (int dy = 0; dy < box_h; dy += CAPTURE_DEPTH) {
                        int current_y = start_y + dy;
                        if ((size_t)(current_y * stride) >= max_mapped_size)
                                break;

                        const unsigned char *row = data + (current_y * stride);

                        for (int dx = 0; dx < box_w; dx += CAPTURE_DEPTH) {
                                size_t byte_offset = (start_x + dx) * 4;
                                if ((size_t)(current_y * stride) + byte_offset + 3 >=
                                    max_mapped_size)
                                        break;

                                const unsigned char *px = row + byte_offset;
                                total_r += px[0];
                                total_g += px[1];
                                total_b += px[2];
                                count++;
                        }
                }
        }

        if (count == 0)
                count = 1;
        result->r = total_r / count;
        result->g = total_g / count;
        result->b = total_b / count;
}

static void on_stream_process(void *data) {
        struct PipeWireCtx *ctx = data;
        struct pw_buffer *pw_buf;

        if ((pw_buf = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
                return;
        }

        uint64_t now = get_time_ns();
        if (now - last_frame_time < 1000000000ULL / 60) {
                pw_stream_queue_buffer(ctx->stream, pw_buf);
                return;
        }
        last_frame_time = now;

        if (ctx->real_width == 0 || ctx->real_height == 0) {
                pw_stream_queue_buffer(ctx->stream, pw_buf);
                return;
        }

        struct spa_buffer *buf = pw_buf->buffer;
        if (!buf || !buf->datas || buf->n_datas == 0) {
                pw_stream_queue_buffer(ctx->stream, pw_buf);
                return;
        }

        uint32_t current_stride = ctx->real_stride;
        if (buf->datas[0].chunk != NULL && buf->datas[0].chunk->stride > 0) {
                current_stride = buf->datas[0].chunk->stride;
        }

        uint32_t size = buf->datas[0].maxsize;
        uint32_t true_size = current_stride * ctx->real_height;
        if (size < true_size) {
                size = true_size;
        }

        uint8_t *raw_pixels = NULL;
        bool mapped = false;
        int fd = -1;
        uint32_t offset = buf->datas[0].mapoffset;
        struct dma_buf_sync sync = {0};

        if (buf->datas[0].data != NULL) {
                // PipeWire already mapped it for us
                raw_pixels = buf->datas[0].data;
        } else if (buf->datas[0].type == SPA_DATA_MemFd || buf->datas[0].type == SPA_DATA_DmaBuf) {
                fd = buf->datas[0].fd;
                raw_pixels = get_cached_mmap(fd, size, offset);
                if (raw_pixels != NULL) {
                        mapped = true;
                        raw_pixels += offset;
                }
        }

        if (raw_pixels != NULL) {
                if (buf->datas[0].type == SPA_DATA_DmaBuf && fd != -1) {
                        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
                        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
                } else if (buf->datas[0].type == SPA_DATA_DmaBuf && buf->datas[0].fd != -1) {
                        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
                        ioctl(buf->datas[0].fd, DMA_BUF_IOCTL_SYNC, &sync);
                }

                for (uint32_t i = 0; i < size; i += 4096) {
                        if (raw_pixels[i] != 0) {
                                break;
                        }
                }

                memset(g_final_buffer, 0, sizeof(g_final_buffer));
                int buffer_index = 0;

                int real_box_w = (CAPTURE_DEPTH * ctx->real_width) / CAPTURE_WIDTH;
                int real_box_h = (CAPTURE_DEPTH * ctx->real_height) / CAPTURE_HEIGHT;
                if (real_box_w < 1)
                        real_box_w = 1;
                if (real_box_h < 1)
                        real_box_h = 1;

                // 1. LEFT EDGE: Bottom to top
                for (int y = CAPTURE_HEIGHT - CAPTURE_DEPTH; y >= 0; y -= CAPTURE_DEPTH) {
                        int real_x = 0;
                        int real_y = (y * ctx->real_height) / CAPTURE_HEIGHT;

                        average_pixel_box(raw_pixels, size, real_x, real_y, real_box_w, real_box_h,
                                          current_stride, &g_final_buffer[buffer_index++]);
                }

                // 2. TOP EDGE: Left to right
                for (int x = 0; x < CAPTURE_WIDTH; x += CAPTURE_DEPTH) {
                        int real_x = (x * ctx->real_width) / CAPTURE_WIDTH;
                        int real_y = 0;

                        average_pixel_box(raw_pixels, size, real_x, real_y, real_box_w, real_box_h,
                                          current_stride, &g_final_buffer[buffer_index++]);
                }

                // 3. RIGHT EDGE: Top to bottom
                for (int y = 0; y < CAPTURE_HEIGHT; y += CAPTURE_DEPTH) {
                        int real_x =
                            ((CAPTURE_WIDTH - CAPTURE_DEPTH) * ctx->real_width) / CAPTURE_WIDTH;
                        int real_y = (y * ctx->real_height) / CAPTURE_HEIGHT;

                        average_pixel_box(raw_pixels, size, real_x, real_y, real_box_w, real_box_h,
                                          current_stride, &g_final_buffer[buffer_index++]);
                }

                int num_leds = sizeof(g_final_buffer) / sizeof(RGB);

#ifdef DEBUG
                // PRINT TO TERMINAL
                printf("\r");
                for (int i = 0; i < num_leds; i++) {
                        printf("\x1b[48;2;%d;%d;%dm  ", g_final_buffer[i].r, g_final_buffer[i].g,
                               g_final_buffer[i].b);
                }
                printf("\x1b[0m"); // reset color
                fflush(stdout);
#endif

#if defined(WIFI)
                ssize_t tx_res = wifi_tx((uint8_t *)g_final_buffer, sizeof(g_final_buffer));
#ifdef DEBUG
                if (tx_res < 0) {
                        printf("\r[FRAME] Transmission error\n");
                }
#endif
#endif

                if (buf->datas[0].type == SPA_DATA_DmaBuf && fd != -1) {
                        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
                } else if (buf->datas[0].type == SPA_DATA_DmaBuf && buf->datas[0].fd != -1) {
                        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                        ioctl(buf->datas[0].fd, DMA_BUF_IOCTL_SYNC, &sync);
                }
        }

        pw_stream_queue_buffer(ctx->stream, pw_buf);
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
        struct PipeWireCtx *ctx = data;

        if (param == NULL || id != SPA_PARAM_Format) {
                return;
        }

        struct spa_video_info_raw info;
        if (spa_format_video_raw_parse(param, &info) < 0) {
                g_printerr("Failed to parse video format layout\n");
                return;
        }

        if (info.format == SPA_VIDEO_FORMAT_BGRx || info.format == SPA_VIDEO_FORMAT_BGRA) {
                g_format_info.is_bgr = true;
        } else {
                g_format_info.is_bgr = false;
        }

        ctx->real_width = info.size.width;
        ctx->real_height = info.size.height;
        ctx->real_stride = ctx->real_width * 4;

        clear_mmap_cache();

#ifdef DEBUG
        g_print("\nScreen Capture Active: Natively sampling at %dx%d (Format: %s)\n",
                ctx->real_width, ctx->real_height,
                g_format_info.is_bgr ? "BGRx/BGRA" : "RGBx/RGBA");
#endif
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
#ifdef DEBUG
                g_print("PipeWire Node: %d\n", node);
#endif
                g_variant_unref(options);
        }

        int fd = xdp_session_open_pipewire_remote(session);
#ifdef DEBUG
        g_print("PipeWire FD: %d\n", fd);
#endif

#if defined(WIFI)
        if (wifi_init("192.168.1.100", 4210, 1000) == -1) {
#ifdef DEBUG
                printf("No device found\n");
#endif
                exit(1);
        }
        if (send_config(g_brightness) == -1) {
#ifdef DEBUG
                printf("Failed to Send Config.\n");
#endif
                exit(1);
        }
#ifdef DEBUG
        printf("Config sent: brightness=%d\n", g_brightness);
#endif
#endif

        pw_init(NULL, NULL);

        g_pw.thread_loop = pw_thread_loop_new("pipewire-render-thread", NULL);
        struct pw_loop *loop = pw_thread_loop_get_loop(g_pw.thread_loop);

        g_pw.context = pw_context_new(loop, NULL, 0);
        g_pw.core = pw_context_connect_fd(g_pw.context, fd, NULL, 0);
        if (!g_pw.core) {
                g_printerr("Failed to connect to PipeWire remote FD\n");
                return;
        }

        g_pw.stream =
            pw_stream_new(g_pw.core, "screencast-capture",
                          pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                            "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL));

        pw_stream_add_listener(g_pw.stream, &g_pw.stream_listener, &stream_events, &g_pw);

        uint8_t buffer[2048];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[2];

        params[0] = spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
            SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(
                9, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_xRGB,
                SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_ABGR),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(320, 240), &SPA_RECTANGLE(1, 1),
                                           &SPA_RECTANGLE(16384, 16384)),
            SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1),
                                          &SPA_FRACTION(1000, 1)),
            // Use 0 as modifier choice just to be safe but pipewire might negotiate without it
            SPA_FORMAT_VIDEO_modifier, SPA_POD_CHOICE_ENUM_Long(2, 0, 0), NULL);

        params[1] = spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
            SPA_POD_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)),
            NULL);

        int res_conn =
            pw_stream_connect(g_pw.stream, PW_DIRECTION_INPUT, node,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 2);

        if (res_conn < 0) {
                g_printerr("Stream connection failed\n");
                return;
        }

        pw_thread_loop_start(g_pw.thread_loop);
#ifdef DEBUG
        g_print("PipeWire stream processing thread started natively.\n");
#endif
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

#ifdef DEBUG
        g_print("Session created\n");
#endif
        g_session = session;

        xdp_session_start(session, NULL, NULL, start_session_cb, NULL);
}

int main(int argc, const char *argv[]) {
        if (argc >= 2) {
                g_brightness = atoi(argv[1]);
        }
        if (argc >= 3) {
                g_saturation = atof(argv[2]);
        }
        if (argc >= 4) {
                g_smoothing = atof(argv[3]);
                if (g_smoothing < 0.1f)
                        g_smoothing = 0.1f;
                if (g_smoothing > 1.0f)
                        g_smoothing = 1.0f;
        }

        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        XdpPortal *portal = xdp_portal_new();

        xdp_portal_create_screencast_session(portal, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
                                             XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_TRANSIENT,
                                             NULL, NULL, create_session_cb, NULL);

        g_main_loop_run(loop);

        return 0;
}
