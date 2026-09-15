// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtl8367s_leds.c - case LEDs and display-only port netdevs for a board
 * whose RTL8367S-VB has been taken off DSA.
 *
 * rtl8367s_nss puts the switch back into a plain 802.1q trunk and leaves;
 * what it cannot put back is everything that hung off the DSA user ports.
 * There is no netdev per jack any more, so the kernel's netdev LED trigger
 * has nothing to follow, LuCI's port panel has nothing to read, and the
 * per-port counters are gone.
 *
 * This module polls BMSR on the front PHYs to drive the case LEDs, and
 * registers one net_device per jack carrying that port's link state,
 * negotiated speed and the switch's own MIB counters. Those devices are
 * display-only: the data path is the trunk, and a frame handed to one has
 * nowhere to go, so ndo_start_xmit drops it and counts it. Do not bridge
 * them for anything but the port panel's colouring.
 *
 * It finds the switch itself rather than leaning on rtl8367s_nss, which
 * does its work in one pass and refuses to stay loaded. The register
 * plumbing below is therefore a copy of that module's.
 *
 * Register reads on this chip only return the addressed register's
 * contents shortly after a write; every read here is preceded by a
 * harmless one.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/array_size.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/rtnetlink.h>

#define REALTEK_MDIO_CTRL0_REG		31
#define REALTEK_MDIO_CTRL1_REG		21
#define REALTEK_MDIO_ADDRESS_REG	23
#define REALTEK_MDIO_DATA_WRITE_REG	24
#define REALTEK_MDIO_DATA_READ_REG	25
#define REALTEK_MDIO_ADDR_OP		0x000E
#define REALTEK_MDIO_READ_OP		0x0001
#define REALTEK_MDIO_WRITE_OP		0x0003
#define RTL_MAGIC_REG			0x13C2	/* 0 is its idle value; see rtl_chip_id() */
#define RTL_MAGIC_VALUE			0x0249
#define RTL_CHIP_ID_REG			0x1300
#define RTL_CHIP_VER_REG		0x1301
#define RTL_CHIP_ID_RTL8367S_VB		0x6642
#define RTL_D_FORCE_BASE		0x12C0
#define RTL_D_FORCE_EN_BASE		0x12C8
#define RTL_D_FORCE_EN_ALL		0xFFFF
#define RTL_MSTI_CTRL_REG(_p)		(0x0A00 + ((_p) >> 3))
#define   RTL_MSTI_STATE_MASK(_p)	(0x3 << (((_p) & 7) << 1))
#define   RTL_MSTI_STATE_FORWARDING	3
#define RTL_PORT_ISOLATION_REG(_p)	(0x08A2 + (_p))
#define   RTL_PORT_ISOLATION_MASK	0x00FF
#define RTL_TABLE_CTRL_REG		0x0500
#define   RTL_TABLE_CTRL_TABLE_CVLAN	0x0003	/* bits 2:0 */
#define   RTL_TABLE_CTRL_OP_WRITE	0x0008	/* bit 3 */
#define   RTL_TABLE_CTRL_OP_READ	0x0000
#define RTL_TABLE_ADDR_REG		0x0501
#define RTL_TABLE_STATUS_REG		0x0502
#define   RTL_TABLE_STATUS_BUSY		0x2000	/* bit 13 */
#define RTL_TABLE_WRITE_BASE		0x0510
#define RTL_TABLE_READ_BASE		0x0520
#define RTL_CVLAN_ENTRY_SIZE		3
#define   RTL_TABLE_CTRL_TABLE_L2	0x0004	/* bits 2:0 */
#define   RTL_TABLE_CTRL_METHOD_NEXT_UC	0x0030	/* method 3 in bits 6:4 */
#define   RTL_TABLE_STATUS_ADDRESS_MASK	0x07FF
#define   RTL_TABLE_STATUS_HIT		0x1000
#define RTL_L2_ENTRY_SIZE		6
#define RTL_LEARN_LIMIT_REG(_p)		(0x0A20 + (_p))
#define   RTL_LEARN_LIMIT_MAX		2112
#define RTL_D_FID_BITS(_fid)		(((_fid) & 0x3) | BIT(3) | BIT(2))
#define RTL_VLAN_CTRL_REG		0x07A8
#define   RTL_VLAN_CTRL_EN		0x0001
#define RTL_MIB_COUNTER_REG(_x)		(0x1000 + (_x))
#define RTL_MIB_ADDRESS_REG		0x1004
#define   RTL_MIB_PORT_OFFSET		0x007C
#define   RTL_MIB_ADDRESS(_p, _x)	(((_p) * RTL_MIB_PORT_OFFSET + (_x)) >> 2)
#define RTL_MIB_CTRL0_REG		0x1005
#define   RTL_MIB_CTRL0_RESET_MASK	0x0002
#define   RTL_MIB_CTRL0_BUSY_MASK	0x0001
#define RTL_MIB_IF_IN_OCTETS		0
#define RTL_MIB_IF_OUT_OCTETS		60
#define RTL_MIB_IF_IN_UCAST		16
#define RTL_MIB_IF_OUT_UCAST		82
#define RTL_MIB_IF_OUT_MCAST		84
#define RTL_MIB_IF_OUT_BCAST		86
#define RTL_MIB_IF_IN_MCAST		20
#define RTL_MIB_IF_IN_BCAST		22
#define RTL_MIB_IN_PAUSE		8
#define RTL_MIB_OUT_PAUSE		76
#define RTL_PORT_MISC_CFG_REG(_p)	(0x000E + ((_p) << 5))
#define   RTL_VLAN_EGRESS_MODE_MASK	0x0030
#define   RTL_VLAN_EGRESS_MODE_ORIGINAL	0x0000
#define RTL_D_PVID_REG(_p)		(0x0700 + (_p))
#define   RTL_D_PVID_MASK		0x0FFF
#define RTL_ACCEPT_FRAME_REG(_p)	(0x07AA + ((_p) >> 3))
#define   RTL_ACCEPT_FRAME_MASK(_p)	(0x3 << (((_p) & 0x7) << 1))
#define RTL_CPU_CTRL_REG		0x121A
#define   RTL_CPU_CTRL_EN_MASK		0x0001
#define   RTL_CPU_CTRL_INSERTMODE_MASK	0x0006
#define RTL_IA_CTRL_REG			0x1F00
#define   RTL_IA_CTRL_RW_WRITE		0x0002
#define   RTL_IA_CTRL_CMD		0x0001
#define RTL_IA_STATUS_REG		0x1F01
#define RTL_IA_ADDRESS_REG		0x1F02
#define RTL_IA_WRITE_DATA_REG		0x1F03
#define RTL_IA_READ_DATA_REG		0x1F04
#define RTL_PHY_BASE			0x2000
#define RTL_GPHY_OCP_MSB_0_REG		0x1D15
#define   RTL_GPHY_OCP_MSB_0_MASK	0x0FC0
#define RTL_PHY_OCP_ADDR_PHYREG_BASE	0xA400

static char *bus_id = "90000.mdio-1";
module_param(bus_id, charp, 0444);
MODULE_PARM_DESC(bus_id, "MDIO bus holding the switch");

static int sw_addr = 0x1d;
module_param(sw_addr, int, 0444);
MODULE_PARM_DESC(sw_addr, "switch MDIO address");

/* Front-port link state has no netdev to hang off any more: DSA is gone
 * and the trunk is up whatever the jacks are doing. Poll BMSR on the
 * front PHYs instead and drive the case LEDs directly.
 */
static char *led_lan = "green:lan";
module_param(led_lan, charp, 0444);
MODULE_PARM_DESC(led_lan, "LED lit while any LAN PHY has link, empty to disable");

static char *led_lan_phys = "1,2,3,4";
module_param(led_lan_phys, charp, 0444);

static char *led_wan = "";
module_param(led_wan, charp, 0444);
MODULE_PARM_DESC(led_wan,
		 "LED for WAN PHY link. Empty by default: wan-online-led owns\n"
		 "both WAN LEDs, since only userspace can tell a live cable\n"
		 "from a working uplink");

static char *led_wan_phys = "0";
module_param(led_wan_phys, charp, 0444);

/* Exported so userspace can tell "cable in the jack" from "the other end
 * answers": with no per-port netdev there is nothing else to ask.
 */
static int lan_link;
module_param(lan_link, int, 0444);
MODULE_PARM_DESC(lan_link, "read-only: 1 while any LAN PHY has link");

static int wan_link;
module_param(wan_link, int, 0444);
MODULE_PARM_DESC(wan_link, "read-only: 1 while the WAN PHY has link");

static char *port_map = "0:wan,1:lan1,2:lan2,3:lan3,4:lan4";
module_param(port_map, charp, 0444);
MODULE_PARM_DESC(port_map,
		 "display-only netdevs to register, as switchport:name pairs;\n"
		 "empty to register none");

static char *base_mac;
module_param(base_mac, charp, 0444);
MODULE_PARM_DESC(base_mac,
		 "MAC to derive the port netdev addresses from, usually the\n"
		 "trunk's; empty gives each a random one, which changes on\n"
		 "every boot");

static int stats_ms = 2000;
module_param(stats_ms, int, 0644);
MODULE_PARM_DESC(stats_ms,
		 "how often to sweep the MIB into the port netdevs, in ms");

static int poll_ms = 1000;
module_param(poll_ms, int, 0644);
MODULE_PARM_DESC(poll_ms, "link poll interval in ms, 0 to disable the LEDs");

/* PVID per front port, "port:vid,..." - what an untagged ingress frame
 * gets tagged with.
 */
static char *pvids = "1:1,2:1,3:1,4:1,0:2";
module_param(pvids, charp, 0444);
MODULE_PARM_DESC(pvids, "per-port PVID, e.g. 0:1,1:1,2:1,3:1,4:2");

static bool dry_run;
module_param(dry_run, bool, 0444);
MODULE_PARM_DESC(dry_run, "read and report, change nothing");

/* Every register touch used to be logged, which is how the switch was
 * reverse engineered but is far too loud for normal boots.
 */
static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "log every register access, not just the changes");

static struct mii_bus *rbus;

static void rtl_write(u16 reg, u16 val)
{
	mutex_lock(&rbus->mdio_lock);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_ADDRESS_REG, reg);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_DATA_WRITE_REG, val);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_WRITE_OP);
	mutex_unlock(&rbus->mdio_lock);
}

static int rtl_read(u16 reg, u16 *val)
{
	int ret;

	/* See the file header: a read only returns the addressed register
	 * shortly after a write, so prime the interface with one.
	 */
	rtl_write(RTL_MAGIC_REG, 0);

	mutex_lock(&rbus->mdio_lock);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_ADDRESS_REG, reg);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_READ_OP);
	ret = rbus->read(rbus, sw_addr, REALTEK_MDIO_DATA_READ_REG);
	mutex_unlock(&rbus->mdio_lock);

	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

/* The chip ID and version registers only answer while the magic register
 * holds 0x0249, which is also the register rtl_read() primes with - so the
 * ordinary read path cannot see them, and this has to drive the bus itself.
 * Same sequence as rtl8365mb_read_chip_id_and_ver().
 */
static int rtl_chip_id(u16 *id, u16 *ver)
{
	int ret;

	*id = 0;
	*ver = 0;

	rtl_write(RTL_MAGIC_REG, RTL_MAGIC_VALUE);

	mutex_lock(&rbus->mdio_lock);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_ADDRESS_REG, RTL_CHIP_ID_REG);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_READ_OP);
	ret = rbus->read(rbus, sw_addr, REALTEK_MDIO_DATA_READ_REG);
	if (ret >= 0) {
		*id = ret;
		rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
		rbus->write(rbus, sw_addr, REALTEK_MDIO_ADDRESS_REG, RTL_CHIP_VER_REG);
		rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_READ_OP);
		ret = rbus->read(rbus, sw_addr, REALTEK_MDIO_DATA_READ_REG);
		if (ret >= 0)
			*ver = ret;
	}
	mutex_unlock(&rbus->mdio_lock);

	rtl_write(RTL_MAGIC_REG, 0);

	return ret < 0 ? ret : 0;
}

/* Read with no priming write in front. Only valid immediately after a
 * write - which is exactly the case for the indirect window, where the
 * command register write is itself the priming write, and where anything
 * inserted between the command and 0x1F04 destroys the result.
 */
static int rtl_read_raw(u16 reg, u16 *val)
{
	int ret;

	mutex_lock(&rbus->mdio_lock);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_ADDRESS_REG, reg);
	rbus->write(rbus, sw_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_READ_OP);
	ret = rbus->read(rbus, sw_addr, REALTEK_MDIO_DATA_READ_REG);
	mutex_unlock(&rbus->mdio_lock);

	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

/* Set while the LED poll runs: it touches registers four times a second
 * and must not narrate.
 */
static bool rtl_quiet;

static int rtl_update(u16 reg, u16 mask, u16 val)
{
	u16 old, new;
	int ret;

	ret = rtl_read(reg, &old);
	if (ret)
		return ret;

	new = (old & ~mask) | (val & mask);
	if (new == old) {
		if (verbose && !rtl_quiet)
			pr_info("rtl8367s-leds: 0x%04X already 0x%04X\n",
				reg, old);
		return 0;
	}

	if (dry_run) {
		pr_info("rtl8367s-leds: 0x%04X would go 0x%04X -> 0x%04X\n",
			reg, old, new);
		return 0;
	}

	rtl_write(reg, new);
	ret = rtl_read(reg, &old);
	if (!rtl_quiet && (verbose || old != new))
		pr_info("rtl8367s-leds: 0x%04X set to 0x%04X, reads back 0x%04X\n",
			reg, new, old);

	return ret;
}

/* ===== indirect access to the front PHYs ===== */

static int rtl_ia_wait(void)
{
	u16 status;
	int i;

	for (i = 0; i < 20; i++) {
		if (rtl_read(RTL_IA_STATUS_REG, &status))
			return -EIO;
		if (!status)
			return 0;
		usleep_range(10, 20);
	}

	return -ETIMEDOUT;
}

static int rtl_ocp_prepare(int phy, u32 ocp_addr)
{
	u16 val;

	if (rtl_update(RTL_GPHY_OCP_MSB_0_REG, RTL_GPHY_OCP_MSB_0_MASK,
		       (ocp_addr >> 4) & RTL_GPHY_OCP_MSB_0_MASK))
		return -EIO;

	val = RTL_PHY_BASE;
	val |= (phy << 5) & 0x00E0;		/* PHYNUM, bits 7:5 */
	val |= (ocp_addr >> 1) & 0x001F;	/* OCPADR 5:1 */
	val |= ((ocp_addr >> 6) << 8) & 0x0F00;	/* OCPADR 9:6 */

	rtl_write(RTL_IA_ADDRESS_REG, val);
	return 0;
}

static int rtl_phy_read(int phy, int regnum, u16 *out)
{
	u32 ocp_addr = RTL_PHY_OCP_ADDR_PHYREG_BASE + regnum * 2;
	int ret;

	ret = rtl_ia_wait();
	if (ret)
		return ret;

	ret = rtl_ocp_prepare(phy, ocp_addr);
	if (ret)
		return ret;

	/* Command, then straight to the data register. No poll, no priming
	 * write: either would clobber the result.
	 */
	rtl_write(RTL_IA_CTRL_REG, RTL_IA_CTRL_CMD);

	return rtl_read_raw(RTL_IA_READ_DATA_REG, out);
}

static int rtl_phy_write(int phy, int regnum, u16 val)
{
	u32 ocp_addr = RTL_PHY_OCP_ADDR_PHYREG_BASE + regnum * 2;
	int ret;

	ret = rtl_ia_wait();
	if (ret)
		return ret;

	ret = rtl_ocp_prepare(phy, ocp_addr);
	if (ret)
		return ret;

	rtl_write(RTL_IA_WRITE_DATA_REG, val);
	rtl_write(RTL_IA_CTRL_REG, RTL_IA_CTRL_RW_WRITE | RTL_IA_CTRL_CMD);

	return rtl_ia_wait();
}






/* ===== MIB ===== */

static int rtl_mib_read(int port, u32 offset, u32 length, u64 *out)
{
	u16 val = 0;
	u64 v = 0;
	int i, ret;

	rtl_write(RTL_MIB_ADDRESS_REG, RTL_MIB_ADDRESS(port, offset));

	for (i = 0; i < 100; i++) {
		ret = rtl_read(RTL_MIB_CTRL0_REG, &val);
		if (ret)
			return ret;
		if (!(val & RTL_MIB_CTRL0_BUSY_MASK))
			break;
		usleep_range(10, 20);
	}
	if (val & RTL_MIB_CTRL0_BUSY_MASK)
		return -ETIMEDOUT;
	if (val & RTL_MIB_CTRL0_RESET_MASK)
		return -EIO;

	/* Four counter registers hold one counter; a 4-word value starts at
	 * the top, a 2-word value at (offset + 1) % 4.
	 */
	offset = (length == 4) ? 3 : (offset + 1) % 4;

	for (i = 0; i < length; i++) {
		ret = rtl_read(RTL_MIB_COUNTER_REG(offset - i), &val);
		if (ret)
			return ret;
		v = (v << 16) | val;
	}

	*out = v;
	return 0;
}

static void rtl_mib_show(void)
{
	static const int ports[] = { 0, 1, 2, 3, 4, 6 };
	u64 in, out;
	int i;

	pr_info("rtl8367s-leds: MIB port    rx octets      tx octets   rx ucast  tx ucast  tx mcast  tx bcast  pause in  pause out\n");

	for (i = 0; i < (int)ARRAY_SIZE(ports); i++) {
		int port = ports[i];
		u64 rxp, txp, txm, txb, pin, pout;

		if (rtl_mib_read(port, RTL_MIB_IF_IN_OCTETS, 4, &in) ||
		    rtl_mib_read(port, RTL_MIB_IF_OUT_OCTETS, 4, &out) ||
		    rtl_mib_read(port, RTL_MIB_IF_IN_UCAST, 2, &rxp) ||
		    rtl_mib_read(port, RTL_MIB_IF_OUT_UCAST, 2, &txp) ||
		    rtl_mib_read(port, RTL_MIB_IF_OUT_MCAST, 2, &txm) ||
		    rtl_mib_read(port, RTL_MIB_IF_OUT_BCAST, 2, &txb) ||
		    rtl_mib_read(port, RTL_MIB_IN_PAUSE, 2, &pin) ||
		    rtl_mib_read(port, RTL_MIB_OUT_PAUSE, 2, &pout)) {
			pr_warn("rtl8367s-leds: MIB port %d read failed\n",
				port);
			continue;
		}

		pr_info("rtl8367s-leds: MIB %4d %12llu %14llu %10llu %9llu %9llu %9llu %9llu %9llu\n",
			port, in, out, rxp, txp, txm, txb, pin, pout);
	}
}

/* ===== front-port link LEDs ===== */

#define RTL_BMSR_LSTATUS	0x0004

static struct delayed_work rtl_led_work;
static int rtl_lan_last = -1;
static int rtl_wan_last = -1;

static void rtl_led_set(const char *led, int on)
{
	char path[96];
	struct file *f;
	loff_t pos = 0;
	char v[2];

	if (!led || !*led)
		return;

	snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", led);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return;

	v[0] = on ? '1' : '0';
	v[1] = '\n';
	kernel_write(f, v, 2, &pos);
	filp_close(f, NULL);
}

/* BMSR latches link-down, so a single read can report a stale drop right
 * after one; read twice and take the second.
 */
static int rtl_phy_has_link(int phy)
{
	u16 bmsr;

	if (rtl_phy_read(phy, MII_BMSR, &bmsr))
		return 0;
	if (rtl_phy_read(phy, MII_BMSR, &bmsr))
		return 0;

	return !!(bmsr & RTL_BMSR_LSTATUS);
}

static int rtl_any_link(const char *spec)
{
	char *list, *tok, *p;
	int phy, any = 0;

	if (!spec || !*spec)
		return -1;

	list = kstrdup(spec, GFP_KERNEL);
	if (!list)
		return -1;

	p = list;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (!*tok)
			continue;
		if (kstrtoint(tok, 0, &phy) || phy < 0 || phy > 7)
			continue;
		if (rtl_phy_has_link(phy)) {
			any = 1;
			break;
		}
	}

	kfree(list);
	return any;
}

/* Defined with the port netdevs, below the MIB helpers it needs. */
static void rtl_pnd_poll(void);

static void rtl_led_poll(struct work_struct *w)
{
	int lan, wan;

	if (!rbus)
		return;

	/* Four register reads a second is not something to narrate. */
	rtl_quiet = true;

	lan = rtl_any_link(led_lan_phys);
	if (lan >= 0) {
		lan_link = lan;
		if (lan != rtl_lan_last) {
			rtl_led_set(led_lan, lan);
			rtl_lan_last = lan;
		}
	}

	wan = rtl_any_link(led_wan_phys);
	if (wan >= 0) {
		wan_link = wan;
		if (wan != rtl_wan_last) {
			rtl_led_set(led_wan, wan);
			rtl_wan_last = wan;
		}
	}

	rtl_pnd_poll();

	rtl_quiet = false;

	if (poll_ms > 0)
		schedule_delayed_work(&rtl_led_work,
				      msecs_to_jiffies(poll_ms));
}

/* ===== Front-port netdevs =====
 *
 * One net_device per front jack, carrying that port's link state, speed and
 * MIB counters.  They are display-only: the data path is the trunk, and a
 * frame handed to one of these has nowhere to go, so ndo_start_xmit drops
 * it and counts it.  Do not bridge them.
 */

#define RTL_PND_MAX		8

struct rtl_pnd {
	struct net_device	*ndev;
	int			port;
	int			phy;
	int			speed;		/* SPEED_* or SPEED_UNKNOWN */
	u8			duplex;		/* DUPLEX_* */
	struct rtnl_link_stats64 stats;
	spinlock_t		lock;		/* guards stats/speed/duplex */
};

static struct net_device *rtl_pnd_dev[RTL_PND_MAX];
static int rtl_pnd_count;
static int rtl_pnd_due;			/* ms left until the next sweep */

static netdev_tx_t rtl_pnd_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rtl_pnd *p = netdev_priv(dev);

	/* tx_dropped, not tx_errors: nothing went wrong, the frame simply
	 * has nowhere to go. The bridge hands one to every member port and
	 * this one is a display, so a broadcast lands here on every hop.
	 */
	spin_lock(&p->lock);
	p->stats.tx_dropped++;
	spin_unlock(&p->lock);

	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int rtl_pnd_open(struct net_device *dev)
{
	/* The poll sets the real state within poll_ms. */
	netif_carrier_off(dev);
	return 0;
}

static int rtl_pnd_stop(struct net_device *dev)
{
	netif_carrier_off(dev);
	return 0;
}

static void rtl_pnd_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *out)
{
	struct rtl_pnd *p = netdev_priv(dev);

	spin_lock(&p->lock);
	*out = p->stats;
	spin_unlock(&p->lock);
}

static const struct net_device_ops rtl_pnd_ops = {
	.ndo_open		= rtl_pnd_open,
	.ndo_stop		= rtl_pnd_stop,
	.ndo_start_xmit		= rtl_pnd_xmit,
	.ndo_get_stats64	= rtl_pnd_get_stats64,
};

static int rtl_pnd_get_ksettings(struct net_device *dev,
				 struct ethtool_link_ksettings *cmd)
{
	struct rtl_pnd *p = netdev_priv(dev);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	ethtool_link_ksettings_add_link_mode(cmd, supported, TP);
	ethtool_link_ksettings_add_link_mode(cmd, supported, Autoneg);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 100baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 1000baseT_Full);

	cmd->base.port = PORT_TP;
	cmd->base.autoneg = AUTONEG_ENABLE;

	spin_lock(&p->lock);
	cmd->base.speed = p->speed;
	cmd->base.duplex = p->duplex;
	spin_unlock(&p->lock);

	return 0;
}

static void rtl_pnd_get_drvinfo(struct net_device *dev,
				struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "rtl8367s-nss", sizeof(info->driver));
	strscpy(info->bus_info, "mdio", sizeof(info->bus_info));
}

static const struct ethtool_ops rtl_pnd_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= rtl_pnd_get_ksettings,
	.get_drvinfo		= rtl_pnd_get_drvinfo,
};

static void rtl_pnd_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &rtl_pnd_ops;
	dev->ethtool_ops = &rtl_pnd_ethtool_ops;
	dev->needs_free_netdev = true;
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->flags &= ~IFF_MULTICAST;
}

/* Resolve the link the PHY settled on.  Clause-22 only: the advertisement
 * both sides agreed on is the highest bit set in both our register and the
 * partner's.
 */
static void rtl_pnd_read_link(struct rtl_pnd *p)
{
	u16 bmsr, lpa = 0, stat1000 = 0, ctrl1000 = 0;
	int speed = SPEED_UNKNOWN;
	u8 duplex = DUPLEX_UNKNOWN;

	if (rtl_phy_read(p->phy, MII_BMSR, &bmsr) ||
	    rtl_phy_read(p->phy, MII_BMSR, &bmsr))
		goto out;

	if (!(bmsr & RTL_BMSR_LSTATUS))
		goto out;

	rtl_phy_read(p->phy, MII_LPA, &lpa);
	rtl_phy_read(p->phy, MII_STAT1000, &stat1000);
	rtl_phy_read(p->phy, MII_CTRL1000, &ctrl1000);

	if ((stat1000 & LPA_1000FULL) && (ctrl1000 & ADVERTISE_1000FULL)) {
		speed = SPEED_1000;
		duplex = DUPLEX_FULL;
	} else if ((stat1000 & LPA_1000HALF) && (ctrl1000 & ADVERTISE_1000HALF)) {
		speed = SPEED_1000;
		duplex = DUPLEX_HALF;
	} else if (lpa & LPA_100FULL) {
		speed = SPEED_100;
		duplex = DUPLEX_FULL;
	} else if (lpa & LPA_100HALF) {
		speed = SPEED_100;
		duplex = DUPLEX_HALF;
	} else if (lpa & LPA_10FULL) {
		speed = SPEED_10;
		duplex = DUPLEX_FULL;
	} else if (lpa & LPA_10HALF) {
		speed = SPEED_10;
		duplex = DUPLEX_HALF;
	}

out:
	spin_lock(&p->lock);
	p->speed = speed;
	p->duplex = duplex;
	spin_unlock(&p->lock);

	if (speed == SPEED_UNKNOWN) {
		if (netif_carrier_ok(p->ndev))
			netif_carrier_off(p->ndev);
	} else if (!netif_carrier_ok(p->ndev)) {
		netif_carrier_on(p->ndev);
	}
}

static void rtl_pnd_read_mib(struct rtl_pnd *p)
{
	u64 in_oct, out_oct, rx_uc, rx_mc, rx_bc, tx_uc, tx_mc, tx_bc;

	if (rtl_mib_read(p->port, RTL_MIB_IF_IN_OCTETS, 4, &in_oct) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_OUT_OCTETS, 4, &out_oct) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_IN_UCAST, 2, &rx_uc) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_IN_MCAST, 2, &rx_mc) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_IN_BCAST, 2, &rx_bc) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_OUT_UCAST, 2, &tx_uc) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_OUT_MCAST, 2, &tx_mc) ||
	    rtl_mib_read(p->port, RTL_MIB_IF_OUT_BCAST, 2, &tx_bc))
		return;

	spin_lock(&p->lock);
	p->stats.rx_bytes = in_oct;
	p->stats.tx_bytes = out_oct;
	p->stats.rx_packets = rx_uc + rx_mc + rx_bc;
	p->stats.tx_packets = tx_uc + tx_mc + tx_bc;
	p->stats.multicast = rx_mc;
	spin_unlock(&p->lock);
}

/* Called from the LED poll, which already holds the switch quiet. */
static void rtl_pnd_poll(void)
{
	bool sweep;
	int i;

	if (!rtl_pnd_count)
		return;

	rtl_pnd_due -= poll_ms > 0 ? poll_ms : 1000;
	sweep = stats_ms > 0 && rtl_pnd_due <= 0;
	if (sweep)
		rtl_pnd_due = stats_ms;

	for (i = 0; i < rtl_pnd_count; i++) {
		struct rtl_pnd *p = netdev_priv(rtl_pnd_dev[i]);

		/* ethtool and LuCI read nothing off a device that is
		 * administratively down, and the init script cannot do this
		 * for us: at START=15 /sbin/ip is still a symlink into an
		 * overlay that is not there yet.
		 */
		if (!(rtl_pnd_dev[i]->flags & IFF_UP)) {
			rtnl_lock();
			dev_change_flags(rtl_pnd_dev[i],
					 rtl_pnd_dev[i]->flags | IFF_UP, NULL);
			rtnl_unlock();
		}

		rtl_pnd_read_link(p);
		if (sweep)
			rtl_pnd_read_mib(p);
	}
}

static void rtl_pnd_unregister(void)
{
	int i;

	for (i = 0; i < rtl_pnd_count; i++) {
		if (rtl_pnd_dev[i])
			unregister_netdev(rtl_pnd_dev[i]);
		rtl_pnd_dev[i] = NULL;
	}
	rtl_pnd_count = 0;
}

static int rtl_pnd_add(int port, int phy, const char *name)
{
	struct net_device *ndev;
	struct rtl_pnd *p;
	u8 mac[ETH_ALEN];
	int ret;

	if (rtl_pnd_count >= RTL_PND_MAX)
		return -ENOSPC;

	ndev = alloc_netdev(sizeof(*p), name, NET_NAME_PREDICTABLE,
			    rtl_pnd_setup);
	if (!ndev)
		return -ENOMEM;

	/* A stable address, so the port list does not look new after every
	 * reboot. The locally-administered bit and a last byte no real
	 * interface here uses keep a display-only device from ever being
	 * mistaken for one that forwards.
	 */
	if (base_mac && *base_mac && mac_pton(base_mac, mac)) {
		mac[0] |= 0x02;
		mac[ETH_ALEN - 1] = 0xF0 | (port & 0x0F);
		eth_hw_addr_set(ndev, mac);
	} else {
		eth_hw_addr_random(ndev);
	}

	p = netdev_priv(ndev);
	memset(p, 0, sizeof(*p));
	spin_lock_init(&p->lock);
	p->ndev = ndev;
	p->port = port;
	p->phy = phy;
	p->speed = SPEED_UNKNOWN;
	p->duplex = DUPLEX_UNKNOWN;

	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		return ret;
	}

	netif_carrier_off(ndev);
	rtl_pnd_dev[rtl_pnd_count++] = ndev;
	return 0;
}

/* port_map is "switchport:name" pairs.  The PHY number equals the switch
 * port number on this part - the five jacks are its own internal PHYs 0-4.
 */
static void rtl_pnd_register(void)
{
	char *list, *tok, *p, *colon;
	int port, n = 0;

	if (!port_map || !*port_map)
		return;

	list = kstrdup(port_map, GFP_KERNEL);
	if (!list)
		return;

	p = list;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (!*tok)
			continue;
		colon = strchr(tok, ':');
		if (!colon || !colon[1])
			continue;
		*colon = '\0';
		if (kstrtoint(tok, 0, &port) || port < 0 || port > 7)
			continue;
		if (rtl_pnd_add(port, port, colon + 1) == 0)
			n++;
	}

	kfree(list);

	if (n)
		pr_info("rtl8367s-leds: %d port netdevs registered (display only)\n",
			n);
}

static int __init rtl_leds_init(void)
{
	rbus = mdio_find_bus(bus_id);
	if (!rbus) {
		pr_err("rtl8367s-leds: no MDIO bus '%s'\n", bus_id);
		return -ENODEV;
	}

	if (poll_ms <= 0) {
		pr_err("rtl8367s-leds: poll_ms is 0, nothing to do\n");
		put_device(&rbus->dev);
		rbus = NULL;
		return -EINVAL;
	}

	rtl_pnd_register();
	INIT_DELAYED_WORK(&rtl_led_work, rtl_led_poll);
	schedule_delayed_work(&rtl_led_work, 0);
	pr_info("rtl8367s-leds: polling front-PHY link every %d ms\n", poll_ms);

	return 0;
}

static void __exit rtl_leds_exit(void)
{
	cancel_delayed_work_sync(&rtl_led_work);
	rtl_pnd_unregister();

	if (rbus) {
		put_device(&rbus->dev);
		rbus = NULL;
	}
}

module_init(rtl_leds_init);
module_exit(rtl_leds_exit);

MODULE_DESCRIPTION("Case LEDs and display-only port netdevs for the RTL8367S-VB");
MODULE_LICENSE("GPL");
