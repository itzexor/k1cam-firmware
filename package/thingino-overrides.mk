################################################################################
#
# Thingino package overrides entry point
#
################################################################################

# Add new overrides here so we only need a single BR2_PACKAGE_OVERRIDE_FILE.
# Keep the includes alphabetized for readability. (None of the overrides that
# lived here applied to a package the K1 build uses.)

# Allow developers to keep personal overrides in either the root local.mk
# (ignored by git) or the default $(CONFIG_DIR)/local.mk without losing this
# aggregated file.
THINGINO_EXTERNAL_PATH := $(patsubst "%",%,$(strip $(BR2_EXTERNAL_THINGINO_PATH)))
-include $(THINGINO_EXTERNAL_PATH)/local.mk
-include $(CONFIG_DIR)/local.mk

