# k1cam's build variables first: the packages below read them as they are
# parsed.
include $(BR2_EXTERNAL_K1CAM_PATH)/board/k1cam/uboot.mk

include $(sort $(wildcard $(BR2_EXTERNAL_K1CAM_PATH)/package/*/*.mk))
