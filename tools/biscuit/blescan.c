/* Raw HCI BLE scan on /dev/stpbt: reset, set scan params, enable, collect adverts. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

static int cmd(int fd, const unsigned char *c, int n, const char *what)
{
	unsigned char b[260];
	int r = write(fd, c, n);
	printf("%-22s write=%d", what, r);
	r = read(fd, b, sizeof(b));
	printf(" resp=%d:", r);
	for (int i = 0; i < r && i < 8; i++) printf(" %02x", b[i]);
	printf("%s\n", (r >= 7 && b[6] == 0) ? "   [status OK]" : "");
	return r;
}

int main(void)
{
	unsigned char reset[]  = { 0x01, 0x03, 0x0c, 0x00 };
	/* LE Set Scan Parameters: active, interval 0x0010, window 0x0010, public, no filter */
	unsigned char param[]  = { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };
	/* LE Set Scan Enable: enable, no dup filter */
	unsigned char enable[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x00 };
	unsigned char dis[]    = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
	unsigned char b[300];
	int fd = open("/dev/stpbt", O_RDWR);
	int seen = 0;
	struct pollfd p;

	if (fd < 0) { printf("open /dev/stpbt: %s\n", strerror(errno)); return 1; }
	cmd(fd, reset, sizeof(reset), "HCI_Reset");
	cmd(fd, param, sizeof(param), "LE_Set_Scan_Params");
	cmd(fd, enable, sizeof(enable), "LE_Set_Scan_Enable");

	printf("scanning 8s...\n");
	p.fd = fd; p.events = POLLIN;
	for (int t = 0; t < 80; t++) {
		if (poll(&p, 1, 100) > 0) {
			int r = read(fd, b, sizeof(b));
			/* 04 3E = LE Meta Event, subevent 02 = Advertising Report */
			if (r > 12 && b[0] == 0x04 && b[1] == 0x3e && b[3] == 0x02) {
				seen++;
				printf("  adv #%d addr %02x:%02x:%02x:%02x:%02x:%02x rssi %d\n",
				       seen, b[11], b[10], b[9], b[8], b[7], b[6], (signed char)b[r - 1]);
				if (seen >= 12) break;
			}
		}
	}
	cmd(fd, dis, sizeof(dis), "LE_Scan_Disable");
	printf("TOTAL ADVERTISEMENTS: %d\n", seen);
	close(fd);
	return 0;
}
