BUILD_DIR = build
CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lbinder_ndk -ldl
TARGET = $(BUILD_DIR)/clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

all: $(TARGET) $(BUILD_DIR)/test_clip

$(BUILD_DIR):
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

$(TARGET): $(OBJS) protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
	rmdir $(BUILD_DIR) 2>/dev/null || true

install: $(TARGET)
	adb push $(TARGET) /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/clipsyncd"

module: $(TARGET)
	@if not exist module\system\bin mkdir module\system\bin
	@copy /Y build\clipsyncd module\system\bin\clipsyncd >nul
	@echo ========================================
	@echo KernelSU module ready in module/
	@echo Push to device:
	@echo   adb push module /data/local/tmp/module/
	@echo   adb shell su -c "cp -r /data/local/tmp/module /data/adb/modules/clipsyncd"
	@echo   adb reboot
	@echo Check: adb logcat -s clipsyncd

.PHONY: all clean install module test

test: $(BUILD_DIR)/test_clip
	adb push $(BUILD_DIR)/test_clip /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_clip && /data/local/tmp/test_clip"

$(BUILD_DIR)/test_clip: test_clip.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
