// SPDX-License-Identifier: GPL-2.0+
/*
 * This is i.MX low power i2c controller driver.
 *
 * Copyright 2016 Freescale Semiconductor, Inc.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_data/i2c-imx.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/slab.h>

#define DRIVER_NAME "imx-lpi2c"

#define LPI2C_PARAM	0x04	/* i2c RX/TX FIFO size */
#define LPI2C_MCR	0x10	/* i2c contrl register */
#define LPI2C_MSR	0x14	/* i2c status register */
#define LPI2C_MIER	0x18	/* i2c interrupt enable */
#define LPI2C_MCFGR0	0x20	/* i2c master configuration */
#define LPI2C_MCFGR1	0x24	/* i2c master configuration */
#define LPI2C_MCFGR2	0x28	/* i2c master configuration */
#define LPI2C_MCFGR3	0x2C	/* i2c master configuration */
#define LPI2C_MCCR0	0x48	/* i2c master clk configuration */
#define LPI2C_MCCR1	0x50	/* i2c master clk configuration */
#define LPI2C_MFCR	0x58	/* i2c master FIFO control */
#define LPI2C_MFSR	0x5C	/* i2c master FIFO status */
#define LPI2C_MTDR	0x60	/* i2c master TX data register */
#define LPI2C_MRDR	0x70	/* i2c master RX data register */

/* i2c command */
#define TRAN_DATA	0X00
#define RECV_DATA	0X01
#define GEN_STOP	0X02
#define RECV_DISCARD	0X03
#define GEN_START	0X04
#define START_NACK	0X05
#define START_HIGH	0X06
#define START_HIGH_NACK	0X07

#define MCR_MEN		BIT(0)
#define MCR_RST		BIT(1)
#define MCR_DOZEN	BIT(2)
#define MCR_DBGEN	BIT(3)
#define MCR_RTF		BIT(8)
#define MCR_RRF		BIT(9)
#define MSR_TDF		BIT(0)
#define MSR_RDF		BIT(1)
#define MSR_SDF		BIT(9)
#define MSR_NDF		BIT(10)
#define MSR_ALF		BIT(11)
#define MSR_MBF		BIT(24)
#define MSR_BBF		BIT(25)
#define MIER_TDIE	BIT(0)
#define MIER_RDIE	BIT(1)
#define MIER_SDIE	BIT(9)
#define MIER_NDIE	BIT(10)
#define MCFGR1_AUTOSTOP	BIT(8)
#define MCFGR1_IGNACK	BIT(9)
#define MRDR_RXEMPTY	BIT(14)

#define MCCRx_SETHOLF_MASK     0x3F
#define MCFGR1_PRESCALE_MASK   0x07

#define I2C_CLK_RATIO	2
#define CHUNK_DATA	256

#define LPI2C_DEFAULT_RATE	100000
#define STARDARD_MAX_BITRATE	400000
#define FAST_MAX_BITRATE	1000000
#define FAST_PLUS_MAX_BITRATE	3400000
#define HIGHSPEED_MAX_BITRATE	5000000

#define I2C_PM_TIMEOUT		1000 /* ms */

enum lpi2c_imx_mode {
	STANDARD,	/* 100+Kbps */
	FAST,		/* 400+Kbps */
	FAST_PLUS,	/* 1.0+Mbps */
	HS,		/* 3.4+Mbps */
	ULTRA_FAST,	/* 5.0+Mbps */
};

enum lpi2c_imx_pincfg {
	TWO_PIN_OD,
	TWO_PIN_OO,
	TWO_PIN_PP,
	FOUR_PIN_PP,
};

struct lpi2c_imx_struct {
	struct i2c_adapter	adapter;
	int			irq;
	struct clk		*clk_per;
	struct clk		*clk_ipg;
	void __iomem		*base;
	__u8			*rx_buf;
	__u8			*tx_buf;
	struct completion	complete;
	unsigned int		msglen;
	unsigned int		delivered;
	unsigned int		block_data;
	unsigned int		bitrate;
	unsigned int		txfifosize;
	unsigned int		rxfifosize;
	enum lpi2c_imx_mode	mode;

	struct i2c_bus_recovery_info rinfo;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_pins_default;
	struct pinctrl_state *pinctrl_pins_gpio;
	unsigned int		hold_time;
	unsigned int		buf_time;
};

static void lpi2c_imx_intctrl(struct lpi2c_imx_struct *lpi2c_imx,
			      unsigned int enable)
{
	writel(enable, lpi2c_imx->base + LPI2C_MIER);
}

static int lpi2c_imx_bus_busy(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned long orig_jiffies = jiffies;
	unsigned int temp;

	while (1) {
		temp = readl(lpi2c_imx->base + LPI2C_MSR);

		/* check for arbitration lost, clear if set */
		if (temp & MSR_ALF) {
			writel(temp, lpi2c_imx->base + LPI2C_MSR);
			return -EAGAIN;
		}

		if (temp & (MSR_BBF | MSR_MBF))
			break;

		if (time_after(jiffies, orig_jiffies + msecs_to_jiffies(500))) {
			dev_dbg(&lpi2c_imx->adapter.dev, "bus not work\n");
			if (lpi2c_imx->adapter.bus_recovery_info)
				i2c_recover_bus(&lpi2c_imx->adapter);
			return -ETIMEDOUT;
		}
		schedule();
	}

	return 0;
}

static void lpi2c_imx_set_mode(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned int bitrate = lpi2c_imx->bitrate;
	enum lpi2c_imx_mode mode;

	if (bitrate < STARDARD_MAX_BITRATE)
		mode = STANDARD;
	else if (bitrate < FAST_MAX_BITRATE)
		mode = FAST;
	else if (bitrate < FAST_PLUS_MAX_BITRATE)
		mode = FAST_PLUS;
	else if (bitrate < HIGHSPEED_MAX_BITRATE)
		mode = HS;
	else
		mode = ULTRA_FAST;

	lpi2c_imx->mode = mode;
}

static int lpi2c_imx_start(struct lpi2c_imx_struct *lpi2c_imx,
			   struct i2c_msg *msgs)
{
	unsigned int temp;

	temp = readl(lpi2c_imx->base + LPI2C_MCR);
	temp |= MCR_RRF | MCR_RTF;
	writel(temp, lpi2c_imx->base + LPI2C_MCR);
	writel(0x7f00, lpi2c_imx->base + LPI2C_MSR);

	temp = i2c_8bit_addr_from_msg(msgs) | (GEN_START << 8);
	writel(temp, lpi2c_imx->base + LPI2C_MTDR);

	return lpi2c_imx_bus_busy(lpi2c_imx);
}

static void lpi2c_imx_stop(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned long orig_jiffies = jiffies;
	unsigned int temp;

	writel(GEN_STOP << 8, lpi2c_imx->base + LPI2C_MTDR);

	do {
		temp = readl(lpi2c_imx->base + LPI2C_MSR);
		if (temp & MSR_SDF)
			break;

		if (time_after(jiffies, orig_jiffies + msecs_to_jiffies(500))) {
			dev_dbg(&lpi2c_imx->adapter.dev, "stop timeout\n");
			if (lpi2c_imx->adapter.bus_recovery_info)
				i2c_recover_bus(&lpi2c_imx->adapter);
			break;
		}
		schedule();

	} while (1);
}

/* CLKLO = I2C_CLK_RATIO * CLKHI, SETHOLD = CLKHI, DATAVD = CLKHI/2 */
static int lpi2c_imx_config(struct lpi2c_imx_struct *lpi2c_imx)
{
	u16 prescale, filt, sethold = 0, clkhi, clklo, datavd;
	unsigned int clk_rate, clk_cycle;
	enum lpi2c_imx_pincfg pincfg;
	unsigned int temp;
	unsigned int min_prescaler = 0;
	struct imxi2c_platform_data *pdata = dev_get_platdata(&lpi2c_imx->adapter.dev);

	if (pdata != NULL) {
		if (pdata->bitrate && pdata->bitrate != lpi2c_imx->bitrate) {
			dev_warn(&lpi2c_imx->adapter.dev,
				 "<%s> Changing bitrate to %d\n",
				__func__, pdata->bitrate);
			lpi2c_imx->bitrate = pdata->bitrate;
		}
	}

	lpi2c_imx_set_mode(lpi2c_imx);

	clk_rate = clk_get_rate(lpi2c_imx->clk_per);
	if (!clk_rate) {
		dev_dbg(&lpi2c_imx->adapter.dev, "clk_per rate is 0\n");
		return -EINVAL;
	}

	if (lpi2c_imx->mode == HS || lpi2c_imx->mode == ULTRA_FAST)
		filt = 0;
	else
		filt = 2;

	if (lpi2c_imx->hold_time) {
		unsigned long hold_time_ns;

		for (min_prescaler = 0; min_prescaler <= MCFGR1_PRESCALE_MASK;
		     min_prescaler++) {
			hold_time_ns = (MCCRx_SETHOLF_MASK + 1) * (1 << min_prescaler) *
				       1000 / (clk_rate / 1000 / 1000);
			if (hold_time_ns >= lpi2c_imx->hold_time)
				break;
		}

		sethold = lpi2c_imx->hold_time / (1 << min_prescaler) *
				 (clk_rate / 1000 / 1000) / 1000 - 1;
		sethold &= MCCRx_SETHOLF_MASK;
	}

	for (prescale = min_prescaler; prescale <= MCFGR1_PRESCALE_MASK; prescale++) {
		clk_cycle = clk_rate / ((1 << prescale) * lpi2c_imx->bitrate)
			    - 3 - (filt >> 1);
		clkhi = (clk_cycle + I2C_CLK_RATIO) / (I2C_CLK_RATIO + 1);
		clklo = clk_cycle - clkhi;
		if (clklo < 64)
			break;
	}

	if (prescale > MCFGR1_PRESCALE_MASK)
		return -EINVAL;

	/* set MCFGR1: PINCFG, PRESCALE, IGNACK */
	if (lpi2c_imx->mode == ULTRA_FAST)
		pincfg = TWO_PIN_OO;
	else
		pincfg = TWO_PIN_OD;
	temp = prescale | pincfg << 24;

	if (lpi2c_imx->mode == ULTRA_FAST)
		temp |= MCFGR1_IGNACK;

	writel(temp, lpi2c_imx->base + LPI2C_MCFGR1);

	/* set MCFGR2: FILTSDA, FILTSCL */
	temp = (filt << 16) | (filt << 24);
	writel(temp, lpi2c_imx->base + LPI2C_MCFGR2);

	/* set MCCR: DATAVD, SETHOLD, CLKHI, CLKLO */
	if (!lpi2c_imx->hold_time)
		sethold = clkhi;

	datavd = clkhi >> 1;

	/* Some timing parameter restrictions established by the manufacturer */
	if (clkhi < 1)
		clkhi = 1;
	if (clklo < 3)
		clklo = 3;
	if (sethold < 2)
		sethold = 2;
	if (datavd < clkhi)
		datavd = clkhi;

	temp = datavd << 24 | sethold << 16 | clkhi << 8 | clklo;

	if (lpi2c_imx->mode == HS)
		writel(temp, lpi2c_imx->base + LPI2C_MCCR1);
	else
		writel(temp, lpi2c_imx->base + LPI2C_MCCR0);

	return 0;
}

static int lpi2c_imx_master_enable(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned int temp;
	int ret;

	ret = pm_runtime_get_sync(lpi2c_imx->adapter.dev.parent);
	if (ret < 0)
		goto rpm_put;

	temp = MCR_RST;
	writel(temp, lpi2c_imx->base + LPI2C_MCR);
	writel(0, lpi2c_imx->base + LPI2C_MCR);

	ret = lpi2c_imx_config(lpi2c_imx);
	if (ret)
		goto rpm_put;

	temp = readl(lpi2c_imx->base + LPI2C_MCR);
	temp |= MCR_MEN;
	writel(temp, lpi2c_imx->base + LPI2C_MCR);

	return 0;

rpm_put:
	pm_runtime_mark_last_busy(lpi2c_imx->adapter.dev.parent);
	pm_runtime_put_autosuspend(lpi2c_imx->adapter.dev.parent);

	return ret;
}

static int lpi2c_imx_master_disable(struct lpi2c_imx_struct *lpi2c_imx)
{
	u32 temp;

	temp = readl(lpi2c_imx->base + LPI2C_MCR);
	temp &= ~MCR_MEN;
	writel(temp, lpi2c_imx->base + LPI2C_MCR);

	pm_runtime_mark_last_busy(lpi2c_imx->adapter.dev.parent);
	pm_runtime_put_autosuspend(lpi2c_imx->adapter.dev.parent);

	return 0;
}

static int lpi2c_imx_msg_complete(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned long timeout;

	timeout = wait_for_completion_timeout(&lpi2c_imx->complete, HZ);

	return timeout ? 0 : -ETIMEDOUT;
}

static int lpi2c_imx_txfifo_empty(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned long orig_jiffies = jiffies;
	u32 txcnt;

	do {
		txcnt = readl(lpi2c_imx->base + LPI2C_MFSR) & 0xff;

		if (readl(lpi2c_imx->base + LPI2C_MSR) & MSR_NDF) {
			dev_dbg(&lpi2c_imx->adapter.dev, "NDF detected\n");
			return -EIO;
		}

		if (time_after(jiffies, orig_jiffies + msecs_to_jiffies(500))) {
			dev_dbg(&lpi2c_imx->adapter.dev, "txfifo empty timeout\n");
			if (lpi2c_imx->adapter.bus_recovery_info)
				i2c_recover_bus(&lpi2c_imx->adapter);
			return -ETIMEDOUT;
		}
		schedule();

	} while (txcnt);

	return 0;
}

static void lpi2c_imx_set_tx_watermark(struct lpi2c_imx_struct *lpi2c_imx)
{
	writel(lpi2c_imx->txfifosize >> 1, lpi2c_imx->base + LPI2C_MFCR);
}

static void lpi2c_imx_set_rx_watermark(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned int temp, remaining;

	remaining = lpi2c_imx->msglen - lpi2c_imx->delivered;

	if (remaining > (lpi2c_imx->rxfifosize >> 1))
		temp = lpi2c_imx->rxfifosize >> 1;
	else
		temp = 0;

	writel(temp << 16, lpi2c_imx->base + LPI2C_MFCR);
}

static void lpi2c_imx_write_txfifo(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned int data, txcnt;

	txcnt = readl(lpi2c_imx->base + LPI2C_MFSR) & 0xff;

	while (txcnt < lpi2c_imx->txfifosize) {
		if (lpi2c_imx->delivered == lpi2c_imx->msglen)
			break;

		data = lpi2c_imx->tx_buf[lpi2c_imx->delivered++];
		writel(data, lpi2c_imx->base + LPI2C_MTDR);
		txcnt++;
	}

	if (lpi2c_imx->delivered < lpi2c_imx->msglen)
		lpi2c_imx_intctrl(lpi2c_imx, MIER_TDIE | MIER_NDIE);
	else
		complete(&lpi2c_imx->complete);
}

static void lpi2c_imx_read_rxfifo(struct lpi2c_imx_struct *lpi2c_imx)
{
	unsigned int blocklen, remaining;
	unsigned int temp, data;

	do {
		data = readl(lpi2c_imx->base + LPI2C_MRDR);
		if (data & MRDR_RXEMPTY)
			break;

		lpi2c_imx->rx_buf[lpi2c_imx->delivered++] = data & 0xff;
	} while (1);

	/*
	 * First byte is the length of remaining packet in the SMBus block
	 * data read. Add it to msgs->len.
	 */
	if (lpi2c_imx->block_data) {
		blocklen = lpi2c_imx->rx_buf[0];
		lpi2c_imx->msglen += blocklen;
	}

	remaining = lpi2c_imx->msglen - lpi2c_imx->delivered;

	if (!remaining) {
		complete(&lpi2c_imx->complete);
		return;
	}

	/* not finished, still waiting for rx data */
	lpi2c_imx_set_rx_watermark(lpi2c_imx);

	/* multiple receive commands */
	if (lpi2c_imx->block_data) {
		lpi2c_imx->block_data = 0;
		temp = remaining;
		temp |= (RECV_DATA << 8);
		writel(temp, lpi2c_imx->base + LPI2C_MTDR);
	} else if (!(lpi2c_imx->delivered & 0xff)) {
		temp = (remaining > CHUNK_DATA ? CHUNK_DATA : remaining) - 1;
		temp |= (RECV_DATA << 8);
		writel(temp, lpi2c_imx->base + LPI2C_MTDR);
	}

	lpi2c_imx_intctrl(lpi2c_imx, MIER_RDIE);
}

static void lpi2c_imx_write(struct lpi2c_imx_struct *lpi2c_imx,
			    struct i2c_msg *msgs)
{
	lpi2c_imx->tx_buf = msgs->buf;
	lpi2c_imx_set_tx_watermark(lpi2c_imx);
	lpi2c_imx_write_txfifo(lpi2c_imx);
}

static void lpi2c_imx_read(struct lpi2c_imx_struct *lpi2c_imx,
			   struct i2c_msg *msgs)
{
	unsigned int temp;

	lpi2c_imx->rx_buf = msgs->buf;
	lpi2c_imx->block_data = msgs->flags & I2C_M_RECV_LEN;

	lpi2c_imx_set_rx_watermark(lpi2c_imx);
	temp = msgs->len > CHUNK_DATA ? CHUNK_DATA - 1 : msgs->len - 1;
	temp |= (RECV_DATA << 8);
	writel(temp, lpi2c_imx->base + LPI2C_MTDR);

	lpi2c_imx_intctrl(lpi2c_imx, MIER_RDIE | MIER_NDIE);
}

static int lpi2c_imx_xfer(struct i2c_adapter *adapter,
			  struct i2c_msg *msgs, int num)
{
	struct lpi2c_imx_struct *lpi2c_imx = i2c_get_adapdata(adapter);
	unsigned int temp;
	int i, result;

	result = lpi2c_imx_master_enable(lpi2c_imx);
	if (result)
		return result;

	for (i = 0; i < num; i++) {
		result = lpi2c_imx_start(lpi2c_imx, &msgs[i]);
		if (result)
			goto disable;

		/* quick smbus */
		if (num == 1 && msgs[0].len == 0)
			goto stop;

		lpi2c_imx->delivered = 0;
		lpi2c_imx->msglen = msgs[i].len;
		init_completion(&lpi2c_imx->complete);

		if (msgs[i].flags & I2C_M_RD)
			lpi2c_imx_read(lpi2c_imx, &msgs[i]);
		else
			lpi2c_imx_write(lpi2c_imx, &msgs[i]);

		result = lpi2c_imx_msg_complete(lpi2c_imx);
		if (result)
			goto stop;

		if (!(msgs[i].flags & I2C_M_RD)) {
			result = lpi2c_imx_txfifo_empty(lpi2c_imx);
			if (result)
				goto stop;
		}

		if (lpi2c_imx->buf_time)
			udelay(lpi2c_imx->buf_time);
	}

stop:
	lpi2c_imx_stop(lpi2c_imx);

	temp = readl(lpi2c_imx->base + LPI2C_MSR);
	if ((temp & MSR_NDF) && !result)
		result = -EIO;

disable:
	lpi2c_imx_master_disable(lpi2c_imx);

	dev_dbg(&lpi2c_imx->adapter.dev, "<%s> exit with: %s: %d\n", __func__,
		(result < 0) ? "error" : "success msg",
		(result < 0) ? result : num);

	return (result < 0) ? result : num;
}

static irqreturn_t lpi2c_imx_isr(int irq, void *dev_id)
{
	struct lpi2c_imx_struct *lpi2c_imx = dev_id;
	unsigned int temp;

	lpi2c_imx_intctrl(lpi2c_imx, 0);
	temp = readl(lpi2c_imx->base + LPI2C_MSR);

	if (temp & MSR_NDF) {
		complete(&lpi2c_imx->complete);
		goto ret;
	}

	if (temp & MSR_RDF)
		lpi2c_imx_read_rxfifo(lpi2c_imx);
	else if (temp & MSR_TDF)
		lpi2c_imx_write_txfifo(lpi2c_imx);

ret:
	return IRQ_HANDLED;
}

static void lpi2c_imx_prepare_recovery(struct i2c_adapter *adap)
{
	struct lpi2c_imx_struct *lpi2c_imx;

	lpi2c_imx = container_of(adap, struct lpi2c_imx_struct, adapter);

	pinctrl_select_state(lpi2c_imx->pinctrl, lpi2c_imx->pinctrl_pins_gpio);
}

static void lpi2c_imx_unprepare_recovery(struct i2c_adapter *adap)
{
	struct lpi2c_imx_struct *lpi2c_imx;

	lpi2c_imx = container_of(adap, struct lpi2c_imx_struct, adapter);

	pinctrl_select_state(lpi2c_imx->pinctrl, lpi2c_imx->pinctrl_pins_default);
}

/*
 * We switch SCL and SDA to their GPIO function and do some bitbanging
 * for bus recovery. These alternative pinmux settings can be
 * described in the device tree by a separate pinctrl state "gpio". If
 * this is missing this is not a big problem, the only implication is
 * that we can't do bus recovery.
 */
static int lpi2c_imx_init_recovery_info(struct lpi2c_imx_struct *lpi2c_imx,
		struct platform_device *pdev)
{
	struct i2c_bus_recovery_info *rinfo = &lpi2c_imx->rinfo;

	lpi2c_imx->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (!lpi2c_imx->pinctrl || IS_ERR(lpi2c_imx->pinctrl)) {
		dev_info(&pdev->dev, "can't get pinctrl, bus recovery not supported\n");
		return PTR_ERR(lpi2c_imx->pinctrl);
	}

	lpi2c_imx->pinctrl_pins_default = pinctrl_lookup_state(lpi2c_imx->pinctrl,
			PINCTRL_STATE_DEFAULT);
	lpi2c_imx->pinctrl_pins_gpio = pinctrl_lookup_state(lpi2c_imx->pinctrl,
			"gpio");

	rinfo->sda_gpiod = devm_gpiod_get(&pdev->dev, "sda", GPIOD_IN);
	rinfo->scl_gpiod = devm_gpiod_get(&pdev->dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);

	if (PTR_ERR(rinfo->sda_gpiod) == -EPROBE_DEFER ||
	    PTR_ERR(rinfo->scl_gpiod) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(rinfo->sda_gpiod) ||
		   IS_ERR(rinfo->scl_gpiod) ||
		   IS_ERR(lpi2c_imx->pinctrl_pins_default) ||
		   IS_ERR(lpi2c_imx->pinctrl_pins_gpio)) {
		dev_dbg(&pdev->dev, "recovery information incomplete\n");
		return 0;
	}

	dev_info(&pdev->dev, "using scl%s for recovery\n",
		 rinfo->sda_gpiod ? ",sda" : "");

	rinfo->prepare_recovery = lpi2c_imx_prepare_recovery;
	rinfo->unprepare_recovery = lpi2c_imx_unprepare_recovery;
	rinfo->recover_bus = i2c_generic_scl_recovery;
	lpi2c_imx->adapter.bus_recovery_info = rinfo;

	return 0;
}

static u32 lpi2c_imx_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL |
		I2C_FUNC_SMBUS_READ_BLOCK_DATA;
}

static const struct i2c_algorithm lpi2c_imx_algo = {
	.master_xfer	= lpi2c_imx_xfer,
	.functionality	= lpi2c_imx_func,
};

static const struct of_device_id lpi2c_imx_of_match[] = {
	{ .compatible = "fsl,imx7ulp-lpi2c" },
	{ },
};
MODULE_DEVICE_TABLE(of, lpi2c_imx_of_match);

static int lpi2c_imx_probe(struct platform_device *pdev)
{
	struct lpi2c_imx_struct *lpi2c_imx;
	unsigned int temp;
	int ret;

	lpi2c_imx = devm_kzalloc(&pdev->dev, sizeof(*lpi2c_imx), GFP_KERNEL);
	if (!lpi2c_imx)
		return -ENOMEM;

	lpi2c_imx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lpi2c_imx->base))
		return PTR_ERR(lpi2c_imx->base);

	lpi2c_imx->irq = platform_get_irq(pdev, 0);
	if (lpi2c_imx->irq < 0) {
		dev_err(&pdev->dev, "can't get irq number\n");
		return lpi2c_imx->irq;
	}

	lpi2c_imx->adapter.owner	= THIS_MODULE;
	lpi2c_imx->adapter.algo		= &lpi2c_imx_algo;
	lpi2c_imx->adapter.dev.parent	= &pdev->dev;
	lpi2c_imx->adapter.dev.of_node	= pdev->dev.of_node;
	strlcpy(lpi2c_imx->adapter.name, pdev->name,
		sizeof(lpi2c_imx->adapter.name));

	lpi2c_imx->clk_per = devm_clk_get(&pdev->dev, "per");
	if (IS_ERR(lpi2c_imx->clk_per)) {
		dev_err(&pdev->dev, "can't get I2C peripheral clock\n");
		return PTR_ERR(lpi2c_imx->clk_per);
	}

	lpi2c_imx->clk_ipg = devm_clk_get(&pdev->dev, "ipg");
	if (IS_ERR(lpi2c_imx->clk_ipg)) {
		dev_err(&pdev->dev, "can't get I2C ipg clock\n");
		return PTR_ERR(lpi2c_imx->clk_ipg);
	}

	ret = of_property_read_u32(pdev->dev.of_node,
				   "clock-frequency", &lpi2c_imx->bitrate);
	if (ret)
		lpi2c_imx->bitrate = LPI2C_DEFAULT_RATE;

	ret = of_property_read_u32(pdev->dev.of_node,
				   "digi,hold-time-ns", &lpi2c_imx->hold_time);
	if (ret)
		lpi2c_imx->hold_time = 0;

	ret = of_property_read_u32(pdev->dev.of_node,
				   "digi,buffer-time-us", &lpi2c_imx->buf_time);
	if (ret)
		lpi2c_imx->buf_time = 0;

	/* Init optional bus recovery function */
	ret = lpi2c_imx_init_recovery_info(lpi2c_imx, pdev);
	/* Give it another chance if pinctrl used is not ready yet */
	if (ret == -EPROBE_DEFER)
		goto rpm_disable;

	i2c_set_adapdata(&lpi2c_imx->adapter, lpi2c_imx);
	platform_set_drvdata(pdev, lpi2c_imx);

	pm_runtime_set_autosuspend_delay(&pdev->dev, I2C_PM_TIMEOUT);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	pm_runtime_get_sync(&pdev->dev);
	temp = readl(lpi2c_imx->base + LPI2C_PARAM);
	lpi2c_imx->txfifosize = 1 << (temp & 0x0f);
	lpi2c_imx->rxfifosize = 1 << ((temp >> 8) & 0x0f);

	pm_runtime_put(&pdev->dev);

	ret = i2c_add_adapter(&lpi2c_imx->adapter);
	if (ret)
		goto rpm_disable;

	dev_info(&lpi2c_imx->adapter.dev, "LPI2C adapter registered\n");

	return 0;

rpm_disable:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);

	return ret;
}

static int lpi2c_imx_remove(struct platform_device *pdev)
{
	struct lpi2c_imx_struct *lpi2c_imx = platform_get_drvdata(pdev);

	i2c_del_adapter(&lpi2c_imx->adapter);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);

	return 0;
}

static int __maybe_unused lpi2c_runtime_suspend(struct device *dev)
{
	struct lpi2c_imx_struct *lpi2c_imx = dev_get_drvdata(dev);

	devm_free_irq(dev, lpi2c_imx->irq, lpi2c_imx);
	clk_disable_unprepare(lpi2c_imx->clk_ipg);
	clk_disable_unprepare(lpi2c_imx->clk_per);
	pinctrl_pm_select_idle_state(dev);

	return 0;
}

static int __maybe_unused lpi2c_runtime_resume(struct device *dev)
{
	struct lpi2c_imx_struct *lpi2c_imx = dev_get_drvdata(dev);
	int ret;

	pinctrl_pm_select_default_state(dev);
	ret = clk_prepare_enable(lpi2c_imx->clk_per);
	if (ret) {
		dev_err(dev, "can't enable I2C per clock, ret=%d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(lpi2c_imx->clk_ipg);
	if (ret) {
		clk_disable_unprepare(lpi2c_imx->clk_per);
		dev_err(dev, "can't enable I2C ipg clock, ret=%d\n", ret);
	}

	ret = devm_request_irq(dev, lpi2c_imx->irq, lpi2c_imx_isr,
			       IRQF_NO_SUSPEND,
			       dev_name(dev), lpi2c_imx);
	if (ret) {
		dev_err(dev, "can't claim irq %d\n", lpi2c_imx->irq);
		return ret;
	}

	return ret;
}

static int lpi2c_suspend_noirq(struct device *dev)
{
	int ret;

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int lpi2c_resume_noirq(struct device *dev)
{
	return pm_runtime_force_resume(dev);
}

static const struct dev_pm_ops lpi2c_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(lpi2c_suspend_noirq,
				     lpi2c_resume_noirq)
	SET_RUNTIME_PM_OPS(lpi2c_runtime_suspend,
			   lpi2c_runtime_resume, NULL)
};

static struct platform_driver lpi2c_imx_driver = {
	.probe = lpi2c_imx_probe,
	.remove = lpi2c_imx_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = lpi2c_imx_of_match,
		.pm = &lpi2c_pm_ops,
	},
};

static int __init lpi2c_imx_init(void)
{
	return platform_driver_register(&lpi2c_imx_driver);
}
subsys_initcall(lpi2c_imx_init);

static void __exit lpi2c_imx_exit(void)
{
	platform_driver_unregister(&lpi2c_imx_driver);
}
module_exit(lpi2c_imx_exit);

MODULE_AUTHOR("Gao Pan <pandy.gao@nxp.com>");
MODULE_DESCRIPTION("I2C adapter driver for LPI2C bus");
MODULE_LICENSE("GPL");
