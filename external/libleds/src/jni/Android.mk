# This file is included by the top level services directory to collect source
# files
LOCAL_REL_DIR := src/jni

LOCAL_CFLAGS += -Wall -Werror -Wno-unused-parameter

LOCAL_SRC_FILES += \
    $(LOCAL_REL_DIR)/co_megafone_leds_Leds.cpp \
    $(LOCAL_REL_DIR)/onload.cpp

LOCAL_C_INCLUDES += \
    $(JNI_H_INCLUDE) 

LOCAL_SHARED_LIBRARIES += \
    liblog \
    libutils 

