CC          = gcc
GTK_VERSION ?= 3

GTK2_NOWARN = $(if $(filter 2,$(GTK_VERSION)),-Wno-deprecated-declarations,)

CFLAGS  = -Wall -Wextra -Wformat=2 -Wformat-security -O2 \
          -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE \
          $(GTK2_NOWARN) \
          $(shell pkg-config --cflags gtk+-$(GTK_VERSION).0)
LDFLAGS = $(shell pkg-config --libs gtk+-$(GTK_VERSION).0) -lpam \
          -pie -Wl,-z,relro,-z,now

TARGET = xlogin-gtk$(GTK_VERSION)
SRC    = src/main.c

.PHONY: all both install clean uninstall

all: $(TARGET)

both:
	$(MAKE) GTK_VERSION=3
	$(MAKE) GTK_VERSION=2

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: all
	install -m 755 $(TARGET) /usr/local/bin/xlogin
	install -m 755 xlogin-launcher /usr/local/bin/
	install -m 644 pam.d/xlogin /etc/pam.d/

clean:
	rm -f xlogin-gtk3 xlogin-gtk2

uninstall:
	rm -f /usr/local/bin/xlogin
	rm -f /usr/local/bin/xlogin-launcher
	rm -f /etc/pam.d/xlogin
	rm -f /etc/xlogin.conf
