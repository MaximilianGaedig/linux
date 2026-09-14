subdir-ccflags-y += -DDEBUG
subdir-ccflags-y += \
	-I$(src)/linux/include \
	-I$(src)/linux/pri/include \
	-I$(src)/core/include \
	-I$(src)/include \
	-I$(src)/mt8163/include \
	-I$(src)/../common_detect \
	-I$(srctree)/drivers/misc/mediatek/include/mt-plat \
	-I$(srctree)/drivers/misc/mediatek/btif/common/inc

subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/$(MTK_PLATFORM)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/eemcs/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/conn_md/include

EXT_FLAG=_soc
COMMON_SRC_PATH := $(src)
COMMON_OBJ_PATH := $(src)

ifeq ($(CONFIG_ARCH_MT6580), y)
subdir-ccflags-y += -D CFG_WMT_READ_EFUSE_VCN33
endif

ifeq ($(CONFIG_MTK_COMBO), m)
# WMT DRIVER
obj-$(CONFIG_MTK_COMBO) += mtk_stp_wmt$(EXT_FLAG).o
# WMT DRIVER-core part
mtk_stp_wmt$(EXT_FLAG)-objs	:= core/wmt_core.o core/wmt_ctrl.o core/wmt_func.o core/wmt_ic_soc.o core/wmt_lib.o core/wmt_conf.o


# WMT DRIVER-linux private part
mtk_stp_wmt$(EXT_FLAG)-objs	+= linux/pri/wmt_dev.o linux/pri/wmt_exp.o
mtk_stp_wmt$(EXT_FLAG)-objs	+= linux/pri/stp_btif.o


# WMT DRIVER-OSAL
mtk_stp_wmt$(EXT_FLAG)-objs	+= linux/pub/osal.o linux/pub/bgw_desense.o

# WMT DRIVER-platform implementation
#
# NOTE: the upstream (frank-w/BPI-Router-Linux) Makefile only wired the
# mt7623/ SoC backend (wmt_plat_alps.o, mtk_wcn_consys_hw.o) into the
# CONFIG_MTK_COMBO=y (built-in) case below, not =m (module) -- these
# symbols (wmt_plat_init, wmt_plat_pwr_ctrl, wmt_plat_gpio_ctrl, ...)
# are referenced by core/wmt_lib.c and were coming up undefined at
# modpost time when building as a module. Wired in explicitly here so
# `make M=... modules` produces a link-complete module; biscuit uses
# mt8163/ instead of mt7623/ (see that directory for the SoC-specific
# port, still in progress).
mtk_stp_wmt$(EXT_FLAG)-objs	+= mt8163/wmt_plat_alps.o mt8163/mtk_wcn_consys_hw.o

mtk_stp_wmt$(EXT_FLAG)-objs	+= linux/pri/stp_exp.o core/stp_core.o core/psm_core.o core/btm_core.o linux/pri/stp_dbg.o



obj-$(CONFIG_MTK_COMBO_BT) 	+= mtk_stp_bt$(EXT_FLAG).o
mtk_stp_bt$(EXT_FLAG)-objs	:= linux/pub/stp_chrdev_bt.o


obj-$(CONFIG_MTK_COMBO_WIFI) += mtk_wmt_wifi$(EXT_FLAG).o
mtk_wmt_wifi$(EXT_FLAG)-objs := linux/pub/wmt_chrdev_wifi.o

endif

ifeq ($(CONFIG_MTK_COMBO), y)
# subdir-ccflags-y += -D WMT_PLAT_ALPS
obj-y += core/
obj-y += linux/
#obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
obj-y += mt8163/
endif
