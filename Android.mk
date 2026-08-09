LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
ifeq ($(TARGET_ARCH_ABI), armeabi-v7a)
    LOCAL_MODULE := CarAudio
else
    LOCAL_MODULE := CarAudio64
endif

LOCAL_SRC_FILES := main.cpp mod/logger.cpp mod/config.cpp

# Idagdag ang AML_ImGui folder dito
LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/AML_ImGui \
    $(LOCAL_PATH)/aml-psdk-gtasa \
    $(LOCAL_PATH)/aml-psdk-gtasa/aml-psdk/game_sa

LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)