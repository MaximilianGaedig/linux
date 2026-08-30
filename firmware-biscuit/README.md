# biscuit proprietary firmware (extracted from stock Fire OS)

## i2s_to_spi_v34.bin
Mic-array "dough" FPGA bitstream. Lattice iCE40UL1K-SWG16, built with
iCEcube2 2016.08. 30964 bytes, md5 2c3c5e029b7998f8cbcd7a899b19e13d.

Extracted 2026-08-30 from Amazon's stock Fire OS 5.5.5.4 boot.img kernel
(Linux 3.18.19+ build@...fos-17), which embeds it via CONFIG_EXTRA_FIRMWARE.
Method: unpack Android boot.img -> strip MTK 512-byte KERNEL header (magic
0x88168858) -> gunzip -> locate the builtin_fw struct (arm64 kernel virt base
0xffffffc000080000): name ptr -> "i2s_to_spi_v34.bin", data ptr 0xffffffc0008f02e8
(file off 0x8702e8), size 30964. See scratchpad extract steps.

Install to /lib/firmware/i2s_to_spi_v34.bin on the target (or embed via
CONFIG_EXTRA_FIRMWARE like Amazon does) for the mt8163-spi-fpga-pcm driver.
