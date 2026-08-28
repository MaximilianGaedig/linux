# SPDX-License-Identifier: GPL-2.0
#
# MediaTek MT66xx combo connectivity subsystem.
#
# This file did not come with the vendored driver source -- frank-w's
# BPI-Router-Linux tree carries the leaf directories (conn_soc, common_detect,
# wlan/gen2) but not the parent Makefile that ties them together.  Without it
# several -D flags that the *code* branches on were simply never defined, so
# whole feature blocks silently compiled themselves out and then failed at
# link time with undefined references to the entry points they were supposed
# to provide.  Modelled on the equivalent file in Amazon's own 3.18 kernel for
# this device (amazon-biscuit-kernel/drivers/misc/mediatek/connectivity/
# Makefile), which is the authority on what these flags should be for
# CONSYS_8163.

subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat

# See common_detect/Makefile: Kconfig strings reach Makefiles unquoted now.
COMBO_CHIP := $(subst ",,$(CONFIG_MTK_COMBO_CHIP))

ifneq ($(CONFIG_MTK_COMBO),)

# LTE/WiFi in-device coexistence signalling.  No LTE modem on this SoC.
subdir-ccflags-y += -D WMT_IDC_SUPPORT=0

# Route the STP/WMT export surface through the function-pointer indirection
# layer in common_detect/wmt_stp_exp.c rather than calling conn_soc's
# implementations directly.  That indirection is what lets common_detect (the
# chip-detection layer, which has to load first) call into whichever transport
# driver registers itself later, instead of hard-linking against one.
# Without this define, common_detect compiles to an empty translation unit and
# mtk_wcn_wmt_ic_info_get() and friends come up undefined at link time.
subdir-ccflags-y += -D MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT

ifeq ($(CONFIG_ARM64), y)
subdir-ccflags-y += -D CONFIG_MTK_WCN_ARM64
endif

ifeq ($(CONFIG_MTK_COMBO), y)
# Built-in rather than loadable.  Both of these switch the respective driver
# from exposing module_init()/module_exit() to exposing plain init/exit
# entry points (mtk_wcn_soc_common_drv_init(), mtk_wcn_stpbt_drv_init(),
# mtk_wcn_wmt_wifi_init(), mtk_wcn_wlan_gen2_init()) that common_detect's
# drv_init/ dispatcher calls in the right order once the chip has been
# identified.  MTK_WCN_REMOVE_KERNEL_MODULE covers conn_soc; the gen2 WiFi
# driver spells the same idea MTK_WCN_BUILT_IN_DRIVER.
subdir-ccflags-y += -D MTK_WCN_REMOVE_KERNEL_MODULE
subdir-ccflags-y += -D MTK_WCN_BUILT_IN_DRIVER
endif

obj-y += common/common_detect/

ifneq ($(filter CONSYS_%,$(COMBO_CHIP)),)
obj-y += common/conn_soc/
endif

# Build the WiFi driver as a loadable module even though CONFIG_MTK_COMBO_WIFI
# is y. Everything else here has to stay built-in: conn_soc owns the
# reserved-memory node via RESERVEDMEM_OF_DECLARE, which only runs for built-in
# code, and wmt_chrdev_wifi exports the symbols wlan/gen2 links against. Having
# it as a module means a driver change is an rmmod/insmod instead of a kernel
# rebuild, a 15MB flash and a reboot - seconds instead of minutes.
obj-$(CONFIG_MTK_COMBO_WIFI) += wlan/gen2/

endif # CONFIG_MTK_COMBO
