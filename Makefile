# Makefile for c0pin

TARGET = c0pin
SRC    = c0pin.c

UNITS = c0pin-performance.service c0pin-aggressive.service

# Overridable by the caller
PREFIX  ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin

UNITDIR ?= $(shell pkg-config systemd --variable=systemdsystemunitdir 2>/dev/null)
ifeq ($(strip $(UNITDIR)),)
UNITDIR := /etc/systemd/system
endif

CC ?= gcc

# Baseline optimization + hardening.
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wformat -Wformat=2 -Werror=format-security \
          -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
          -fstack-clash-protection -fPIE

CPPFLAGS ?=

LDFLAGS ?=
LDFLAGS += -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(SBINDIR)/$(TARGET)

	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 c0pin-performance.service \
		$(DESTDIR)$(UNITDIR)/c0pin-performance.service
	install -m 0644 c0pin-aggressive.service \
		$(DESTDIR)$(UNITDIR)/c0pin-aggressive.service

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(UNITDIR)/c0pin-performance.service
	rm -f $(DESTDIR)$(UNITDIR)/c0pin-aggressive.service

clean:
	rm -f $(TARGET)
