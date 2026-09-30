################################################################################
#
# k1cam -- build variables for the one camera this tree builds
#
################################################################################

# The packages kept from thingino were written for its multi-camera Makefile,
# which worked these out per camera and exported them into Buildroot's
# environment. k1cam builds one camera, so they are set here instead, inside
# the Buildroot build (external.mk includes this before any package), and
# exported so the post-build scripts see them as well: thingino's
# rootfs_script.sh reads CAMERA, SOC_FAMILY, SOC_ARCH and the U-Boot board.

# SoC and kernel, as ingenic-lib, ingenic-sdk, raptor-hal and thingino-kopt
# read them.
export SOC_FAMILY := t31
export SOC_FAMILY_CAPS := T31
export SOC_ARCH := xburst1
export KERNEL_VERSION := 3.10.14
export KERNEL_VERSION_7 := n

# Sensor and tx-isp module parameters for /etc/modules.d (ingenic-sdk),
# from the Kconfig choices in Config.soc.in and the defconfig.
export SENSOR_1_MODEL := $(call qstrip,$(BR2_SENSOR_1_NAME))
export ISP_CLK := $(strip $(foreach mhz,90 100 120 125 150 175 200 220 225 250 300 350,\
	$(if $(BR2_ISP_CLK_$(mhz)MHZ),isp_clk=$(mhz)000000)))
export ISP_MEMOPT := $(strip $(foreach n,1 2 3,$(if $(BR2_ISP_MEMOPT_$(n)),isp_memopt=$(n))))
export ISP_PRINT_LEVEL := $(strip $(foreach n,0 1 2 3,\
	$(if $(BR2_ISP_PRINT_LEVEL_$(n)),print_level=$(n))))

# Board identity. thingino-core and thingino-uboot look for
# $(CAMERA_SUBDIR)/$(CAMERA)/thingino.json (this board has none), and
# rootfs_script.sh names the image and its bootloader in os-release.
export CAMERA := k1cam
export CAMERA_SUBDIR := board
export BR2_TARGET_UBOOT_BOARD_DEFCONFIG
