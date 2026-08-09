# MT6625L WiFi/BT/GPS port notes (Echo Dot 2016 "biscuit", MT8163)

Status as of 2026-08-06 (third pass): **the entire driver set --
conn_soc (WMT/STP/BT core), common_detect, btif, and wlan/gen2 (the WiFi
MAC driver) -- now compiles and links against this tree's 7.0-rc6
headers, in both the built-in and the modular configuration, and a
complete `arch/arm64/boot/Image` builds with all of it linked in.**
mt8163-amazon-biscuit.dtb now compiles too (it never had before), which
finally gives real `dtc` validation of the consys/wifi DT nodes.
**Nothing has been boot-tested on real hardware.** This document is the
handoff for whoever continues. Sections 4b/5/6 (bottom) have the
up-to-date state, next steps, and honest assessment -- read those first
if you're picking this up. Sections 3 and 4 are historical and describe
earlier passes.

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
  - **Update:** this source has since been vendored into this tree (see
    section 3). `git clone`/tarball download of that repo was extremely
    unreliable in this sandbox (stalled/truncated repeatedly, likely a
    network shaping issue, not a repo problem) -- what worked was walking
    `gh api repos/frank-w/BPI-Router-Linux/git/trees/4.14-mt6625?recursive=1`
    for the file list, then fetching each blob individually via
    `gh api repos/.../git/blobs/<sha>` in parallel (~15 at a time). Slower
    per-file but far more reliable than a full clone here. If you need
    more of that repo (e.g. actual `btif`/`common_detect` glue turned out
    to be needed too, see section 3), use the same approach.

## 3. What this session actually changed (committed)

**This section is from the first pass and is now superseded by section 4
below (which covers the actual driver import/build work) -- kept for the
DT-specific detail, which is still accurate.**

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
- DT reset wiring was completed in a later commit (see section 4) once the
  driver code showed it was needed -- ignore the "no kernel driver code
  was added" line above, it described an earlier point in the session.

## 4. Driver import and 6.x build work actually done (this pass)

### 4.1 What's vendored in and where it came from

All fetched from `frank-w/BPI-Router-Linux` branch `4.14-mt6625` (commit
`6ee4af5`) via the GitHub API (see section 2 update on why), landed at:

- `drivers/misc/mediatek/connectivity/common/conn_soc/` -- WMT/STP core,
  BT chardev, WiFi chardev stub. SoC backend directory renamed
  `mt7623/` -> `mt8163/` (git mv) and adapted (compatible string,
  register-read loop, reset-controller wiring -- see 4.3).
- `drivers/misc/mediatek/connectivity/wlan/gen2/` -- the actual WiFi MAC
  driver (MLME FSMs in `mgmt/`, NIC cmd/event in `nic/`, AHB HIF in
  `os/linux/hif/ahb/`, OS glue in `os/linux/gl_*.c`).
- `drivers/misc/mediatek/connectivity/common/common_detect/` -- combo
  chip detection/GPIO-probe layer (`wmt_detect.c`, `sdio_detect.c`,
  `wmt_gpio.c`, per-subsystem `drv_init/` stubs). Needed because
  conn_soc's Makefile pulls `wmt_stp_exp.h` from here -- missed on the
  first pass, added in a follow-up commit.
- `drivers/misc/mediatek/include/mt-plat/` -- shared MediaTek headers
  (`mtk_wcn_cmb_stub.h` etc.) that conn_soc/common_detect expect at a
  fixed path.
- `drivers/misc/mediatek/btif/` -- the actual BTIF (Bluetooth transport
  interface) layer: `mtk_btif.c`, `mtk_btif_exp.c`, `btif_dma_plat.c`.
  This is the piece the 2017 banana-pi forum thread flagged as
  missing/unfinished -- frank-w's tree has a real implementation of it,
  not a stub. Not yet exercised/tested, just confirmed to compile as
  part of conn_soc's dependency chain.

Five separate git commits under `misc: mediatek: import ...` cover this
(each is an unmodified vendor import for a clean diff base -- all actual
edits are in later commits). `git log --oneline` on this branch shows the
full sequence.

### 4.2 conn_soc: fully compiles and links (real milestone)

`make M=drivers/misc/mediatek/connectivity/common/conn_soc
CONFIG_MTK_COMBO=m CONFIG_MTK_COMBO_BT=m CONFIG_MTK_COMBO_WIFI=m modules`
now produces three loadable `.ko` files: `mtk_stp_wmt_soc.ko` (WMT/STP
core), `mtk_stp_bt_soc.ko` (BT chardev), `mtk_wmt_wifi_soc.ko` (WiFi
chardev stub). As far as the prior-art research in section 2 shows, this
is the first time this chip's driver stack has built against a
post-4.14 kernel at all.

Build environment note for whoever continues in this same sandbox: there
was no cross-compiler preinstalled. `nix-shell -p flex bison lld
llvmPackages.bintools` gets you flex/bison (for Kconfig) and a full LLVM
toolchain. The nix-wrapped `clang` on PATH injects a spurious
`-nostdlibinc` flag that becomes a hard error under this kernel's own
`-Werror=unused-command-line-argument` -- work around it by pointing `CC`
directly at the *unwrapped* clang binary, e.g.
`CC=/nix/store/<hash>-clang-21.1.8/bin/clang` (find the exact hash with
`readlink -f $(which clang)` inside the nix-shell, then look one
directory up from the `clang-wrapper-*` path it resolves to -- or just
`find /nix/store -maxdepth 1 -iname 'clang-21*'` and pick the one that
isn't `*-wrapper-*`). Then: `make ARCH=arm64 CC=$CC LLVM=1 O=<build-dir>
defconfig && make ARCH=arm64 CC=$CC LLVM=1 O=<build-dir>
modules_prepare` once, then `make ARCH=arm64 CC=$CC LLVM=1 O=<build-dir>
M=<driver-dir> modules` per module directory. Full `.config` symbols for
this driver (`CONFIG_MTK_COMBO`, `_BT`, `_WIFI`) aren't wired into
Kconfig yet -- passed on the `make` command line instead, which works
for compile-testing but should become real `Kconfig`/`menuconfig`
entries eventually (not done this session -- see section 5).

Also note: this was only compile-tested via `modules_prepare`, not a
full `vmlinux`/kernel `modules` build, so `Module.symvers` for core
kernel exports doesn't exist and modpost reports ~180 "undefined"
warnings for symbols like `jiffies`, `__stack_chk_fail`, `_printk`,
`proc_create`, etc. (use `KBUILD_MODPOST_WARN=1` to downgrade these to
warnings so the `.ko` still gets produced). **These are not real bugs**
-- they're all genuinely-exported core kernel symbols that would
resolve fine against a real built kernel; it's purely an artifact of
not doing a full kernel build in this sandbox. Don't rediscover this and
think something's broken.

Fixes applied (all committed, see `git log` for the two conn_soc commits
for full prose per-fix -- summarized here):

- Android `<linux/wakelock.h>`/`CONFIG_PM_WAKELOCKS` dual-path collapsed
  to mainline's pointer-based `wakeup_source_register()`/
  `wakeup_source_unregister()` (`osal.h`/`osal.c`,
  `mt8163/wmt_plat_alps.c`). Mainline's own `wakeup_source_init()`/
  `_trash()` helpers (which this code was actually written against) have
  *also* since been removed in favor of the pointer-returning
  register/unregister pair -- two generations of API removed here, not
  one.
- `struct timeval`/`do_gettimeofday()` (y2038 cleanup, ancient) ->
  `struct timespec64` + `ktime_get_real_ts64()`/`ktime_get_seconds()`
  (`wmt_dev.c`, `psm_core.c`, `stp_dbg.c`, `osal.c`).
- `init_timer()` + `timer->data` (`unsigned long`) -> `timer_setup()`.
  Kept `OSAL_TIMER`'s public API (`P_TIMEOUT_HANDLER` taking an
  `unsigned long`) unchanged for its many callers via a small trampoline
  using `timer_container_of()` (`osal.c`). Note `from_timer()` has
  *already* been renamed to `timer_container_of()`, and
  `del_timer()`/`del_timer_sync()` to `timer_delete()`/
  `timer_delete_sync()` -- this kernel tree is recent enough to have
  picked up both the original 4.15-era timer_setup migration AND a much
  more recent (~6.x) second round of renames on top of it.
- `get_fs()`/`set_fs()`/`mm_segment_t` (fully removed) -> `kernel_read()`
  for reading files from kernel context (`wmt_dev.c`).
- `proc_create()` now takes `struct proc_ops`, not `struct
  file_operations` (v5.6+) (`wmt_dev.c` debug/aee proc entries).
- `genl_register_family_with_ops()`/`GENL_ID_GENERATE` (removed) -> ops
  embedded directly in `struct genl_family` + `genl_register_family()`
  (`stp_dbg.c` netlink debug interface).
- `platform_driver.remove` changed from `int` to `void` return
  (`mt8163/mtk_wcn_consys_hw.c`).
- `ioremap_nocache()` (removed, nocache is the only mode now) ->
  `ioremap()`.
- `show_stack()` gained a 3rd (loglvl) argument.
- A real latent bug, not API drift: `apwmt_of_ids[]` (the
  `of_device_id` table) was missing its NULL sentinel entry -- modpost
  correctly flagged this (`is not terminated with a NULL entry`).
- A handful of missing `#include <linux/pinctrl/consumer.h>`.
- A genuinely strange one: `mtk_wcn_wmt_ic_info_get()` (declared in
  `common_detect/wmt_stp_exp.h`, defined+exported in
  `wmt_stp_exp.c`) never becomes visible in `stp_chrdev_bt.c` even
  through `#include "wmt_stp_exp.h"` -- traced it far enough to see
  `wmt_exp.h` nested-includes `wmt_stp_exp.h` itself and *most* of that
  header's declarations do show up except this one specifically; did not
  fully root-cause it (not worth the time -- see section 5 if you want
  to). Worked around with a direct `extern` forward-declaration at the
  call site. This class of bug (implicit-function-declaration silently
  tolerated) is a sign this codebase was never built with a strict
  modern compiler before.

### 4.3 mt8163/ SoC backend: renamed from mt7623/, register wiring fixed

The frank-w tree's SoC-specific glue lived under `mt7623/` (their
board). Renamed to `mt8163/` and adapted for real (not just renamed):

- `of_device_id`/`of_find_compatible_node`/platform driver name changed
  to `"mediatek,mt8163-consys"` / `"mt8163consys"` to match this tree's
  DT node.
- The register-read loop in `mtk_wcn_consys_hw_init()` only read 2
  `reg` entries (mcu_base, topckgen_base) by index, but this port's DT
  node (copied from Amazon's real production DT) has 3 entries in order
  mcu/AP_RGU/topckgen. Left as-is, `of_iomap(node, 1)` would have
  silently mapped the AP_RGU window onto `topckgen_base`. Fixed to
  consume all 3 (the struct already had an `ap_rgu_base` field, just
  unused -- confirms this frank-w port had already moved reset handling
  to the reset-controller framework, see next point, and `ap_rgu_base`
  was vestigial).
- `devm_reset_control_get(&pdev->dev, "connsys")` needed a real DT
  `resets`/`reset-names` property, which didn't exist anywhere in this
  tree. Added `resets = <&watchdog MT8163_TOPRGU_CONN_MCU_RST>;
  reset-names = "connsys";` to the `consys` DT node
  (`arch/arm64/boot/dts/mediatek/mt8163.dtsi`). Confidence on this one is
  *fairly high but not hardware-verified*: the driver's own comment on
  the `reset_control_reset()` call site says it asserts "bit 12 of
  0x10007018" for the CONNSYS CPU SW reset, and
  `include/dt-bindings/reset/mediatek,mt8163-wdt.h` defines
  `MT8163_TOPRGU_CONN_MCU_RST = 12` exactly -- a real match, not a guess,
  but still needs a boot test to be sure the watchdog/TOPRGU reset
  controller driver in this tree actually implements bit 12 correctly
  for this purpose.
- The raw MT6323 PMIC-register VCN28/VCN33 BT/WIFI power-sequencing path
  (`pwrap_node_to_regmap()`, offsets 0x41C/0x416/0x418) is NOT a mainline
  API (`<soc/mediatek/pmic_wrap.h>` doesn't exist here). Rather than
  guess at a replacement, left this explicitly unported: `pmic_regmap`
  is now permanently `NULL` and every `regmap_update_bits(pmic_regmap,
  ...)` call site is guarded to skip instead of NULL-deref. There's a
  long comment at the `#include` site explaining this. **Whether these
  raw pokes are actually load-bearing, or redundant with the
  `regulator_*()` calls already present elsewhere in the same file for
  the same rails (`reg_VCN18`/`reg_VCN28`/`reg_VCN33_BT`/
  `reg_VCN33_WIFI`, wired to `mt6323.dtsi`'s already-working
  regulator-fixed nodes), is unverified** -- this needs either a logic
  analyzer comparison against Amazon's real boot sequence, or just
  trying without them first and see if consys comes up.

### 4.4 wlan/gen2: partially ported, stops at a real (non-mechanical) wall

**[SUPERSEDED by section 4b -- this wall has since been gone through.
Kept because the description of what the blockers were is still an
accurate account of what had to be reconciled.]**

Ran the same compile-and-fix loop on
`drivers/misc/mediatek/connectivity/wlan/gen2` (`make
M=drivers/misc/mediatek/connectivity/wlan/gen2 CONFIG_MTK_COMBO_WIFI=m
MTK_PLATFORM=mt8127 modules` -- note `MTK_PLATFORM=mt8127` selects the
AHB PDMA variant via the Makefile's `HIF_AHB_PDMA` path, which is the
right one; there's no `mt8163`-named PDMA variant, `mt8127`'s is reused
as-is and untouched here since it's already a plain-AHB PDMA
implementation, not further MT8127-specific).

Got substantially further than expected before hitting a real wall:
`common/{dump,wlan_lib,wlan_oid,wlan_bow}.c`, `common/debug.c`, and the
dependency chain through `os/linux/gl_p2p.c` all compile clean now
(same class of mechanical fixes as conn_soc -- see the wlan/gen2 import
commit for the list: a hardcoded `#define CONFIG_ANDROID 1` forcing the
same dead wakelock.h path, a bare `#include <stddef.h>` that isn't
reachable under this kernel's plain `-nostdinc` with no compiler-isystem
fallback, a missing `sched_clock()` include, two `#error "...ENABLE
CONFIG_NL80211_TESTMODE..."` guards that turned out to be pure
reminders -- every real use elsewhere was already `#ifdef
CONFIG_NL80211_TESTMODE` guarded so P2P/Wi-Fi-Direct testmode support
just compiles out cleanly instead of erroring now, a real feature gap
but not a build blocker).

It stops at `os/linux/gl_init.c`, the net_device/cfg80211 registration
glue, with **~14 real `cfg80211_ops`/`wiphy` API-surface mismatches**:

- `add_key`/`del_key`/`get_key`/`set_default_key` and friends: signature
  changed (key index type, parameter reordering).
- `.mgmt_frame_register` field doesn't exist anymore -- replaced by a
  differently-shaped `update_mgmt_frame_registrations` callback with
  different semantics (bulk registration state, not per-frame-type
  register/unregister calls).
- `WIPHY_FLAG_SUPPORTS_SCHED_SCAN` removed -- scheduled-scan capability
  is now declared by setting `wiphy->max_sched_scan_reqs` (or similar;
  didn't chase this down fully) rather than an opt-in flag bit.
- `ndo_select_queue` signature changed (dropped the `accel_priv`/
  fallback-function-pointer parameter shape this code expects).
- A `const unsigned char *` -> `u8 *` qualifier-discard bug (real bug,
  not API drift -- the source pointer should have been `const` in the
  driver's own function signature and wasn't).

**These are categorically different from everything fixed so far.**
Every fix up to this point was a mechanical rename/relocation with an
unambiguous 1:1 replacement. These cfg80211 ops changes each reflect a
real behavioral/semantic change in how net_devices register with the
wireless stack across ~10+ years of kernel evolution, and need to be
understood and correctly re-implemented per callback -- not
find-replaced. This is genuine driver engineering, not mechanical
porting, and is where this session's effort budget ran out.

## 4b. Third pass: everything compiles, and the whole tree builds

### 4b.0 Build environment (reproduce this first)

There is no cross-toolchain on `PATH` by default in this sandbox, but a
working aarch64 GCC 15.3 is already sitting in
`/home/user/proj/alexa2-rev/cross-bin/` (as
`aarch64-unknown-linux-gnu-*`). flex/bison (for Kconfig) and openssl
headers (for `certs/extract-cert`) come from nix. The helper script used
for every build in this pass is `/home/user/proj/alexa2-rev/build-wifi/mk.sh`:

    #!/usr/bin/env bash
    SRC=/home/user/proj/alexa2-rev/linux-mtk-biscuit-wifi
    O=/home/user/proj/alexa2-rev/build-wifi
    export PATH=/home/user/proj/alexa2-rev/cross-bin:$PATH
    exec nix-shell -p flex bison openssl pkg-config --run \
      "make -C $SRC O=$O ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- $*"

Then:

    ./mk.sh mt8163_biscuit_defconfig
    ./mk.sh -j8

That is verified to go all the way to `arch/arm64/boot/Image` plus
`arch/arm64/boot/dts/mediatek/mt8163-amazon-biscuit.dtb`.
`arch/arm64/configs/mt8163_biscuit_defconfig` is new in this pass and is
exactly the configuration that was tested (`make savedefconfig` of it,
with the previous session's machine-specific `CONFIG_INITRAMFS_SOURCE`
absolute path stripped out -- add your own back if you want an embedded
initramfs).

**Use GCC, not clang.** The previous pass built with clang, which treats
`-Wincompatible-pointer-types` as a warning. GCC 14+ makes it a hard
error. That difference is not cosmetic: an ops table whose members have
the wrong function-pointer types is an ABI mismatch that the kernel will
happily call into and crash on. Something like fifteen of the errors
fixed in this pass were invisible under clang.

Two config prerequisites that are easy to get wrong: without
`CONFIG_NET`/`CONFIG_CFG80211` this driver cannot even be type-checked
(`struct net_device` has no `ieee80211_ptr` member at all), and
`CONFIG_WEXT_PRIV` -- which `gl_wext.c` requires for
`iw_handler_def`'s `.private`/`.private_args` -- is a hidden symbol that
can only be turned on by a driver `select`ing it, which is one of the
reasons real Kconfig entries had to be written (4b.2).

### 4b.1 wlan/gen2 now compiles and links

`wlan_gen2.ko` (~600 KB) builds. The cfg80211 wall described in 4.4 was
worked through callback by callback against this tree's
`include/net/cfg80211.h`. The interesting ones:

- **Key ops** (`add_key`/`get_key`/`del_key`/`set_default_key`/
  `set_default_mgmt_key`) and `tdls_mgmt` gained an `int link_id` naming
  which link of an 802.11be multi-link device is meant (v6.0 MLO
  rework); -1 for non-MLO. This is single-link 802.11n silicon, so all
  of them accept and ignore it. Same story for the newer `int radio_idx`
  on `set_wiphy_params`/`set_tx_power`/`get_tx_power`.

- **`mgmt_frame_register` -> `update_mgmt_frame_registrations`** (v5.8)
  is the one genuinely semantic change, not a rename. The old callback
  was an *edge* notification: one call per (frame_type, register vs
  unregister) transition, handed the full 16-bit frame control value and
  a bool. The new one is a *level* notification: cfg80211 recomputes the
  complete set of registered subtypes and hands over
  `struct mgmt_frame_regs`, whose bitmaps are indexed by
  `BIT(frame_type >> 4)` -- i.e. by 802.11 subtype, so probe request is
  BIT(4) and action is BIT(13) -- split into per-interface vs
  device-global and unicast vs multicast, distinctions the old callback
  could not express. Both copies (AIS in `gl_cfg80211.c`, P2P in
  `gl_p2p_cfg80211.c`) now rebuild their two filter bits from
  `upd->interface_stypes` and only poke the firmware when the recomputed
  filter actually differs, because unlike the old callback this one is
  allowed to fire with nothing changed.

- **`WIPHY_FLAG_SUPPORTS_SCHED_SCAN`** was deleted in v4.12 when
  cfg80211 learned to run several scheduled scans concurrently: the
  boolean flag became `wiphy->max_sched_scan_reqs`, a count, which
  `nl80211_start_sched_scan()` now gates on. Set to 1, because
  `mtk_cfg80211_sched_scan_stop()` ignores the reqid it is handed and
  cancels "the" scan -- it can only track one.

- **`change_beacon`** takes `struct cfg80211_ap_update` (a superset of
  `cfg80211_beacon_data`, adding FILS discovery and unsolicited
  broadcast probe response parameters this driver does not implement)
  since v6.7; **`stop_ap`** and **`set_bitrate_mask`** gained link_id;
  **`set_monitor_channel`** gained the `struct net_device *` it applies
  to.

- **`ndo_select_queue`** lost `accel_priv` (v4.19) and the
  `select_queue_fallback_t` pointer (v5.2), gaining
  `struct net_device *sb_dev`. Neither of this driver's two
  implementations ever called `fallback()` -- they pick the queue purely
  from the 802.1d priority derived from the DSCP bits -- so nothing was
  lost.

- **`cfg80211_roam_info`**'s flat `.bss`/`.bssid`/`.channel` moved into a
  per-link array. For a non-MLO roam the contract is `valid_links == 0`
  and the new AP in `links[0]`, which is what `cfg80211_roamed()` reads.

- `struct station_parameters`' `supported_rates`/`ht_capa`/`vht_capa`
  moved into the nested `link_sta_params`.

Non-cfg80211 drift in the same driver: `sched_clock()` moved header,
`netif_rx_ni()` removed (v5.18 -- `netif_rx()` handles process context
itself now), `struct timespec`/`get_monotonic_boottime()` ->
`timespec64`/`ktime_get_boottime_ts64()`, `proc_create()` wants
`struct proc_ops`, `access_ok()` lost its VERIFY_READ/WRITE argument,
`init_timer()`+`->data` -> `timer_setup()`+`timer_container_of()`,
`del_timer_sync()` -> `timer_delete_sync()`, `class_create()` lost
`THIS_MODULE`, `dma_zalloc_coherent()` -> `dma_alloc_coherent()`,
`platform_driver::remove` returns void, `show_stack()` gained a loglvl
argument (and is not exported to modules at all -- `sched_show_task()`
is the exported equivalent), `sched_setscheduler()` unexported in favour
of `sched_set_fifo()`/`sched_set_normal()`.

**The firmware loader was rewritten on `request_firmware()`.** The old
`kalFirmwareOpen/Load/Size/Close` chain hand-rolled `filp_open()` +
`vfs_read()` of `/etc/firmware/WIFI_RAM_CODE_<chipid>` wrapped in
`set_fs(get_ds())` (gone since v5.10), a direct poke at
`filp->f_op->llseek` to get the file size, and -- worst -- temporarily
setting the *calling task's* fsuid/fsgid to 0 by taking
`get_current_cred()`, casting away the const and writing through it.
That is not a scoped credential override; it mutates the live cred the
calling process is running under, and this path is reachable from
process context. All of it is replaced by ~40 lines on
`request_firmware()`.

**Consequence for whoever sources the blob** (still open, see section 5):
it is now looked up on the standard firmware search path
(`/lib/firmware`, `/lib/firmware/updates`, `CONFIG_EXTRA_FIRMWARE_DIR`,
...), *not* at the literal path `/etc/firmware`. The file name is
unchanged and on this SoC resolves to **`WIFI_RAM_CODE_8163`**
(`CFG_FW_FILENAME` + the chip id `glGetChipInfo()` formats from
`MTK_CHIP_ID_8163`).

### 4b.2 Real Kconfig/Makefile integration (`make` now finds it by itself)

Previously the driver could only be built with `make M=<dir>` and config
symbols supplied on the command line. Now:

- `drivers/misc/mediatek/Kconfig` -- `MTK_CONNECTIVITY` menu with
  `MTK_COMBO`, `MTK_COMBO_BT`, `MTK_COMBO_WIFI`, `MTK_COMBO_CHIP`
  (string, default `"CONSYS_8163"`, the value Amazon's own
  `biscuit_defconfig` uses), `MTK_WAPI_SUPPORT`, and a `source` of
  `btif/Kconfig`. `MTK_COMBO_WIFI` depends on `CFG80211` and selects
  `WIRELESS_EXT`/`WEXT_PRIV`/`CFG80211_WEXT`; `MTK_COMBO` selects
  `ZLIB_DEFLATE` (stp_dbg.c compresses coredumps) and `MTK_COMBO_BT`
  depends on `BT`.
- `drivers/misc/mediatek/Makefile` and
  `drivers/misc/mediatek/connectivity/Makefile`, hooked in from
  `drivers/misc/{Kconfig,Makefile}`.

The `connectivity/Makefile` deserves a note, because its *absence* was
causing silent misconfiguration rather than a visible failure. frank-w's
tree ships the leaf driver directories but not this parent Makefile, and
three `-D` flags that the code branches on were therefore never defined:

- `MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT` selects the function-pointer
  indirection layer in `common_detect/wmt_stp_exp.c`. That indirection
  is the entire point of common_detect: it loads first, detects the
  chip, and then dispatches into whichever transport driver registered
  itself, instead of hard-linking against one. Undefined, essentially
  all of `wmt_stp_exp.h` (lines 50-616) vanishes and `wmt_stp_exp.c`
  compiles to nothing.
  **This is the root cause of the `mtk_wcn_wmt_ic_info_get()`
  "declaration mysteriously never visible" puzzle from section 4.2 and
  next-step 7 of the old section 5** -- the declaration was inside that
  `#ifdef` the whole time. The `extern` workaround at the call site has
  been removed.
- `MTK_WCN_REMOVE_KERNEL_MODULE` (conn_soc) and `MTK_WCN_BUILT_IN_DRIVER`
  (wlan/gen2) switch those drivers from `module_init()`/`module_exit()`
  to plain entry points (`mtk_wcn_soc_common_drv_init()`,
  `mtk_wcn_stpbt_drv_init()`, `mtk_wcn_wmt_wifi_init()`,
  `mtk_wcn_wlan_gen2_init()`) that common_detect's `drv_init/`
  dispatcher calls in order once the chip is identified.
- `WMT_IDC_SUPPORT=0` (no LTE modem here) and `CONFIG_MTK_WCN_ARM64`.

All modelled on
`amazon-biscuit-kernel/drivers/misc/mediatek/connectivity/Makefile`,
which is the authority for CONSYS_8163.

A related trap, hit three separate times: **an `obj-y` object in a
subtree reached through `obj-m` is silently dropped by kbuild.** That is
why `btif` had in fact never been compiled by this port at all (contrary
to what section 4.1 claims), and why common_detect's
`mtk_wcn_stub_alps.o`/`wmt_stp_exp.o`/`wmt_gpio.o` produced a pile of
undefined symbols in the modular build. Both fixed by gating on the
config symbol / folding into the module's object list.

And another: **Kconfig string values now reach Makefiles unquoted.**
`include/config/auto.conf` used to write `CONFIG_FOO="bar"` and now
writes `CONFIG_FOO=bar`, so every `$(filter "CONSYS_%",$(CONFIG_MTK_COMBO_CHIP))`
test in common_detect silently matched nothing -- which is how the
SoC-vs-discrete-chip selection this whole driver is built around had
quietly turned itself off.

### 4b.3 Both configurations verified

- **built-in** (`CONFIG_MTK_COMBO=y` etc., what Amazon ships and what
  `mt8163_biscuit_defconfig` selects): full `make` completes; `nm
  vmlinux` shows `mtk_wcn_soc_common_drv_init`, `mtk_wcn_stpbt_drv_init`,
  `mtk_wcn_wlan_gen2_init`, `mtk_wcn_btif_write`, `mtk_wmt_probe`,
  `HifAhbProbe`, `wlanProbe`.
- **modular** (`=m`): `btif.ko`, `mtk_wmt_detect.ko`,
  `mtk_stp_wmt_soc.ko`, `mtk_stp_bt_soc.ko`, `mtk_wmt_wifi_soc.ko`,
  `wlan_gen2.ko`, no unresolved symbols, no dependency cycle.

Built-in is the one to trust: the `drv_init` dispatcher design means
common_detect calls into conn_soc while conn_soc calls back into
common_detect's export layer, and Amazon ships `=y` for exactly that
reason. The modular build is convenient for iteration (rebuilding one
`.ko` beats rebuilding vmlinux) but is the less-travelled path.

### 4b.4 Real bugs found and fixed along the way

Not API drift -- actual defects, mostly surfaced by building with a
modern strict compiler for the first time:

1. **Unbounded `copy_from_user()` from a world-writable procfs file.**
   `procDbgLevelWrite()` and `procTxDoneCfgWrite()` in `gl_proc.c` sized
   their copy as `kalStrLen(aucProcBuf)` (the length of whatever a
   previous *reader* left in the shared static buffer) then
   `if (u4CopySize >= count + 1) u4CopySize = count;`. `count` is
   userspace-controlled, so `count + 1` wraps to 0 at `SIZE_MAX`, the
   test always passes, and the copy size becomes `SIZE_MAX` -- into a
   1536-byte static array, from a 0664 procfs file. GCC's compile-time
   `check_copy_size()` proved it (`__bad_copy_to`). The correct
   `sizeof()` was still there, commented out next to the `strlen` that
   replaced it.
2. **`const PUINT8` does not mean what its author thought.** `PUINT8` is
   a typedef for `UINT8 *`, so `const PUINT8` is `UINT8 * const` -- a
   const *pointer* to mutable data -- not a pointer to const data. Both
   `mtk_bt_hci_receive()` and common_detect's `MTK_WCN_STP_IF_RX`
   typedef had it, and disagreed with conn_soc's own `stp_exp.h`.
3. **Section mismatch on `platform_driver::remove`** in the AHB HIF:
   declared `__exit` and referenced via `__exit_p()` from a `.data` ops
   table. `->remove` runs on *any* device unbind, not just module
   unload, so in a built-in build (where `.exit.text` is discarded) that
   is a dangling function pointer.
4. **Two timestamp bugs in btif**, found by porting `struct timeval` by
   hand rather than mechanically: the "BTIF Tx IRQ happened N times"
   message printed `end_timer.tv_usec` twice and never printed
   `end_timer.tv_sec`, and `mtk_btif_rxd_be_blocked_by_timer()`
   open-coded a borrow when subtracting timestamps and got the borrow
   branch wrong (added where it should have subtracted), inflating the
   reported gap whenever the microsecond field wrapped. Replaced with
   `timespec64_sub()`.
5. **The AGPS notify path was never guarded.**
   `kalIndicateAgpsNotify()` calls `cfg80211_testmode_alloc_event_skb()`
   / `cfg80211_testmode_event()`, which have always been
   `CONFIG_NL80211_TESTMODE`-only. It only ever built because MediaTek's
   own configs always enabled testmode.
6. Two **pre-existing blockers in this tree, unrelated to the WiFi
   work**, but in the way of any full build, so fixed here:
   `mt8163.dtsi` used `THERMAL_NO_LIMIT` without including
   `<dt-bindings/thermal/thermal.h>` (so the biscuit DTB had never been
   compiled at all), and `drivers/clk/mediatek/clk-mt8163-apmixedsys.c`
   passed a `struct device_node *` to `mtk_clk_register_plls()`, which
   takes a `struct device *`.

### 4b.5 Bonus: the DT wiring is now dtc-validated

Because the DTB compiles for the first time, the consys node added in an
earlier pass has finally been checked by `dtc` rather than only by the C
preprocessor. `dtc -I dtb -O dts` on
`mt8163-amazon-biscuit.dtb` shows the node with all three `reg` windows
(mcu / AP_RGU / topckgen), `resets = <&watchdog 12>` (=
`MT8163_TOPRGU_CONN_MCU_RST`), `reset-names = "connsys"`, the four
`vcn*-supply` phandles and the `wifi_reset_init` pinctrl group, all
resolving. That closes the "no dtc available, only preprocessor-checked"
caveat from section 3 -- though it still says nothing about whether
those values are *correct* for the hardware, only that they are
well-formed and their phandles resolve.

## 5. Concrete next steps, in order

Everything below is now genuinely gated on hardware, not on more
compiling. Rewritten for the third pass; the old list is superseded.

1. **Source the firmware blob.** Nothing will come up without it, and
   this is now the single biggest unknown. WMT downloads a ROM
   patch/firmware image into CONSYS during init; the driver asks
   `request_firmware()` for **`WIFI_RAM_CODE_8163`**, so it needs to
   land in `/lib/firmware/` (see 4b.1 -- the path changed from the old
   hardcoded `/etc/firmware`). Look in the device's own vendor
   partition, Fire OS `/vendor/firmware` or `/etc/firmware`, or
   `amazon-biscuit-kernel`'s companion filesystem images. There may also
   be a BT patch blob and an NVRAM/EEPROM calibration file
   (`WIFI_CUSTOM_parameter`-ish) -- the `CFG_SUPPORT_NVRAM` path in
   `os/linux/platform.c` reads one, and MAC address + TX power
   calibration come from it, so WiFi may associate but perform badly (or
   come up with a random MAC) without it.

2. **Boot-test.** `make ARCH=arm64 mt8163_biscuit_defconfig && make`
   gives an Image with everything built in. The first questions to
   answer, in order:
   - does `mtk_wmt_probe()` (conn_soc, `mt8163/mtk_wcn_consys_hw.c`)
     bind to the `consys@18070000` node at all;
   - does `devm_reset_control_get(dev, "connsys")` succeed -- i.e. does
     this tree's TOPRGU/watchdog reset controller actually implement bit
     12 the way the DT wiring assumes (see 4.3, still unverified against
     silicon);
   - do the four `regulator_enable()` calls on
     vcn18/vcn28/vcn33_bt/vcn33_wifi succeed;
   - does CONSYS come out of reset and does the firmware download
     handshake complete.
   Expect to need `CONFIG_DYNAMIC_DEBUG` and the driver's own
   `/proc/driver/wmt_dbg` level knobs.

3. **Resolve the raw-PMIC-regmap question** (section 4.3). The
   `pwrap_node_to_regmap()` path pokes MT6323 registers 0x41C/0x416/0x418
   directly and is currently stubbed out with `pmic_regmap == NULL` and
   every call site guarded. If step 2 shows CONSYS powering up fine
   through the `regulator_*()` calls alone, delete the dead path rather
   than carrying the NULL-guards. If it does not, that is the first
   place to look.

4. **BT bring-up.** `/dev/stpbt` should appear once conn_soc probes.
   Decide between MediaTek's userspace-attach model (a small
   `mtkbt`/`hciattach`-alike opening `/dev/stpbt`) and finishing the
   in-kernel `hci_register_dev()` path -- note that path *does* exist in
   `stp_chrdev_bt.c` behind `#ifdef MTK_BT_HCI` and now compiles
   (`MTK_COMBO_BT` depends on `BT` for that reason), it is just untested
   and its `mtk_bt_hci_*` callbacks are visibly half-finished
   (`mtk_bt_hci_flush()` is a `pr_err("todo")` stub). The userspace
   model is less work and is what shipped on the device.

5. **WiFi bring-up.** `wlan0` should register once `HifAhbProbe()` binds
   to `wifi@180f0000`. Beyond "does it associate", two things are known
   to be feature-gaps rather than bugs, and are worth knowing before
   chasing them: `CONFIG_NL80211_TESTMODE` is off in the tested
   defconfig, so MediaTek's testmode/AGPS-assist channel compiles out
   (see 4b.4 item 5); and the P2P/Wi-Fi-Direct path is compiled but has
   had zero exercise.

6. **Then, and only then, worry about code quality.** The build is
   currently noisy with `-Wmissing-prototypes` and similar; none of it
   is load-bearing and cleaning it up before the thing works would be
   backwards.

## 6. Honest assessment

What is verified, first-hand, in this pass:

- `make ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu-
  mt8163_biscuit_defconfig && make` exits 0 and produces
  `arch/arm64/boot/Image` (~14 MB) and
  `arch/arm64/boot/dts/mediatek/mt8163-amazon-biscuit.dtb`.
- `nm vmlinux` shows the driver's probe and init entry points actually
  linked in, so it is not being silently dropped.
- The same tree also builds cleanly with the drivers as modules, giving
  six `.ko` files with no unresolved symbols.
- `dtc -I dtb` on the produced DTB shows the consys node with its reg
  windows, reset, regulators and pinctrl resolving.

What is **not** verified, and should not be represented as working:

- **Nothing has run on hardware. Not once.** No probe, no power-on
  sequence, no firmware download, no interface. "Builds" is a real
  milestone -- as far as the available prior art shows, no post-4.14
  kernel has ever built this chip's driver stack, let alone a 7.0-rc6
  one -- but it is a long way from "works".
- The reset-controller wiring (bit 12 of TOPRGU) is reasoned from a
  source comment cross-referenced against
  `dt-bindings/reset/mediatek,mt8163-wdt.h`. It is a real match, not a
  guess, but it is not a measurement.
- The raw MT6323 PMIC pokes are stubbed out entirely. Whether they were
  load-bearing or redundant with the regulator calls is unknown.
- No firmware blob has been located, so the WMT init path has never been
  exercised even in theory.
- The `mt8127` AHB PDMA backend is reused for MT8163 on the reasoning
  that it contains nothing MT8127-specific. That reasoning is from
  reading the code, not from a datasheet.
- Every cfg80211 callback whose new `link_id`/`radio_idx` argument is
  ignored is *correct for single-link, single-radio hardware*, which
  this is -- but that is an argument, not a test.
- The `request_firmware()` rewrite changes where the blob is looked for.
  That is the right call, but it means an old-style `/etc/firmware`
  layout copied off the device will not be found.

Confidence ordering, updated:

    builds (high, directly verified)
      > DT is well-formed (high, now dtc-validated)
        > module/symbol wiring is sane (high, both configs link)
          > the API semantics reconciliations are right (medium-high --
            reasoned per callback against this tree's headers, several
            real bugs found in the process, but untested)
            > the register/reset/power choices are right (medium --
              cross-referenced, not measured)
              > anything actually works on silicon (unknown, zero
                evidence either way)
