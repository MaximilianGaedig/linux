/*
* Copyright (C) 2011-2014 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG         "[WCN-MOD-INIT]"

#include <linux/mutex.h>
#include "wmt_detect.h"
#include "conn_drv_init.h"
#include "common_drv_init.h"
#include "fm_drv_init.h"
#include "wlan_drv_init.h"
#include "bluetooth_drv_init.h"
#include "gps_drv_init.h"
#include "ant_drv_init.h"

int __weak do_wlan_drv_init(int chip_id)
{
	WMT_DETECT_ERR_FUNC("Can not find wlan module for chip: %d !\n", chip_id);
	return 0;
}

int __weak do_ant_drv_init(int chip_id)
{
	WMT_DETECT_DBG_FUNC("Chip: %d can not support ANT !\n", chip_id);
	return 0;
}

static int __do_connectivity_driver_init(int chip_id)
{
	int i_ret = 0;
	int tmp_ret = 0;

	tmp_ret = do_common_drv_init(chip_id);
	i_ret += tmp_ret;
	if (tmp_ret) {
		WMT_DETECT_ERR_FUNC("do common driver init failed, ret:%d\n", tmp_ret);
		WMT_DETECT_ERR_FUNC("abort connectivity driver init, because common part is not ready\n");
		return i_ret;
	}

	tmp_ret = do_bluetooth_drv_init(chip_id);
	i_ret += tmp_ret;
	if (tmp_ret)
		WMT_DETECT_ERR_FUNC("do bluetooth driver init failed, ret:%d\n", tmp_ret);

	tmp_ret = do_gps_drv_init(chip_id);
	i_ret += tmp_ret;
	if (tmp_ret)
		WMT_DETECT_ERR_FUNC("do gps driver init failed, ret:%d\n", tmp_ret);

	tmp_ret = do_fm_drv_init(chip_id);
	i_ret += tmp_ret;
	if (tmp_ret)
		WMT_DETECT_ERR_FUNC("do fm module init failed, ret:%d\n", tmp_ret);

	tmp_ret = do_wlan_drv_init(chip_id);
	i_ret += tmp_ret;
	if (tmp_ret)
		WMT_DETECT_ERR_FUNC("do wlan module init failed, ret:%d\n", tmp_ret);

	tmp_ret = do_ant_drv_init(chip_id);
	i_ret += tmp_ret;
	if (tmp_ret)
		WMT_DETECT_ERR_FUNC("do ANT module init failed, ret:%d\n", tmp_ret);

	return i_ret;
}

/*
 * Creates the WMT core (WMT_init) and the function drivers exactly once.
 * It is reached both from the COMBO_IOCTL_DO_MODULE_INIT ioctl and, on
 * boards without the Android loader, from the first in-kernel consumer
 * (see mtk_wcn_wmt_core_up()); WMT_init must not run twice.
 */
int do_connectivity_driver_init(int chip_id)
{
	static DEFINE_MUTEX(lock);
	static bool done;
	static int ret;

	mutex_lock(&lock);
	if (!done) {
		ret = __do_connectivity_driver_init(chip_id);
		done = true;
	}
	mutex_unlock(&lock);
	return ret;
}
