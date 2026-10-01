INGENIC_SDK_SITE_METHOD = git
INGENIC_SDK_SITE = https://github.com/thingino/ingenic-sdk
INGENIC_SDK_SITE_BRANCH = main
INGENIC_SDK_VERSION = 7b4b0f462780464f3de15ac82168df93922f5993

INGENIC_SDK_LICENSE = GPL-2.0+
INGENIC_SDK_LICENSE_FILES = LICENSE

# This external tree builds one fixed camera. Keep the SDK's component matrix
# out of Kconfig and state the exact T31/GC2083 module set here.
INGENIC_SDK_MODULE_MAKE_OPTS = \
	SOC_FAMILY=t31 \
	KERNEL_VERSION=3.10.14 \
	INSTALL_MOD_PATH=$(TARGET_DIR) \
	INSTALL_MOD_DIR=ingenic \
	SENSOR_1_MODEL=gc2083 \
	CONFIG_INGENIC_ISP=y \
	CONFIG_INGENIC_SENSOR=y \
	CONFIG_INGENIC_AVPU=y \
	CONFIG_INGENIC_AUDIO=n \
	CONFIG_INGENIC_SOC_NNA=n \
	CONFIG_INGENIC_MPSYS=n \
	CONFIG_INGENIC_JZ_DTRNG=n \
	CONFIG_INGENIC_GPIO_USERKEYS=n \
	CONFIG_INGENIC_JZ_AES=n \
	CONFIG_INGENIC_TCU_ALLOC=n \
	CONFIG_INGENIC_PWM=n \
	CONFIG_INGENIC_MOTOR=n \
	EXTRA_CFLAGS="-DCONFIG_KERNEL_3_10"

define INGENIC_SDK_INSTALL_TARGET_CMDS
	krel="$$( $(MAKE) -s -C $(LINUX_DIR) kernelrelease 2>/dev/null )"; \
	if [ -z "$$krel" ]; then krel="$(LINUX_VERSION_PROBED)"; fi; \
	for root in "$(TARGET_DIR)" "$(BASE_TARGET_DIR)"; do \
		[ -n "$$root" ] || continue; \
		[ -d "$$root" ] || continue; \
		libdir="$$root/lib"; \
		if [ "$(BR2_ROOTFS_MERGED_USR)" = "y" ]; then libdir="$$root/usr/lib"; fi; \
		find "$$libdir/modules" -mindepth 1 -maxdepth 1 -type d ! -name "$$krel" \
			-exec rm -rf {} + 2>/dev/null || true; \
		$(INSTALL) -m 0755 -d "$$libdir/modules/$$krel"; \
		touch "$$libdir/modules/$$krel/modules.builtin.modinfo"; \
	done

	# The K1's own sensor tuning, not the SDK's sensor-iq/t31/gc2083.bin:
	# Creality's August 2026 GC2083 tuning (black level, colour matrix,
	# softer demosaic, refined denoise), with the SDK tuning's gamma curve
	# (BT.709-like; Creality's lifts blacks into haze) and the auto-exposure
	# target lowered to suit that curve. Tuned against a phone raw of a
	# reference scene in the printer chamber.
	$(INSTALL) -D -m 0644 $(INGENIC_SDK_PKGDIR)/gc2083-t31.bin \
		$(TARGET_DIR)/usr/share/sensor/gc2083-t31.bin
	ln -sfn /usr/share/sensor $(TARGET_DIR)/etc/sensor
	echo gc2083 > $(TARGET_DIR)/usr/share/sensor/model

	$(INSTALL) -m 0755 -d $(TARGET_DIR)/etc/modules.d
	echo "avpu" > $(TARGET_DIR)/etc/modules.d/10-avpu
	echo "tx_isp_t31 isp_clk=200000000 isp_memopt=1 print_level=1" \
		> $(TARGET_DIR)/etc/modules.d/20-isp
	echo "sensor_gc2083_t31" > $(TARGET_DIR)/etc/modules.d/30-sensor
endef

$(eval $(kernel-module))
$(eval $(generic-package))
