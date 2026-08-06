# MT6625L WiFi/BT/GPS port notes (Echo Dot 2016 "biscuit", MT8163)

Status as of 2026-08-06: **devicetree wiring committed; kernel driver port
not started (only researched).** This document is the handoff for whoever
continues.

## 1. Hardware/architecture facts established

- The combo chip is MediaTek's **MT6625L** (WiFi + BT + FM + GPS), but on
  MT8163 (like MT7623/MT8127/MT6797 etc.) it is NOT a discrete SDIO/USB
  peripheral from the kernel's point of view. It is wired to the SoC's
  **CONSYS** connectivity subsystem over a dedicated AHB bus, and shows up
  in the devicetree as two directly memory-mapped nodes:
  - `consys@18070000` (`mediatek,mt8163-consys`) -- CONN_MCU config regs,
    AP RGU regs, TOPCKGEN regs. This is MediaTek's WMT (Wireless
    Management Team) subsystem: does power-on sequencing, clocking,
    firmware/ROM-patch download into CONSYS's own AHB address space, and
    exposes the shared transport (STP) that muxes WiFi/BT/GPS/FM over one
    logical channel to the CONSYS co-processor.
  - `wifi@180f0000` (`mediatek,wifi`) -- the actual WiFi MAC/baseband
    register block + DMA (PDMA), driven directly over AHB, not SDIO.
  - Confirmed by reading `drivers/misc/mediatek/connectivity/conn_soc/
    drv_wlan/mt_wifi/wlan/os/linux/hif/ahb/ahb.c` (2099 lines) in
    amazon-biscuit-kernel -- this is the HIF (host interface) backend
    actually compiled in for this SoC family; there is no SDIO/USB HIF
    variant used here.
  - Reset/handshake pins: `WB_RSTB` (GPIO60), `WB_SCLK` (GPIO63),
    `WB_SDATA` (GPIO64), `WB_SEN` (GPIO65) -- a 4-wire proprietary WMT
    control interface, NOT SPI (despite SCLK/SDATA naming).
  - Power: 4 PMIC rails off the MT6323 PMIC -- `vcn18`, `vcn28`,
    `vcn33_bt`, `vcn33_wifi` (all already defined as regulator nodes in
    this tree's `mt6323.dtsi`, confirmed unmodified/pre-existing).
  - Amazon's downstream reference for all of the above:
    `amazon-biscuit-kernel/arch/arm64/boot/dts/biscuit.dtsi` (search
    `&consys`, `wifi_reset_init`, `vcn33_bt`), and
    `amazon-biscuit-kernel/arch/arm64/boot/dts/mt8163.dtsi` lines
    ~1432-1448 for the raw `consys@18070000` / `wifi@180f0000` node
    definitions this port's DT nodes were copied from.

- Driver stack shape (from `amazon-biscuit-kernel/drivers/misc/mediatek/
  connectivity/conn_soc`, a 3.18.19 kernel):
  - `common/mt8163/mtk_wcn_consys_hw.c` + `wmt_plat_alps.c` -- SoC-specific
    platform driver (`mtk_wmt`) that probes `mediatek,mt8163-consys`,
    manages the 4 regulators, CCF clocks (`clk_scp_conn_main`,
    `clk_infra_conn_main`), and pmic-wrap access.
  - `common/core/stp_core.c` (3369 lines) + `common/core/btm_core.c` --
    the WMT/STP (shared transport) core: firmware download, low-level
    protocol muxing BT/WiFi/GPS/FM over the CONSYS link.
  - `common/linux/pri/stp_btif.c`, `stp_dbg.c`, `stp_exp.c`,
    `common/linux/pub/stp_chrdev_bt.c`, `stp_chrdev_gps.c` -- Linux glue:
    BT exposes as a **character device** (`/dev/stpbt`) that a userspace
    HCI attach daemon (`hciattach`-equivalent, MediaTek calls it
    `mtkbt`/`bt_ctrl`) opens and attaches to a virtual HCI UART line
    discipline -- it is NOT a native `struct hci_dev` in-kernel driver.
    This matters: BTIF integration is a userspace-assisted flow, not a
    clean `hci_register_dev()` module, which is exactly what the 2017
    Banana Pi forum thread flagged as unfinished/missing.
  - `drv_wlan/mt_wifi/wlan/*` -- a **fully self-contained vendor WiFi
    stack** (its own `os/linux/gl_init.c`, `gl_kal.c`, `gl_wext.c` glue,
    `nic/`, `mgmt/` MLME) that implements `struct net_device` directly.
    It does **not** use mac80211 and only nominally touches cfg80211
    (legacy `iwconfig`/wireless-extensions ioctls via `gl_wext.c`, not the
    modern cfg80211 ops table). This is consistent with what the 2017 BPI
    forum thread describes for the same 4.4-era MediaTek driver
    generation.

## 2. Critical discovery: prior art further along than the 2017 BPI thread

The 2017 forum thread (banana-pi.org, BPI-R2, mt7623) only documents a
4.4.70 -> 4.14 attempt that compiled but panicked at runtime and never
got BTIF working. **However**, the associated GitHub org has continued
work since, on a maintained multi-branch kernel repo:

- **https://github.com/frank-w/BPI-Router-Linux** -- actively maintained,
  branch-per-kernel-version repo for BPI-R2/R64/R2Pro/R3/R4 boards. It has
  a dedicated branch **`4.14-mt6625`** (there is also a
  `4.14-mt6625_vs_4.14-main.diff` file at its root, ~6.7KB, showing
  exactly what was changed to add the driver on top of plain 4.14-main).
  There is also branch `5.4-wifi-patching`, suggesting the port was
  carried forward for some time.
  - Its driver tree lives at
    `drivers/misc/mediatek/connectivity/common/conn_soc/` (STP/WMT core,
    same shape as Amazon's, but already 4.14-clean: `core/wmt_core.c`,
    `core/stp_core.c`, `linux/pub/stp_chrdev_bt.c`,
    `linux/pub/wmt_chrdev_wifi.c`, SoC backend under `mt7623/`) and
    `drivers/misc/mediatek/connectivity/wlan/gen2/` for the WiFi MAC
    driver, with `os/linux/hif/ahb/ahb.c` + `ahb/mt8127/ahb_pdma.c` --
    **confirms the AHB HIF backend is the right one to reference** (their
    SoC, MT7623, and ours, MT8163, both use AHB CONSYS wiring, unlike
    MT6797-family SDIO variants).
  - Per the banana-pi forum and repo issue history: WiFi 2.4GHz was
    reported working on this port at one point. Bluetooth/BTIF
    integration was flagged as incomplete in a `frank-w/BPI-R2-4.14`
    issue titled "Add Bluetooth support" (#6).
  - Per web search of forum/issue content: **internal WiFi/BT on this
    driver stopped working somewhere around kernel 6.0+** due to
    mainline internal API changes -- consistent with what we'd expect
    (regulator/clk/platform-driver API churn, `netdev_ops` const-ification,
    `iowrite`/DMA API changes, etc. across 4.14 -> 6.x).
  - **This is the single best next step for whoever continues this task**:
    diff `4.14-mt6625` against `4.14-main` in that repo (the included
    `.diff` file at the branch root does most of this already), pull the
    resulting conn_soc + wlan/gen2 driver trees (already far more
    modern-kernel-shaped than Amazon's raw 3.18 source -- correct
    Makefile/Kconfig integration, already-adapted platform_driver probe,
    etc.), and then do a second, smaller modernization pass from 4.14 ->
    6.x APIs, rather than starting from Amazon's 3.18 source directly.
    That skips the hardest and most error-prone part of this task (initial
    3.18->modern API adaptation of a ~15-20k line vendor stack) since it
    has already been done once for the same chip/HIF combination.
  - Did not clone/vendor that repo's driver source into this tree yet --
    ran out of session budget after locating it. It is public and MIT/GPL
    licensed same as any kernel fork; cloning
    `git@github.com:frank-w/BPI-Router-Linux.git` branch `4.14-mt6625` and
    extracting `drivers/misc/mediatek/connectivity/` is the literal next
    action.

## 3. What this session actually changed (committed)

- `arch/arm64/boot/dts/mediatek/mt8163.dtsi`: added the `consys@18070000`
  and `wifi@180f0000` SoC nodes (both `status = "disabled"` at the SoC
  level, matching the tree's convention of enabling per-board).
- `arch/arm64/boot/dts/mediatek/mt8163-amazon-common.dtsi`:
  - added `consys_reserved_memory` (2MB, 2MB-aligned) carveout under
    `reserved-memory`, for CONSYS firmware/coredump, matching Amazon's
    downstream size.
  - added the `wifi_reset_init` pinctrl group for the 4 WB_* pins.
  - enabled `&consys` (`status = "okay"`, wired to all 4 regulators and
    the pinctrl group) and `&wifi` (`status = "okay"`) for all
    biscuit-family boards that include this common dtsi.
- Verified with `clang -E` C-preprocessor pass (no `dtc` binary available
  in this environment) that all new labels/macros
  (`consys_reserved_memory`, `wifi_reset_init`,
  `MT8163_PIN_60_WB_RSTB__FUNC_WB_RSTB` etc.) resolve with no missing
  includes or undefined references. Could not do a full `dtc` semantic
  validation (no dtc/cross-compiler toolchain installed in this
  environment) or a real boot test -- next person should run
  `make ARCH=arm64 mt8163-amazon-biscuit.dtb` with a real toolchain and
  `dtc` (or `dtx_diff`) to confirm no binding/phandle errors, and ideally
  boot-test.
- **No kernel driver code was added in this session** -- nothing in this
  tree yet binds to `mediatek,mt8163-consys` or `mediatek,wifi`. The DT
  nodes are inert until a driver is ported.

## 4. Concrete next steps, in order

1. Clone `frank-w/BPI-Router-Linux` at branch `4.14-mt6625`, extract
   `drivers/misc/mediatek/connectivity/{common/conn_soc,wlan/gen2}` plus
   its `4.14-mt6625_vs_4.14-main.diff` for context on what glue/Kconfig/
   Makefile hooks it needed in the surrounding tree.
2. Cross-reference against Amazon's `amazon-biscuit-kernel` conn_soc tree
   for MT8163-specific bits (the frank-w port targets MT7623 SoC-glue
   under a `mt7623/` subdir -- our tree needs an `mt8163/` equivalent,
   which Amazon's `common/mt8163/mtk_wcn_consys_hw.c` +
   `wmt_plat_alps.c` already provide as a model, just on ancient APIs).
3. Do a **compile-only** pass first: try `make M=drivers/misc/mediatek/
   connectivity/common/conn_soc modules` (out-of-tree module build,
   doesn't need a full kernel build/toolchain first) against this repo's
   6.x headers, fix errors top-down. Expect breakage at minimum in:
   regulator API (`regulator_get`/`regulator_set_voltage` signatures
   largely stable, but check `devm_regulator_get` usage), `clk_prepare_enable`
   vs old CCF, `platform_driver` `.remove` return-type change
   (`void` since ~5.x, was `int`), `pinctrl_lookup_state` usage, and
   general `THIS_MODULE`/`MODULE_LICENSE` boilerplate -- all mechanical.
4. Once conn_soc (WMT/STP core) compiles standalone, tackle
   `wlan/gen2` (WiFi net_device driver) the same way -- expect more work
   here since `netdev_ops`/`ethtool_ops` and DMA API have moved more since
   4.14.
5. BT: decide whether to keep MediaTek's userspace-attach chardev model
   (`/dev/stpbt` + a small `mtkbt`-alike userspace daemon calling
   `hci_uart`-style ioctls) or invest in writing a proper in-kernel
   `hci_register_dev()`-based driver on top of the STP core -- the latter
   is cleaner for a mainline-style port and avoids needing Amazon's
   closed userspace binary, but is more work. The 2017 BPI thread's
   "missing BTIF integration" complaint was almost certainly about this
   same fork.
6. No firmware blob has been sourced yet. MT6625 needs a ROM patch/EEPROM
   file at minimum (WMT downloads it during consys init) -- check
   `amazon-biscuit-kernel`'s vendor partition / initramfs, or Fire OS
   `/vendor/firmware` equivalent, for `WIFI_RAM_CODE_MT6625.bin` or
   similarly named blobs; without it consys init will hang even once the
   driver compiles and probes.

## 5. Honest assessment

This is still at the "architecture understood, DT wired, driver source
located but not yet ported" stage. No driver code compiles or runs yet.
The single highest-leverage finding is #2 above (the frank-w 4.14-mt6625
branch) -- it turns this from "port a 3.18 vendor blob cold" into "take an
already-4.14-ported driver and re-modernize it," which is a substantially
smaller job. Whoever picks this up next should start there rather than
re-deriving the 3.18->modern port from Amazon's source directly.
