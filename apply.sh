#!/bin/sh
# Put the AX55 v1 files into a checked-out ipq50xx-rebase tree.
#
#   ./apply.sh /path/to/openwrt-nss-edma
#
# The files under package/ and target/ are ours alone and are copied. The
# seven files the board shares with the branch - the board table, the image
# recipe, the LED and caldata scripts, the upgrade path and two kernel
# configs - are a patch instead, so a `git pull` in the tree can merge them
# rather than have them overwritten. Re-run after every pull.
set -e

here=$(cd "$(dirname "$0")" && pwd)
tree=${1:-.}

[ -f "$tree/rules.mk" ] && [ -d "$tree/target/linux/qualcommax" ] || {
	echo "$tree does not look like an OpenWrt tree" >&2
	exit 1
}

cd "$tree"

echo "Copying:"
for d in package/kernel/rtl8367s-nss \
         target/linux/generic/pending-6.18 \
         target/linux/qualcommax/dts \
         target/linux/qualcommax/ipq50xx/base-files; do
	[ -d "$here/$d" ] || continue
	mkdir -p "$d"
	cp -a "$here/$d/." "$d/"
	echo "  $d"
done

echo "Board files:"
p=$here/patches/0001-ax55-v1-board-support.patch
if git apply --reverse --check "$p" 2>/dev/null; then
	echo "  already applied"
elif git apply --check "$p" 2>/dev/null; then
	git apply "$p"
	echo "  patch applied"
else
	echo "  patch does not apply cleanly - the branch has moved under it." >&2
	echo "  Try: git apply --3way $p" >&2
	exit 1
fi

cat <<'EOF'

Done. Two things this cannot do for you:

  - the board's calibration data. Extract it from your own router's 0:ART
    and drop it in package/firmware/ipq-wifi/files/ as
    board-tplink_archer-ax55-v1.ipq5018 and board-tplink_archer-ax55-v1.qcn6122

  - .config. Run make menuconfig, pick
      Target System  -> Qualcomm Atheros IPQ50xx
      Target Profile -> TP-Link Archer AX55 v1
    and check that kmod-rtl8367s-nss and the qca-nss-* packages are selected.

The tree's own ipq5018-archer-ax55-v1.dts is left alone; DEVICE_DTS in the
patch points the build at ours, which carries the NSS node and the
fixed-link pause.
EOF
