CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2 -static
TARGET = clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) protocol.h
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

install:
	adb push $(TARGET) /data/local/tmp/
	adb shell chmod 755 /data/local/tmp/$(TARGET)

.PHONY: all clean install
