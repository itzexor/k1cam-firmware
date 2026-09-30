K1CAM_UVCD_SITE_METHOD = local
K1CAM_UVCD_SITE = $(K1CAM_UVCD_PKGDIR)/src
K1CAM_UVCD_LICENSE = GPL-3.0
K1CAM_UVCD_DEPENDENCIES = thingino-raptor-hal

ifeq ($(BR2_TOOLCHAIN_USES_MUSL),y)
K1CAM_UVCD_DEPENDENCIES += ingenic-musl
K1CAM_UVCD_SHIM = -Wl,--no-as-needed -lmuslshim -Wl,--as-needed
endif
ifeq ($(BR2_TOOLCHAIN_USES_UCLIBC),y)
K1CAM_UVCD_DEPENDENCIES += ingenic-uclibc
K1CAM_UVCD_SHIM = -Wl,--no-as-needed -luclibcshim -Wl,--as-needed
endif

define K1CAM_UVCD_BUILD_CMDS
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
		-c $(@D)/uvcd_ctl.c -o $(@D)/uvcd_ctl.o
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -flto -ffunction-sections -fdata-sections \
		-I$(STAGING_DIR)/usr/include \
		-c $(@D)/uvcd_main.c -o $(@D)/uvcd_main.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -Os -flto -Wl,--gc-sections \
		-Wl,-z,max-page-size=0x1000 \
		$(@D)/uvcd_pipeline.o $(@D)/uvcd_gadget.o \
		$(@D)/uvcd_config.o $(@D)/uvcd_ctl.o $(@D)/uvcd_main.o \
		-L$(STAGING_DIR)/usr/lib -L$(TARGET_DIR)/usr/lib -lraptor_hal_video \
		-limp -lalog $(K1CAM_UVCD_SHIM) -lpthread -lrt -latomic -lm \
		-o $(@D)/uvcd
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -ffunction-sections -fdata-sections -Wl,--gc-sections \
		$(K1CAM_UVCD_PKGDIR)/uvcdctl/uvcdctl.c -o $(@D)/uvcdctl
endef

define K1CAM_UVCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uvcd $(TARGET_DIR)/usr/bin/uvcd
	$(INSTALL) -D -m 0755 $(@D)/uvcdctl $(TARGET_DIR)/usr/bin/uvcdctl
	$(INSTALL) -D -m 0755 $(K1CAM_UVCD_PKGDIR)/files/S31uvcd \
		$(TARGET_DIR)/etc/init.d/S31uvcd
endef

$(eval $(generic-package))
