K1CAM_UVCD_SITE_METHOD = local
K1CAM_UVCD_SITE = $(K1CAM_UVCD_PKGDIR)/src
K1CAM_UVCD_LICENSE = GPL-3.0
# The host side builds k1cam-mkupd, which post-image.sh packs k1cam.upd with.
K1CAM_UVCD_DEPENDENCIES = thingino-raptor-hal host-k1cam-uvcd

ifeq ($(BR2_TOOLCHAIN_USES_MUSL),y)
K1CAM_UVCD_DEPENDENCIES += ingenic-musl
K1CAM_UVCD_SHIM = -Wl,--no-as-needed -lmuslshim -Wl,--as-needed
endif
ifeq ($(BR2_TOOLCHAIN_USES_UCLIBC),y)
K1CAM_UVCD_DEPENDENCIES += ingenic-uclibc
K1CAM_UVCD_SHIM = -Wl,--no-as-needed -luclibcshim -Wl,--as-needed
endif

K1CAM_UVCD_OBJS = uvcd_camera uvcd_pipeline uvcd_gadget uvcd_config uvcd_ctl uvcd_main \
	uvcd_diag uvcd_update fw_bundle fw_crc fw_env fw_flash fw_sha256
# What uvcdctl and k1cam-mkupd share with the daemon: the bundle format.
K1CAM_UVCD_FW_SRCS = fw_bundle.c fw_crc.c fw_sha256.c

define K1CAM_UVCD_BUILD_CMDS
	$(foreach o,$(K1CAM_UVCD_OBJS),
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -flto -ffunction-sections -fdata-sections \
		-I$(STAGING_DIR)/usr/include \
		-c $(@D)/$(o).c -o $(@D)/$(o).o)
	$(TARGET_CC) $(TARGET_LDFLAGS) -Os -flto -Wl,--gc-sections \
		-Wl,-z,max-page-size=0x1000 \
		$(addprefix $(@D)/,$(addsuffix .o,$(K1CAM_UVCD_OBJS))) \
		-L$(STAGING_DIR)/usr/lib -L$(TARGET_DIR)/usr/lib -lraptor_hal_video \
		-limp -lalog $(K1CAM_UVCD_SHIM) -lpthread -lrt -latomic -lm \
		-o $(@D)/uvcd
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
		-Os -ffunction-sections -fdata-sections -Wl,--gc-sections \
		$(K1CAM_UVCD_PKGDIR)/uvcdctl/uvcdctl.c $(K1CAM_UVCD_PKGDIR)/uvcdctl/firmware.c $(@D)/uvcd_diag.c \
		$(addprefix $(@D)/,$(K1CAM_UVCD_FW_SRCS)) -o $(@D)/uvcdctl
endef

define K1CAM_UVCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uvcd $(TARGET_DIR)/usr/bin/uvcd
	$(INSTALL) -D -m 0755 $(@D)/uvcdctl $(TARGET_DIR)/usr/bin/uvcdctl
	$(INSTALL) -D -m 0755 $(K1CAM_UVCD_PKGDIR)/files/S31uvcd \
		$(TARGET_DIR)/etc/init.d/S31uvcd
endef

define HOST_K1CAM_UVCD_BUILD_CMDS
	$(HOSTCC) $(HOST_CFLAGS) -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -I$(@D) \
		$(K1CAM_UVCD_PKGDIR)/mkupd/k1cam-mkupd.c \
		$(addprefix $(@D)/,$(K1CAM_UVCD_FW_SRCS)) $(HOST_LDFLAGS) -o $(@D)/k1cam-mkupd
endef

define HOST_K1CAM_UVCD_INSTALL_CMDS
	$(INSTALL) -D -m 0755 $(@D)/k1cam-mkupd $(HOST_DIR)/bin/k1cam-mkupd
endef

$(eval $(generic-package))
$(eval $(host-generic-package))
