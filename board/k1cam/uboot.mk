# K1-specific hooks for Buildroot's U-Boot package.
ifeq ($(BR2_TARGET_UBOOT)$(BR_BUILDING),yy)

# Tell the Ingenic tree to use the text environment supplied by Buildroot.
UBOOT_MAKE_OPTS += CONFIG_BOOTARGS_EXTERNAL=1

# The patched tree's tools build expects this generated-header counterpart.
define K1CAM_UBOOT_COPY_SHA1_HEADER
	if [ -f $(@D)/include/sha1.h ]; then \
		cp $(@D)/include/sha1.h $(@D)/tools/sha1.h; \
	fi
endef
UBOOT_POST_PATCH_HOOKS += K1CAM_UBOOT_COPY_SHA1_HEADER

endif
