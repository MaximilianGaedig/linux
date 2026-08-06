# SPDX-License-Identifier: GPL-2.0
#
# MediaTek MT66xx combo connectivity (WiFi/BT/GPS/FM).
#
# MTK_PLATFORM selects the SoC-specific AHB PDMA backend under
# connectivity/wlan/gen2/os/linux/hif/ahb/<plat>/.  There is no
# mt8163-specific variant; mt8127's is a plain AHB PDMA implementation
# with nothing MT8127-specific in it and is reused as-is.  Override on
# the make command line if you are building for a different SoC.
MTK_PLATFORM ?= mt8127
export MTK_PLATFORM

obj-$(CONFIG_MTK_COMBO)		+= connectivity/common/conn_soc/
obj-$(CONFIG_MTK_COMBO)		+= connectivity/common/common_detect/
obj-$(CONFIG_MTK_COMBO_WIFI)	+= connectivity/wlan/gen2/
obj-$(CONFIG_MTK_BTIF)		+= btif/
