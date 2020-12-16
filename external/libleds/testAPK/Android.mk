LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := \
        $(call all-java-files-under, src)

LOCAL_PACKAGE_NAME := libleds_test

LOCAL_STATIC_JAVA_LIBRARIES := leds 

# LOCAL_JNI_SHARED_LIBRARIES := libleds

# LOCAL_CERTIFICATE := platform

# LOCAL_PRIVILEGED_MODULE := true

include $(BUILD_PACKAGE)
