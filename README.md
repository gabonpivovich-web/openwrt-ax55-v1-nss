## Important!

This build is designed for the Realtek `RTL8367S-VB` switch (Family D, chip ID `0x6642`). It will therefore not work on the `RTL8367S` (Family C). Please bear this in mind.

Please also bear in mind that this is a test version. It should by no means be considered FULLY FUNCTIONAL.
At the moment, everything is working on my board and I haven’t encountered any major problems.

# OpenWrt with NSS offload — TP-Link Archer AX55 v1 (IPQ5018)

Everything needed to build @kuncy7's [`ipq50xx-rebase`][branch] for the Archer
AX55 v1, plus a prebuilt image. The board is in OpenWrt main; what is here is
the part that is not: the NSS data path over a Realtek switch.

[branch]: https://github.com/kuncy7/openwrt-nss-edma/tree/ipq50xx-rebase

**Forum thread:** <https://forum.openwrt.org/t/253014>

## Hardware

| | |
|---|---|
| SoC | Qualcomm IPQ5018 |
| RAM | 512 MB |
| Flash | 128 MB SPI-NAND |
| Switch | Realtek RTL8367S-VB, **2.5G** HSGMII trunk on GMAC1 |
| Ports | WAN + 4× LAN, all on the switch |
| Wi-Fi 2.4 GHz | IPQ5018 integrated |
| Wi-Fi 5 GHz | QCN6122 |

## Measured

| Check | Mbit/s | router CPU |
|---|---|---|
| wired, routed both ways | 943 | ~2% |
| Wi-Fi 5 GHz (160 MHz, HE-NSS 2, −50 dBm), AP → STA | 915 | 1–3% |
| Wi-Fi 5 GHz, STA → AP | 818 | 1–3% |

Both radios run on wifili. Stock TP-Link firmware reaches the same figures.

## What is in here

```
package/kernel/rtl8367s-nss/          two modules: the re-arm, and the LEDs
target/linux/generic/pending-6.18/    RTL8367S-VB family D support
target/linux/qualcommax/              board DTS and board files
apply.sh                              copies the above into a checked-out tree
```

### `rtl8367s-nss`

The NSS firmware parses 802.1q but not the Realtek CPU tag, so a board that
wants the offload has to give up DSA and drive the switch as a plain trunk.
Unbinding `rtl8365mb` is the easy half; the module is the other half, putting
back the force word, the VLAN table, the PVIDs, the egress mode, the learning
limit and the front PHYs — everything the driver takes with it on the way out.
It is the Realtek counterpart to `qca8337-nss` in the branch.

It does that in one pass and returns `-EAGAIN`, so it never stays resident,
and it refuses anything but chip ID 0x6642 - a family C board is left alone.
This half is upstream as [kuncy7/openwrt-nss-edma#5][pr].

[pr]: https://github.com/kuncy7/openwrt-nss-edma/pull/5

### `rtl8367s-leds`

The cosmetics, in their own module because they are not needed to carry a
frame. It polls the front PHYs to drive the case LEDs, and registers a
display-only `net_device` per jack so LuCI's port panel, the netdev LED
triggers and the per-port counters have something to read. Those devices
carry link state, negotiated speed and the switch's MIB counters, and a
stable MAC derived from the board's; they cannot carry a frame, and
`ndo_start_xmit` drops and counts anything handed to them.

It finds the switch itself rather than leaning on the re-arm module, so
either can be left out.

### The `930-*` patches

RTL8367S-VB (family D) support for `rtl8365mb`, by **Mieczyslaw Nalewaj
(@namiltd)**, taken unmodified from his [`Realtek_DSA`][nam] branch.

[nam]: https://github.com/namiltd/openwrt/tree/Realtek_DSA/target/linux/generic/pending-6.18

### The board DTS

The one thing worth reading if you have a different board: both `fixed-link`
nodes on the trunk carry `pause`.

```
&gmac1 {
	fixed-link {
		speed = <2500>;
		full-duplex;
		pause;          /* <- this */
	};
};
```

Without it phylink resolves the link as pauseless, `dwmac1000_flow_ctrl()`
never sets `GMAC_FLOW_CTRL_RFE`, and the MAC discards the pause frames the
switch is sending it — the switch then drops ~13% of a 400 Mbit/s stream on
the 2.5G→1G step, with every NSS counter clean. Worth adding to any
`fixed-link` faster than the front ports.

## Building

```sh
git clone -b ipq50xx-rebase https://github.com/kuncy7/openwrt-nss-edma.git
cd openwrt-nss-edma

cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/kuncy7/nss-packages.git;ipq50xx-rebase" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
./scripts/feeds list -r nss | grep -q qca-nss-drv && echo "nss feed OK"

/path/to/this/repo/apply.sh .

make menuconfig    # Target: Qualcomm Atheros IPQ50xx, Profile: TP-Link Archer AX55 v1
make -j$(nproc)
```

`apply.sh` copies the files and prints what it changed. The board table entry,
the `DEVICE_DTS` line and the LuCI port wiring are patched into the branch's
own files, so re-run it after every `git pull`.


## Credits

- [@kuncy7](https://github.com/kuncy7) — the NSS branch this builds on, and the
  `qca8337-nss` design the Realtek module follows
- [@namiltd](https://github.com/namiltd) — RTL8367S-VB family D support
- OpenWrt, for the board port itself

## Licence

GPL-2.0-only, as the tree it plugs into.
