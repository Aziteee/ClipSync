CC ?= aarch64-linux-android33-clang
NDK_SYSROOT ?= $(NDK_HOME)/toolchains/llvm/prebuilt/windows-x86_64/sysroot
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lbinder_ndk -ldl
TARGET = clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) protocol.h
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

install:
	adb push $(TARGET) /data/local/tmp/
	adb shell chmod 755 /data/local/tmp/$(TARGET)

module: $(TARGET)
	mkdir -p module/system/bin
	mkdir -p module/config
	cp $(TARGET) module/system/bin/
	cp config/clipsync.toml module/config/
	cd module && zip -r ../clipsync-module.zip .
	@echo "Module package: clipsync-module.zip"

.PHONY: all clean install module
