#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "serial.h"

typedef struct {
        int fd;
        struct termios old_tio;
        struct termios new_tio;
} linux_handle_t;

static bool g_initialized = false;
static linux_handle_t g_handle = {0};
static pthread_mutex_t g_serial_mutex = PTHREAD_MUTEX_INITIALIZER;

static speed_t get_baud_rate(uint32_t baud) {
        switch (baud) {
        case 9600:
                return B9600;
        case 19200:
                return B19200;
        case 38400:
                return B38400;
        case 57600:
                return B57600;
        case 115200:
                return B115200;
        case 230400:
                return B230400;
        case 460800:
                return B460800;
        case 921600:
                return B921600;
        default:
                return 0;
        }
}

bool is_serial_initialized() { return g_initialized; }

int serial_init(const char *port_name, const uint32_t baud_rate, const uint32_t timeout_ms) {
        pthread_mutex_lock(&g_serial_mutex);

        if (!port_name) {
                errno = EINVAL;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        if (g_initialized) {
                errno = EALREADY;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        g_handle.fd = open(port_name, O_RDWR | O_NOCTTY);
        if (g_handle.fd < 0) {
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        if (tcgetattr(g_handle.fd, &g_handle.old_tio) != 0) {
                close(g_handle.fd);
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        struct termios *tio = &g_handle.new_tio;

        cfmakeraw(tio);
        speed_t baud = get_baud_rate(baud_rate);
        if (baud == 0) {
                errno = EINVAL;
                close(g_handle.fd);
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }
        cfsetispeed(tio, baud);
        cfsetospeed(tio, baud);

        tio->c_cflag &= ~CSIZE;  // Clear size mask
        tio->c_cflag |= CS8;     // 8 data bits
        tio->c_cflag &= ~PARENB; // No parity
        tio->c_cflag &= ~CSTOPB; // 1 stop bit

        // Enable receiver, ignore modem control lines
        tio->c_cflag |= (CLOCAL | CREAD);

        tio->c_cflag &= ~CRTSCTS;                // Disable hardware flow control
        tio->c_iflag &= ~(IXON | IXOFF | IXANY); // Disable software flow control

        // 6. Set read timeouts
        // VMIN = 0, VTIME > 0: non-blocking read with a timeout.
        // read() will wait VTIME deciseconds (0.1s * VTIME) for at least 1 byte.
        tio->c_cc[VMIN] = 0;
        // Correctly round up ms to deciseconds. (100ms -> 1, 50ms -> 1)
        tio->c_cc[VTIME] = (timeout_ms + 99) / 100;

        // Apply settings
        if (tcsetattr(g_handle.fd, TCSANOW, tio) != 0) {
                close(g_handle.fd);
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        // Flush buffers
        tcflush(g_handle.fd, TCIOFLUSH);
        g_initialized = true;
        pthread_mutex_unlock(&g_serial_mutex);

        return 0;
}

int serial_deinit() {
        pthread_mutex_lock(&g_serial_mutex);
        if (!g_initialized) {
                errno = ENXIO;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        tcsetattr(g_handle.fd, TCSANOW, &g_handle.old_tio);
        close(g_handle.fd);
        g_initialized = false;
        pthread_mutex_unlock(&g_serial_mutex);

        return 0;
}

ssize_t serial_tx(const uint8_t *data, size_t len) {
        pthread_mutex_lock(&g_serial_mutex);
        if (!g_initialized) {
                errno = ENXIO;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        ssize_t written = 0;
        while (written < len) {
                ssize_t n = write(g_handle.fd, data + written, len - written);

                if (n < 0) {
                        if (errno == EINTR) {
                                continue;
                        }
                        pthread_mutex_unlock(&g_serial_mutex);
                        return -1;
                }

                written += n;
        }

        if (tcdrain(g_handle.fd) < 0) {
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        pthread_mutex_unlock(&g_serial_mutex);
        return written;
}

ssize_t serial_rx(uint8_t *buffer, size_t max_len) {
        pthread_mutex_lock(&g_serial_mutex);
        if (!buffer) {
                errno = EINVAL;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        if (g_initialized == false) {
                errno = ENXIO;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        ssize_t n = 0;
        while (1) {
                n = read(g_handle.fd, buffer, max_len);
                if (n < 0) {
                        if (errno == EINTR) {
                                continue;
                        }
                        pthread_mutex_unlock(&g_serial_mutex);
                        return -1;
                }
                break;
        }

        pthread_mutex_unlock(&g_serial_mutex);
        return n;
}

int serial_available() {
        pthread_mutex_lock(&g_serial_mutex);
        if (!g_initialized) {
                errno = ENXIO;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        int bytes_available = 0;
        if (ioctl(g_handle.fd, FIONREAD, &bytes_available) < 0) {
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        pthread_mutex_unlock(&g_serial_mutex);
        return bytes_available;
}

int serial_flush_rx() {
        pthread_mutex_lock(&g_serial_mutex);

        if (!g_initialized) {
                errno = ENXIO;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        int ret = tcflush(g_handle.fd, TCIFLUSH);
        pthread_mutex_unlock(&g_serial_mutex);
        return ret;
}

int serial_flush_tx() {
        pthread_mutex_lock(&g_serial_mutex);

        if (!g_initialized) {
                errno = ENXIO;
                pthread_mutex_unlock(&g_serial_mutex);
                return -1;
        }

        int ret = tcflush(g_handle.fd, TCOFLUSH);
        pthread_mutex_unlock(&g_serial_mutex);
        return ret;
}
