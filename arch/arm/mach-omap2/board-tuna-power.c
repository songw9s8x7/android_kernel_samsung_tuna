/* Power support for Samsung Tuna Board.
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/max17040_battery.h>
#include <linux/moduleparam.h>
#include <linux/pda_power.h>
#include <linux/platform_device.h>
#include <linux/i2c/twl6030-madc.h>
#include <linux/delay.h>

#include <plat/cpu.h>

#include "board-tuna.h"
#include "mux.h"
#include "pm.h"

/* These will be different on pre-lunchbox, lunchbox, and final */
#define GPIO_CHARGING_N		83
#define GPIO_TA_NCONNECTED	142
#define GPIO_CHARGE_N		13
#define GPIO_CHG_CUR_ADJ	102
#define GPIO_FUEL_ALERT		44

#define TPS62361_GPIO		7
#define ADC_NUM_SAMPLES		5
#define ADC_LIMIT_ERR_COUNT	5
#define ISET_ADC_CHANNEL	3
#define TEMP_ADC_CHANNEL	1

#define CHARGE_FULL_ADC		203

/**
** temp_adc_table_data
** @adc_value : thermistor adc value
** @temperature : temperature(C) * 10
**/
struct temp_adc_table_data {
	int adc_value;
	int temperature;
};

static DEFINE_SPINLOCK(charge_en_lock);
static int charger_state;

static struct temp_adc_table_data temper_table[] = {
	/* ADC, Temperature (C/10) */
	{ 75,	700     },
	{ 78,	690     },
	{ 82,	680     },
	{ 84,	670     },
	{ 87,	660     },
	{ 89,	650     },
	{ 92,	640     },
	{ 95,	630     },
	{ 99,	620     },
	{ 102,	610     },
	{ 105,	600     },
	{ 109,	590     },
	{ 113,	580     },
	{ 117,	570     },
	{ 121,	560     },
	{ 124,	550     },
	{ 127,	540     },
	{ 135,	530     },
	{ 139,	520     },
	{ 143,	510     },
	{ 147,	500     },
	{ 153,	490     },
	{ 158,	480     },
	{ 163,	470     },
	{ 169,	460     },
	{ 175,	450     },
	{ 181,	440     },
	{ 187,	430     },
	{ 193,	420     },
	{ 199,	410     },
	{ 205,	400     },
	{ 212,	390     },
	{ 218,	380     },
	{ 227,	370     },
	{ 233,	360     },
	{ 240,	350     },
	{ 249,	340     },
	{ 258,	330     },
	{ 267,	320     },
	{ 276,	310     },
	{ 285,	300     },
	{ 299,	290     },
	{ 308,	280     },
	{ 313,	270     },
	{ 322,	260     },
	{ 331,	250     },
	{ 342,	240     },
	{ 355,	230     },
	{ 363,	220     },
	{ 373,	210     },
	{ 383,	200     },
	{ 394,	190     },
	{ 407,	180     },
	{ 417,	170     },
	{ 427,	160     },
	{ 437,	150     },
	{ 450,	140     },
	{ 465,	130     },
	{ 475,	120     },
	{ 487,	110     },
	{ 500,	100     },
	{ 514,	90      },
	{ 526,	80      },
	{ 540,	70      },
	{ 552,	60      },
	{ 565,	50      },
	{ 577,	40      },
	{ 589,	30      },
	{ 603,	20      },
	{ 614,	10      },
	{ 628,	0       },
	{ 639,	(-10)   },
	{ 664,	(-20)   },
	{ 689,	(-30)   },
	{ 717,	(-40)   },
	{ 744,	(-50)   },
	{ 754,	(-60)   },
	{ 765,	(-70)   },
	{ 776,	(-80)   },
	{ 787,	(-90)   },
	{ 798,	(-100)  },
};

static bool enable_sr = true;
module_param(enable_sr, bool, S_IRUSR | S_IRGRP | S_IROTH);

static struct gpio charger_gpios[] = {
	{ .gpio = GPIO_CHARGING_N, .flags = GPIOF_IN, .label = "charging_n" },
	{ .gpio = GPIO_TA_NCONNECTED, .flags = GPIOF_IN, .label = "charger_n" },
	{ .gpio = GPIO_CHARGE_N, .flags = GPIOF_OUT_INIT_HIGH, .label = "charge_n" },
	{ .gpio = GPIO_CHG_CUR_ADJ, .flags = GPIOF_OUT_INIT_LOW, .label = "charge_cur_adj" },
};

static int twl6030_get_adc_data(int ch)
{
	int adc_data;
	int adc_max = -1;
	int adc_min = 1 << 11;
	int adc_total = 0;
	int i, j;

	for (i = 0; i < ADC_NUM_SAMPLES; i++) {
		adc_data = twl6030_get_madc_conversion(ch);
		if (adc_data == -EAGAIN) {
			for (j = 0; j < ADC_LIMIT_ERR_COUNT; j++) {
				msleep(20);
				adc_data = twl6030_get_madc_conversion(ch);
				if (adc_data > 0)
					break;
			}
			if (j >= ADC_LIMIT_ERR_COUNT) {
				pr_err("%s: Retry count exceeded[ch:%d]\n",
					__func__, ch);
				return adc_data;
			}
		} else if (adc_data < 0) {
			pr_err("%s: Failed read adc value : %d [ch:%d]\n",
				__func__, adc_data, ch);
			return adc_data;
		}

		if (adc_data > adc_max)
			adc_max = adc_data;
		if (adc_data < adc_min)
			adc_min = adc_data;

		adc_total += adc_data;
	}
	return (adc_total - adc_max - adc_min) / (ADC_NUM_SAMPLES - 2);
}

static int iset_adc_value(void)
{
	return twl6030_get_adc_data(ISET_ADC_CHANNEL);
}

static int temp_adc_value(void)
{
	return twl6030_get_adc_data(TEMP_ADC_CHANNEL);
}

static bool check_charge_full(void)
{
	int ret;

	ret = iset_adc_value();
	if (ret < 0) {
		pr_err("%s: invalid iset adc value [%d]\n",
			__func__, ret);
		return false;
	}
	pr_debug("%s : iset adc value : %d\n", __func__, ret);

	return ret < CHARGE_FULL_ADC;
}

static int get_bat_temp_by_adc(void)
{
	int array_size = ARRAY_SIZE(temper_table);
	int temp_adc = temp_adc_value();
	int mid;
	int left_side = 0;
	int right_side = array_size - 1;
	int temp = 0;

	if (temp_adc < 0) {
		pr_err("%s : Invalid temperature adc value [%d]\n",
			__func__, temp_adc);
		return temp_adc;
	}

	while (left_side <= right_side) {
		mid = (left_side + right_side) / 2;
		if (mid == 0 || mid == array_size - 1 ||
				(temper_table[mid].adc_value <= temp_adc &&
				 temper_table[mid+1].adc_value > temp_adc)) {
			temp = temper_table[mid].temperature;
			break;
		} else if (temp_adc - temper_table[mid].adc_value > 0) {
			left_side = mid + 1;
		} else {
			right_side = mid - 1;
		}
	}

	pr_debug("%s: temp adc : %d, temp : %d\n", __func__, temp_adc, temp);
	return temp;
}

static int charger_init(struct device *dev)
{
	return gpio_request_array(charger_gpios, ARRAY_SIZE(charger_gpios));
}

static void charger_exit(struct device *dev)
{
	gpio_free_array(charger_gpios, ARRAY_SIZE(charger_gpios));
}

static void set_charge_en(int state)
{
	gpio_set_value(GPIO_CHARGE_N, !state);
}

static void charger_set_charge(int state)
{
	gpio_set_value(GPIO_CHG_CUR_ADJ, !!(state & PDA_POWER_CHARGE_AC));
	spin_lock(&charge_en_lock);
	charger_state = state;
	set_charge_en(state);
	spin_unlock(&charge_en_lock);
}

static void charger_set_only_charge(int state)
{
	spin_lock(&charge_en_lock);
	if (charger_state)
		set_charge_en(state);
	spin_unlock(&charge_en_lock);
	/* CHG_ING_N level changed after set charge_en and 150ms */
	msleep(150);
}

static int charger_is_online(void)
{
	return !gpio_get_value(GPIO_TA_NCONNECTED);
}

static int charger_is_charging(void)
{
	return !gpio_get_value(GPIO_CHARGING_N);
}

static char *tuna_charger_supplied_to[] = {
	"battery",
};

static const __initdata struct pda_power_pdata charger_pdata = {
	.init = charger_init,
	.exit = charger_exit,
	.set_charge = charger_set_charge,
	.wait_for_status = 500,
	.wait_for_charger = 500,
	.supplied_to = tuna_charger_supplied_to,
	.num_supplicants = ARRAY_SIZE(tuna_charger_supplied_to),
	.use_otg_notifier = true,
};

static struct max17040_platform_data max17043_pdata = {
	.charger_online = charger_is_online,
	.charger_enable = charger_is_charging,
	.allow_charging = charger_set_only_charge,
	.skip_reset = true,
	.min_capacity = 3,
	.is_full_charge = check_charge_full,
	.get_bat_temp = get_bat_temp_by_adc,
	.high_block_temp = 500,
	.high_recover_temp = 420,
	.low_block_temp = (-50),
	.low_recover_temp = 0,
	.fully_charged_vol = 4150000,
	.recharge_vol = 4140000,
	.limit_charging_time = 21600,  /* 6 hours */
	.limit_recharging_time = 5400, /* 90 min */
};

static const __initdata struct i2c_board_info max17043_i2c[] = {
	{
		I2C_BOARD_INFO("max17040", (0x6C >> 1)),
		.platform_data = &max17043_pdata,
		.irq = OMAP_GPIO_IRQ(GPIO_FUEL_ALERT),
	}
};

void __init omap4_tuna_power_init(void)
{
	struct platform_device *pdev;
	int status;

	/* Vsel0 = gpio, vsel1 = gnd */
	status = omap_tps6236x_board_setup(true, TPS62361_GPIO, -1,
				OMAP_PIN_OFF_OUTPUT_HIGH, -1);
	if (status)
		pr_err("TPS62361 initialization failed: %d\n", status);
	/*
	 * Some Tuna devices have a 4430 chip on a 4460 board, manually
	 * tweak the power tree to the 4460 style with the TPS regulator.
	 */
	if (cpu_is_omap443x()) {
		/* Disable 4430 mapping */
		omap_twl_pmic_update("mpu", CHIP_IS_OMAP443X, 0x0);
		omap_twl_pmic_update("core", CHIP_IS_OMAP443X, 0x0);
		/* make 4460 map usable for 4430 */
		omap_twl_pmic_update("core", CHIP_IS_OMAP446X, CHIP_IS_OMAP443X);
		omap_tps6236x_update("mpu", CHIP_IS_OMAP446X, CHIP_IS_OMAP443X);
	}

	/* Update oscillator information */
	if (omap4_tuna_get_revision() <= 0x3) {
		/*
		 * until sample 4 (Toro and Maguro), we used KC2520B38:
		 * ST = 10ms
		 * Output Disable time = 100ns
		 * Output enable time = 5ms
		 * tstart = 10ms + 5ms = 15ms.
		 * tshut = 1us (rounded)
		 */
		omap_pm_set_osc_lp_time(15000, 1);
	} else {
		/*
		 * sample 5 onwards (Toro and Maguro), we use SQ200384:
		 * ST = 10ms
		 * Output Disable time = 100ns
		 * Output enable time = 10ms
		 * tstart = 10ms + 10ms = 20ms.
		 * tshut = 1us (rounded)
		 */
		omap_pm_set_osc_lp_time(20000, 1);
	}

	omap_mux_init_gpio(charger_gpios[0].gpio, OMAP_PIN_INPUT);
	omap_mux_init_gpio(charger_gpios[1].gpio, OMAP_PIN_INPUT);
	omap_mux_init_gpio(charger_gpios[2].gpio, OMAP_PIN_OUTPUT);
	omap_mux_init_gpio(charger_gpios[3].gpio, OMAP_PIN_OUTPUT);
	omap_mux_init_gpio(GPIO_FUEL_ALERT, OMAP_PIN_INPUT);

	pdev = platform_device_register_resndata(NULL, "pda-power", -1,
		NULL, 0, &charger_pdata, sizeof(charger_pdata));
	if (IS_ERR_OR_NULL(pdev))
		pr_err("cannot register pda-power\n");

	i2c_register_board_info(4, max17043_i2c, ARRAY_SIZE(max17043_i2c));

	if (enable_sr)
		omap_enable_smartreflex_on_init();
}