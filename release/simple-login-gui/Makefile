CC=gcc
CFLAGS=-Wall -Wextra -O2 `pkg-config --cflags gtk+-3.0`
LDFLAGS=`pkg-config --libs gtk+-3.0` -lpam

TARGET=xlogin
SRC=src/main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: all
	install -m 4750 $(TARGET) /usr/local/bin/
	install -m 755 xlogin-launcher /usr/local/bin/
	install -m 644 pam.d/xlogin /etc/pam.d/

clean:
	rm -f $(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	rm -f /usr/local/bin/xlogin-launcher
	rm -f /etc/pam.d/xlogin
