#!/usr/bin/env bash
# kexec-test.sh — boot a freshly-built kernel+dtb on a running Biscuit over
# the network, with ZERO writes to eMMC. For iterating on kernel/DTS changes
# without a single physical recovery cycle: a bad kernel just hangs, and a
# plain power-cycle returns the board to whatever was actually flashed,
# untouched. Discovered and validated end-to-end 2026-09-05 (EchoMuse
# bring-up session); this script exists so that recipe survives past this
# session's own context instead of being re-derived from scratch next time.
#
# REQUIRES: the board already booted (any boot: cold, or a PREVIOUS
# kexec — see the one-shot limitation below) with the debug console USB
# gadget up and a shell reachable over telnet (busybox telnetd, default
# port 2323 on this initrd) at DEVICE_IP.
#
# WHAT MAKES THIS WORK: kexec-tools defaults to the newer KEXEC_FILE_LOAD
# syscall (-a/--kexec-syscall-auto), which on this kernel silently ignores a
# custom --dtb and reuses whatever FDT the CURRENTLY RUNNING kernel booted
# with — so a DTS edit compiles fine, transfers fine, kexec -l/-e both
# report success, and the new kernel boots on the OLD device tree with no
# error anywhere. Forcing the legacy raw kexec_load syscall (-c) makes
# kexec-tools assemble every segment itself, including the DTB, and is the
# only path that actually respects --dtb. Confirmed by decompiling
# /proc/device-tree on the live post-kexec kernel and diffing it against the
# source DTS.
#
# ONE-SHOT LIMITATION (not fixable from software): after a kexec transition,
# secondary CPUs are PSCI-parked by the outgoing kernel's own shutdown path
# ("psci: CPU1 killed" in the console log), and no live kernel puts them back
# into a state the legacy kexec_load syscall's own CPU-idle check accepts
# ("Can't kexec: CPUs are stuck in the kernel" / EBUSY) — confirmed live:
# even forcing them back online via the standard hotplug sysfs interface
# (echo 1 > .../cpuN/online) failed with EINVAL, i.e. they refuse to come
# back at all short of a real reset. So: kexec -c only succeeds from a
# kernel that itself was never kexec'd into. Testing kernel build #2 always
# needs a real power-cycle first, not a second kexec from build #1.
#
# WHAT THIS DOES NOT DO: touch eMMC, preloader, GPT, or any amonet/LK stage.
# The board's actual boot media is exactly as it was before this ran.
#
# SMP DOES NOT SURVIVE THE TRANSITION either, even on this one-shot-from-
# pristine path: confirmed on a real run (build #480) that nproc/cpu/online
# come back as 1 - only CPU0 - while cpu/present still correctly reads 0-3.
# The new kernel's own boot-time PSCI CPU_ON calls for cpu1-3 silently do not
# bring them up, likely the same root cause as the CPU-stuck-in-kernel error
# above. Fine for testing peripherals/drivers/DTS changes (everything this
# script has been used for so far); do not trust a kexec cycle for anything
# SMP- or scheduler-sensitive - use a real boot for that.
#
# Usage:
#   ./kexec-test.sh [Image] [dtb]
# Defaults to this tree's just-built arch/arm64/boot/{Image,dts/mediatek/mt8163-amazon-biscuit.dtb}.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

IMAGE="${1:-arch/arm64/boot/Image}"
DTB="${2:-arch/arm64/boot/dts/mediatek/mt8163-amazon-biscuit.dtb}"

DEVICE_IP="${DEVICE_IP:-10.42.0.2}"
DEVICE_TELNET_PORT="${DEVICE_TELNET_PORT:-2323}"
HOST_IP="${HOST_IP:-10.42.0.1}"
HTTP_PORT="${HTTP_PORT:-8080}"   # must already be firewall-allowed on HOST_IP's
                                 # interface for every interface, or on the
                                 # specific device-facing one - do not open a
                                 # new port for this, reuse one that already is
                                 # (nixos-fw's generic 8080 rule works on any host)

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "== fetching a static aarch64 kexec binary (cached after first run) =="
KEXEC_BIN="$(nix build --no-link --print-out-paths --impure --expr \
  '(import <nixpkgs> {}).pkgsCross.aarch64-multiplatform.pkgsStatic.kexec-tools' \
  )/bin/kexec"
cp "$KEXEC_BIN" "$STAGE/kexec"

cp "$IMAGE" "$STAGE/Image"
cp "$DTB" "$STAGE/biscuit.dtb"

echo "== serving $STAGE on $HOST_IP:$HTTP_PORT =="
( cd "$STAGE" && python3 -m http.server "$HTTP_PORT" --bind "$HOST_IP" \
    >/tmp/kexec-test-httpd.log 2>&1 & echo $! > "$STAGE/httpd.pid" )
sleep 1

remote() {
  # Minimal telnet client: connect, drain any stale banner, send one
  # command, collect output for $2 seconds. No expect/pexpect dependency.
  python3 - "$1" "${2:-3}" <<'PYEOF'
import socket, sys, time
cmd, settle = sys.argv[1], float(sys.argv[2])
s = socket.create_connection((__import__("os").environ["DEVICE_IP"],
                               int(__import__("os").environ["DEVICE_PORT"])), timeout=5)
s.settimeout(0.3)
try:
    while True:
        if not s.recv(65536): break
except socket.timeout:
    pass
s.sendall(cmd.encode() + b"\n")
s.settimeout(0.3)
out = b""
t0 = time.time()
while time.time() - t0 < settle:
    try:
        chunk = s.recv(65536)
        if chunk:
            out += chunk
            t0 = time.time()
    except socket.timeout:
        pass
s.close()
sys.stdout.write(out.decode("utf-8", "replace"))
PYEOF
}
export DEVICE_IP DEVICE_PORT="$DEVICE_TELNET_PORT"

echo "== downloading kernel+dtb+kexec onto the device =="
remote "cd /tmp && wget -q http://$HOST_IP:$HTTP_PORT/kexec -O kexec && \
  wget -q http://$HOST_IP:$HTTP_PORT/Image -O Image && \
  wget -q http://$HOST_IP:$HTTP_PORT/biscuit.dtb -O biscuit.dtb && \
  chmod +x kexec && echo TRANSFER_OK" 15

echo "== repacking the live rootfs into a fresh initrd (this IS the initrd - it's a pure initramfs boot, nothing else to source it from) =="
remote "cd / && find . -xdev -not -path './tmp*' -not -name 'new-initrd.cpio.gz' | \
  cpio -o -H newc 2>/dev/null | gzip -1 > /new-initrd.cpio.gz && echo REPACK_OK" 12

CMDLINE="console=ttyS0,921600n8 console=ttyGS0 earlycon=uart8250,mmio32,0x11002000 keep_bootcon irqchip.gicv2_force_probe=1"

echo "== kexec -c -l (legacy syscall - the only one that honours --dtb) =="
remote "/tmp/kexec -c -l /tmp/Image --dtb=/tmp/biscuit.dtb --initrd=/new-initrd.cpio.gz --append=\"$CMDLINE\" 2>&1; echo KEXEC_LOAD_RC=\$?" 6

echo "== kexec -e (point of no return - jumping now) =="
remote "/tmp/kexec -e 2>&1 &" 3

echo "== board is switching kernels. USB will re-enumerate (biscuit-debug and"
echo "   the USB-network gadget's ttyACM/interface numbers WILL shift - check"
echo "   udevadm info -q property -n /dev/ttyACM* for ID_MODEL=biscuit-debug"
echo "   again, and re-run: sudo ip link set <iface> up; sudo ip addr add"
echo "   $HOST_IP/24 dev <iface> — NetworkManager does not reliably reconfigure"
echo "   this interface on its own)."

kill "$(cat "$STAGE/httpd.pid")" 2>/dev/null || true
