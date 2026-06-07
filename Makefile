CC ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local

PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags libportal glib-2.0 libpipewire-0.3)
PKG_LIBS   = $(shell $(PKG_CONFIG) --libs libportal glib-2.0 libpipewire-0.3)

CFLAGS += -g -O2 $(PKG_CFLAGS) -lm
LDLIBS += $(PKG_LIBS)

ifeq ($(DEBUG), 1)
        CFLAGS += -DDEBUG
endif

TARGET = blight

SRCS = src/main.c src/wifi.c

CFLAGS += -DWIFI

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
