#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>
#include <sys/types.h>

int wifi_init(const char *esp_hostname, uint16_t port, int timeout_ms);
ssize_t wifi_tx(const uint8_t *data, size_t len);
void wifi_close(void);

#endif
