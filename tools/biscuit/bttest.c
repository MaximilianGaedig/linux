/* Send an HCI Reset to /dev/stpbt and print whatever comes back. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
int main(void)
{
	unsigned char reset[] = { 0x01, 0x03, 0x0c, 0x00 };   /* HCI_Reset */
	unsigned char buf[64];
	int fd = open("/dev/stpbt", O_RDWR);
	int n, i;

	if (fd < 0) { printf("open /dev/stpbt: %s\n", strerror(errno)); return 1; }
	n = write(fd, reset, sizeof(reset));
	printf("write HCI_Reset -> %d (%s)\n", n, n < 0 ? strerror(errno) : "ok");
	memset(buf, 0, sizeof(buf));
	n = read(fd, buf, sizeof(buf));
	printf("read -> %d\n", n);
	for (i = 0; i < n && i < 32; i++) printf("%02x ", buf[i]);
	printf("\n");
	close(fd);
	return 0;
}
