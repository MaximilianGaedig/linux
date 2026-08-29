/*
** Id: //Department/DaVinci/BRANCHES/MT6620_WIFI_DRIVER_V2_3/os/linux/gl_proc.c#1
*/

/*! \file   "gl_proc.c"
    \brief  This file defines the interface which can interact with users in /proc fs.

    Detail description.
*/

/*
** Log: gl_proc.c
 *
 * 11 10 2011 cp.wu
 * [WCXRP00001098] [MT6620 Wi-Fi][Driver] Replace printk by DBG LOG macros in linux porting layer
 * 1. eliminaite direct calls to printk in porting layer.
 * 2. replaced by DBGLOG, which would be XLOG on ALPS platforms.
 *
 * 12 10 2010 kevin.huang
 * [WCXRP00000128] [MT6620 Wi-Fi][Driver] Add proc support to Android Driver for debug and driver status check
 * Add Linux Proc Support
**  \main\maintrunk.MT5921\19 2008-09-02 21:08:37 GMT mtk01461
**  Fix the compile error of SPRINTF()
**  \main\maintrunk.MT5921\18 2008-08-10 18:48:28 GMT mtk01461
**  Update for Driver Review
**  \main\maintrunk.MT5921\17 2008-08-04 16:52:01 GMT mtk01461
**  Add proc dbg print message of DOMAIN_INDEX level
**  \main\maintrunk.MT5921\16 2008-07-10 00:45:16 GMT mtk01461
**  Remove the check of MCR offset, we may use the MCR address which is not align to DW boundary or proprietary usage.
**  \main\maintrunk.MT5921\15 2008-06-03 20:49:44 GMT mtk01461
**  \main\maintrunk.MT5921\14 2008-06-02 22:56:00 GMT mtk01461
**  Rename some functions for linux proc
**  \main\maintrunk.MT5921\13 2008-06-02 20:23:18 GMT mtk01461
**  Revise PROC mcr read / write for supporting TELNET
**  \main\maintrunk.MT5921\12 2008-03-28 10:40:25 GMT mtk01461
**  Remove temporary set desired rate in linux proc
**  \main\maintrunk.MT5921\11 2008-01-07 15:07:29 GMT mtk01461
**  Add User Update Desired Rate Set for QA in Linux
**  \main\maintrunk.MT5921\10 2007-12-11 00:11:14 GMT mtk01461
**  Fix SPIN_LOCK protection
**  \main\maintrunk.MT5921\9 2007-12-04 18:07:57 GMT mtk01461
**  Add additional debug category to proc
**  \main\maintrunk.MT5921\8 2007-11-02 01:03:23 GMT mtk01461
**  Unify TX Path for Normal and IBSS Power Save + IBSS neighbor learning
**  \main\maintrunk.MT5921\7 2007-10-25 18:08:14 GMT mtk01461
**  Add VOIP SCAN Support  & Refine Roaming
** Revision 1.3  2007/07/05 07:25:33  MTK01461
** Add Linux initial code, modify doc, add 11BB, RF init code
**
** Revision 1.2  2007/06/27 02:18:51  MTK01461
** Update SCAN_FSM, Initial(Can Load Module), Proc(Can do Reg R/W), TX API
**
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/

#include "precomp.h"

/* #include "wlan_lib.h" */
/* #include "debug.h" */

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

#define PROC_WLAN_THERMO                        "wlanThermo"
#define PROC_DRV_STATUS                         "status"
#define PROC_RX_STATISTICS                      "rx_statistics"
#define PROC_TX_STATISTICS                      "tx_statistics"
#define PROC_DBG_LEVEL_NAME						"dbgLevel"
#define PROC_NEED_TX_DONE						"TxDoneCfg"
#define PROC_AUTO_PER_CFG						"autoPerCfg"
#define PROC_ROOT_NAME			"wlan"
#define PROC_CMD_DEBUG_NAME		"cmdDebug"

#define PROC_MCR_ACCESS_MAX_USER_INPUT_LEN      20
#define PROC_RX_STATISTICS_MAX_USER_INPUT_LEN   10
#define PROC_TX_STATISTICS_MAX_USER_INPUT_LEN   10
#define PROC_DBG_LEVEL_MAX_USER_INPUT_LEN       (20*10)
#define PROC_DBG_LEVEL_MAX_DISPLAY_STR_LEN      8

#define PROC_UID_SHELL							2000
#define PROC_GID_WIFI							1010

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/
/* static UINT_32 u4McrOffset; */
#if CFG_SUPPORT_THERMO_THROTTLING
static P_GLUE_INFO_T g_prGlueInfo_proc;
#endif

/*
 * biscuit_mcr: firmware-mediated MCR (MAC/BB/RF) register access.
 *
 * procMCRRead()/procMCRWrite() below already implement this, but they use the
 * pre-3.10 proc API (char *page, off_t off, int *eof) and sit inside a #if 0,
 * so nothing was ever registered - there is no way to read a single chip
 * register from userspace.  That is the one instrument needed to tell a
 * mis-programmed radio from a deaf one, so provide it with the modern
 * proc_ops API.
 *
 *   echo 60000      > /proc/net/wlan/biscuit_mcr   # select one register
 *   echo "60000 +64" > /proc/net/wlan/biscuit_mcr  # select a 64-dword window
 *   echo "60000 1234" > /proc/net/wlan/biscuit_mcr # write 0x1234
 *   cat /proc/net/wlan/biscuit_mcr
 *
 * Values are hex with no 0x prefix (the kernel's sscanf %x accepts both, but
 * "0x%x" would not match a bare digit string).  The reads go through
 * wlanoidQueryMcrRead, i.e. CMD_ID_ACCESS_REG to the firmware, so they only
 * work while the firmware is up - a wedged chip returns a read failure rather
 * than silently reporting zeros, which matters because "all zeros" is exactly
 * what a dead radio and a dead bus look like.
 */
#define PROC_BISCUIT_MCR	"biscuit_mcr"
#define BISCUIT_MCR_MAX_COUNT	256
static P_GLUE_INFO_T g_prGlueInfo_mcr;
static UINT_32 g_u4BiscuitMcrOffset;
static UINT_32 g_u4BiscuitMcrCount = 1;

/*
 * biscuit_ate: RF test (ATE) control.
 *
 * The RF-test OIDs (wlanoidRftestSetTestMode / SetAutoTest / QueryAutoTest)
 * were only ever reachable through the Android iwpriv path, which does not
 * exist here.  They are the only way to park the radio on one channel and ask
 * the firmware what it is actually receiving, which is what separates a radio
 * that hears nothing from one that hears energy it cannot demodulate.
 *
 *   echo "mode 1"     > /proc/net/wlan/biscuit_ate   # enter RF test mode
 *   echo "s 18 2437"  > /proc/net/wlan/biscuit_ate   # RF_AT_FUNCID_CHNL_FREQ
 *   echo "s 1 2"      > /proc/net/wlan/biscuit_ate   # COMMAND = STARTRX
 *   echo "q 34"       > /proc/net/wlan/biscuit_ate   # RXOK_COUNT
 *   cat /proc/net/wlan/biscuit_ate
 *   echo "mode 0"     > /proc/net/wlan/biscuit_ate   # leave RF test mode
 *
 * Useful query indices: 32 TXED, 33 TXOK, 34 RXOK, 35 RXERROR, 41 RX_PHY_STATIS,
 * 45 READ_EFUSE, 46 RX_RSSI.  Set indices: 1 COMMAND, 18 CHNL_FREQ.
 */
#define PROC_BISCUIT_ATE	"biscuit_ate"
static UINT_32 g_u4AteIdx;
static UINT_32 g_u4AteVal;
static UINT_32 g_u4AteStatus = 0xffffffff;
/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

/*******************************************************************************
*                   F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/
/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for reading MCR register to User Space, the offset of
*        the MCR is specified in u4McrOffset.
*
* \param[in] page       Buffer provided by kernel.
* \param[in out] start  Start Address to read(3 methods).
* \param[in] off        Offset.
* \param[in] count      Allowable number to read.
* \param[out] eof       End of File indication.
* \param[in] data       Pointer to the private data structure.
*
* \return number of characters print to the buffer from User Space.
*/
/*----------------------------------------------------------------------------*/
#if 0
static int procMCRRead(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	P_GLUE_INFO_T prGlueInfo;
	PARAM_CUSTOM_MCR_RW_STRUCT_T rMcrInfo;
	UINT_32 u4BufLen;
	char *p = page;
	UINT_32 u4Count;
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;

	ASSERT(data);

	/* Kevin: Apply PROC read method 1. */
	if (off != 0)
		return 0;	/* To indicate end of file. */

	prGlueInfo = *((P_GLUE_INFO_T *) netdev_priv((struct net_device *)data));

	rMcrInfo.u4McrOffset = u4McrOffset;

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidQueryMcrRead,
			   (PVOID)&rMcrInfo, sizeof(rMcrInfo), TRUE, TRUE, TRUE, FALSE, &u4BufLen);

	/* SPRINTF(p, ("MCR (0x%08lxh): 0x%08lx\n", */
	/* rMcrInfo.u4McrOffset, rMcrInfo.u4McrData)); */

	u4Count = (UINT_32) (p - page);

	*eof = 1;

	return (int)u4Count;

}				/* end of procMCRRead() */

/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for writing MCR register to HW or update u4McrOffset
*        for reading MCR later.
*
* \param[in] file   pointer to file.
* \param[in] buffer Buffer from user space.
* \param[in] count  Number of characters to write
* \param[in] data   Pointer to the private data structure.
*
* \return number of characters write from User Space.
*/
/*----------------------------------------------------------------------------*/
static int procMCRWrite(struct file *file, const char *buffer, unsigned long count, void *data)
{
	P_GLUE_INFO_T prGlueInfo;
	char acBuf[PROC_MCR_ACCESS_MAX_USER_INPUT_LEN + 1];	/* + 1 for "\0" */
	int i4CopySize;
	PARAM_CUSTOM_MCR_RW_STRUCT_T rMcrInfo;
	UINT_32 u4BufLen;
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;

	ASSERT(data);

	i4CopySize = (count < (sizeof(acBuf) - 1)) ? count : (sizeof(acBuf) - 1);
	if (copy_from_user(acBuf, buffer, i4CopySize))
		return 0;
	acBuf[i4CopySize] = '\0';

	if (sscanf(acBuf, "0x%lx 0x%lx", &rMcrInfo.u4McrOffset, &rMcrInfo.u4McrData) == 2) {
		/* NOTE: Sometimes we want to test if bus will still be ok, after accessing
		 * the MCR which is not align to DW boundary.
		 */
		/* if (IS_ALIGN_4(rMcrInfo.u4McrOffset)) */
		prGlueInfo = *((P_GLUE_INFO_T *) netdev_priv((struct net_device *)data));

		u4McrOffset = rMcrInfo.u4McrOffset;

		/* printk("Write 0x%lx to MCR 0x%04lx\n", */
		/* rMcrInfo.u4McrOffset, rMcrInfo.u4McrData); */

		rStatus = kalIoctl(prGlueInfo,
				wlanoidSetMcrWrite,
				(PVOID)&rMcrInfo, sizeof(rMcrInfo), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
	}

	if (sscanf(acBuf, "0x%lx 0x%lx", &rMcrInfo.u4McrOffset, &rMcrInfo.u4McrData) == 1) {
		/* if (IS_ALIGN_4(rMcrInfo.u4McrOffset)) */
		u4McrOffset = rMcrInfo.u4McrOffset;
	}

	return count;

}				/* end of procMCRWrite() */
#endif

#if 0
/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for reading Driver Status to User Space.
*
* \param[in] page       Buffer provided by kernel.
* \param[in out] start  Start Address to read(3 methods).
* \param[in] off        Offset.
* \param[in] count      Allowable number to read.
* \param[out] eof       End of File indication.
* \param[in] data       Pointer to the private data structure.
*
* \return number of characters print to the buffer from User Space.
*/
/*----------------------------------------------------------------------------*/
static int procDrvStatusRead(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	P_GLUE_INFO_T prGlueInfo = ((struct net_device *)data)->priv;
	char *p = page;
	UINT_32 u4Count;

	GLUE_SPIN_LOCK_DECLARATION();

	ASSERT(data);

	/* Kevin: Apply PROC read method 1. */
	if (off != 0)
		return 0;	/* To indicate end of file. */

	SPRINTF(p, ("GLUE LAYER STATUS:"));
	SPRINTF(p, ("\n=================="));

	SPRINTF(p, ("\n* Number of Pending Frames: %ld\n", prGlueInfo->u4TxPendingFrameNum));

	GLUE_ACQUIRE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

	wlanoidQueryDrvStatusForLinuxProc(prGlueInfo->prAdapter, p, &u4Count);

	GLUE_RELEASE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

	u4Count += (UINT_32) (p - page);

	*eof = 1;

	return (int)u4Count;

}				/* end of procDrvStatusRead() */

/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for reading Driver RX Statistic Counters to User Space.
*
* \param[in] page       Buffer provided by kernel.
* \param[in out] start  Start Address to read(3 methods).
* \param[in] off        Offset.
* \param[in] count      Allowable number to read.
* \param[out] eof       End of File indication.
* \param[in] data       Pointer to the private data structure.
*
* \return number of characters print to the buffer from User Space.
*/
/*----------------------------------------------------------------------------*/
static int procRxStatisticsRead(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	P_GLUE_INFO_T prGlueInfo = ((struct net_device *)data)->priv;
	char *p = page;
	UINT_32 u4Count;

	GLUE_SPIN_LOCK_DECLARATION();

	ASSERT(data);

	/* Kevin: Apply PROC read method 1. */
	if (off != 0)
		return 0;	/* To indicate end of file. */

	SPRINTF(p, ("RX STATISTICS (Write 1 to clear):"));
	SPRINTF(p, ("\n=================================\n"));

	GLUE_ACQUIRE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

	wlanoidQueryRxStatisticsForLinuxProc(prGlueInfo->prAdapter, p, &u4Count);

	GLUE_RELEASE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

	u4Count += (UINT_32) (p - page);

	*eof = 1;

	return (int)u4Count;

}				/* end of procRxStatisticsRead() */

/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for reset Driver RX Statistic Counters.
*
* \param[in] file   pointer to file.
* \param[in] buffer Buffer from user space.
* \param[in] count  Number of characters to write
* \param[in] data   Pointer to the private data structure.
*
* \return number of characters write from User Space.
*/
/*----------------------------------------------------------------------------*/
static int procRxStatisticsWrite(struct file *file, const char *buffer, unsigned long count, void *data)
{
	P_GLUE_INFO_T prGlueInfo = ((struct net_device *)data)->priv;
	char acBuf[PROC_RX_STATISTICS_MAX_USER_INPUT_LEN + 1];	/* + 1 for "\0" */
	UINT_32 u4CopySize;
	UINT_32 u4ClearCounter;

	GLUE_SPIN_LOCK_DECLARATION();

	ASSERT(data);

	u4CopySize = (count < (sizeof(acBuf) - 1)) ? count : (sizeof(acBuf) - 1);
	copy_from_user(acBuf, buffer, u4CopySize);
	acBuf[u4CopySize] = '\0';

	if (kstrtoint(acBuf, 10, &u4ClearCounter) == 1) {
		if (u4ClearCounter == 1) {
			GLUE_ACQUIRE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

			wlanoidSetRxStatisticsForLinuxProc(prGlueInfo->prAdapter);

			GLUE_RELEASE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);
		}
	}

	return count;

}				/* end of procRxStatisticsWrite() */

/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for reading Driver TX Statistic Counters to User Space.
*
* \param[in] page       Buffer provided by kernel.
* \param[in out] start  Start Address to read(3 methods).
* \param[in] off        Offset.
* \param[in] count      Allowable number to read.
* \param[out] eof       End of File indication.
* \param[in] data       Pointer to the private data structure.
*
* \return number of characters print to the buffer from User Space.
*/
/*----------------------------------------------------------------------------*/
static int procTxStatisticsRead(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	P_GLUE_INFO_T prGlueInfo = ((struct net_device *)data)->priv;
	char *p = page;
	UINT_32 u4Count;

	GLUE_SPIN_LOCK_DECLARATION();

	ASSERT(data);

	/* Kevin: Apply PROC read method 1. */
	if (off != 0)
		return 0;	/* To indicate end of file. */

	SPRINTF(p, ("TX STATISTICS (Write 1 to clear):"));
	SPRINTF(p, ("\n=================================\n"));

	GLUE_ACQUIRE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

	wlanoidQueryTxStatisticsForLinuxProc(prGlueInfo->prAdapter, p, &u4Count);

	GLUE_RELEASE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

	u4Count += (UINT_32) (p - page);

	*eof = 1;

	return (int)u4Count;

}				/* end of procTxStatisticsRead() */

/*----------------------------------------------------------------------------*/
/*!
* \brief The PROC function for reset Driver TX Statistic Counters.
*
* \param[in] file   pointer to file.
* \param[in] buffer Buffer from user space.
* \param[in] count  Number of characters to write
* \param[in] data   Pointer to the private data structure.
*
* \return number of characters write from User Space.
*/
/*----------------------------------------------------------------------------*/
static int procTxStatisticsWrite(struct file *file, const char *buffer, unsigned long count, void *data)
{
	P_GLUE_INFO_T prGlueInfo = ((struct net_device *)data)->priv;
	char acBuf[PROC_RX_STATISTICS_MAX_USER_INPUT_LEN + 1];	/* + 1 for "\0" */
	UINT_32 u4CopySize;
	UINT_32 u4ClearCounter;

	GLUE_SPIN_LOCK_DECLARATION();

	ASSERT(data);

	u4CopySize = (count < (sizeof(acBuf) - 1)) ? count : (sizeof(acBuf) - 1);
	copy_from_user(acBuf, buffer, u4CopySize);
	acBuf[u4CopySize] = '\0';

	if (kstrtoint(acBuf, 10, &u4ClearCounter) == 1) {
		if (u4ClearCounter == 1) {
			GLUE_ACQUIRE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);

			wlanoidSetTxStatisticsForLinuxProc(prGlueInfo->prAdapter);

			GLUE_RELEASE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_FSM);
		}
	}

	return count;

}				/* end of procTxStatisticsWrite() */
#endif
static struct proc_dir_entry *gprProcRoot;
static UINT_8 aucDbModuleName[][PROC_DBG_LEVEL_MAX_DISPLAY_STR_LEN] = {
	"INIT", "HAL", "INTR", "REQ", "TX", "RX", "RFTEST", "EMU", "SW1", "SW2",
	"SW3", "SW4", "HEM", "AIS", "RLM", "MEM", "CNM", "RSN", "BSS", "SCN",
	"SAA", "AAA", "P2P", "QM", "SEC", "BOW", "WAPI", "ROAMING", "TDLS", "OID",
	"NIC"
};
static UINT_8 aucProcBuf[1536];
static ssize_t procDbgLevelRead(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	UINT_8 *temp = &aucProcBuf[0];
	UINT_32 u4CopySize = 0;
	UINT_16 i;
	UINT_16 u2ModuleNum = 0;

	/* if *f_ops>0, we should return 0 to make cat command exit */
	if (*f_pos > 0)
		return 0;

	kalStrCpy(temp, "\nTEMP |LOUD |INFO |TRACE|EVENT|STATE|WARN |ERROR\n"
			"bit7 |bit6 |bit5 |bit4 |bit3 |bit2 |bit1 |bit0\n\n"
			"Debug Module\tIndex\tLevel\tDebug Module\tIndex\tLevel\n\n");
	temp += kalStrLen(temp);

	u2ModuleNum = (sizeof(aucDbModuleName) / PROC_DBG_LEVEL_MAX_DISPLAY_STR_LEN) & 0xfe;
	for (i = 0; i < u2ModuleNum; i += 2)
		SPRINTF(temp, ("DBG_%s_IDX\t(0x%02x):\t0x%02x\tDBG_%s_IDX\t(0x%02x):\t0x%02x\n",
				&aucDbModuleName[i][0], i, aucDebugModule[i],
				&aucDbModuleName[i+1][0], i+1, aucDebugModule[i+1]));

	if ((sizeof(aucDbModuleName) / PROC_DBG_LEVEL_MAX_DISPLAY_STR_LEN) & 0x1)
		SPRINTF(temp, ("DBG_%s_IDX\t(0x%02x):\t0x%02x\n",
				&aucDbModuleName[u2ModuleNum][0], u2ModuleNum, aucDebugModule[u2ModuleNum]));

	u4CopySize = kalStrLen(aucProcBuf);
	if (u4CopySize > count)
		u4CopySize = count;
	if (copy_to_user(buf, aucProcBuf, u4CopySize)) {
		kalPrint("copy to user failed\n");
		return -EFAULT;
	}

	*f_pos += u4CopySize;
	return (ssize_t)u4CopySize;
}

static ssize_t procDbgLevelWrite(struct file *file, const char *buffer, size_t count, loff_t *data)
{
	UINT_32 u4NewDbgModule, u4NewDbgLevel;
	UINT_8 i = 0;
	/* Real bug, not API drift: this was kalStrLen(aucProcBuf) -- the length
	 * of whatever the *previous* reader happened to leave in the shared
	 * buffer -- combined with "if (u4CopySize >= count + 1) u4CopySize =
	 * count;" below.  count comes straight from userspace, so count + 1
	 * wraps to 0 for count == SIZE_MAX, the test then always passes and
	 * u4CopySize becomes SIZE_MAX: an unbounded copy_from_user() into a
	 * 1536-byte static buffer.  GCC's check_copy_size() proves this at
	 * compile time (__bad_copy_to).  Clamp to the buffer instead, leaving
	 * one byte for the NUL terminator written below. */
	UINT_32 u4CopySize = sizeof(aucProcBuf) - 1;
	UINT_8 *temp = &aucProcBuf[0];

	if (count < u4CopySize)
		u4CopySize = count;

	kalMemSet(aucProcBuf, 0, sizeof(aucProcBuf));

	if (copy_from_user(aucProcBuf, buffer, u4CopySize)) {
		kalPrint("error of copy from user\n");
		return -EFAULT;
	}
	aucProcBuf[u4CopySize] = '\0';

	while (temp) {
		if (sscanf(temp, "0x%x:0x%x", &u4NewDbgModule, &u4NewDbgLevel) != 2)  {
			kalPrint("debug module and debug level should be one byte in length\n");
			break;
		}
		if (u4NewDbgModule == 0xFF) {
			for (i = 0; i < DBG_MODULE_NUM; i++)
				aucDebugModule[i] = u4NewDbgLevel & DBG_CLASS_MASK;

			break;
		} else if (u4NewDbgModule >= DBG_MODULE_NUM) {
			kalPrint("debug module index should less than %d\n", DBG_MODULE_NUM);
			break;
		}
		aucDebugModule[u4NewDbgModule] =  u4NewDbgLevel & DBG_CLASS_MASK;
		temp = kalStrChr(temp, ',');
		if (!temp)
			break;
		temp++; /* skip ',' */
	}
	return count;
}


static const struct proc_ops dbglevel_ops = {
	.proc_read = procDbgLevelRead,
	.proc_write = procDbgLevelWrite,
};

static ssize_t procTxDoneCfgRead(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	UINT_8 *temp = &aucProcBuf[0];
	UINT_32 u4CopySize = 0;
	UINT_16 u2TxDoneCfg = 0;

	/* if *f_ops>0, we should return 0 to make cat command exit */
	if (*f_pos > 0)
		return 0;

	u2TxDoneCfg = StatsGetCfgTxDone();
	SPRINTF(temp, ("Tx Done Configure:\nARP %d\tDNS %d\nTCP %d\tUDP %d\nEAPOL %d\tDHCP %d\nICMP %d\n",
			!!(u2TxDoneCfg & CFG_ARP), !!(u2TxDoneCfg & CFG_DNS), !!(u2TxDoneCfg & CFG_TCP),
			!!(u2TxDoneCfg & CFG_UDP), !!(u2TxDoneCfg & CFG_EAPOL), !!(u2TxDoneCfg & CFG_DHCP),
			!!(u2TxDoneCfg & CFG_ICMP)));

	u4CopySize = kalStrLen(aucProcBuf);
	if (u4CopySize > count)
		u4CopySize = count;
	if (copy_to_user(buf, aucProcBuf, u4CopySize)) {
		kalPrint("copy to user failed\n");
		return -EFAULT;
	}

	*f_pos += u4CopySize;
	return (ssize_t)u4CopySize;
}

static ssize_t procTxDoneCfgWrite(struct file *file, const char *buffer, size_t count, loff_t *data)
{
#define MODULE_NAME_LENGTH 6

	UINT_8 i = 0;
	/* Real bug, not API drift: this was kalStrLen(aucProcBuf) -- the length
	 * of whatever the *previous* reader happened to leave in the shared
	 * buffer -- combined with "if (u4CopySize >= count + 1) u4CopySize =
	 * count;" below.  count comes straight from userspace, so count + 1
	 * wraps to 0 for count == SIZE_MAX, the test then always passes and
	 * u4CopySize becomes SIZE_MAX: an unbounded copy_from_user() into a
	 * 1536-byte static buffer.  GCC's check_copy_size() proves this at
	 * compile time (__bad_copy_to).  Clamp to the buffer instead, leaving
	 * one byte for the NUL terminator written below. */
	UINT_32 u4CopySize = sizeof(aucProcBuf) - 1;
	UINT_8 *temp = &aucProcBuf[0];
	UINT_16 u2SetTxDoneCfg = 0;
	UINT_16 u2ClsTxDoneCfg = 0;
	UINT_8 aucModule[MODULE_NAME_LENGTH];
	UINT_32 u4Enabled;
	UINT_8 aucModuleArray[][MODULE_NAME_LENGTH] = {"ARP", "DNS", "TCP", "UDP", "EAPOL", "DHCP", "ICMP"};

	if (count < u4CopySize)
		u4CopySize = count;

	kalMemSet(aucProcBuf, 0, sizeof(aucProcBuf));
	if (copy_from_user(aucProcBuf, buffer, u4CopySize)) {
		kalPrint("error of copy from user\n");
		return -EFAULT;
	}
	aucProcBuf[u4CopySize] = '\0';
	temp = &aucProcBuf[0];
	while (temp) {
		/* pick up a string and teminated after meet : */
		if (sscanf(temp, "%s %d", aucModule, &u4Enabled) != 2)  {
			kalPrint("read param fail, aucModule=%s\n", aucModule);
			break;
		}
		for (i = 0; i < sizeof(aucModuleArray)/MODULE_NAME_LENGTH; i++) {
			if (kalStrniCmp(aucModule, aucModuleArray[i], MODULE_NAME_LENGTH) == 0) {
				if (u4Enabled)
					u2SetTxDoneCfg |= 1 << i;
				else
					u2ClsTxDoneCfg |= 1 << i;
				break;
			}
		}
		temp = kalStrChr(temp, ',');
		if (!temp)
			break;
		temp++; /* skip ',' */
	}
	if (u2SetTxDoneCfg)
		StatsSetCfgTxDone(u2SetTxDoneCfg, TRUE);

	if (u2ClsTxDoneCfg)
		StatsSetCfgTxDone(u2ClsTxDoneCfg, FALSE);
	return count;
}

static const struct proc_ops proc_txdone_ops = {
	.proc_read = procTxDoneCfgRead,
	.proc_write = procTxDoneCfgWrite,
};

static ssize_t procAutoPerCfgRead(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	UINT_8 *temp = &aucProcBuf[0];
	UINT_32 u4CopySize = 0;

	/* if *f_ops>0, we should return 0 to make cat command exit */
	if (*f_pos > 0)
		return 0;

	SPRINTF(temp, ("Auto Performance Configure:\nperiod\tL1\nL2\tL3\n"));

	u4CopySize = kalStrLen(aucProcBuf);
	if (u4CopySize > count)
		u4CopySize = count;
	if (copy_to_user(buf, aucProcBuf, u4CopySize)) {
		kalPrint("copy to user failed\n");
		return -EFAULT;
	}

	*f_pos += u4CopySize;
	return (ssize_t)u4CopySize;
}

static ssize_t procAutoPerCfgWrite(struct file *file, const char *buffer, size_t count, loff_t *data)
{
	DBGLOG(INIT, WARN, "%s\n", __func__);
	return 0;
}

static const struct proc_ops auto_per_ops = {
	.proc_read = procAutoPerCfgRead,
	.proc_write = procAutoPerCfgWrite,
};


static ssize_t procCmdDebug(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	UINT_32 u4CopySize = 0;

	/* if *f_ops>0, we should return 0 to make cat command exit */
	if (*f_pos > 0)
		return 0;

	wlanDumpTcResAndTxedCmd(aucProcBuf, sizeof(aucProcBuf));

	u4CopySize = kalStrLen(aucProcBuf);
	if (u4CopySize > count)
		u4CopySize = count;
	if (copy_to_user(buf, aucProcBuf, u4CopySize)) {
		kalPrint("copy to user failed\n");
		return -EFAULT;
	}

	*f_pos += u4CopySize;
	return (ssize_t)u4CopySize;
}

static const struct proc_ops proc_CmdDebug_ops = {
	.proc_read = procCmdDebug,
};

/*----------------------------------------------------------------------------*/
/*!
* \brief This function create a PROC fs in linux /proc/net subdirectory.
*
* \param[in] prDev      Pointer to the struct net_device.
* \param[in] pucDevName Pointer to the name of net_device.
*
* \return N/A
*/
/*----------------------------------------------------------------------------*/

#if CFG_SUPPORT_THERMO_THROTTLING

/**
 * This function is called then the /proc file is read
 *
 */
typedef struct _COEX_BUF1 {
	UINT8 buffer[128];
	INT32 availSize;
} COEX_BUF1, *P_COEX_BUF1;

COEX_BUF1 gCoexBuf1;

static ssize_t procfile_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{

	INT32 retval = 0;
	INT32 i_ret = 0;
	CHAR *warn_msg = "no data available, please run echo 15 xx > /proc/driver/wmt_psm first\n";

	if (*f_pos > 0) {
		retval = 0;
	} else {
		/*len = sprintf(page, "%d\n", g_psm_enable); */
#if 1
		if (gCoexBuf1.availSize <= 0) {
			DBGLOG(INIT, WARN, "no data available\n");
			retval = strlen(warn_msg) + 1;
			if (count < retval)
				retval = count;
			i_ret = copy_to_user(buf, warn_msg, retval);
			if (i_ret) {
				DBGLOG(INIT, ERROR, "copy to buffer failed, ret:%d\n", retval);
				retval = -EFAULT;
				goto err_exit;
			}
			*f_pos += retval;
		} else
#endif
		{
			INT32 i = 0;
			INT32 len = 0;
			CHAR msg_info[128];
			INT32 max_num = 0;
			/*we do not check page buffer, because there are only 100 bytes in g_coex_buf, no reason page
			buffer is not enough, a bomb is placed here on unexpected condition */

			DBGLOG(INIT, TRACE, "%d bytes available\n", gCoexBuf1.availSize);
			max_num = ((sizeof(msg_info) > count ? sizeof(msg_info) : count) - 1) / 5;

			if (max_num > gCoexBuf1.availSize)
				max_num = gCoexBuf1.availSize;
			else
				DBGLOG(INIT, TRACE,
				"round to %d bytes due to local buffer size limitation\n", max_num);

			for (i = 0; i < max_num; i++)
				len += sprintf(msg_info + len, "%d", gCoexBuf1.buffer[i]);

			len += sprintf(msg_info + len, "\n");
			retval = len;

			i_ret = copy_to_user(buf, msg_info, retval);
			if (i_ret) {
				DBGLOG(INIT, ERROR, "copy to buffer failed, ret:%d\n", retval);
				retval = -EFAULT;
				goto err_exit;
			}
			*f_pos += retval;
		}
	}
	gCoexBuf1.availSize = 0;
err_exit:

	return retval;
}

#if 1
typedef INT32 (*WLAN_DEV_DBG_FUNC)(void);
static INT32 wlan_get_thermo_power(void);
static INT32 wlan_get_link_mode(void);

static const WLAN_DEV_DBG_FUNC wlan_dev_dbg_func[] = {
	[0] = wlan_get_thermo_power,
	[1] = wlan_get_link_mode,

};

INT32 wlan_get_thermo_power(void)
{
	P_ADAPTER_T prAdapter;

	prAdapter = g_prGlueInfo_proc->prAdapter;

	if (prAdapter->u4AirDelayTotal > 100)
		gCoexBuf1.buffer[0] = 100;
	else
		gCoexBuf1.buffer[0] = prAdapter->u4AirDelayTotal;
	gCoexBuf1.availSize = 1;
	DBGLOG(RLM, TRACE, "PROC %s thrmo_power(%d)\n", __func__, gCoexBuf1.buffer[0]);

	return 0;
}

INT32 wlan_get_link_mode(void)
{
	UINT_8 ucLinkMode = 0;
	P_ADAPTER_T prAdapter;
	BOOLEAN fgIsAPmode;

	prAdapter = g_prGlueInfo_proc->prAdapter;
	fgIsAPmode = p2pFuncIsAPMode(prAdapter->rWifiVar.prP2pFsmInfo);

	DBGLOG(RLM, TRACE, "PROC %s AIS(%d)P2P(%d)AP(%d)\n",
			   __func__,
			   prAdapter->rWifiVar.arBssInfo[NETWORK_TYPE_AIS_INDEX].eConnectionState,
			   prAdapter->rWifiVar.arBssInfo[NETWORK_TYPE_P2P_INDEX].eConnectionState, fgIsAPmode);

#if 1

	if (prAdapter->rWifiVar.arBssInfo[NETWORK_TYPE_AIS_INDEX].eConnectionState == PARAM_MEDIA_STATE_CONNECTED)
		ucLinkMode |= BIT(0);
	if (prAdapter->rWifiVar.arBssInfo[NETWORK_TYPE_P2P_INDEX].eConnectionState == PARAM_MEDIA_STATE_CONNECTED)
		ucLinkMode |= BIT(1);
	if (fgIsAPmode)
		ucLinkMode |= BIT(2);

#endif
	gCoexBuf1.buffer[0] = ucLinkMode;
	gCoexBuf1.availSize = 1;

	return 0;
}

static ssize_t procfile_write(struct file *filp, const char __user *buffer, size_t count, loff_t *f_pos)
{
	char buf[256];
	char *pBuf;
	ULONG len = count;
	INT32 x = 0, y = 0, z = 0;
	char *pToken = NULL;
	char *pDelimiter = " \t";
	INT32 i4Ret = 0;

	if (copy_from_user(gCoexBuf1.buffer, buffer, count))
		return -EFAULT;
	/* gCoexBuf1.availSize = count; */

	/* return gCoexBuf1.availSize; */
#if 1
	DBGLOG(INIT, TRACE, "write parameter len = %d\n\r", (INT32) len);
	if (len >= sizeof(buf)) {
		DBGLOG(INIT, ERROR, "input handling fail!\n");
		len = sizeof(buf) - 1;
		return -1;
	}

	if (copy_from_user(buf, buffer, len))
		return -EFAULT;
	buf[len] = '\0';
	DBGLOG(INIT, TRACE, "write parameter data = %s\n\r", buf);

	pBuf = buf;
	pToken = strsep(&pBuf, pDelimiter);

	if (pToken) /* x = NULL != pToken ? simple_strtol(pToken, NULL, 16) : 0; */
		i4Ret = kalkStrtos32(pToken, 16, &x);
	if (!i4Ret)
		DBGLOG(INIT, TRACE, "x = 0x%x\n", x);

#if 1
	pToken = strsep(&pBuf, "\t\n ");
	if (pToken != NULL) {
		i4Ret = kalkStrtos32(pToken, 16, &y); /* y = simple_strtol(pToken, NULL, 16); */
		if (!i4Ret)
			DBGLOG(INIT, TRACE, "y = 0x%08x\n\r", y);
	} else {
		y = 3000;
		/*efuse, register read write default value */
		if (0x11 == x || 0x12 == x || 0x13 == x)
			y = 0x80000000;
	}

	pToken = strsep(&pBuf, "\t\n ");
	if (pToken != NULL) {
		i4Ret = kalkStrtos32(pToken, 16, &z); /* z = simple_strtol(pToken, NULL, 16); */
		if (!i4Ret)
			DBGLOG(INIT, TRACE, "z = 0x%08x\n\r", z);
	} else {
		z = 10;
		/*efuse, register read write default value */
		if (0x11 == x || 0x12 == x || 0x13 == x)
			z = 0xffffffff;
	}

	DBGLOG(INIT, TRACE, " x(0x%08x), y(0x%08x), z(0x%08x)\n\r", x, y, z);
#endif

	if (((sizeof(wlan_dev_dbg_func) / sizeof(wlan_dev_dbg_func[0])) > x) && NULL != wlan_dev_dbg_func[x])
		(*wlan_dev_dbg_func[x]) ();
	else
		DBGLOG(INIT, ERROR, "no handler defined for command id(0x%08x)\n\r", x);
#endif

	/* len = gCoexBuf1.availSize; */
	return len;
}
#endif
	static const struct proc_ops proc_fops = {
		.proc_read = procfile_read,
		.proc_write = procfile_write,
	};
#endif

static ssize_t procBiscuitMcrRead(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	PARAM_CUSTOM_MCR_RW_STRUCT_T rMcrInfo;
	UINT_32 u4BufLen = 0;
	UINT_32 i;
	WLAN_STATUS rStatus;
	char *pcBuf;
	size_t cap;
	int len = 0;

	if (*f_pos > 0)
		return 0;
	if (!g_prGlueInfo_mcr)
		return -ENODEV;

	cap = (size_t) g_u4BiscuitMcrCount * 32 + 64;
	pcBuf = kalMemAlloc(cap, VIR_MEM_TYPE);
	if (!pcBuf)
		return -ENOMEM;

	for (i = 0; i < g_u4BiscuitMcrCount; i++) {
		if ((size_t) len + 40 >= cap)
			break;

		kalMemZero(&rMcrInfo, sizeof(rMcrInfo));
		rMcrInfo.u4McrOffset = g_u4BiscuitMcrOffset + (i * 4);

		rStatus = kalIoctl(g_prGlueInfo_mcr,
				   wlanoidQueryMcrRead,
				   (PVOID) &rMcrInfo,
				   sizeof(rMcrInfo), TRUE, TRUE, TRUE, FALSE, &u4BufLen);

		if (rStatus != WLAN_STATUS_SUCCESS) {
			len += snprintf(pcBuf + len, cap - len, "0x%08x: READ-FAILED(0x%08x)\n",
					(unsigned int)rMcrInfo.u4McrOffset, (unsigned int)rStatus);
			break;
		}

		len += snprintf(pcBuf + len, cap - len, "0x%08x: 0x%08x\n",
				(unsigned int)rMcrInfo.u4McrOffset, (unsigned int)rMcrInfo.u4McrData);
	}

	if ((size_t) len > count)
		len = (int)count;

	if (copy_to_user(buf, pcBuf, len)) {
		kalMemFree(pcBuf, VIR_MEM_TYPE, cap);
		return -EFAULT;
	}

	kalMemFree(pcBuf, VIR_MEM_TYPE, cap);
	*f_pos += len;
	return len;
}

static ssize_t procBiscuitMcrWrite(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	char acBuf[64];
	unsigned int u4Off = 0, u4Val = 0, u4Cnt = 0;
	PARAM_CUSTOM_MCR_RW_STRUCT_T rMcrInfo;
	UINT_32 u4BufLen = 0;
	size_t len;

	len = (count < (sizeof(acBuf) - 1)) ? count : (sizeof(acBuf) - 1);
	if (copy_from_user(acBuf, buf, len))
		return -EFAULT;
	acBuf[len] = '\0';

	/* "<off> +<n>" must be tried before "<off> <val>": the kernel's %x
	 * accepts a leading '+', so a range request would otherwise be taken
	 * as a register write of that value.
	 */
	if (sscanf(acBuf, "%x +%u", &u4Off, &u4Cnt) == 2) {
		g_u4BiscuitMcrOffset = u4Off;
		if (u4Cnt == 0)
			u4Cnt = 1;
		g_u4BiscuitMcrCount = (u4Cnt > BISCUIT_MCR_MAX_COUNT) ? BISCUIT_MCR_MAX_COUNT : u4Cnt;
	} else if (sscanf(acBuf, "%x %x", &u4Off, &u4Val) == 2) {
		if (!g_prGlueInfo_mcr)
			return -ENODEV;
		kalMemZero(&rMcrInfo, sizeof(rMcrInfo));
		rMcrInfo.u4McrOffset = u4Off;
		rMcrInfo.u4McrData = u4Val;
		kalIoctl(g_prGlueInfo_mcr,
			 wlanoidSetMcrWrite,
			 (PVOID) &rMcrInfo, sizeof(rMcrInfo), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
		g_u4BiscuitMcrOffset = u4Off;
		g_u4BiscuitMcrCount = 1;
	} else if (sscanf(acBuf, "%x", &u4Off) == 1) {
		g_u4BiscuitMcrOffset = u4Off;
		g_u4BiscuitMcrCount = 1;
	}

	return count;
}

static const struct proc_ops biscuit_mcr_ops = {
	.proc_read = procBiscuitMcrRead,
	.proc_write = procBiscuitMcrWrite,
};

static ssize_t procBiscuitAteRead(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	char acBuf[96];
	int len;

	if (*f_pos > 0)
		return 0;

	len = snprintf(acBuf, sizeof(acBuf), "idx=%u data=0x%08x (%u) status=0x%08x\n",
		       (unsigned int)g_u4AteIdx, (unsigned int)g_u4AteVal,
		       (unsigned int)g_u4AteVal, (unsigned int)g_u4AteStatus);

	if ((size_t) len > count)
		len = (int)count;
	if (copy_to_user(buf, acBuf, len))
		return -EFAULT;

	*f_pos += len;
	return len;
}

static ssize_t procBiscuitAteWrite(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	char acBuf[64];
	unsigned int u4Idx = 0, u4Val = 0, u4Mode = 0;
	PARAM_MTK_WIFI_TEST_STRUCT_T rAtInfo;
	UINT_32 u4BufLen = 0;
	size_t len;

	if (!g_prGlueInfo_mcr)
		return -ENODEV;

	len = (count < (sizeof(acBuf) - 1)) ? count : (sizeof(acBuf) - 1);
	if (copy_from_user(acBuf, buf, len))
		return -EFAULT;
	acBuf[len] = '\0';

	if (sscanf(acBuf, "mode %u", &u4Mode) == 1) {
		/* Both OIDs take a zero-length set buffer but still ASSERT() on a
		 * non-NULL pointer, so pass the scratch struct with length 0.
		 */
		g_u4AteStatus = kalIoctl(g_prGlueInfo_mcr,
					 u4Mode ? wlanoidRftestSetTestMode : wlanoidRftestSetAbortTestMode,
					 (PVOID) &rAtInfo, 0, FALSE, FALSE, TRUE, FALSE, &u4BufLen);
		g_u4AteIdx = 0;
		g_u4AteVal = u4Mode;
	} else if (sscanf(acBuf, "s %u %u", &u4Idx, &u4Val) == 2) {
		rAtInfo.u4FuncIndex = u4Idx;
		rAtInfo.u4FuncData = u4Val;
		g_u4AteStatus = kalIoctl(g_prGlueInfo_mcr,
					 wlanoidRftestSetAutoTest,
					 (PVOID) &rAtInfo, sizeof(rAtInfo),
					 FALSE, FALSE, TRUE, FALSE, &u4BufLen);
		g_u4AteIdx = u4Idx;
		g_u4AteVal = u4Val;
	} else if (sscanf(acBuf, "q %u", &u4Idx) == 1) {
		rAtInfo.u4FuncIndex = u4Idx;
		rAtInfo.u4FuncData = 0;
		g_u4AteStatus = kalIoctl(g_prGlueInfo_mcr,
					 wlanoidRftestQueryAutoTest,
					 (PVOID) &rAtInfo, sizeof(rAtInfo),
					 TRUE, TRUE, TRUE, FALSE, &u4BufLen);
		g_u4AteIdx = u4Idx;
		g_u4AteVal = rAtInfo.u4FuncData;
	}

	return count;
}

static const struct proc_ops biscuit_ate_ops = {
	.proc_read = procBiscuitAteRead,
	.proc_write = procBiscuitAteWrite,
};

INT_32 procInitFs(VOID)
{
	struct proc_dir_entry *prEntry;

	if (init_net.proc_net == (struct proc_dir_entry *)NULL) {
		kalPrint("init proc fs fail: proc_net == NULL\n");
		return -ENOENT;
	}

	/*
	 * Directory: Root (/proc/net/wlan0)
	 */

	gprProcRoot = proc_mkdir(PROC_ROOT_NAME, init_net.proc_net);
	if (!gprProcRoot) {
		kalPrint("gprProcRoot == NULL\n");
		return -ENOENT;
	}
	proc_set_user(gprProcRoot, KUIDT_INIT(PROC_UID_SHELL), KGIDT_INIT(PROC_GID_WIFI));

	prEntry = proc_create(PROC_DBG_LEVEL_NAME, 0664, gprProcRoot, &dbglevel_ops);
	if (prEntry == NULL) {
		kalPrint("Unable to create /proc entry dbgLevel\n\r");
		return -1;
	}
	proc_set_user(prEntry, KUIDT_INIT(PROC_UID_SHELL), KGIDT_INIT(PROC_GID_WIFI));

	prEntry = proc_create(PROC_NEED_TX_DONE, 0664, gprProcRoot, &proc_txdone_ops);
	if (prEntry == NULL) {
		kalPrint("Unable to create /proc entry dbgLevel\n\r");
		return -1;
	}
	proc_set_user(prEntry, KUIDT_INIT(PROC_UID_SHELL), KGIDT_INIT(PROC_GID_WIFI));

	prEntry = proc_create(PROC_AUTO_PER_CFG, 0664, gprProcRoot, &auto_per_ops);
	if (prEntry == NULL) {
		kalPrint("Unable to create /proc entry autoPerCfg\n\r");
		return -1;
	}
	proc_set_user(prEntry, KUIDT_INIT(PROC_UID_SHELL), KGIDT_INIT(PROC_GID_WIFI));

	return 0;
}				/* end of procInitProcfs() */

INT_32 procUninitProcFs(VOID)
{
	remove_proc_entry(PROC_DBG_LEVEL_NAME, gprProcRoot);
	remove_proc_subtree(PROC_ROOT_NAME, init_net.proc_net);
	remove_proc_entry(PROC_AUTO_PER_CFG, gprProcRoot);
	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
* \brief This function clean up a PROC fs created by procInitProcfs().
*
* \param[in] prDev      Pointer to the struct net_device.
* \param[in] pucDevName Pointer to the name of net_device.
*
* \return N/A
*/
/*----------------------------------------------------------------------------*/
INT_32 procRemoveProcfs(VOID)
{
	/* remove root directory (proc/net/wlan0) */
	/* remove_proc_entry(pucDevName, init_net.proc_net); */
	remove_proc_entry(PROC_WLAN_THERMO, gprProcRoot);
	remove_proc_entry(PROC_CMD_DEBUG_NAME, gprProcRoot);
	remove_proc_entry(PROC_BISCUIT_MCR, gprProcRoot);
	remove_proc_entry(PROC_BISCUIT_ATE, gprProcRoot);
	g_prGlueInfo_mcr = NULL;
#if CFG_SUPPORT_THERMO_THROTTLING
	g_prGlueInfo_proc = NULL;
#endif
	return 0;
}				/* end of procRemoveProcfs() */

INT_32 procCreateFsEntry(P_GLUE_INFO_T prGlueInfo)
{
	struct proc_dir_entry *prEntry;

	DBGLOG(INIT, TRACE, "[%s]\n", __func__);

#if CFG_SUPPORT_THERMO_THROTTLING
	g_prGlueInfo_proc = prGlueInfo;
#endif

	prGlueInfo->pProcRoot = gprProcRoot;

	prEntry = proc_create(PROC_WLAN_THERMO, 0664, gprProcRoot, &proc_fops);
	if (prEntry == NULL) {
		DBGLOG(INIT, ERROR, "Unable to create /proc entry\n\r");
		return -1;
	}

	prEntry = proc_create(PROC_CMD_DEBUG_NAME, 0444, gprProcRoot, &proc_CmdDebug_ops);
	if (prEntry == NULL) {
		kalPrint("Unable to create /proc entry dbgLevel\n\r");
		return -1;
	}
	proc_set_user(prEntry, KUIDT_INIT(PROC_UID_SHELL), KGIDT_INIT(PROC_GID_WIFI));

	g_prGlueInfo_mcr = prGlueInfo;
	prEntry = proc_create(PROC_BISCUIT_MCR, 0664, gprProcRoot, &biscuit_mcr_ops);
	if (prEntry == NULL) {
		kalPrint("Unable to create /proc entry biscuit_mcr\n\r");
		return -1;
	}

	prEntry = proc_create(PROC_BISCUIT_ATE, 0664, gprProcRoot, &biscuit_ate_ops);
	if (prEntry == NULL) {
		kalPrint("Unable to create /proc entry biscuit_ate\n\r");
		return -1;
	}

	return 0;
}

