# k1cam -- firmware for the USB webcam module of the Creality K1 3D printer.
#
#   make                 configure on first use, then build
#                        output/k1cam/images/k1cam.bin (the whole 8 MiB flash)
#   make menuconfig      change the configuration; keep it with savedefconfig
#   make savedefconfig   write it back to configs/k1cam_defconfig
#   make clean           remove output/k1cam (downloads in dl/ stay)
#   make update          initialize/update the Buildroot submodule
#   make <target>        anything else is Buildroot's: k1cam-uvcd-rebuild,
#                        linux-menuconfig, uboot-rebuild, ...
#
# The kernel is the commit the defconfig pins. Push kernel work, bump the pin,
# then `make clean all`: like any Buildroot tree, a configuration change does
# not rebuild what was already built.

TOPDIR := $(CURDIR)
O ?= $(TOPDIR)/output/k1cam
BR2_DL_DIR ?= $(TOPDIR)/dl
# Packages build in parallel: the defconfig keeps per-package directories,
# which Buildroot requires for that.
JOBS ?= $(shell nproc)
DEFCONFIG := $(TOPDIR)/configs/k1cam_defconfig
BR2_MAKE = $(MAKE) -C $(TOPDIR)/buildroot BR2_EXTERNAL=$(TOPDIR) O=$(O) \
	BR2_DL_DIR=$(BR2_DL_DIR)

.PHONY: all savedefconfig clean update

all: $(O)/.config
	$(BR2_MAKE) -j$(JOBS)

# A defconfig newer than the configuration replaces it: save menuconfig
# changes with savedefconfig first.
$(O)/.config: $(DEFCONFIG) | $(TOPDIR)/buildroot/Makefile
	$(BR2_MAKE) BR2_DEFCONFIG=$(DEFCONFIG) defconfig

savedefconfig: $(O)/.config
	$(BR2_MAKE) BR2_DEFCONFIG=$(DEFCONFIG) savedefconfig

clean:
	rm -rf $(O)

$(TOPDIR)/buildroot/Makefile:
	$(MAKE) update

update:
	git -C $(TOPDIR) submodule update --init buildroot

# Everything else goes to Buildroot.
%: $(O)/.config
	$(BR2_MAKE) $@

# Make must not try to rebuild the makefiles or the defconfig through the rule
# above: the defconfig is a prerequisite of .config, so that would be circular.
Makefile $(DEFCONFIG): ;
