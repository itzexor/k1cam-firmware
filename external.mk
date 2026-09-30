# k1cam's build variables first: the packages below read them as they are
# parsed.
include $(BR2_EXTERNAL_THINGINO_PATH)/k1cam.mk

ifneq ($(BR2_SOC_INGENIC_DUMMY),y)
# include makefiles from packages
include $(sort $(wildcard $(BR2_EXTERNAL)/package/*/*.mk))
endif
