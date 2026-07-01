BUILD_DIR = build
CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lbinder_ndk -ldl
TARGET = $(BUILD_DIR)/clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS) protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
	rmdir $(BUILD_DIR) 2>/dev/null || true

install: $(TARGET)
	adb push $(TARGET) /data/local/tmp/
	adb shell chmod 755 /data/local/tmp/$(TARGET)

module: $(TARGET)
	mkdir -p module/system/bin
	mkdir -p module/config
	cp $(TARGET) module/system/bin/
	cp module/config/clipsync.toml module/config/ 2>/dev/null || true
	cd module && zip -r ../clipsync-module.zip .
	@echo "Module package: clipsync-module.zip"

.PHONY: all clean install module
