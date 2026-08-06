# Kconfig string values used to arrive here quoted (CONFIG_FOO="bar") because
# include/config/auto.conf quoted them.  Modern kbuild writes them unquoted
# (CONFIG_MTK_COMBO_CHIP=CONSYS_8163), so every $(filter "CONSYS_%",...) test
# below silently stopped matching -- which is how MTK_WCN_SOC_CHIP_SUPPORT
# quietly went undefined and wmt_detect.c lost the declaration of
# wmt_plat_get_soc_chipid().  Strip quotes if present and match unquoted.
COMBO_CHIP := $(subst ",,$(CONFIG_MTK_COMBO_CHIP))

ifeq ($(CONFIG_MTK_COMBO),y)
	ccflags-y += -I$(src)/inc/
	ccflags-y += -I$(src)/../

ifneq ($(filter MT6630,$(COMBO_CHIP)),)
	ccflags-y += -D MTK_WCN_WLAN_GEN3
endif
ifneq ($(filter CONSYS_6797,$(COMBO_CHIP)),)
	ccflags-y += -D MTK_WCN_WLAN_GEN3
else ifneq ($(filter CONSYS_%,$(COMBO_CHIP)),)
	ccflags-y += -D MTK_WCN_WLAN_GEN2
endif

	obj-y += conn_drv_init.o
	obj-y += common_drv_init.o
	obj-y += bluetooth_drv_init.o
	obj-y += gps_drv_init.o
	obj-y += fm_drv_init.o
	obj-y += wlan_drv_init.o
	obj-($(CONFIG_MTK_COMBO_ANT)) += ant_drv_init.o

endif
