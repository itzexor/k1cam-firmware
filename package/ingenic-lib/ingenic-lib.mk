INGENIC_LIB_SITE_METHOD = git
INGENIC_LIB_SITE = https://github.com/gtxaspec/ingenic-lib
INGENIC_LIB_SITE_BRANCH = master
INGENIC_LIB_VERSION = 99ed33fd55fdf4fdfaa378a0924c5c6a7a31943b
INGENIC_LIB_INSTALL_STAGING = YES

INGENIC_LIB_LICENSE = GPL-2.0
INGENIC_LIB_LICENSE_FILES = COPYING

INGENIC_LIB_T31_DIR = $(@D)/T31/lib/1.1.6/uclibc/5.4.0

define INGENIC_LIB_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(INGENIC_LIB_T31_DIR)/libimp.so \
		$(STAGING_DIR)/usr/lib/libimp.so
	$(INSTALL) -D -m 0644 $(INGENIC_LIB_T31_DIR)/libalog.so \
		$(STAGING_DIR)/usr/lib/libalog.so
endef

define INGENIC_LIB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(INGENIC_LIB_T31_DIR)/libimp.so \
		$(TARGET_DIR)/usr/lib/libimp.so
endef

$(eval $(generic-package))
