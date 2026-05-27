CC      = gcc
CFLAGS  = -Wall -Wextra -Wformat=2 -Wformat-security -O2 \
          -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE \
          $(shell pkg-config --cflags gtk+-3.0)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0) -lpam \
          -pie -Wl,-z,relro,-z,now

TARGET = xlogin
SRC    = src/main.c

.PHONY: all install clean uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: all
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 755 xlogin-launcher /usr/local/bin/
	install -m 644 pam.d/xlogin /etc/pam.d/

clean:
	rm -f $(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	rm -f /usr/local/bin/xlogin-launcher
	rm -f /etc/pam.d/xlogin
	rm -f /etc/xlogin.conf
