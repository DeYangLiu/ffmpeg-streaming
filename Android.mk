LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := file_server
<<<<<<< HEAD
LOCAL_CFLAGS := -DFFMPEG_SRC=0 -DPLUGIN_ZLIB=0
LOCAL_C_INCLUDES += #

LOCAL_SRC_FILES := ffserver.c compact.c avstring.c
LOCAL_LD_FLAGS := -static
=======
LOCAL_CFLAGS := -DFFMPEG_SRC=0 -DPLUGIN_ZLIB=0 
LOCAL_C_INCLUDES += #

LOCAL_SRC_FILES := ffserver.c compact.c avstring.c
#LOCAL_LDFLAGS := -static
>>>>>>> d98d3f2f40eb53ba0c8c2a894eda98a62fd7107c

include $(BUILD_EXECUTABLE)
