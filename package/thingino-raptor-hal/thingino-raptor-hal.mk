THINGINO_RAPTOR_HAL_VERSION = 7e62abede02b41ecfb097303b3103b1a5d8740a4
THINGINO_RAPTOR_HAL_SITE = https://github.com/gtxaspec/raptor-hal
THINGINO_RAPTOR_HAL_SITE_METHOD = git
THINGINO_RAPTOR_HAL_GIT_SUBMODULES = YES
THINGINO_RAPTOR_HAL_INSTALL_STAGING = YES
THINGINO_RAPTOR_HAL_INSTALL_TARGET = NO

THINGINO_RAPTOR_HAL_DEPENDENCIES = ingenic-lib

define THINGINO_RAPTOR_HAL_BUILD_CMDS
	$(MAKE) -C $(@D) \
		PLATFORM=T31 \
		CROSS_COMPILE=$(TARGET_CROSS) \
		INGENIC_HEADERS=$(@D)/ingenic-headers
endef

define THINGINO_RAPTOR_HAL_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libraptor_hal_video.a \
		$(STAGING_DIR)/usr/lib/libraptor_hal_video.a
	$(INSTALL) -D -m 0644 $(@D)/libraptor_hal_audio.a \
		$(STAGING_DIR)/usr/lib/libraptor_hal_audio.a
	$(INSTALL) -D -m 0644 $(@D)/include/raptor_hal.h \
		$(STAGING_DIR)/usr/include/raptor_hal.h
endef

$(eval $(generic-package))
