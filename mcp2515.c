/*
 * mcp2515.c: driver for Microchip MCP2515 SPI CAN controller
 *
 * Copyright (c) 2010 Andre B. Oliveira <anbadeol@gmail.com>
 * Copyright (c) 2012 Marc Kleine-Budde <mkl@pengutronix.de>, Pengutronix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Example of mcp2515 platform spi_board_info definition:
 *
 * static struct mcp251x_platform_data mcp251x_info = {
 *         .oscillator_frequency = 8000000,
 *         .board_specific_setup = mcp251x_setup,
 *         .power_enable = mcp251x_power_enable,
 *         .transceiver_enable = NULL,
 * };
 *
 * static struct spi_board_info spi_board_info[] = {
 *	{
 *		.modalias = "mcp2515",
 *		.bus_num = 2,
 *		.chip_select = 0,
 *		.irq = IRQ_GPIO(28),
 *		.max_speed_hz = 10000000,
 *		.platform_data = &mcp251x_info,
 *	},
 * };
 */

/*
 * References: Microchip MCP2515 data sheet, DS21801E, 2007.
 */

#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/platform/mcp251x.h>

MODULE_DESCRIPTION("Driver for Microchip MCP2515 SPI CAN controller");
MODULE_AUTHOR("Andre B. Oliveira <anbadeol@gmail.com>, "
	      "Marc Kleine-Budde <mkl@pengutronix.de>");
MODULE_LICENSE("GPL");

/* SPI interface instruction set */
#define MCP2515_INSTRUCTION_WRITE	0x02
#define MCP2515_INSTRUCTION_READ	0x03
#define MCP2515_INSTRUCTION_BIT_MODIFY	0x05
#define MCP2515_INSTRUCTION_LOAD_TXB(n)	(0x40 + ((n) << 1))
#define MCP2515_INSTRUCTION_RTS(n)	(0x80 + (1 << (n)))
#define MCP2515_INSTRUCTION_READ_RXB(n)	(0x90 + ((n) << 2))
#define MCP2515_INSTRUCTION_RESET	0xc0

/* Registers */
#define CANSTAT				0x0e
#define CANCTRL				0x0f
#define TEC				0x1c
#define REC				0x1d
#define CANINTF				0x2c
#define EFLAG				0x2d
#define CNF3				0x28
#define RXB0CTRL			0x60
#define RXB1CTRL			0x70

/* CANCTRL bits */
#define CANCTRL_REQOP_NORMAL		0x00
#define CANCTRL_REQOP_SLEEP		0x20
#define CANCTRL_REQOP_LOOPBACK		0x40
#define CANCTRL_REQOP_LISTEN_ONLY	0x60
#define CANCTRL_REQOP_CONF		0x80
#define CANCTRL_REQOP_MASK		0xe0
#define CANCTRL_OSM			BIT(3)
#define CANCTRL_ABAT			BIT(4)

/* CANINTF bits */
#define CANINTF_RX0IF			BIT(0)
#define CANINTF_RX1IF			BIT(1)
#define CANINTF_TX0IF			BIT(2)
#define CANINTF_TX1IF			BIT(3)
#define CANINTF_TX2IF			BIT(4)
#define CANINTF_ERRIF			BIT(5)
#define CANINTF_WAKIF			BIT(6)
#define CANINTF_MERRF			BIT(7)

/* EFLG bits */
#define EFLG_RX0OVR			BIT(6)
#define EFLG_RX1OVR			BIT(7)

/* CNF2 bits */
#define CNF2_BTLMODE			BIT(7)
#define CNF2_SAM			BIT(6)

/* CANINTE bits */
#define CANINTE_RX0IE			BIT(0)
#define CANINTE_RX1IE			BIT(1)
#define CANINTE_TX0IE			BIT(2)
#define CANINTE_TX1IE			BIT(3)
#define CANINTE_TX2IE			BIT(4)
#define CANINTE_ERRIE			BIT(5)
#define CANINTE_WAKIE			BIT(6)
#define CANINTE_MERRE			BIT(7)
#define CANINTE_RX			(CANINTE_RX0IE | CANINTE_RX1IE)
#define CANINTE_TX \
	(CANINTE_TX0IE | CANINTE_TX1IE | CANINTE_TX2IE)
#define CANINTE_ERR			(CANINTE_ERRIE)

/* RXBnCTRL bits */
#define RXBCTRL_BUKT			BIT(2)
#define RXBCTRL_RXM0			BIT(5)
#define RXBCTRL_RXM1			BIT(6)

/* RXBnSIDL bits */
#define RXBSIDL_IDE			BIT(3)
#define RXBSIDL_SRR			BIT(4)

/* RXBnDLC bits */
#define RXBDLC_RTR			BIT(6)


#define MCP2515_DMA_SIZE		32

/* Network device private data */
struct mcp2515_priv {
	struct can_priv can;	/* must be first for all CAN network devices */
	struct spi_device *spi;	/* SPI device */
	struct mcp251x_platform_data *pdata;

	u8 canintf;		/* last read value of CANINTF register */
	u8 eflg;		/* last read value of EFLG register */

	struct sk_buff *skb;	/* skb to transmit or currently transmitting */

	spinlock_t lock;	/* Lock for the following flags: */
	unsigned busy:1;	/* set when pending async spi transaction */
	unsigned interrupt:1;	/* set when pending interrupt handling */
	unsigned transmit:1;	/* set when pending transmission */

	/* Message, transfer and buffers for one async spi transaction */
	struct spi_message message;
	struct spi_transfer transfer;
	u8 rx_buf[14] __attribute__((aligned(8)));
	u8 tx_buf[14] __attribute__((aligned(8)));
};

static struct can_bittiming_const mcp2515_bittiming_const = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 2,
	.tseg1_max = 16,
	.tseg2_min = 2,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 64,
	.brp_inc = 1,
};

/*
 * SPI asynchronous completion callback functions.
 */
static void mcp2515_read_flags_complete(void *context);
static void mcp2515_read_rxb0_complete(void *context);
static void mcp2515_read_rxb1_complete(void *context);
static void mcp2515_clear_canintf_complete(void *context);
static void mcp2515_clear_eflg_complete(void *context);
static void mcp2515_load_txb0_complete(void *context);
static void mcp2515_rts_txb0_complete(void *context);

/*
 * Write VALUE to register at address ADDR.
 * Synchronous.
 */
static int mcp2515_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
	const u8 buf[] __attribute__((aligned(8))) = {
		[0] = MCP2515_INSTRUCTION_WRITE,
		[1] = reg,	/* address */
		[2] = val,	/* data */
	};

	return spi_write(spi, buf, sizeof(buf));
}

/*
 * Read VALUE from register at address ADDR.
 * Synchronous.
 */
static int mcp2515_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
	const u8 buf[] __attribute__((aligned(8))) = {
		[0] = MCP2515_INSTRUCTION_READ,
		[1] = reg,	/* address */
	};

	return spi_write_then_read(spi, buf, sizeof(buf), val, sizeof(*val));
}

static int mcp2515_read_2regs(struct spi_device *spi, u8 reg, u8 *v1, u8 *v2)
{
	const u8 tx_buf[] __attribute__((aligned(8))) = {
		[0] = MCP2515_INSTRUCTION_READ,
		[1] = reg,	/* address */
	};
	u8 rx_buf[2] __attribute__((aligned(8)));
	int err;

	err = spi_write_then_read(spi, tx_buf, sizeof(tx_buf),
				  rx_buf, sizeof(rx_buf));
	if (err)
		return err;

	*v1 = rx_buf[0];
	*v2 = rx_buf[1];

	return 0;
}

/*
 * Reset internal registers to default state and enter configuration mode.
 * Synchronous.
 */
static int mcp2515_hw_reset(struct spi_device *spi)
{
	const u8 cmd = MCP2515_INSTRUCTION_RESET;

	return spi_write(spi, &cmd, sizeof(cmd));
}

static int mcp2515_hw_sleep(struct spi_device *spi)
{
	return mcp2515_write_reg(spi, CANCTRL, CANCTRL_REQOP_SLEEP);
}

static void mcp2515_power_switch(const struct mcp2515_priv *priv, int on)
{
 /*

	if (priv->pdata->power_enable)
		priv->pdata->power_enable(on);
	else if (!on)
		mcp2515_hw_sleep(priv->spi);
*/
}

static void mcp2515_transceiver_switch(const struct mcp2515_priv *priv, int on)
{
/*

	if (priv->pdata->transceiver_enable)
		priv->pdata->transceiver_enable(on);
*/
}

static void mcp2515_board_specific_setup(const struct mcp2515_priv *priv)
{
/*	if (priv->pdata->board_specific_setup)
		priv->pdata->board_specific_setup(priv->spi);
*/
}

/*
 * Set the bit timing configuration registers, the interrupt enable register
 * and the receive buffers control registers.
 * Synchronous.
 */
static int mcp2515_chip_start(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;
	struct can_bittiming *bt = &priv->can.bittiming;
	unsigned long timeout;
	u8 *buf = (u8 *)priv->transfer.tx_buf;
	u8 mode;
	int err;

	err = mcp2515_hw_reset(spi);
	if (err)
		return err;

	/* set bittiming */
	buf[0] = MCP2515_INSTRUCTION_WRITE;
	buf[1] = CNF3;

	/* CNF3 */
	buf[2] = bt->phase_seg2 - 1;

	/* CNF2 */
	buf[3] = CNF2_BTLMODE |
		(priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES ? CNF2_SAM : 0x0) |
		(bt->phase_seg1 - 1) << 3 | (bt->prop_seg - 1);

	/* CNF1 */
	buf[4] = (bt->sjw - 1) << 6 | (bt->brp - 1);

	/* CANINTE */
	buf[5] = CANINTE_RX | CANINTE_TX | CANINTE_ERR;

	netdev_info(dev, "writing CNF: 0x%02x 0x%02x 0x%02x\n",
		    buf[4], buf[3], buf[2]);
	err = spi_write(spi, buf, 6);
	if (err)
		return err;

	/* config RX buffers */
	/* buf[0] = MCP2515_INSTRUCTION_WRITE; already set */
	buf[1] = RXB0CTRL;

	/* RXB0CTRL */
	buf[2] = RXBCTRL_RXM1 | RXBCTRL_RXM0 | RXBCTRL_BUKT;

	/* RXB1CTRL */
	buf[3] = RXBCTRL_RXM1 | RXBCTRL_RXM0;

	err = spi_write(spi, buf, 4);
	if (err)
		return err;

	/* handle can.ctrlmode */
	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		mode = CANCTRL_REQOP_LOOPBACK;
	else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		mode = CANCTRL_REQOP_LISTEN_ONLY;
	else
		mode = CANCTRL_REQOP_NORMAL;

	if (mode & CAN_CTRLMODE_ONE_SHOT)
		mode |= CANCTRL_OSM;

	/* Put device into requested mode */
	mcp2515_transceiver_switch(priv, 1);
	mcp2515_write_reg(spi, CANCTRL, mode);

	/* Wait for the device to enter requested mode */
	timeout = jiffies + HZ;
	do {
		u8 reg_stat;

		err = mcp2515_read_reg(spi, CANSTAT, &reg_stat);
		if (err)
			goto failed_request;
		else if ((reg_stat & CANCTRL_REQOP_MASK) == mode)
			break;

		schedule();
		if (time_after(jiffies, timeout)) {
			dev_err(&spi->dev,
				"MCP2515 didn't enter in requested mode\n");
			err = -EBUSY;
			goto failed_request;
		}
	} while (1);

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	return 0;

 failed_request:
	mcp2515_transceiver_switch(priv, 0);

	return err;
}

static void mcp2515_chip_stop(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;

	mcp2515_hw_reset(spi);
	mcp2515_transceiver_switch(priv, 0);
	priv->can.state = CAN_STATE_STOPPED;

	return;
}

/*
 * Start an asynchronous SPI transaction.
 */
static void mcp2515_spi_async(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	int err;

	err = spi_async(priv->spi, &priv->message);
	if (err)
		netdev_err(dev, "%s failed with err=%d\n", __func__, err);
}

/*
 * Read CANINTF and EFLG registers in one shot.
 * Asynchronous.
 */
static void mcp2515_read_flags(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_READ;
	buf[1] = CANINTF;
	buf[2] = 0;	/* CANINTF */
	buf[3] = 0;	/* EFLG */
	priv->transfer.len = 4;
	priv->message.complete = mcp2515_read_flags_complete;

	mcp2515_spi_async(dev);
}

/*
 * Read receive buffer 0 (instruction 0x90) or 1 (instruction 0x94).
 * Asynchronous.
 */
static void mcp2515_read_rxb(struct net_device *dev, u8 instruction,
			     void (*complete)(void *))
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	memset(buf, 0, 14);
	buf[0] = instruction;
	priv->transfer.len = 14; /* instruction + id(4) + dlc + data(8) */
	priv->message.complete = complete;

	mcp2515_spi_async(dev);
}

/*
 * Read receive buffer 0.
 * Asynchronous.
 */
static void mcp2515_read_rxb0(struct net_device *dev)
{
	mcp2515_read_rxb(dev, MCP2515_INSTRUCTION_READ_RXB(0),
			 mcp2515_read_rxb0_complete);
}

/*
 * Read receive buffer 1.
 * Asynchronous.
 */
static void mcp2515_read_rxb1(struct net_device *dev)
{
	mcp2515_read_rxb(dev, MCP2515_INSTRUCTION_READ_RXB(1),
			 mcp2515_read_rxb1_complete);
}

/*
 * Clear CANINTF bits.
 * Asynchronous.
 */
static void mcp2515_clear_canintf(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_BIT_MODIFY;
	buf[1] = CANINTF;
	buf[2] = priv->canintf & ~(CANINTF_RX0IF | CANINTF_RX1IF); /* mask */
	buf[3] = 0;	/* data */
	priv->transfer.len = 4;
	priv->message.complete = mcp2515_clear_canintf_complete;

	mcp2515_spi_async(dev);
}

/*
 * Clear EFLG bits.
 * Asynchronous.
 */
static void mcp2515_clear_eflg(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_BIT_MODIFY;
	buf[1] = EFLAG;
	buf[2] = priv->eflg;	/* mask */
	buf[3] = 0;		/* data */
	priv->transfer.len = 4;
	priv->message.complete = mcp2515_clear_eflg_complete;

	mcp2515_spi_async(dev);
}

/*
 * Set the transmit buffer, starting at TXB0SIDH, for an skb.
 */
static int mcp2515_set_txbuf(u8 *buf, const struct sk_buff *skb)
{
	struct can_frame *frame = (struct can_frame *)skb->data;

	if (frame->can_id & CAN_EFF_FLAG) {
		buf[0] = frame->can_id >> 21;
		buf[1] = (frame->can_id >> 13 & 0xe0) | 8 |
			(frame->can_id >> 16 & 3);
		buf[2] = frame->can_id >> 8;
		buf[3] = frame->can_id;
	} else {
		buf[0] = frame->can_id >> 3;
		buf[1] = frame->can_id << 5;
		buf[2] = 0;
		buf[3] = 0;
	}

	buf[4] = frame->can_dlc;
	if (frame->can_id & CAN_RTR_FLAG)
		buf[4] |= 0x40;

	memcpy(buf + 5, frame->data, frame->can_dlc);

	return 5 + frame->can_dlc;
}

/*
 * Send the "load transmit buffer 0" SPI message.
 * Asynchronous.
 */
static void mcp2515_load_txb0(struct sk_buff *skb, struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_LOAD_TXB(0);
	priv->transfer.len = mcp2515_set_txbuf(buf + 1, skb) + 1;
	priv->message.complete = mcp2515_load_txb0_complete;

	can_put_echo_skb(skb, dev, 0);

	mcp2515_spi_async(dev);
}

/*
 * Send the "request to send transmit buffer 0" SPI message.
 * Asynchronous.
 */
static void mcp2515_rts_txb0(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = (u8 *)priv->transfer.tx_buf;

	buf[0] = MCP2515_INSTRUCTION_RTS(0);
	priv->transfer.len = 1;
	priv->message.complete = mcp2515_rts_txb0_complete;

	mcp2515_spi_async(dev);
}

/*
 * Called when the "read CANINTF and EFLG registers" SPI message completes.
 */
static void mcp2515_read_flags_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);
	u8 *buf = priv->transfer.rx_buf;
	unsigned canintf;
	unsigned long flags;

	priv->canintf = canintf = buf[2];
	priv->eflg = buf[3];

	if (canintf & CANINTF_RX0IF)
		mcp2515_read_rxb0(dev);
	else if (canintf & CANINTF_RX1IF)
		mcp2515_read_rxb1(dev);
	else if (canintf)
		mcp2515_clear_canintf(dev);
	else {
		spin_lock_irqsave(&priv->lock, flags);
		if (priv->transmit) {
			priv->transmit = 0;
			spin_unlock_irqrestore(&priv->lock, flags);
			mcp2515_load_txb0(priv->skb, dev);
		} else if (priv->interrupt) {
			priv->interrupt = 0;
			spin_unlock_irqrestore(&priv->lock, flags);
			mcp2515_read_flags(dev);
		} else {
			priv->busy = 0;
			spin_unlock_irqrestore(&priv->lock, flags);
		}
	}
}

/*
 * Called when one of the "read receive buffer i" SPI message completes.
 */
static void mcp2515_read_rxb_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	struct can_frame *frame;
	u8 *buf = priv->transfer.rx_buf;

	skb = alloc_can_skb(dev, &frame);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}

	if (buf[2] & RXBSIDL_IDE) {
		frame->can_id = buf[1] << 21 | (buf[2] & 0xe0) << 13 |
			(buf[2] & 3) << 16 | buf[3] << 8 | buf[4] |
			CAN_EFF_FLAG;
		if (buf[5] & RXBDLC_RTR)
			frame->can_id |= CAN_RTR_FLAG;
	} else {
		frame->can_id = buf[1] << 3 | buf[2] >> 5;
		if (buf[2] & RXBSIDL_SRR)
			frame->can_id |= CAN_RTR_FLAG;
	}

	frame->can_dlc = get_can_dlc(buf[5] & 0xf);

	if (!(frame->can_id & CAN_RTR_FLAG))
		memcpy(frame->data, buf + 6, frame->can_dlc);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += frame->can_dlc;

	netif_rx_ni(skb);
}

/*
 * Transmit a frame if transmission pending, else read and process flags.
 */
static void mcp2515_transmit_or_read_flags(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->transmit) {
		priv->transmit = 0;
		spin_unlock_irqrestore(&priv->lock, flags);
		mcp2515_load_txb0(priv->skb, dev);
	} else {
		spin_unlock_irqrestore(&priv->lock, flags);
		mcp2515_read_flags(dev);
	}
}

/*
 * Called when the "read receive buffer 0" SPI message completes.
 */
static void mcp2515_read_rxb0_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);

	mcp2515_read_rxb_complete(context);

	if (priv->canintf & CANINTF_RX1IF)
		mcp2515_read_rxb1(dev);
	else
		mcp2515_transmit_or_read_flags(dev);
}

/*
 * Called when the "read receive buffer 1" SPI message completes.
 */
static void mcp2515_read_rxb1_complete(void *context)
{
	struct net_device *dev = context;

	mcp2515_read_rxb_complete(context);

	mcp2515_transmit_or_read_flags(dev);
}

/*
 * Called when the "clear CANINTF bits" SPI message completes.
 */
static void mcp2515_clear_canintf_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);

	if (priv->canintf & CANINTF_TX0IF) {
		struct sk_buff *skb = priv->skb;
		if (skb) {
			dev->stats.tx_bytes += can_get_echo_skb(dev, 0);
			dev->stats.tx_packets++;
		}
		priv->skb = NULL;
		netif_wake_queue(dev);
	}

	if (priv->eflg)
		mcp2515_clear_eflg(dev);
	else
		mcp2515_read_flags(dev);
}

/*
 * Called when the "clear EFLG bits" SPI message completes.
 */
static void mcp2515_clear_eflg_complete(void *context)
{
	struct net_device *dev = context;
	struct mcp2515_priv *priv = netdev_priv(dev);

	/*
	 * The receive flow chart (figure 4-3) of the data sheet (DS21801E)
	 * says that, if RXB0CTRL.BUKT is set (our case), the overflow
	 * flag that is set is EFLG.RX1OVR, when in fact it is EFLG.RX0OVR
	 * that is set.  To be safe, we test for any one of them.
	 */
	if (priv->eflg & (EFLG_RX0OVR | EFLG_RX1OVR))
		dev->stats.rx_over_errors++;

	mcp2515_read_flags(dev);
}

/*
 * Called when the "load transmit buffer 0" SPI message completes.
 */
static void mcp2515_load_txb0_complete(void *context)
{
	struct net_device *dev = context;

	mcp2515_rts_txb0(dev);
}

/*
 * Called when the "request to send transmit buffer 0" SPI message completes.
 */
static void mcp2515_rts_txb0_complete(void *context)
{
	struct net_device *dev = context;

	mcp2515_read_flags(dev);
}

/*
 * Interrupt handler.
 */
static irqreturn_t mcp2515_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct mcp2515_priv *priv = netdev_priv(dev);

	spin_lock(&priv->lock);
	if (priv->busy) {
		priv->interrupt = 1;
		spin_unlock(&priv->lock);
		return IRQ_HANDLED;
	}
	priv->busy = 1;
	spin_unlock(&priv->lock);

	mcp2515_read_flags(dev);

	return IRQ_HANDLED;
}

/*
 * Transmit a frame.
 */
static netdev_tx_t mcp2515_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	unsigned long flags;

	if (can_dropped_invalid_skb(dev, skb))
		return NETDEV_TX_OK;

	netif_stop_queue(dev);
	priv->skb = skb;

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->busy) {
		priv->transmit = 1;
		spin_unlock_irqrestore(&priv->lock, flags);
		return NETDEV_TX_OK;
	}
	priv->busy = 1;
	spin_unlock_irqrestore(&priv->lock, flags);

	mcp2515_load_txb0(skb, dev);

	return NETDEV_TX_OK;
}

/*
 * Called when the network device transitions to the up state.
 */
static int mcp2515_open(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;
	int err;

	mcp2515_power_switch(priv, 1);

	err = open_candev(dev);
	if (err)
		goto failed_open;

	err = request_irq(spi->irq, mcp2515_interrupt,
			  IRQF_TRIGGER_FALLING, dev->name, dev);
	if (err)
		goto failed_irq;

	err = mcp2515_chip_start(dev);
	if (err)
		goto failed_start;

	netif_start_queue(dev);

	return 0;

 failed_start:
	free_irq(spi->irq, dev);
 failed_irq:
	close_candev(dev);
 failed_open:
	mcp2515_power_switch(priv, 0);
	return err;
}

/*
 * Called when the network device transitions to the down state.
 */
static int mcp2515_close(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;

	netif_stop_queue(dev);
	mcp2515_chip_stop(dev);
	free_irq(spi->irq, dev);

	mcp2515_power_switch(priv, 0);

	close_candev(dev);

	return 0;
}

/*
 * Set up SPI messages.
 */
static void mcp2515_setup_spi_messages(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct device *device;
	void *buf;
	dma_addr_t dma;

	spi_message_init(&priv->message);
	priv->message.context = dev;

	/* FIXME */
	device = &priv->spi->dev;
	device->coherent_dma_mask = 0xffffffff;

	BUILD_BUG_ON(MCP2515_DMA_SIZE <
		     sizeof(priv->tx_buf) + sizeof(priv->rx_buf));

	buf = dma_alloc_coherent(device, MCP2515_DMA_SIZE, &dma, GFP_KERNEL);
	if (buf) {
		priv->transfer.tx_buf = buf;
		priv->transfer.rx_buf = buf + MCP2515_DMA_SIZE / 2;
		priv->transfer.tx_dma = dma;
		priv->transfer.rx_dma = dma + MCP2515_DMA_SIZE / 2;
		priv->message.is_dma_mapped = 1;
	} else {
		priv->transfer.tx_buf = priv->tx_buf;
		priv->transfer.rx_buf = priv->rx_buf;
	}

	spi_message_add_tail(&priv->transfer, &priv->message);
}

static void mcp2515_cleanup_spi_messages(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);

	if (!priv->message.is_dma_mapped)
		return;

	dma_free_coherent(&priv->spi->dev, MCP2515_DMA_SIZE,
			  (void *)priv->transfer.tx_buf, priv->transfer.tx_dma);
}

static int mcp2515_set_mode(struct net_device *dev, enum can_mode mode)
{
	int err;

	switch (mode) {
	case CAN_MODE_START:
		err = mcp2515_chip_start(dev);
		if (err)
			return err;

		netif_wake_queue(dev);
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mcp2515_get_berr_counter(const struct net_device *dev,
				    struct can_berr_counter *bec)
{
	const struct mcp2515_priv *priv = netdev_priv(dev);
	int err;
	u8 reg_tec, reg_rec;

	err = mcp2515_read_2regs(priv->spi, TEC, &reg_tec, &reg_rec);
	if (err)
		return err;

	bec->txerr = reg_tec;
	bec->rxerr = reg_rec;

	return 0;
}

/*
 * Network device operations.
 */
static const struct net_device_ops mcp2515_netdev_ops = {
	.ndo_open = mcp2515_open,
	.ndo_stop = mcp2515_close,
	.ndo_start_xmit = mcp2515_start_xmit,
};

static int mcp2515_register_candev(struct net_device *dev)
{
	struct mcp2515_priv *priv = netdev_priv(dev);
	struct spi_device *spi = priv->spi;
	u8 reg_stat, reg_ctrl;
	int err = 0;

	mcp2515_board_specific_setup(priv);
	mcp2515_power_switch(priv, 1);
	mcp2515_hw_reset(spi);

	/*
	 * Please note that these are "magic values" based on after
	 * reset defaults taken from data sheet which allows us to see
	 * if we really have a chip on the bus (we avoid common all
	 * zeroes or all ones situations)
	 */
	err |= mcp2515_read_reg(spi, CANSTAT, &reg_stat);
	err |= mcp2515_read_reg(spi, CANCTRL, &reg_ctrl);
	dev_dbg(&spi->dev, "%s: canstat=0x%02x canctrl=0x%02x\n",
		__func__, reg_stat, reg_ctrl);

	/* Check for power up default values */
	if (!((reg_stat & 0xee) == 0x80 && (reg_ctrl & 0x17) == 0x07) || err) {
		dev_err(&spi->dev, "%s: failed to detect chip"
			"(canstat=0x%02x, canctrl=0x%02x, err=%d)\n",
			__func__, reg_stat, reg_ctrl, err);
		err = -ENODEV;
		goto failed_detect;
	}

	err = register_candev(dev);
	if (err)
		goto failed_register;

	mcp2515_power_switch(priv, 0);

	return 0;

 failed_register:
 failed_detect:
	mcp2515_power_switch(priv, 0);

	return err;
}

static void mcp2515_unregister_candev(struct net_device *dev)
{
	unregister_candev(dev);
}

/*
 * Binds this driver to the spi device.
 */
static int mcp2515_probe(struct spi_device *spi)
{
	struct net_device *dev;
	struct mcp2515_priv *priv;
	struct mcp251x_platform_data *pdata = spi->dev.platform_data;
	int err;

	if (!pdata) {
		/* Platform data is required for osc freq */
		err = -ENODEV;
		goto failed_pdata;
	}

	dev = alloc_candev(sizeof(struct mcp2515_priv), 1);
	if (!dev) {
		err = -ENOMEM;
		goto failed_alloc;
	}

	dev_set_drvdata(&spi->dev, dev);
	SET_NETDEV_DEV(dev, &spi->dev);

	dev->netdev_ops = &mcp2515_netdev_ops;
	dev->flags |= IFF_ECHO;

	priv = netdev_priv(dev);
	priv->can.bittiming_const = &mcp2515_bittiming_const;
	priv->can.clock.freq = pdata->oscillator_frequency / 2;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
		CAN_CTRLMODE_LISTENONLY | CAN_CTRLMODE_3_SAMPLES |
		CAN_CTRLMODE_ONE_SHOT;
	priv->can.do_set_mode = mcp2515_set_mode;
	priv->can.do_get_berr_counter = mcp2515_get_berr_counter;
	priv->spi = spi;
	priv->pdata = pdata;

	spin_lock_init(&priv->lock);

	mcp2515_setup_spi_messages(dev);

	err = mcp2515_register_candev(dev);
	if (err) {
		netdev_err(dev, "registering netdev failed");
		goto failed_register;
	}

	netdev_info(dev, "device registered (cs=%u, irq=%d)\n",
		    spi->chip_select, spi->irq);

	return 0;

 failed_register:
	mcp2515_cleanup_spi_messages(dev);
	dev_set_drvdata(&spi->dev, NULL);
	free_candev(dev);
 failed_alloc:
 failed_pdata:
	return err;
}

/*
 * Unbinds this driver from the spi device.
 */
static int mcp2515_remove(struct spi_device *spi)
{
	struct net_device *dev = dev_get_drvdata(&spi->dev);

	mcp2515_unregister_candev(dev);
	mcp2515_cleanup_spi_messages(dev);
	dev_set_drvdata(&spi->dev, NULL);
	free_candev(dev);

	return 0;
}

static struct spi_driver mcp2515_can_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
	},
	.probe = mcp2515_probe,
	.remove = mcp2515_remove,
};

module_spi_driver(mcp2515_can_driver);

