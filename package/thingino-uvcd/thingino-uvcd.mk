THINGINO_UVCD_SITE_METHOD = local
THINGINO_UVCD_SITE = $(THINGINO_UVCD_PKGDIR)/src
THINGINO_UVCD_LICENSE = GPL-3.0
THINGINO_UVCD_DEPENDENCIES = thingino-raptor-hal

ifeq ($(BR2_TOOLCHAIN_USES_MUSL),y)
THINGINO_UVCD_DEPENDENCIES += ingenic-musl
THINGINO_UVCD_SHIM = -Wl,--no-as-needed -lmuslshim -Wl,--as-needed
endif
ifeq ($(BR2_TOOLCHAIN_USES_UCLIBC),y)
THINGINO_UVCD_DEPENDENCIES += ingenic-uclibc
THINGINO_UVCD_SHIM = -Wl,--no-as-needed -luclibcshim -Wl,--as-needed
endif

define THINGINO_UVCD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -flto -ffunction-sections -fdata-sections \
		-I$(STAGING_DIR)/usr/include \
		-c $(@D)/uvcd_pipeline.c -o $(@D)/uvcd_pipeline.o
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -flto -ffunction-sections -fdata-sections \
		-I$(STAGING_DIR)/usr/include \
		-c $(@D)/uvcd_gadget.c -o $(@D)/uvcd_gadget.o
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -flto -ffunction-sections -fdata-sections \
		-I$(STAGING_DIR)/usr/include \
		-c $(@D)/uvcd_config.c -o $(@D)/uvcd_config.o
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -flto -ffunction-sections -fdata-sections \
		-I$(STAGING_DIR)/usr/include \
		-c $(@D)/uvcd_main.c -o $(@D)/uvcd_main.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -Os -flto -Wl,--gc-sections \
		-Wl,-z,max-page-size=0x1000 \
		$(@D)/uvcd_pipeline.o $(@D)/uvcd_gadget.o \
		$(@D)/uvcd_config.o $(@D)/uvcd_main.o \
		-L$(STAGING_DIR)/usr/lib -L$(TARGET_DIR)/usr/lib -lraptor_hal_video \
		-limp -lalog $(THINGINO_UVCD_SHIM) -lpthread -lrt -latomic -lm \
		-o $(@D)/uvcd
endef

define THINGINO_UVCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uvcd $(TARGET_DIR)/usr/bin/uvcd
	$(INSTALL) -D -m 0755 $(THINGINO_UVCD_PKGDIR)/files/S31uvcd \
		$(TARGET_DIR)/etc/init.d/S31uvcd
endef

$(eval $(generic-package))
