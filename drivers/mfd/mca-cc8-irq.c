/*
 *  Copyright 2018 - 2019 Digi International Inc
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/mfd/core.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/mfd/mca-common/core.h>
#include <linux/mfd/mca-cc8/core.h>
#include <soc/imx/soc.h>

#define MCA_IRQ_0_OFFSET		0
#define MCA_IRQ_1_OFFSET		1
#define MCA_IRQ_2_OFFSET		2
#define MCA_IRQ_3_OFFSET		3

static const struct regmap_irq mca_cc8_irqs[] = {
	/* MCA irqs A register */
	[MCA_CC8_IRQ_RTC_ALARM] = {
                .reg_offset = MCA_IRQ_0_OFFSET,
                .mask = MCA_M_RTC_ALARM,
        },
	[MCA_CC8_IRQ_RTC_1HZ] = {
                .reg_offset = MCA_IRQ_0_OFFSET,
                .mask = MCA_M_RTC_1HZ,
        },
	[MCA_CC8_IRQ_WATCHDOG] = {
                .reg_offset = MCA_IRQ_0_OFFSET,
                .mask = MCA_M_WATCHDOG,
        },
	[MCA_CC8_IRQ_PWR_SLEEP] = {
                .reg_offset = MCA_IRQ_0_OFFSET,
                .mask = MCA_M_PWR_SLEEP,
        },
	[MCA_CC8_IRQ_PWR_OFF] = {
                .reg_offset = MCA_IRQ_0_OFFSET,
                .mask = MCA_M_PWR_OFF,
        },
	[MCA_CC8_IRQ_TAMPER0] = {
		.reg_offset = MCA_IRQ_0_OFFSET,
		.mask = MCA_M_TAMPER0,
	},
	[MCA_CC8_IRQ_TAMPER1] = {
		.reg_offset = MCA_IRQ_0_OFFSET,
		.mask = MCA_M_TAMPER1,
	},
	[MCA_CC8_IRQ_ADC] = {
		.reg_offset = MCA_IRQ_0_OFFSET,
		.mask = MCA_M_ADC,
	},
	[MCA_CC8_IRQ_GPIO_BANK_0] = {
		.reg_offset = MCA_IRQ_1_OFFSET,
		.mask = MCA_GPIO_BANK_0,
	},
	[MCA_CC8_IRQ_GPIO_BANK_1] = {
		.reg_offset = MCA_IRQ_1_OFFSET,
		.mask = MCA_GPIO_BANK_1,
	},
	[MCA_CC8_IRQ_GPIO_BANK_2] = {
		.reg_offset = MCA_IRQ_1_OFFSET,
		.mask = MCA_GPIO_BANK_2,
	},
	[MCA_CC8_IRQ_TAMPER2] = {
		.reg_offset = MCA_IRQ_2_OFFSET,
		.mask = MCA_M_TAMPER2,
	},
	[MCA_CC8_IRQ_TAMPER3] = {
		.reg_offset = MCA_IRQ_2_OFFSET,
		.mask = MCA_M_TAMPER3,
	},
	[MCA_CC8_IRQ_UART0] = {
		.reg_offset = MCA_IRQ_2_OFFSET,
		.mask = MCA_M_UART0,
	},
	[MCA_CC8_IRQ_UART1] = {
		.reg_offset = MCA_IRQ_2_OFFSET,
		.mask = MCA_M_UART1,
	},
	[MCA_CC8_IRQ_UART2] = {
		.reg_offset = MCA_IRQ_2_OFFSET,
		.mask = MCA_M_UART2,
	},
	[MCA_CC8_IRQ_RTC_PERIODIC_IRQ] = {
		.reg_offset = MCA_IRQ_2_OFFSET,
		.mask = MCA_M_RTC_PERIODIC_IRQ,
	},
};

static struct regmap_irq_chip mca_cc8_irq_chip = {
	.name		= "mca-cc8-irq",
	.irqs		= mca_cc8_irqs,
	.num_irqs	= ARRAY_SIZE(mca_cc8_irqs),
	.num_regs	= MCA_NUM_IRQ_REGS,
	.status_base	= MCA_IRQ_STATUS_0,
	.mask_base	= MCA_IRQ_MASK_0,
	.ack_base	= MCA_IRQ_STATUS_0,
	.init_ack_masked = true,
};

int mca_cc8_irq_init(struct mca_drv *mca)
{
	int ret;

	if (!mca->chip_irq) {
		dev_err(mca->dev, "No IRQ configured\n");
		return -EINVAL;
	}

	mca->irq_base = -1;

	if (of_machine_is_compatible("digi,ccimx8x")) {
		ret = mca_cc8x_add_irq_chip(mca->regmap, mca->chip_irq,
					    mca->irq_base, &mca_cc8_irq_chip,
					    &mca->regmap_irq);
	} else {
		ret = regmap_add_irq_chip(mca->regmap, mca->chip_irq,
					  IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED,
 					  mca->irq_base, &mca_cc8_irq_chip,
					  &mca->regmap_irq);
	}

	if (ret) {
		dev_err(mca->dev, "Failed to reguest IRQ %d: %d\n",
			mca->chip_irq, ret);
	}

	return ret;
}

void mca_cc8_irq_exit(struct mca_drv *mca)
{
	if (!mca->chip_irq) {
		if (of_machine_is_compatible("fsl,imx8qxp") ||
		    of_machine_is_compatible("fsl,imx8dxp"))
			mca_cc8x_del_irq_chip(mca->regmap_irq);
		else
			regmap_del_irq_chip(mca->chip_irq, mca->regmap_irq);
	}
}
