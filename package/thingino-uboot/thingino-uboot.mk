################################################################################
#
# Thingino integration hooks for Buildroot's U-Boot package
#
################################################################################

# The U-Boot environment is not generated here: k1cam keeps the whole of it,
# flash layout included, in board/k1cam/uenv.txt. The defconfig names it
# as U-Boot's built-in default (BR2_TARGET_UBOOT_DEFAULT_ENV_FILE) and
# board/k1cam/post-image.sh packs the same file into the env partition.
ifeq ($(BR2_TARGET_UBOOT)$(BR_BUILDING),yy)

ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_ENABLE_BAR),y)
UBOOT_MAKE_OPTS += CONFIG_SPI_FLASH_BAR=1
endif

ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_EXTERNAL_ENV_ENABLE),y)
UBOOT_MAKE_OPTS += CONFIG_BOOTARGS_EXTERNAL=1
endif

ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_PHY_RESET_AFTER_CONFIG),y)
UBOOT_MAKE_OPTS += CONFIG_PHY_RESET_AFTER_CONFIG=1
UBOOT_MAKE_OPTS += CONFIG_GPIO_PHY_RESET=$(call qstrip,$(BR2_PACKAGE_THINGINO_UBOOT_GPIO_PHY_RESET))
UBOOT_MAKE_OPTS += CONFIG_GPIO_PHY_RESET_ENLEVEL=$(call qstrip,$(BR2_PACKAGE_THINGINO_UBOOT_GPIO_PHY_RESET_ENLEVEL))
endif

ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_JZ_SFC),y)
THINGINO_UBOOT_FLASH_CONTROLLER := jz_sfc
else ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_SFC_NAND),y)
THINGINO_UBOOT_FLASH_CONTROLLER := sfc_nand
else ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_SFC0_NOR),y)
THINGINO_UBOOT_FLASH_CONTROLLER := sfc0_nor
else ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_SFC1_NOR),y)
THINGINO_UBOOT_FLASH_CONTROLLER := sfc1_nor
else ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_SFC0_NAND),y)
THINGINO_UBOOT_FLASH_CONTROLLER := sfc0_nand
else ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_SFC1_NAND),y)
THINGINO_UBOOT_FLASH_CONTROLLER := sfc1_nand
else ifeq ($(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_CUSTOM),y)
THINGINO_UBOOT_FLASH_CONTROLLER := $(BR2_PACKAGE_THINGINO_UBOOT_FLASH_CONTROLLER_CUSTOM_STRING)
else
THINGINO_UBOOT_FLASH_CONTROLLER := jz_sfc
endif

# GNU patch cannot apply binary diffs, so the SPL blobs shipped inside
# 0001-from-2013.07-to-thingino.patch (spl/binary/*.bin) come out empty
# after patching, producing a bricking firmware image for boards that
# use a prebuilt SPL (T31LC, Xiaomi MJSXJ03HL & friends).
# Restore the vendored copies from this package's files directory.
# https://github.com/themactep/thingino-firmware/issues/1299
ifeq ($(BR2_THINGINO_UBOOT_VERSION_2013_07),y)
define THINGINO_UBOOT_RESTORE_SPL_BINARIES
	mkdir -p $(@D)/spl/binary
	cp -f $(BR2_EXTERNAL_THINGINO_PATH)/package/thingino-uboot/files/t31lc_sfcnor.bin \
		$(BR2_EXTERNAL_THINGINO_PATH)/package/thingino-uboot/files/t31_xiaomi_sfcnor.bin \
		$(BR2_EXTERNAL_THINGINO_PATH)/package/thingino-uboot/files/t31_xiaomi_sfcnor_2.bin \
		$(@D)/spl/binary/
endef
UBOOT_POST_PATCH_HOOKS += THINGINO_UBOOT_RESTORE_SPL_BINARIES
endif

define THINGINO_UBOOT_COPY_SHA1_HEADER
	if [ -f $(@D)/include/sha1.h ]; then \
		cp $(@D)/include/sha1.h $(@D)/tools/sha1.h; \
	fi
endef
UBOOT_POST_PATCH_HOOKS += THINGINO_UBOOT_COPY_SHA1_HEADER

# Drop the on-chip wired-Ethernet driver (DesignWare GMAC + PHY) when the board has no wired Ethernet.
ifneq ($(BR2_ETHERNET),y)
ifneq ($(BR2_THINGINO_UBOOT_VERSION_2013_07),y)
define THINGINO_UBOOT_DISABLE_WIRED_ETH
	$(call KCONFIG_DISABLE_OPT,CONFIG_ETH_DESIGNWARE_INGENIC,$(@D)/.config)
	$(call KCONFIG_DISABLE_OPT,CONFIG_ETH_DESIGNWARE,$(@D)/.config)
	$(call KCONFIG_DISABLE_OPT,CONFIG_PHY_ICPLUS,$(@D)/.config)
	$(UBOOT_KCONFIG_MAKE) olddefconfig
endef
UBOOT_PRE_BUILD_HOOKS += THINGINO_UBOOT_DISABLE_WIRED_ETH
endif
endif

# Drop the USB-Ethernet host drivers when the board has no USB OTG data port (no dongle possible).
ifneq ($(BR2_PACKAGE_THINGINO_KOPT_DWC2_OTG),y)
ifneq ($(BR2_THINGINO_UBOOT_VERSION_2013_07),y)
define THINGINO_UBOOT_DISABLE_USB_ETH
	$(call KCONFIG_DISABLE_OPT,CONFIG_USB_HOST_ETHER,$(@D)/.config)
	$(call KCONFIG_DISABLE_OPT,CONFIG_USB_ETHER_ASIX,$(@D)/.config)
	$(UBOOT_KCONFIG_MAKE) olddefconfig
endef
UBOOT_PRE_BUILD_HOOKS += THINGINO_UBOOT_DISABLE_USB_ETH
endif
endif

# Drop the audio/sound subsystem (disabling CONFIG_SOUND cascades I2S + codecs) when the board has no audio.
ifneq ($(BR2_THINGINO_AUDIO),y)
ifneq ($(BR2_THINGINO_UBOOT_VERSION_2013_07),y)
define THINGINO_UBOOT_DISABLE_AUDIO
	$(call KCONFIG_DISABLE_OPT,CONFIG_CMD_SOUND,$(@D)/.config)
	$(call KCONFIG_DISABLE_OPT,CONFIG_SOUND,$(@D)/.config)
	$(UBOOT_KCONFIG_MAKE) olddefconfig
endef
UBOOT_PRE_BUILD_HOOKS += THINGINO_UBOOT_DISABLE_AUDIO
endif
endif

# Inject this board's MMC card-detect + slot-power into the per-SoC U-Boot
# device tree from thingino.json (the GPIOs are board-specific, so they can't
# live in the shared .dts). The helper appends a vmmc-supply regulator and, on
# pull-up-capable SoCs, cd-gpios, to this board's build copy of the leaf .dts -
# so the mmc core powers and detects the slot natively, with no env gpio gate
# or power-up. The helper reads thingino.json with python3 (already a U-Boot
# build dependency via binman).
ifneq ($(BR2_THINGINO_UBOOT_VERSION_2013_07),y)
define THINGINO_UBOOT_INJECT_MMC_DT
	@DT=$$(sed -n 's/^CONFIG_DEFAULT_DEVICE_TREE="\(.*\)"/\1/p' $(@D)/.config); \
	[ -n "$$DT" ] && [ -f $(@D)/arch/mips/dts/$$DT.dts ] || exit 0; \
	$(BR2_EXTERNAL_THINGINO_PATH)/package/thingino-uboot/inject-uboot-mmc-dt.sh \
		$(BR2_EXTERNAL_THINGINO_PATH)/$(CAMERA_SUBDIR)/$(CAMERA)/thingino.json \
		$(@D)/arch/mips/dts/$$DT.dts "$$DT"
endef
UBOOT_PRE_BUILD_HOOKS += THINGINO_UBOOT_INJECT_MMC_DT
endif

# Inject boot-window GPIO presets (gpio-hogs) into this board's U-Boot leaf
# .dts from thingino.json, and enable CONFIG_GPIO_HOG so U-Boot drives them
# right after DM init: PTZ stepper phases parked de-energised (the coils cook
# otherwise), Wi-Fi module power/enable lines at their runtime resting level
# (S36wireless only replays them on 3.10 kernels, late in boot; SDIO modules
# must be powered for the kernel MMC scan), multi-pin gpio.mmc_power lists at
# their power-on level (the single-pin form becomes a vmmc-supply regulator
# in the MMC inject above instead), and IR-cut filter coil pins at the
# /usr/sbin/ircut idle level so the solenoid is not left floating or
# energised. The helper self-skips per domain from the json content, so no
# per-domain config gate is needed.
ifneq ($(BR2_THINGINO_UBOOT_VERSION_2013_07),y)
define THINGINO_UBOOT_INJECT_GPIO_DT
	@DT=$$(sed -n 's/^CONFIG_DEFAULT_DEVICE_TREE="\(.*\)"/\1/p' $(@D)/.config); \
	[ -n "$$DT" ] && [ -f $(@D)/arch/mips/dts/$$DT.dts ] || exit 0; \
	$(BR2_EXTERNAL_THINGINO_PATH)/package/thingino-uboot/inject-uboot-gpio-dt.sh \
		$(BR2_EXTERNAL_THINGINO_PATH)/$(CAMERA_SUBDIR)/$(CAMERA)/thingino.json \
		$(@D)/arch/mips/dts/$$DT.dts "$$DT"
	$(call KCONFIG_ENABLE_OPT,CONFIG_GPIO_HOG,$(@D)/.config)
	$(UBOOT_KCONFIG_MAKE) olddefconfig
endef
UBOOT_PRE_BUILD_HOOKS += THINGINO_UBOOT_INJECT_GPIO_DT
endif

endif
