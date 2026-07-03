LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := zygisk_clipsync
LOCAL_SRC_FILES := main.cpp ../../bridge_protocol.c
LOCAL_CFLAGS := -Wall -Wextra -O2 -fvisibility=hidden -fPIC -D_GNU_SOURCE
LOCAL_CPPFLAGS := -std=c++17
LOCAL_LDFLAGS := -shared
LOCAL_LDLIBS := -ldl -llog
include $(BUILD_SHARED_LIBRARY)
