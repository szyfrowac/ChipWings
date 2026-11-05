################################################################################
#
# chipwings-examples: Linux-side programs from examples/linux
#
################################################################################

CHIPWINGS_EXAMPLES_VERSION = local
CHIPWINGS_EXAMPLES_SITE = $(BR2_EXTERNAL_CHIPWINGS_PATH)/../../examples
CHIPWINGS_EXAMPLES_SITE_METHOD = local

define CHIPWINGS_EXAMPLES_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/linux CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define CHIPWINGS_EXAMPLES_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/linux DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
