BUILD_DIR = build
CC ?= aarch64-linux-android33-clang
ANDROID_SDK_ROOT ?= D:\AppData\Android\sdk
ANDROID_PLATFORM ?= android-36
ANDROID_BUILD_TOOLS ?= 36.1.0
D8 ?= $(ANDROID_SDK_ROOT)\build-tools\$(ANDROID_BUILD_TOOLS)\d8.bat
ANDROID_JAR ?= $(ANDROID_SDK_ROOT)\platforms\$(ANDROID_PLATFORM)\android.jar
CFLAGS = -Wall -Wextra -O2 -Ithird_party/mongoose -DMG_ENABLE_MDNS=1
LDFLAGS = -ldl
TARGET = $(BUILD_DIR)/clipsyncd
SRCS = clipsyncd.c clip_bridge_client.c bridge_protocol.c ws_server.c mdns_publish.c protocol_json.c crypto_hmac.c daemon_config.c last_clip.c third_party/mongoose/mongoose.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)
NDK_BUILD ?= ndk-build
ZYGISK_SO = module/zygisk/arm64-v8a.so
ZYGISK_BUILD_SO = zygisk/libs/arm64-v8a/libzygisk_clipsync.so
DIST_DIR = dist
PACKAGE = $(DIST_DIR)/clipsyncd-module.zip
CONFIG_SRC = config/clipsync.toml
HELPER_DEX = module/zygisk/clipsync-helper.dex
HELPER_SRC = zygisk/helper/src/dev/clipsync/bridge/ClipSyncBridgeHelper.java
HELPER_STUB = zygisk/helper/stubs/android/content/IOnPrimaryClipChangedListener.java

all: $(TARGET) $(BUILD_DIR)/test_bridge_client $(BUILD_DIR)/test_crypto_hmac $(BUILD_DIR)/test_protocol_json $(BUILD_DIR)/test_daemon_config $(BUILD_DIR)/test_bridge_protocol $(BUILD_DIR)/test_last_clip $(ZYGISK_SO) $(HELPER_DEX)

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

$(HELPER_DEX): $(HELPER_SRC) $(HELPER_STUB)
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item -LiteralPath '$(BUILD_DIR)\helper','$(HELPER_DEX)' -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force -Path '$(BUILD_DIR)\helper\classes','$(BUILD_DIR)\helper\dex' | Out-Null"
	@if not exist module\zygisk mkdir module\zygisk
	javac -source 8 -target 8 -bootclasspath "$(ANDROID_JAR)" -d $(BUILD_DIR)\helper\classes $(HELPER_STUB) $(HELPER_SRC)
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "$$classes = Get-ChildItem -Path '$(BUILD_DIR)\helper\classes\dev\clipsync\bridge' -Filter '*.class' | ForEach-Object { $$_.FullName }; & '$(D8)' --lib '$(ANDROID_JAR)' --classpath '$(BUILD_DIR)\helper\classes' --output '$(BUILD_DIR)\helper\dex' $$classes"
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "Copy-Item -LiteralPath '$(BUILD_DIR)\helper\dex\classes.dex' -Destination '$(HELPER_DEX)' -Force"

clean:
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item -LiteralPath '$(BUILD_DIR)','$(DIST_DIR)','$(ZYGISK_SO)','$(HELPER_DEX)' -Recurse -Force -ErrorAction SilentlyContinue"
	-@$(NDK_BUILD) -C zygisk clean 2>nul

install: $(TARGET)
	adb push $(TARGET) /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/clipsyncd"

module: $(TARGET) $(ZYGISK_SO) $(HELPER_DEX) $(CONFIG_SRC)
	@if not exist module\system\bin mkdir module\system\bin
	@if not exist module\config mkdir module\config
	@copy /Y build\clipsyncd module\system\bin\clipsyncd >nul
	@copy /Y config\clipsync.toml module\config\clipsync.toml >nul
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "if ([Convert]::ToBase64String([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes('$(ZYGISK_BUILD_SO)'))) -ne [Convert]::ToBase64String([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes('$(ZYGISK_SO)')))) { Copy-Item -Path '$(ZYGISK_BUILD_SO)' -Destination '$(ZYGISK_SO)' -Force }; if ([Convert]::ToBase64String([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes('$(ZYGISK_BUILD_SO)'))) -ne [Convert]::ToBase64String([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes('$(ZYGISK_SO)')))) { throw 'Zygisk module copy verification failed' }"
	@if exist module\libzygisk_clipsync.so del /F /Q module\libzygisk_clipsync.so
	@echo ========================================
	@echo KernelSU module ready in module/
	@echo Push and install:
	@echo   adb push module /data/local/tmp/module/
	@echo   adb shell "su -c 'rm -rf /data/adb/modules/clipsyncd; cp -r /data/local/tmp/module /data/adb/modules/clipsyncd'"
	@echo   adb reboot
	@echo Check: adb logcat -s ClipSyncBridge clipsyncd

package: module
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "if ([Convert]::ToBase64String([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes('$(ZYGISK_BUILD_SO)'))) -ne [Convert]::ToBase64String([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes('$(ZYGISK_SO)')))) { Copy-Item -Path '$(ZYGISK_BUILD_SO)' -Destination '$(ZYGISK_SO)' -Force }; if (!(Test-Path '$(DIST_DIR)')) { New-Item -ItemType Directory -Path '$(DIST_DIR)' | Out-Null }; if (Test-Path '$(PACKAGE)') { Remove-Item '$(PACKAGE)' -Force }; Compress-Archive -Path 'module\*' -DestinationPath '$(PACKAGE)' -Force"
	@echo ========================================
	@echo KernelSU module package ready:
	@echo   $(PACKAGE)

.PHONY: all clean install module package test-crypto test-json test-daemon-config test-bridge-protocol test-bridge-client test-last-clip

$(BUILD_DIR)/test_bridge_client: test_bridge_client.c bridge_protocol.c bridge_protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_bridge_client.c bridge_protocol.c

test-bridge-client: $(BUILD_DIR)/test_bridge_client
	adb push $(BUILD_DIR)/test_bridge_client /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_bridge_client && /data/local/tmp/test_bridge_client read"

$(BUILD_DIR)/test_crypto_hmac: test_crypto_hmac.c crypto_hmac.c crypto_hmac.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_crypto_hmac.c crypto_hmac.c

test-crypto: $(BUILD_DIR)/test_crypto_hmac
	adb push $(BUILD_DIR)/test_crypto_hmac /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_crypto_hmac && /data/local/tmp/test_crypto_hmac"

$(BUILD_DIR)/test_protocol_json: test_protocol_json.c protocol_json.c protocol_json.h third_party/mongoose/mongoose.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_protocol_json.c protocol_json.c third_party/mongoose/mongoose.c

test-json: $(BUILD_DIR)/test_protocol_json
	adb push $(BUILD_DIR)/test_protocol_json /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_protocol_json && /data/local/tmp/test_protocol_json"

$(BUILD_DIR)/test_bridge_protocol: test_bridge_protocol.c bridge_protocol.c bridge_protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_bridge_protocol.c bridge_protocol.c

test-bridge-protocol: $(BUILD_DIR)/test_bridge_protocol
	adb push $(BUILD_DIR)/test_bridge_protocol /data/local/tmp/
	adb shell su -c "chmod 755 /data/local/tmp/test_bridge_protocol && /data/local/tmp/test_bridge_protocol"

$(BUILD_DIR)/test_last_clip: test_last_clip.c last_clip.c last_clip.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_last_clip.c last_clip.c

test-last-clip: $(BUILD_DIR)/test_last_clip
	$(BUILD_DIR)/test_last_clip

$(BUILD_DIR)/test_daemon_config: test_daemon_config.c daemon_config.c daemon_config.h protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ test_daemon_config.c daemon_config.c

test-daemon-config: $(BUILD_DIR)/test_daemon_config
	$(BUILD_DIR)/test_daemon_config
