#ifndef SERIAL_WRAPPER_H
#define SERIAL_WRAPPER_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

bool is_serial_initialized();

int serial_init(const char *port_name, const uint32_t baud_rate, const uint32_t timeout_ms);
int serial_deinit();

ssize_t serial_tx(const uint8_t *data, size_t len);
ssize_t serial_rx(uint8_t *buffer, size_t max_len);

int serial_available();

int serial_flush_tx();
int serial_flush_rx();

#endif
