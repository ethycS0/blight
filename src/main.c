#include <libportal/portal.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define CAPTURE_FRAMES 24

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

// Fixed light quality settings. The host no longer exposes these as CLI
// options. Saturation is gone entirely: the color that reaches the LEDs is the
// physically-correct average of the captured box (see average_pixel_box).
#define BRIGHTNESS 250

// ESP32 command packets share the 0xFF 0xAA magic prefix. Frames are sent as
// raw RGB payloads (see g_final_buffer) and are told apart by their size.
#define CMD_CONFIG 0x01 // [3] = brightness
#define CMD_STATIC 0x02 // [3..5] = R, G, B
#define CMD_LIVE 0x03   // no payload, resume screen-capture mode
#define CMD_FORCE_LIVE 0x04 // no payload, resume live and clear phone override

// How often to re-send the config/live handshake while streaming.
#define HANDSHAKE_INTERVAL_MS 3000

#define ESP_HOST "192.168.1.100"
#define ESP_PORT 4210

// Optional overrides, handy for testing against a local UDP listener.
static const char *esp_host(void) {
        const char *h = getenv("BLIGHT_ESP");
        return (h && *h) ? h : ESP_HOST;
}

static uint16_t esp_port(void) {
        const char *p = getenv("BLIGHT_PORT");
        return (p && *p) ? (uint16_t)atoi(p) : (uint16_t)ESP_PORT;
}

// Byte offsets of R/G/B within the negotiated pixel format. Defaults to BGRx.
static struct {
        int r, g, b;
} g_format_idx = {2, 1, 0};

// Bit shifts derived from g_format_idx, used to pull R/G/B out of a single
// 32-bit pixel load. Reading the pixel as one word instead of three byte loads
// matters a lot here: dmabuf screenshots are typically mapped uncached, so
// every individual load stalls on memory and three of them triple the cost.
static int g_shift_r = 16, g_shift_g = 8, g_shift_b = 0;

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

// Content region of the frame, in real capture pixels, after cropping away
// letterbox (top/bottom) and pillarbox (left/right) black bars. Recomputed
// periodically rather than every frame; when !valid the full frame is used.
typedef struct {
        int x0, y0, x1, y1; // x1/y1 are exclusive
        bool valid;
} ContentCrop;

static ContentCrop g_crop = {0, 0, 0, 0, false};
static uint64_t last_detect_time = 0;
static uint64_t last_handshake_time = 0;

// Default light-quality settings (consumed by Settings / settings() below).
// Rationale for each value lives at its point of use. Everything is
// overridable through the environment for testing.
#define DEFAULT_BOX_SAMPLES 2048    // pixels sampled per edge box
#define DEFAULT_VIBRANCE 0.3f       // saturation lift for bright, vivid colors
#define DEFAULT_VIBRANCE_REF 80.0f  // luminance at which vibrance is full
#define DEFAULT_BRIGHT_REF 70.0f    // scene luminance for full LED brightness
#define DEFAULT_MIN_BRIGHT 0.10f    // auto-brightness floor, 0..1
#define DEFAULT_BLACK_DETECT 1      // skip letterbox/pillarbox black bars
#define DEFAULT_BLACK_THRESHOLD 16  // max channel value still counted as black
#define DEFAULT_DETECT_INTERVAL_MS 1000 // how often to re-scan for black bars

static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
}

// Runtime-tunable light quality. The host intentionally exposes no CLI flags,
// but every value can be overridden through the environment for quick
// experimentation, e.g. BLIGHT_SAMPLES=4096 BLIGHT_VIBRANCE=0 ./blight live.
typedef struct {
        int samples;        // pixels sampled per edge box
        float vibrance;     // saturation lift applied to vivid colors (0 = off)
        float vibrance_ref; // luminance at which vibrance reaches full strength
        float bright_ref;   // scene luminance mapped to full LED brightness
        float min_bright;   // floor for auto-brightness, 0..1
        int auto_bright;    // dim the LEDs when the scene is dark
        int black_detect;   // crop away letterbox/pillarbox black bars
        int black_threshold;// max channel value still considered black
        int detect_interval_ms; // minimum time between bar-detection scans
} Settings;

static float env_float(const char *name, float fallback) {
        const char *e = getenv(name);
        return (e && *e) ? strtof(e, NULL) : fallback;
}

static int env_int(const char *name, int fallback) {
        const char *e = getenv(name);
        return (e && *e) ? atoi(e) : fallback;
}

static const Settings *settings(void) {
        static Settings s;
        static int loaded = 0;
        if (!loaded) {
                loaded = 1;
                s.samples = env_int("BLIGHT_SAMPLES", DEFAULT_BOX_SAMPLES);
                s.vibrance = env_float("BLIGHT_VIBRANCE", DEFAULT_VIBRANCE);
                s.vibrance_ref = env_float("BLIGHT_VIBRANCE_REF", DEFAULT_VIBRANCE_REF);
                s.bright_ref = env_float("BLIGHT_BRIGHT_REF", DEFAULT_BRIGHT_REF);
                s.min_bright = env_float("BLIGHT_MIN_BRIGHT", DEFAULT_MIN_BRIGHT);
                s.auto_bright = env_int("BLIGHT_AUTO_BRIGHT", 0);
                s.black_detect = env_int("BLIGHT_BLACK_DETECT", DEFAULT_BLACK_DETECT);
                s.black_threshold = env_int("BLIGHT_BLACK_THRESHOLD", DEFAULT_BLACK_THRESHOLD);
                s.detect_interval_ms =
                    env_int("BLIGHT_DETECT_INTERVAL_MS", DEFAULT_DETECT_INTERVAL_MS);

                if (s.samples < 16)
                        s.samples = 16;
                if (s.vibrance < 0.0f)
                        s.vibrance = 0.0f;
                if (s.vibrance > 1.0f)
                        s.vibrance = 1.0f;
                if (s.vibrance_ref < 1.0f)
                        s.vibrance_ref = 1.0f;
                if (s.bright_ref < 1.0f)
                        s.bright_ref = 1.0f;
                s.min_bright = clampf(s.min_bright, 0.0f, 1.0f);
                if (s.black_threshold < 0)
                        s.black_threshold = 0;
                if (s.black_threshold > 255)
                        s.black_threshold = 255;
                if (s.detect_interval_ms < 100)
                        s.detect_interval_ms = 100;
        }
        return &s;
}

static int target_box_samples(void) { return settings()->samples; }

// Boosts saturation with a vibrance curve: washed-out colors get lifted a lot,
// vivid ones only slightly, and value (the max channel) is preserved. The lift
// is faded in with luminance so dark, faintly-tinted pixels are left alone --
// boosting them used to saturate near-black averages into a visible hue (dark
// scenes turned blue).
static void apply_vibrance(float *r, float *g, float *b) {
        const Settings *st = settings();
        if (st->vibrance <= 0.0f)
                return;

        float mx = fmaxf(*r, fmaxf(*g, *b));
        float mn = fminf(*r, fminf(*g, *b));
        float delta = mx - mn;
        if (mx <= 0.0f || delta < 2.0f)
                return; // essentially grey: no hue to boost

        float amount = st->vibrance * clampf(mx / st->vibrance_ref, 0.0f, 1.0f);
        if (amount <= 0.0f)
                return;

        float h;
        if (mx == *r && *g >= *b)
                h = (*g - *b) / delta;
        else if (mx == *r)
                h = (*g - *b) / delta + 6.0f;
        else if (mx == *g)
                h = (*b - *r) / delta + 2.0f;
        else
                h = (*r - *g) / delta + 4.0f;
        h /= 6.0f;

        float s = delta / mx;
        float ns = s + (1.0f - s) * amount;
        if (ns > 1.0f)
                ns = 1.0f;

        int i = (int)(h * 6.0f);
        float f = h * 6.0f - i;
        float p = mx * (1.0f - ns);
        float q = mx * (1.0f - f * ns);
        float t = mx * (1.0f - (1.0f - f) * ns);

        switch (i % 6) {
        case 0: *r = mx; *g = t; *b = p; break;
        case 1: *r = q; *g = mx; *b = p; break;
        case 2: *r = p; *g = mx; *b = t; break;
        case 3: *r = p; *g = q; *b = mx; break;
        case 4: *r = t; *g = p; *b = mx; break;
        default: *r = mx; *g = p; *b = q; break;
        }
}

static int send_command(uint8_t cmd, const uint8_t *payload, size_t len) {
        uint8_t packet[8];
        packet[0] = 0xFF;
        packet[1] = 0xAA;
        packet[2] = cmd;
        for (size_t i = 0; i < len; i++)
                packet[3 + i] = payload[i];

        // Commands are always at least 4 bytes so the receiver can tell them
        // apart from raw frames purely by size.
        size_t total = 3 + len;
        if (total < 4)
                packet[total++] = 0x00;

        ssize_t result = -1;
#if defined(WIFI)
        result = wifi_tx(packet, total);
#endif

        if (result < 0) {
#ifdef DEBUG
                printf("Command 0x%02X send failed\n", cmd);
#endif
                return -1;
        }

        return 0;
}

// Re-asserts the ESP32's configuration and live mode. Sent once at session
// start and then periodically (see HANDSHAKE_INTERVAL_MS) so the strip still
// recovers if the ESP rebooted or missed the initial UDP packets. `force`
// additionally clears any phone/web override, so starting a new live session
// reclaims control from the phone.
static void send_handshake(bool force) {
        uint8_t brightness = BRIGHTNESS;
        send_command(CMD_CONFIG, &brightness, 1);
        send_command(force ? CMD_FORCE_LIVE : CMD_LIVE, NULL, 0);
}

// Averages a box of pixels in perceptual (gamma-encoded) space, then applies a
// fixed vibrance lift. This keeps the LEDs as bright as the screen while
// avoiding the washed-out look of a linear-light mean re-encoded through sRGB.
static void average_pixel_box(const unsigned char *data, size_t max_mapped_size, int start_x,
                              int start_y, int box_w, int box_h, int stride, RGB *result) {
        // Read whole rows at an adaptive vertical stride. Contiguous reads make
        // far better use of uncached dmabuf memory than a strided 2D grid: a
        // single cache line then yields ~16 pixels instead of one, so we can
        // afford many more samples for the same CPU. Cost stays bounded because
        // the number of rows is derived from the sample budget.
        int rows = target_box_samples() / box_w;
        if (rows < 1)
                rows = 1;
        else if (rows > box_h)
                rows = box_h;
        int step_y = box_h / rows;
        if (step_y < 1)
                step_y = 1;

        unsigned int total_r = 0, total_g = 0, total_b = 0;
        int count = 0;

        for (int dy = 0; dy < box_h; dy += step_y) {
                int current_y = start_y + dy;
                if ((size_t)(current_y * stride) >= max_mapped_size)
                        break;

                const unsigned char *row = data + (current_y * stride);

                for (int dx = 0; dx < box_w; dx++) {
                        size_t byte_offset = (start_x + dx) * 4;
                        if ((size_t)(current_y * stride) + byte_offset + 3 >= max_mapped_size)
                                break;

                        const unsigned char *px = row + byte_offset;
                        uint32_t word;
                        memcpy(&word, px, sizeof(word));
                        total_r += (word >> g_shift_r) & 0xFF;
                        total_g += (word >> g_shift_g) & 0xFF;
                        total_b += (word >> g_shift_b) & 0xFF;
                        count++;
                }
        }

        if (count == 0)
                count = 1;

        float r = total_r / (float)count;
        float g = total_g / (float)count;
        float b = total_b / (float)count;

        apply_vibrance(&r, &g, &b);

        result->r = (unsigned char)clampf(r + 0.5f, 0.0f, 255.0f);
        result->g = (unsigned char)clampf(g + 0.5f, 0.0f, 255.0f);
        result->b = (unsigned char)clampf(b + 0.5f, 0.0f, 255.0f);
}

// Dims the whole strip to follow the scene's overall brightness. Without this
// the LEDs always ran at full power, so a dark scene (or a dim room) still
// glowed. The scale is derived from the perceptual luminance of the edge
// colors and never goes brighter than 1.0, so bright scenes are untouched.
static void apply_auto_brightness(RGB *buf, int count) {
        const Settings *st = settings();
        if (!st->auto_bright)
                return;

        float lum = 0.0f;
        for (int i = 0; i < count; i++)
                lum += 0.2126f * buf[i].r + 0.7152f * buf[i].g + 0.0722f * buf[i].b;
        lum /= (float)count;

        // Gamma < 1 keeps mid-dark scenes from collapsing too aggressively.
        float scale = powf(lum / st->bright_ref, 0.8f);
        scale = clampf(scale, st->min_bright, 1.0f);
        if (scale >= 1.0f)
                return;

        for (int i = 0; i < count; i++) {
                buf[i].r = (unsigned char)(buf[i].r * scale + 0.5f);
                buf[i].g = (unsigned char)(buf[i].g * scale + 0.5f);
                buf[i].b = (unsigned char)(buf[i].b * scale + 0.5f);
        }
}

// ---------------------------------------------------------------------------
// Black bar (letterbox / pillarbox) detection.
//
// The generic approach used by bias-lighting software: probe inward from each
// edge of the frame along a handful of lines and find where the black bars end.
// A robust median over the probes gives the content rectangle, which the LED
// zones are then mapped onto. Detection is deliberately periodic (see
// detect_interval_ms) rather than per-frame, so the extra uncached reads are
// amortized to near nothing.
// ---------------------------------------------------------------------------

#define DETECT_PROBES 16

static inline bool pixel_is_black(const unsigned char *data, size_t max_size, uint32_t stride,
                                  int x, int y) {
        size_t off = (size_t)y * stride + (size_t)x * 4;
        if (off + 4 > max_size)
                return false;

        uint32_t word;
        memcpy(&word, data + off, sizeof(word));
        unsigned r = (word >> g_shift_r) & 0xFF;
        unsigned g = (word >> g_shift_g) & 0xFF;
        unsigned b = (word >> g_shift_b) & 0xFF;
        unsigned mx = r > g ? r : g;
        mx = mx > b ? mx : b;
        return mx <= (unsigned)settings()->black_threshold;
}

// Walks inward from one edge along a fixed row (vertical=false) or column
// (vertical=true) and returns how many leading pixels are black. Returns -1 if
// the run reached max_depth, i.e. the whole probe line is black. That happens
// when the line sits inside a perpendicular bar (a pillarbox column while
// probing for letterbox bars), so those probes are discarded rather than being
// mistaken for a huge bar.
static int scan_black_run(const unsigned char *data, size_t max_size, uint32_t stride, int width,
                          int height, bool vertical, int line, int dir, int max_depth) {
        for (int d = 0; d < max_depth; d++) {
                int x, y;
                if (vertical) {
                        x = line;
                        y = dir > 0 ? d : height - 1 - d;
                } else {
                        y = line;
                        x = dir > 0 ? d : width - 1 - d;
                }
                if (x < 0 || x >= width || y < 0 || y >= height)
                        return -1;
                if (!pixel_is_black(data, max_size, stride, x, y))
                        return d;
        }
        return -1;
}

// Median of the probes that produced a finite run; 0 if every probe was
// discarded. Median (not mean) keeps a few stray dark-content probes from
// dragging the detected bar around.
static int median_run(const int *runs, int n) {
        int vals[DETECT_PROBES];
        int m = 0;
        for (int i = 0; i < n; i++) {
                if (runs[i] >= 0)
                        vals[m++] = runs[i];
        }
        if (m == 0)
                return 0;

        for (int i = 1; i < m; i++) {
                int v = vals[i], j = i - 1;
                while (j >= 0 && vals[j] > v) {
                        vals[j + 1] = vals[j];
                        j--;
                }
                vals[j + 1] = v;
        }
        return vals[m / 2];
}

static void detect_content_crop(const unsigned char *data, size_t max_size, uint32_t stride,
                                int width, int height) {
        const int max_depth_x = width / 4;
        const int max_depth_y = height / 4;

        // Pass 1: pillarbox. Probe rows in the middle half of the frame so any
        // letterbox bars (fully black rows) are never scanned.
        int left_runs[DETECT_PROBES], right_runs[DETECT_PROBES];
        for (int i = 0; i < DETECT_PROBES; i++) {
                int y = height / 4 + (int)((int64_t)(i + 1) * (height / 2) / (DETECT_PROBES + 1));
                left_runs[i] =
                    scan_black_run(data, max_size, stride, width, height, false, y, +1, max_depth_x);
                right_runs[i] =
                    scan_black_run(data, max_size, stride, width, height, false, y, -1, max_depth_x);
        }
        int left = median_run(left_runs, DETECT_PROBES);
        int right = median_run(right_runs, DETECT_PROBES);

        int cx0 = left;
        int cx1 = width - right;
        int cw = cx1 - cx0;
        if (cw < width / 4) { // implausible: treat as no pillarbox
                cx0 = 0;
                cx1 = width;
                cw = width;
                left = right = 0;
        }

        // Pass 2: letterbox. Probe columns in the middle half of the detected
        // content width so residual pillarbox columns are avoided.
        int top_runs[DETECT_PROBES], bottom_runs[DETECT_PROBES];
        for (int i = 0; i < DETECT_PROBES; i++) {
                int x = cx0 + cw / 4 +
                        (int)((int64_t)(i + 1) * (cw / 2) / (DETECT_PROBES + 1));
                top_runs[i] =
                    scan_black_run(data, max_size, stride, width, height, true, x, +1, max_depth_y);
                bottom_runs[i] =
                    scan_black_run(data, max_size, stride, width, height, true, x, -1, max_depth_y);
        }
        int top = median_run(top_runs, DETECT_PROBES);
        int bottom = median_run(bottom_runs, DETECT_PROBES);

        int x0 = left;
        int y0 = top;
        int x1 = width - right;
        int y1 = height - bottom;

        // A dark scene can look like a bar. Refuse to crop below a sane minimum
        // (standard 16:9 / 4:3 content never loses more than ~40%).
        if (x1 - x0 < width * 3 / 5 || y1 - y0 < height * 3 / 5) {
                g_crop.x0 = 0;
                g_crop.y0 = 0;
                g_crop.x1 = width;
                g_crop.y1 = height;
                g_crop.valid = false;
                return;
        }

        g_crop.x0 = x0;
        g_crop.y0 = y0;
        g_crop.x1 = x1;
        g_crop.y1 = y1;
        g_crop.valid = (x0 > 0 || y0 > 0 || x1 < width || y1 < height);

#ifdef DEBUG
        g_print("[detect] bars L%d R%d T%d B%d -> content %dx%d @ (%d,%d)%s\n", left, right, top,
                bottom, x1 - x0, y1 - y0, x0, y0, g_crop.valid ? "" : " (full frame)");
#endif
}

// Runs detection at most once per detect_interval_ms. Returns true if a fresh
// scan was performed (the crop is then current).
static bool maybe_detect_crop(uint64_t now, const unsigned char *data, size_t max_size,
                              uint32_t stride, int width, int height) {
        const Settings *st = settings();
        if (!st->black_detect) {
                g_crop.valid = false;
                return false;
        }
        if (last_detect_time != 0 &&
            now - last_detect_time < (uint64_t)st->detect_interval_ms * 1000000ULL)
                return false;

        last_detect_time = now;
        detect_content_crop(data, max_size, stride, width, height);
        return true;
}

static void on_stream_process(void *data) {
        struct PipeWireCtx *ctx = data;
        struct pw_buffer *pw_buf;

        if ((pw_buf = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
                return;
        }

        uint64_t now = get_time_ns();
        if (now - last_frame_time < 1000000000ULL / CAPTURE_FRAMES) {
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

                // Periodically re-scan for letterbox/pillarbox bars and update
                // the content crop used to place the LED zones.
                maybe_detect_crop(now, raw_pixels, size, current_stride, ctx->real_width,
                                  ctx->real_height);

                // Re-assert config/live now and then so a rebooted or wedged
                // ESP32 recovers on its own.
                if (now - last_handshake_time >= (uint64_t)HANDSHAKE_INTERVAL_MS * 1000000ULL) {
                        last_handshake_time = now;
                        send_handshake(false);
                }

                memset(g_final_buffer, 0, sizeof(g_final_buffer));
                int buffer_index = 0;

                // Map the LED zones onto the content region (full frame when no
                // bars were detected). "thick" is the physical size of one LED
                // segment; the tiling dimension follows the content extent so
                // the strips always cover exactly the visible picture.
                int content_x0 = 0, content_y0 = 0;
                int content_w = ctx->real_width, content_h = ctx->real_height;
                if (g_crop.valid) {
                        content_x0 = g_crop.x0;
                        content_y0 = g_crop.y0;
                        content_w = g_crop.x1 - g_crop.x0;
                        content_h = g_crop.y1 - g_crop.y0;
                }

                int thick_w = (CAPTURE_DEPTH * ctx->real_width) / CAPTURE_WIDTH;
                int thick_h = (CAPTURE_DEPTH * ctx->real_height) / CAPTURE_HEIGHT;
                int edge_box_h = (CAPTURE_DEPTH * content_h) / CAPTURE_HEIGHT;
                int edge_box_w = (CAPTURE_DEPTH * content_w) / CAPTURE_WIDTH;
                if (thick_w < 1)
                        thick_w = 1;
                if (thick_h < 1)
                        thick_h = 1;
                if (edge_box_h < 1)
                        edge_box_h = 1;
                if (edge_box_w < 1)
                        edge_box_w = 1;

                // 1. LEFT EDGE: Bottom to top
                for (int y = CAPTURE_HEIGHT - CAPTURE_DEPTH; y >= 0; y -= CAPTURE_DEPTH) {
                        int real_x = content_x0;
                        int real_y = content_y0 + (y * content_h) / CAPTURE_HEIGHT;

                        average_pixel_box(raw_pixels, size, real_x, real_y, thick_w, edge_box_h,
                                          current_stride, &g_final_buffer[buffer_index++]);
                }

                // 2. TOP EDGE: Left to right
                for (int x = 0; x < CAPTURE_WIDTH; x += CAPTURE_DEPTH) {
                        int real_x = content_x0 + (x * content_w) / CAPTURE_WIDTH;
                        int real_y = content_y0;

                        average_pixel_box(raw_pixels, size, real_x, real_y, edge_box_w, thick_h,
                                          current_stride, &g_final_buffer[buffer_index++]);
                }

                // 3. RIGHT EDGE: Top to bottom
                for (int y = 0; y < CAPTURE_HEIGHT; y += CAPTURE_DEPTH) {
                        int real_x = content_x0 + content_w - thick_w;
                        int real_y = content_y0 + (y * content_h) / CAPTURE_HEIGHT;

                        average_pixel_box(raw_pixels, size, real_x, real_y, thick_w, edge_box_h,
                                          current_stride, &g_final_buffer[buffer_index++]);
                }

                int num_leds = sizeof(g_final_buffer) / sizeof(RGB);

                // Match overall output brightness to the scene before sending.
                apply_auto_brightness(g_final_buffer, num_leds);

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

        // Pixel formats differ in where R/G/B sit, including alpha-first and
        // x-first layouts. Pick the right byte offsets instead of assuming the
        // channels are always the first three bytes.
        switch (info.format) {
        case SPA_VIDEO_FORMAT_xBGR:
        case SPA_VIDEO_FORMAT_ABGR:
                g_format_idx.r = 3;
                g_format_idx.g = 2;
                g_format_idx.b = 1;
                break;
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA:
                g_format_idx.r = 0;
                g_format_idx.g = 1;
                g_format_idx.b = 2;
                break;
        case SPA_VIDEO_FORMAT_xRGB:
        case SPA_VIDEO_FORMAT_ARGB:
                g_format_idx.r = 1;
                g_format_idx.g = 2;
                g_format_idx.b = 3;
                break;
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA:
        default:
                g_format_idx.r = 2;
                g_format_idx.g = 1;
                g_format_idx.b = 0;
                break;
        }

        ctx->real_width = info.size.width;
        ctx->real_height = info.size.height;
        ctx->real_stride = ctx->real_width * 4;

        g_shift_r = g_format_idx.r * 8;
        g_shift_g = g_format_idx.g * 8;
        g_shift_b = g_format_idx.b * 8;

        clear_mmap_cache();

#ifdef DEBUG
        g_print("\nScreen Capture Active: Natively sampling at %dx%d (RGBA byte offsets %d/%d/%d)\n",
                ctx->real_width, ctx->real_height, g_format_idx.r, g_format_idx.g, g_format_idx.b);
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
        if (wifi_init(esp_host(), esp_port(), 1000) == -1) {
#ifdef DEBUG
                printf("No device found\n");
#endif
                exit(1);
        }
        send_handshake(true);
        last_handshake_time = get_time_ns();
#ifdef DEBUG
        printf("Live mode requested (brightness=%d)\n", BRIGHTNESS);
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

static void print_usage(const char *prog) {
        fprintf(stderr,
                "Usage:\n"
                "  %s live              Stream screen colors to the LEDs\n"
                "  %s static RRGGBB     Set a fixed color (no screen capture)\n"
                "\n"
                "Examples:\n"
                "  %s live\n"
                "  %s static FFFFFF\n"
                "  %s static 2a1a4d\n",
                prog, prog, prog, prog, prog);
}

static int parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
        if (s[0] == '#')
                s++;

        size_t len = strlen(s);
        if (len != 6 && len != 3)
                return -1;

        char expanded[7];
        if (len == 3) {
                expanded[0] = expanded[1] = s[0];
                expanded[2] = expanded[3] = s[1];
                expanded[4] = expanded[5] = s[2];
                expanded[6] = '\0';
                s = expanded;
        }

        char *end = NULL;
        long value = strtol(s, &end, 16);
        if (end == s || *end != '\0' || value < 0)
                return -1;

        *r = (value >> 16) & 0xFF;
        *g = (value >> 8) & 0xFF;
        *b = value & 0xFF;
        return 0;
}

static int run_static(const char *hex) {
        uint8_t r, g, b;
        if (parse_hex_color(hex, &r, &g, &b) == -1) {
                fprintf(stderr, "Invalid color '%s' (expected RRGGBB)\n", hex);
                return 1;
        }

#if defined(WIFI)
        if (wifi_init(esp_host(), esp_port(), 1000) == -1) {
                fprintf(stderr, "Could not reach ESP32\n");
                return 1;
        }

        uint8_t brightness = BRIGHTNESS;
        uint8_t color[3] = {r, g, b};

        // Fire-and-forget UDP: repeat a few times so a just-booted or briefly
        // asleep ESP32 is guaranteed to receive it, then exit. The ESP32 stays
        // in static mode indefinitely (it does not time out) until a live
        // frame arrives.
        for (int i = 0; i < 10; i++) {
                send_command(CMD_CONFIG, &brightness, 1);
                send_command(CMD_STATIC, color, sizeof(color));
                usleep(200000);
        }

        printf("Static color set to #%02X%02X%02X\n", r, g, b);
        wifi_close();
#else
        (void)r;
        (void)g;
        (void)b;
#endif
        return 0;
}

int main(int argc, const char *argv[]) {
        if (argc < 2) {
                print_usage(argv[0]);
                return 1;
        }

        if (strcmp(argv[1], "static") == 0) {
                if (argc < 3) {
                        fprintf(stderr, "Usage: %s static RRGGBB\n", argv[0]);
                        return 1;
                }
                return run_static(argv[2]);
        }

        if (strcmp(argv[1], "live") != 0) {
                fprintf(stderr, "Unknown mode '%s'\n", argv[1]);
                print_usage(argv[0]);
                return 1;
        }

        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        XdpPortal *portal = xdp_portal_new();

        xdp_portal_create_screencast_session(portal, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
                                             XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_TRANSIENT,
                                             NULL, NULL, create_session_cb, NULL);

        g_main_loop_run(loop);

        return 0;
}
