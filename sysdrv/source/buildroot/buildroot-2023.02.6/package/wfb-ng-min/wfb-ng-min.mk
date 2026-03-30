################################################################################
#
# wfb-ng-min
#
################################################################################

WFB_NG_MIN_VERSION = wfb-ng-25.01.2
WFB_NG_MIN_SITE = $(call github,svpcom,wfb-ng,$(WFB_NG_MIN_VERSION))
WFB_NG_MIN_LICENSE = GPL-3.0
WFB_NG_MIN_LICENSE_FILES = LICENSE.txt
WFB_NG_MIN_DEPENDENCIES = libevent libpcap libsodium

define WFB_NG_MIN_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		AR="$(TARGET_AR)" \
		LD="$(TARGET_LD)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		CXXFLAGS="$(TARGET_CXXFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		all_bin
endef

define WFB_NG_MIN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/wfb_tx $(TARGET_DIR)/usr/bin/wfb_tx
	$(INSTALL) -D -m 0755 $(@D)/wfb_rx $(TARGET_DIR)/usr/bin/wfb_rx
	$(INSTALL) -D -m 0755 $(@D)/wfb_keygen $(TARGET_DIR)/usr/bin/wfb_keygen
	$(INSTALL) -D -m 0755 $(@D)/wfb_tun $(TARGET_DIR)/usr/bin/wfb_tun
endef

$(eval $(generic-package))
