LOCAL_PATH:= $(call my-dir)

# java library
# ------------------------------------------------------------
include $(CLEAR_VARS)

LOCAL_MODULE := leds

LOCAL_SRC_FILES := $(call all-java-files-under, src/java)

# LOCAL_STATIC_JAVA_LIBRARIES :=
# LOCAL_JNI_SHARED_LIBRARIES := libleds

include $(BUILD_JAVA_LIBRARY)
# ============================================================


# native library
# ------------------------------------------------------------
include $(CLEAR_VARS)

LOCAL_SRC_FILES :=
LOCAL_SHARED_LIBRARIES := libnativehelper

include $(LOCAL_PATH)/src/jni/Android.mk

LOCAL_CFLAGS += 

LOCAL_MODULE := libleds

# include $(BUILD_SHARED_LIBRARY)
# ------------------------------------------------------------
