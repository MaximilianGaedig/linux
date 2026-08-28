/*
 * Replicate what Amazon's stock userspace does to bring WiFi up, in the same
 * order and with the same arguments, so that nothing is left to assumption.
 *
 * Reconstructed from the device's own /system/bin/wmt_loader and
 * /system/bin/6620_launcher (extracted from system_b and disassembled), and
 * checked against the driver's ioctl table in wmt_dev.c (magic 0xa0).
 *
 * wmt_loader:
 *     open /dev/wmtdetect, detect the chip, COMBO_IOCTL_DO_MODULE_INIT(chipid)
 *
 * 6620_launcher, on /dev/stpwmt, in this order:
 *     22 WMT_QUERY_CHIPID       read the chip id back
 *     20 PORT_NAME              the STP transport's port name
 *     21 WMT_CFG_NAME           the WMT config file name
 *     14 SET_PATCH_NUM          how many ROM patches
 *     15 SET_PATCH_INFO         per patch: seq, address, name
 *      4 SET_PATCH_NAME         patch folder/prefix
 *      5 SET_STP_MODE           transport mode
 *      6 FUNC_ONOFF_CTRL        turn the WiFi function on
 *     13 SET_LAUNCHER_KILL      tell the driver the launcher is going away
 *     24 WMT_COREDUMP_CTRL      coredump on/off
 *
 * The patch address is the one the launcher computes: bytes 24..27 of the
 * patch header with the low byte cleared (it overwrites addRess[0] with the
 * result of the SW-version comparison, which is zero on the accepting path).
 * See the long comment in wmt_ic_soc.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WMT_MAGIC 0xa0
#define IOC_W(nr, sz)   ((1u << 30) | (WMT_MAGIC << 8) | (nr) | ((sz) << 16))
#define IOC_R(nr, sz)   ((2u << 30) | (WMT_MAGIC << 8) | (nr) | ((sz) << 16))
#define IOC_WR(nr, sz)  ((3u << 30) | (WMT_MAGIC << 8) | (nr) | ((sz) << 16))

/* wmt_dev.c */
#define WMT_IOCTL_SET_PATCH_NAME    IOC_W(4, sizeof(char *))
#define WMT_IOCTL_SET_STP_MODE      IOC_W(5, sizeof(int))
#define WMT_IOCTL_FUNC_ONOFF_CTRL   IOC_W(6, sizeof(int))
#define WMT_IOCTL_SET_LAUNCHER_KILL IOC_W(13, sizeof(int))
#define WMT_IOCTL_SET_PATCH_NUM     IOC_W(14, sizeof(int))
#define WMT_IOCTL_SET_PATCH_INFO    IOC_W(15, sizeof(char *))
#define WMT_IOCTL_PORT_NAME         IOC_WR(20, sizeof(char *))
#define WMT_IOCTL_WMT_CFG_NAME      IOC_WR(21, sizeof(char *))
#define WMT_IOCTL_WMT_QUERY_CHIPID  IOC_R(22, sizeof(int))
#define WMT_IOCTL_COREDUMP_CTRL     IOC_W(24, sizeof(int))

/*
 * common_detect uses a different magic ('w') and _IOR, not the 0xa0/_IOW the
 * WMT device uses - mixing them up gets "unknown cmd (4)" from the detect
 * driver and then /dev/stpwmt never appears.
 */
#define DETECT_MAGIC 'w'
#define DETECT_IOR(nr) ((2u << 30) | (DETECT_MAGIC << 8) | (nr) | (4u << 16))
#define COMBO_IOCTL_DO_MODULE_INIT  DETECT_IOR(4)

/* wmt_lib.h WMT_PATCH_INFO */
struct wmt_patch_info {
	unsigned int dowload_seq;
	unsigned char address[4];
	unsigned char name[256];
};

static const char *const patch_files[] = {
	"mediatek/ROMv2_lm_patch_1_0_hdr.bin",
	"mediatek/ROMv2_lm_patch_1_1_hdr.bin",
};
#define PATCH_NUM ((int)(sizeof(patch_files) / sizeof(patch_files[0])))

static int say(const char *what, int ret)
{
	printf("  %-26s ret=%d%s%s\n", what, ret,
	       ret < 0 ? " errno=" : "", ret < 0 ? strerror(errno) : "");
	return ret;
}

/*
 * Read the patch's destination the way the launcher does: seek to 22, read the
 * two SW-version bytes, then read bytes 24..27, then clear the low byte.
 */
static int patch_address(const char *fw_relative, unsigned char out[4])
{
	char path[320];
	int fd;

	snprintf(path, sizeof(path), "/lib/firmware/%s", fw_relative);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("  cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (lseek(fd, 24, SEEK_SET) < 0 || read(fd, out, 4) != 4) {
		close(fd);
		return -1;
	}
	close(fd);
	out[0] = 0;	/* the launcher overwrites this with the version check */
	return 0;
}

int main(int argc, char **argv)
{
	int chipid = (argc > 1) ? (int)strtol(argv[1], NULL, 0) : 0x6625;
	int stp_mode = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0x23;
	int fd, wmt, i, v;
	struct wmt_patch_info pi;
	char portname[64] = "";
	char cfgname[64] = "WMT_SOC.cfg";
	char patchname[64] = "mediatek/";

	/* ---- wmt_loader: module init on /dev/wmtdetect ---- */
	printf("wmt_loader:\n");
	fd = open("/dev/wmtdetect", O_RDWR);
	if (fd < 0) {
		printf("  open /dev/wmtdetect: %s\n", strerror(errno));
		return 1;
	}
	say("DO_MODULE_INIT", ioctl(fd, COMBO_IOCTL_DO_MODULE_INIT, chipid));
	close(fd);

	/* /dev/stpwmt only exists once module init has run */
	for (i = 0; i < 20; i++) {
		if (access("/dev/stpwmt", F_OK) == 0)
			break;
		usleep(100000);
	}

	/* ---- 6620_launcher: everything on /dev/stpwmt ---- */
	printf("6620_launcher:\n");
	wmt = open("/dev/stpwmt", O_RDWR);
	if (wmt < 0) {
		printf("  open /dev/stpwmt: %s\n", strerror(errno));
		return 1;
	}

	v = 0;
	say("QUERY_CHIPID", ioctl(wmt, WMT_IOCTL_WMT_QUERY_CHIPID, &v));
	printf("    chipid reported: 0x%04x\n", v);

	say("PORT_NAME", ioctl(wmt, WMT_IOCTL_PORT_NAME, portname));
	say("WMT_CFG_NAME", ioctl(wmt, WMT_IOCTL_WMT_CFG_NAME, cfgname));
	say("SET_PATCH_NUM", ioctl(wmt, WMT_IOCTL_SET_PATCH_NUM, PATCH_NUM));

	for (i = 0; i < PATCH_NUM; i++) {
		memset(&pi, 0, sizeof(pi));
		pi.dowload_seq = i + 1;
		if (patch_address(patch_files[i], pi.address) < 0)
			printf("    (no address for %s, leaving zero)\n", patch_files[i]);
		strncpy((char *)pi.name, patch_files[i], sizeof(pi.name) - 1);
		printf("    patch %d %s addr=%02x%02x%02x%02x\n", i + 1, patch_files[i],
		       pi.address[3], pi.address[2], pi.address[1], pi.address[0]);
		say("SET_PATCH_INFO", ioctl(wmt, WMT_IOCTL_SET_PATCH_INFO, &pi));
	}

	say("SET_PATCH_NAME", ioctl(wmt, WMT_IOCTL_SET_PATCH_NAME, patchname));
	say("SET_STP_MODE", ioctl(wmt, WMT_IOCTL_SET_STP_MODE, stp_mode));

	/* 0x80000003 = on | WMTDRV_TYPE_WIFI */
	say("FUNC_ONOFF_CTRL(wifi on)", ioctl(wmt, WMT_IOCTL_FUNC_ONOFF_CTRL, 0x80000003));

	say("COREDUMP_CTRL(0)", ioctl(wmt, WMT_IOCTL_COREDUMP_CTRL, 0));
	say("SET_LAUNCHER_KILL", ioctl(wmt, WMT_IOCTL_SET_LAUNCHER_KILL, 1));

	close(wmt);
	return 0;
}
