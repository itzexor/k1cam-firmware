INGENIC_UCLIBC_VERSION = e266b12ddab5dc1685d3df3a7723659b97c2f12e
INGENIC_UCLIBC_SITE = https://github.com/gtxaspec/ingenic-uclibc
INGENIC_UCLIBC_SITE_METHOD = git
INGENIC_UCLIBC_INSTALL_STAGING = YES

INGENIC_UCLIBC_LICENSE = MIT
INGENIC_UCLIBC_LICENSE_FILES = LICENSE

INGENIC_UCLIBC_CFLAGS = -Os -ffunction-sections -fdata-sections -flto \
	-fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident

define INGENIC_UCLIBC_BUILD_CMDS
	$(TARGET_CC) $(INGENIC_UCLIBC_CFLAGS) -fPIC -shared -o $(@D)/libuclibcshim.so $(@D)/uclibc_shim.c
	$(TARGET_CC) $(INGENIC_UCLIBC_CFLAGS) -c -o $(@D)/uclibc_shim.o $(@D)/uclibc_shim.c
	$(TARGET_CROSS)gcc-ar rcs $(@D)/libuclibcshim.a $(@D)/uclibc_shim.o
endef

define INGENIC_UCLIBC_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libuclibcshim.so $(STAGING_DIR)/usr/lib/libuclibcshim.so
	$(INSTALL) -D -m 0644 $(@D)/libuclibcshim.a $(STAGING_DIR)/usr/lib/libuclibcshim.a
endef

define INGENIC_UCLIBC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libuclibcshim.so $(TARGET_DIR)/usr/lib/libuclibcshim.so
endef

$(eval $(generic-package))
