// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8163 AFE ASoC platform driver.
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 *
 * based on: sound/soc/mediatek/mt8183/mt8183-afe-clk.h
 * 	Copyright (c) 2018 MediaTek Inc.
 * 	Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>
 */

#ifndef _MT8163_AFE_CLK_H_
#define _MT8163_AFE_CLK_H_

/* APLL */
#define APLL1_W_NAME "APLL1"
#define APLL2_W_NAME "APLL2"
enum {
	MT8163_APLL1 = 0,
	MT8163_APLL2,
};

struct mtk_base_afe;

int mt8163_init_clock(struct mtk_base_afe *afe);
int mt8163_afe_enable_clock(struct mtk_base_afe *afe);
void mt8163_afe_disable_clock(struct mtk_base_afe *afe);

int mt8163_apll1_enable(struct mtk_base_afe *afe);
void mt8163_apll1_disable(struct mtk_base_afe *afe);

int mt8163_apll2_enable(struct mtk_base_afe *afe);
void mt8163_apll2_disable(struct mtk_base_afe *afe);

int mt8163_get_apll_rate(int apll);
int mt8163_get_apll_by_rate(int rate);
int mt8163_get_apll_by_name(const char *name);

int mt8163_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate);
void mt8163_mck_disable(struct mtk_base_afe *afe, int mck_id);
#endif
