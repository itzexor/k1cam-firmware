# k1cam -- firmware for the USB webcam module of the Creality K1 3D printer.
#
#   make                 configure on first use, then build
#                        output/k1cam/images/k1cam.bin (the whole 8 MiB flash)
#   make menuconfig      change the configuration; keep it with savedefconfig
#   make savedefconfig   write it back to configs/k1cam_defconfig
#   make clean           remove output/k1cam (downloads in dl/ stay)
#   make update          fetch the Buildroot submodule and apply its patches
#   make <target>        anything else is Buildroot's: k1cam-uvcd-rebuild,
#                        linux-menuconfig, uboot-rebuild, ...
#
# The kernel is ../thingino-linux at the SHA the defconfig pins: commit there,
# then bump BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION. Like any Buildroot tree, a
# configuration change does not rebuild what was already built; after one,
# `make clean all`.

TOPDIR := $(CURDIR)
O ?= $(TOPDIR)/output/k1cam
BR2_DL_DIR ?= $(TOPDIR)/dl
# Packages build in parallel: the defconfig keeps per-package directories,
# which Buildroot requires for that.
JOBS ?= $(shell nproc)
DEFCONFIG := $(TOPDIR)/configs/k1cam_defconfig
BUILDROOT_PATCHES := $(sort $(wildcard $(TOPDIR)/package/all-patches/buildroot/*.patch))

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

# The patches go on the submodule's working tree; one already applied is
# left alone.
update:
	git -C $(TOPDIR) submodule update --init buildroot
	@for patch in $(BUILDROOT_PATCHES); do \
		if git -C $(TOPDIR)/buildroot apply --check "$$patch" 2>/dev/null; then \
			echo "applying $${patch##*/}"; \
			git -C $(TOPDIR)/buildroot apply "$$patch" || exit 1; \
		elif git -C $(TOPDIR)/buildroot apply -R --check "$$patch" 2>/dev/null; then \
			echo "already applied: $${patch##*/}"; \
		else \
			echo "cannot apply $${patch##*/}" >&2; \
			exit 1; \
		fi; \
	done

# Everything else goes to Buildroot.
%: $(O)/.config
	$(BR2_MAKE) $@

# Make must not try to rebuild the makefiles themselves through the rule above.
Makefile k1cam.mk: ;
