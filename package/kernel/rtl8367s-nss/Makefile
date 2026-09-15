include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl8367s-nss
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/rtl8367s-nss
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=RTL8367S-VB fabric re-arm after rtl8365mb teardown
  DEPENDS:=@TARGET_qualcommax_ipq50xx
  FILES:=$(PKG_BUILD_DIR)/rtl8367s_nss.ko
endef

define KernelPackage/rtl8367s-nss/description
  Restores the trunk force word, disables CPU tag insertion and wakes
  the front PHYs after the DSA driver has been unbound.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

define KernelPackage/rtl8367s-leds
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=Case LEDs and port netdevs for a trunked RTL8367S-VB
  DEPENDS:=@TARGET_qualcommax_ipq50xx +kmod-rtl8367s-nss
  FILES:=$(PKG_BUILD_DIR)/rtl8367s_leds.ko
endef

define KernelPackage/rtl8367s-leds/description
  Cosmetics for a board whose switch has been taken off DSA: the case
  LEDs follow the front PHYs, and one display-only net_device per jack
  gives LuCI's port panel, the netdev LED triggers and the per-port
  counters something to read. Those devices carry no data.
endef

$(eval $(call KernelPackage,rtl8367s-nss))
$(eval $(call KernelPackage,rtl8367s-leds))
