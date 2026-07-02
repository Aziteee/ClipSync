BUILD_DIR = build
CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2 -Ithird_party/mongoose -DMG_ENABLE_MDNS=1
LDFLAGS = -lbinder_ndk -ldl
TARGET = $(BUILD_DIR)/clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c protocol_json.c crypto_hmac.c third_party/mongoose/mongoose.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)
NDK_BUILD ?= ndk-build
ZYGISK_SO = module/zygisk/arm64-v8a.so

all: $(TARGET) $(BUILD_DIR)/test_clip $(BUILD_DIR)/test_crypto_hmac $(BUILD_DIR)/test_protocol_json $(ZYGISK_SO)

$(BUILD_DIR):
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)
	@if not exist $(BUILD_DIR)\third_party\mongoose mkdir $(BUILD_DIR)\third_party\mongoose

$(TARGET): $(OBJS) protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/third_party/mongoose/mongoose.o: third_party/mongoose/mongoose.c | $(BUILD_DIR)
	@if not exist $(BUILD_DIR)\third_party\mongoose mkdir $(BUILD_DIR)\third_party\mongoose
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)\third_party\mongoose\mongoose.o: $(BUILD_DIR)/third_party/mongoose/mongoose.o

$(ZYGISK_SO): zygisk/jni/main.cpp zygisk/jni/zygisk.hpp zygisk/jni/Android.mk zygisk/jni/Application.mk
	$(NDK_BUILD) -C zygisk
	@if not exist module\zygisk mkdir module\zygisk
	copy /Y zygisk\libs\arm64-v8a\libzygisk_clipsync.so $(ZYGISK_SO)

clean:
	rm -f $(OBJS) $(TARGET) $(ZYGISK_SO)
	rmdir $(BUILD_DIR) 2>/dev/null || true
	$(NDK_BUILD) -C zygisk clean 2>nul || true

install: $(TARGET)
	adb push $(TARGET) /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/clipsyncd"

module: $(TARGET) $(ZYGISK_SO)
	@if not exist module\system\bin mkdir module\system\bin
	@copy /Y build\clipsyncd module\system\bin\clipsyncd >nul
	@echo ========================================
	@echo KernelSU module ready in module/
	@echo Push and install:
	@echo   adb push module /data/local/tmp/module/
	@echo   adb shell "su -c 'rm -rf /data/adb/modules/clipsyncd; cp -r /data/local/tmp/module /data/adb/modules/clipsyncd'"
	@echo   adb reboot
	@echo Check: adb logcat -s ClipSyncBridge clipsyncd

.PHONY: all clean install module test test-crypto

test: $(BUILD_DIR)/test_clip
	adb push $(BUILD_DIR)/test_clip /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_clip && /data/local/tmp/test_clip"

$(BUILD_DIR)/test_clip: test_clip.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BUILD_DIR)/test_crypto_hmac: test_crypto_hmac.c crypto_hmac.c crypto_hmac.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_crypto_hmac.c crypto_hmac.c

test-crypto: $(BUILD_DIR)/test_crypto_hmac
	adb push $(BUILD_DIR)/test_crypto_hmac /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_crypto_hmac && /data/local/tmp/test_crypto_hmac"
