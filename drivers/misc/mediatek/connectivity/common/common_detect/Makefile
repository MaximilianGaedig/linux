# Kconfig string values used to arrive here quoted (CONFIG_FOO="bar") because
# include/config/auto.conf quoted them.  Modern kbuild writes them unquoted
# (CONFIG_MTK_COMBO_CHIP=CONSYS_8163), so every $(filter "CONSYS_%",...) test
# below silently stopped matching -- which is how MTK_WCN_SOC_CHIP_SUPPORT
# quietly went undefined and wmt_detect.c lost the declaration of
# wmt_plat_get_soc_chipid().  Strip quotes if present and match unquoted.
COMBO_CHIP := $(subst ",,$(CONFIG_MTK_COMBO_CHIP))

subdir-ccflags-y += -I$(srctree)/arch/arm/mach-$(MTK_PLATFORM)/$(ARCH_MTK_PROJECT)/dct/dct
# mtk_wcn_cmb_stub.h / mtk_rtc.h live here.  conn_soc's Makefile already
# adds this path, which is why these files built when they were pulled in
# as part of a `make M=.../conn_soc` build; when the kernel descends into
# this directory in its own right (obj-$(CONFIG_MTK_COMBO) += common_detect/
# from drivers/misc/mediatek/Makefile) nothing else supplies it.
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
subdir-ccflags-y += -I$(src)/drv_init/inc
subdir-ccflags-y += -I$(src)/../conn_soc/linux/include
subdir-ccflags-y += -DWMT_PLAT_ALPS=1

COMBO_CHIP_SUPPORT := false
ifneq ($(filter MT6620E3,$(COMBO_CHIP)),)
	COMBO_CHIP_SUPPORT := true
endif
ifneq ($(filter MT6628,$(COMBO_CHIP)),)
	COMBO_CHIP_SUPPORT := true
endif
ifneq ($(filter MT6630,$(COMBO_CHIP)),)
	COMBO_CHIP_SUPPORT := true
endif
ifeq ($(COMBO_CHIP_SUPPORT), true)
	subdir-ccflags-y += -D MTK_WCN_COMBO_CHIP_SUPPORT
	ccflags-y += -I$(src)/../combo/linux/include
endif

ifneq ($(filter CONSYS_%,$(COMBO_CHIP)),)
	subdir-ccflags-y += -D MTK_WCN_SOC_CHIP_SUPPORT
	ccflags-y += -I$(src)/../conn_soc/linux/include
endif


ifeq ($(CONFIG_MTK_COMBO),y)
	ccflags-y += -I$(src)/drv_init/inc
	obj-y += mtk_wcn_stub_alps.o
	obj-y += wmt_stp_exp.o
	obj-y += wmt_gpio.o

	obj-y += wmt_detect.o
	obj-y += sdio_detect.o
	obj-y += wmt_detect_pwr.o

	obj-y += drv_init/
endif

ifeq ($(CONFIG_MTK_COMBO),m)
	obj-$(CONFIG_MTK_COMBO) += mtk_wmt_detect.o
	mtk_wmt_detect-objs := wmt_detect.o
	mtk_wmt_detect-objs += sdio_detect.o
	mtk_wmt_detect-objs += wmt_detect_pwr.o
	# These three were obj-y, which cannot work here: this whole subtree is
	# reached through obj-m, so a built-in object in it is silently dropped
	# and every symbol it defines (mtk_wcn_wmt_chipid_query,
	# mtk_wcn_wmt_set_chipid, mt_combo_plt_*_deep_idle, gpio_ctrl_info, the
	# entire wmt_stp_exp indirection layer) comes up undefined at modpost
	# time.  They belong in the same module as the detect code that uses
	# them.
	mtk_wmt_detect-objs += mtk_wcn_stub_alps.o
	mtk_wmt_detect-objs += wmt_stp_exp.o
	mtk_wmt_detect-objs += wmt_gpio.o
endif
