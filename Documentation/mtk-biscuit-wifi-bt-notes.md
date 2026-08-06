# MT6625L WiFi/BT/GPS port notes (Echo Dot 2016 "biscuit", MT8163)

Status as of 2026-08-06 (updated, second pass): **devicetree wiring done;
the conn_soc WMT/STP/BT core now compiles and links cleanly against this
tree's 6.x kernel headers (three real .ko files); wlan/gen2 (the WiFi MAC
driver) is roughly half-ported and stops at a real, non-mechanical
cfg80211 API blocker.** Nothing has been boot-tested on real hardware.
This document is the handoff for whoever continues. Section 6 (bottom) has
the up-to-date honest assessment and concrete next steps -- read that
first if you're picking this up.

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

## 5. Concrete next steps, in order

1. **Boot-test what's here.** Nothing in this port has touched real
   hardware yet. Get `mt8163-amazon-biscuit.dtb` building with a real
   `dtc` (not available in this sandbox -- only got a C-preprocessor
   syntax check, not full semantic/binding validation) and boot it with
   `mtk_stp_wmt_soc.ko` loaded (needs a full kernel build first, unlike
   the compile-only testing done so far) to see whether `mtk_wmt_probe()`
   gets past `devm_reset_control_get()` and the regulator gets calls, and
   whether CONSYS power-on actually happens. This alone would validate
   or invalidate several of the unverified items in section 4.3.
2. **wlan/gen2's cfg80211_ops surface** (section 4.4) is the concrete
   next blocker. Go through each mismatch in `os/linux/gl_init.c`
   one at a time against a current `include/net/cfg80211.h` in this
   tree -- each one needs its own small research pass (what changed,
   why, what the driver's existing MLME code actually needs from the
   callback) rather than a bulk mechanical fix.
3. **Wire real Kconfig entries** for `CONFIG_MTK_COMBO`/
   `CONFIG_MTK_COMBO_BT`/`CONFIG_MTK_COMBO_WIFI` (currently only passed
   via `make` command line for compile-testing, not real Kconfig/
   menuconfig options) so this can go into a normal defconfig.
4. **Verify the raw-PMIC-regmap question** from section 4.3 (try
   without it first; if VCN28/33 power sequencing works fine through the
   regulator_*() calls alone, delete the dead pwrap path entirely rather
   than carry the NULL-guards forever).
5. **BT**: decide whether to keep MediaTek's userspace-attach chardev
   model (`/dev/stpbt` + a small `mtkbt`-alike userspace daemon calling
   `hci_uart`-style ioctls -- the actual BTIF layer for this now exists
   in-tree, see section 4.1) or invest in writing a proper in-kernel
   `hci_register_dev()`-based driver on top of the STP core. The
   userspace-attach model is less work and BTIF now compiles; that's
   probably the pragmatic path to a working BT stack sooner, even if a
   native `hci_dev` driver would be cleaner long-term.
6. **No firmware blob has been sourced yet.** MT6625 needs a ROM
   patch/EEPROM file at minimum (WMT downloads it during consys init) --
   check `amazon-biscuit-kernel`'s vendor partition / initramfs, or Fire
   OS `/vendor/firmware` equivalent, for `WIFI_RAM_CODE_MT6625.bin` or
   similarly named blobs; without it consys init will hang even once
   everything above compiles, probes, and resets cleanly.
7. If you want to chase the `mtk_wcn_wmt_ic_info_get()` header-visibility
   mystery from section 4.2 down for its own sake (not blocking, just
   unsatisfying), the reproduction is: preprocess
   `linux/pub/stp_chrdev_bt.c` from within
   `drivers/misc/mediatek/connectivity/common/conn_soc/` with the exact
   flags `make V=1` shows for that file, `-E` instead of `-c -o ...`,
   and grep the output for `mtk_wcn_wmt_hwver_get` (shows up, declared
   right before `ic_info_get` in the source) vs `mtk_wcn_wmt_ic_info_get`
   (doesn't).

## 6. Honest assessment

Real, verifiable progress since the first pass: the WMT/STP/BT core
(`conn_soc`) compiles and links into three `.ko` files against this
tree's 6.x kernel headers -- something no prior art (the 2017 BPI
thread, frank-w's own later branches) is documented as having achieved
past kernel 6.0. The WiFi MAC driver (`wlan/gen2`) is roughly half
compiling, with a real, well-scoped, well-documented next blocker
(cfg80211 ops surface drift) rather than a vague "doesn't work."

That said, be honest with whoever reads this: **nothing has been
boot-tested.** Everything above is "compiles cleanly," which is a real
and meaningful milestone (especially given nothing like this has
compiled on any post-4.14 kernel before, per the available prior art),
but it is not the same as "works." The reset-controller wiring, the
raw-PMIC-regmap question, and the whole CONSYS firmware-download/WMT
handshake sequence are all unverified against real silicon. The
honest confidence ordering is: DT syntax (high, but not dtc-validated) >
conn_soc compiles (high) > conn_soc's specific register/reset choices
(medium -- reasoned from source comments and DT bindings, not tested) >
wlan/gen2 compiles (partial, real blocker documented) > anything
actually working on hardware (zero -- untested).
