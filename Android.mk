LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := file_server
LOCAL_CFLAGS := -DFFMPEG_SRC=0 -DPLUGIN_ZLIB=0 -std=c99
LOCAL_C_INCLUDES += #

LOCAL_SRC_FILES := ffserver.c compact.c avstring.c
LOCAL_LD_FLAGS := -static


include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := http_client
LOCAL_CFLAGS := -DFFMPEG_SRC=0 -DPLUGIN_ZLIB=0 -std=c99
LOCAL_C_INCLUDES += #

LOCAL_SRC_FILES := http_client.c compact.c avstring.c
LOCAL_LD_FLAGS := -static


include $(BUILD_EXECUTABLE)
