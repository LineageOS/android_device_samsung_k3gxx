LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# GPS
include $(CLEAR_VARS)

LOCAL_SRC_FILES := samsung_gps.cpp
LOCAL_SHARED_LIBRARIES := libbinder liblog libcutils libgui libutils
LOCAL_MODULE := libshim_gpsd
LOCAL_MODULE_CLASS := SHARED_LIBRARIES

include $(BUILD_SHARED_LIBRARY)