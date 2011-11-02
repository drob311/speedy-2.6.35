/* drivers/i2c/chips/tps65200.c
 *
 * Copyright (C) 2009 HTC Corporation
 * Author: Josh Hsiao <Josh_Hsiao@htc.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <asm/mach-types.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/android_alarm.h>
#include <linux/tps65200.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/wakelock.h>

#define pr_tps_fmt(fmt) "[BATT][tps65200] " fmt
#define pr_tps_err_fmt(fmt) "[BATT][tps65200] err:" fmt
#define pr_tps_info(fmt, ...) \
	printk(KERN_INFO pr_tps_fmt(fmt), ##__VA_ARGS__)
#define pr_tps_err(fmt, ...) \
	printk(KERN_ERR pr_tps_err_fmt(fmt), ##__VA_ARGS__)

#define TPS65200_CHECK_INTERVAL (600)	/* 600 seconds*/

static struct alarm tps65200_check_alarm;
static struct workqueue_struct *tps65200_wq;
static struct work_struct tps65200_work;

static int chg_stat_int;
static unsigned int chg_stat_enabled;
static spinlock_t chg_stat_lock;

static struct tps65200_chg_int_data *chg_int_data;
static struct wake_lock chg_int_lock;
static LIST_HEAD(tps65200_chg_int_list);
static DEFINE_MUTEX(notify_lock);

static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

static int tps65200_initial = -1;
static int tps65200_low_chg;
static int tps65200_chager_check;

#ifdef CONFIG_SUPPORT_DQ_BATTERY
static int htc_is_dq_pass;
#endif

/**
 * Insmod parameters
 */
/* I2C_CLIENT_INSMOD_1(tps65200); */

static int tps65200_probe(struct i2c_client *client,
			const struct i2c_device_id *id);
#if 0
static int tps65200_detect(struct i2c_client *client, int kind,
			 struct i2c_board_info *info);
#endif
static int tps65200_remove(struct i2c_client *client);

/* Supersonic for Switch charger */
struct tps65200_i2c_client {
	struct i2c_client *client;
	u8 address;
	/* max numb of i2c_msg required is for read =2 */
	struct i2c_msg xfer_msg[2];
	/* To lock access to xfer_msg */
	struct mutex xfer_lock;
};
static struct tps65200_i2c_client tps65200_i2c_module;

static void tps65200_set_check_alarm(void)
{
	ktime_t interval;
	ktime_t next_alarm;

	interval = ktime_set(TPS65200_CHECK_INTERVAL, 0);
	next_alarm = ktime_add(alarm_get_elapsed_realtime(), interval);
	alarm_start_range(&tps65200_check_alarm, next_alarm, next_alarm);
}

/**
Function:tps65200_i2c_write
Target:	Write a byte to Switch charger
Timing:	TBD
INPUT: 	value-> write value
		reg  -> reg offset
		num-> number of byte to write
return :TRUE-->OK
		FALSE-->Fail
 */
static int tps65200_i2c_write(u8 *value, u8 reg, u8 num_bytes)
{
	int ret;
	struct tps65200_i2c_client *tps;
	struct i2c_msg *msg;

	tps = &tps65200_i2c_module;

	mutex_lock(&tps->xfer_lock);
	/*
	 * [MSG1]: fill the register address data
	 * fill the data Tx buffer
	 */
	msg = &tps->xfer_msg[0];
	msg->addr = tps->address;
	msg->len = num_bytes + 1;
	msg->flags = 0;
	msg->buf = value;
	/* over write the first byte of buffer with the register address */
	*value = reg;
	ret = i2c_transfer(tps->client->adapter, tps->xfer_msg, 1);
	mutex_unlock(&tps->xfer_lock);

	/* i2cTransfer returns num messages.translate it pls.. */
	if (ret >= 0)
		ret = 0;
	return ret;
}


/**
Function:tps65200_i2c_read
Target:	Read a byte from Switch charger
Timing:	TBD
INPUT: 	value-> store buffer
		reg  -> reg offset to read
		num-> number of byte to read
return :TRUE-->OK
		FALSE-->Fail
 */
static int tps65200_i2c_read(u8 *value, u8 reg, u8 num_bytes)
{
	int ret;
	u8 val;
	struct tps65200_i2c_client *tps;
	struct i2c_msg *msg;

	tps = &tps65200_i2c_module;

	mutex_lock(&tps->xfer_lock);
	/* [MSG1] fill the register address data */
	msg = &tps->xfer_msg[0];
	msg->addr = tps->address;
	msg->len = 1;
	msg->flags = 0; /* Read the register value */
	val = reg;
	msg->buf = &val;
	/* [MSG2] fill the data rx buffer */
	msg = &tps->xfer_msg[1];
	msg->addr = tps->address;
	msg->flags = I2C_M_RD;  /* Read the register value */
	msg->len = num_bytes;   /* only n bytes */
	msg->buf = value;
	ret = i2c_transfer(tps->client->adapter, tps->xfer_msg, 2);
	mutex_unlock(&tps->xfer_lock);

	/* i2cTransfer returns num messages.translate it pls.. */
	if (ret >= 0)
		ret = 0;
	return ret;
}


/**
Function:tps65200_i2c_write_byte
Target:	Write a byte from Switch charger
Timing:	TBD
INPUT: 	value-> store buffer
		reg  -> reg offset to read
return :TRUE-->OK
		FALSE-->Fail
 */
static int tps65200_i2c_write_byte(u8 value, u8 reg)
{
    /* 2 bytes offset 1 contains the data offset 0 is used by i2c_write */
	int result;
	u8 temp_buffer[2] = { 0 };
    /* offset 1 contains the data */
	temp_buffer[1] = value;
	result = tps65200_i2c_write(temp_buffer, reg, 1);
	if (result != 0)
		pr_tps_err("TPS65200 I2C write fail = %d\n", result);

    return result;
}

/**
Function:tps65200_i2c_read_byte
Target:	Read a byte from Switch charger
Timing:	TBD
INPUT: 	value-> store buffer
		reg  -> reg offset to read
return :TRUE-->OK
		FALSE-->Fail
 */
static int tps65200_i2c_read_byte(u8 *value, u8 reg)
{
	int result = 0;
	result = tps65200_i2c_read(value, reg, 1);
	if (result != 0)
		pr_tps_err("TPS65200 I2C read fail = %d\n", result);

	return result;
}

int tps_register_notifier(struct tps65200_chg_int_notifier *notifier)
{
	if (!notifier || !notifier->name || !notifier->func)
		return -EINVAL;

	mutex_lock(&notify_lock);
	list_add(&notifier->notifier_link,
		&tps65200_chg_int_list);
	mutex_unlock(&notify_lock);
	return 0;
}
EXPORT_SYMBOL(tps_register_notifier);

static void send_tps_chg_int_notify(int int_reg, int value)
{
	static struct tps65200_chg_int_notifier *notifier;

	mutex_lock(&notify_lock);
	list_for_each_entry(notifier,
		&tps65200_chg_int_list,
		notifier_link) {
		if (notifier->func != NULL)
			notifier->func(int_reg, value);
	}
	mutex_unlock(&notify_lock);
}

static int tps65200_set_chg_stat(unsigned int ctrl)
{
	unsigned long flags;
	if (!chg_stat_int)
		return -1;
	spin_lock_irqsave(&chg_stat_lock, flags);
	if (ctrl != chg_stat_enabled) {
		if (!chg_stat_enabled) {
			enable_irq(chg_stat_int);
			chg_stat_enabled = ctrl;
		} else {
			disable_irq_nosync(chg_stat_int);
			chg_stat_enabled = ctrl;
		}
	}
	spin_unlock_irqrestore(&chg_stat_lock, flags);

	return 0;
}


int tps_set_charger_ctrl(u32 ctl)
{
	int result = 0;
	u8 status;
	u8 regh;

	if (tps65200_initial < 0)
		return 0;

	switch (ctl) {
	case POWER_SUPPLY_DISABLE_CHARGE:
		pr_tps_info("Switch charger OFF\n");
		tps65200_i2c_write_byte(0x29, 0x01);
		tps65200_i2c_write_byte(0x28, 0x00);
		tps65200_set_chg_stat(0);
		if (tps65200_chager_check)
			/* cancel CHECK_CHG alarm */
			alarm_cancel(&tps65200_check_alarm);
		break;
	case POWER_SUPPLY_ENABLE_SLOW_CHARGE:
	case POWER_SUPPLY_ENABLE_WIRELESS_CHARGE:
		pr_tps_info("Switch charger ON (SLOW)\n");
		tps65200_i2c_write_byte(0x29, 0x01);
		tps65200_i2c_write_byte(0x2A, 0x00);
		/* set DPM regulation voltage to 4.44V */
		regh = 0x83;
#ifdef CONFIG_SUPPORT_DQ_BATTERY
		if (htc_is_dq_pass)
			/* set DPM regulation voltage to 4.6V */
			regh = 0x85;
#endif
		if (tps65200_low_chg)
			regh |= 0x08;	/* enable low charge curent */
		tps65200_i2c_write_byte(regh, 0x03);
		tps65200_i2c_read_byte(&regh, 0x03);
		pr_tps_info("Switch charger ON (SLOW): regh 0x03=%x\n", regh);
		tps65200_i2c_write_byte(0x63, 0x02);
		if (tps65200_chager_check)
			/* set alarm for CHECK_CHG */
			tps65200_set_check_alarm();
		tps65200_set_chg_stat(1);
		break;
	case POWER_SUPPLY_ENABLE_FAST_CHARGE:
		pr_tps_info("Switch charger ON (FAST)\n");
		tps65200_i2c_write_byte(0x29, 0x01);
		tps65200_i2c_write_byte(0x2A, 0x00);
		/* set DPM regulation voltage to 4.44V */
		regh = 0x83;
#ifdef CONFIG_SUPPORT_DQ_BATTERY
		if (htc_is_dq_pass)
			/* set DPM regulation voltage to 4.6V */
			regh = 0x85;
#endif
		if (tps65200_low_chg)
			regh |= 0x08;	/* enable low charge current */
		tps65200_i2c_write_byte(regh, 0x03);
		tps65200_i2c_write_byte(0xA3, 0x02);
		tps65200_i2c_read_byte(&regh, 0x01);
		pr_tps_info("Switch charger ON (FAST): regh 0x01=%x\n", regh);
		tps65200_i2c_read_byte(&regh, 0x00);
		pr_tps_info("Switch charger ON (FAST): regh 0x00=%x\n", regh);
		tps65200_i2c_read_byte(&regh, 0x03);
		pr_tps_info("Switch charger ON (FAST): regh 0x03=%x\n", regh);
		tps65200_i2c_read_byte(&regh, 0x02);
		pr_tps_info("Switch charger ON (FAST): regh 0x02=%x\n", regh);
		if (tps65200_chager_check)
			/* set alarm for CHECK_CHG */
			tps65200_set_check_alarm();
		tps65200_set_chg_stat(1);
		break;
	case POWER_SUPPLY_ENABLE_SLOW_HV_CHARGE:
		pr_tps_info("Switch charger ON (SLOW_HV)\n");
		tps65200_i2c_write_byte(0x29, 0x01);
		tps65200_i2c_write_byte(0x2A, 0x00);
		regh = 0x85;
		if (tps65200_low_chg)
			regh |= 0x08;
		tps65200_i2c_write_byte(regh, 0x03);
		tps65200_i2c_read_byte(&regh, 0x03);
		pr_tps_info("Switch charger ON (SLOW_HV): regh 0x03=%x\n",
			 regh);
		tps65200_i2c_write_byte(0x6A, 0x02);
		tps65200_i2c_read_byte(&regh, 0x02);
		pr_tps_info("Switch charger ON (SLOW_HV): regh 0x02=%x\n",
			 regh);
		if (tps65200_chager_check)
			/* set alarm for CHECK_CHG */
			tps65200_set_check_alarm();
		break;
	case POWER_SUPPLY_ENABLE_FAST_HV_CHARGE:
		pr_tps_info("charger ON (FAST_HV)\n");
		tps65200_i2c_write_byte(0x29, 0x01);
		tps65200_i2c_write_byte(0x2A, 0x00);
		regh = 0x85;
		if (tps65200_low_chg)
			regh |= 0x08;
		tps65200_i2c_write_byte(regh, 0x03);
		tps65200_i2c_write_byte(0xAA, 0x02);
		tps65200_i2c_read_byte(&regh, 0x01);
		pr_tps_info("Switch charger ON (FAST_HV): regh 0x01=%x\n",
			 regh);
		tps65200_i2c_read_byte(&regh, 0x00);
		pr_tps_info("Switch charger ON (FAST_HV): regh 0x00=%x\n",
			 regh);
		tps65200_i2c_read_byte(&regh, 0x03);
		pr_tps_info("Switch charger ON (FAST_HV): regh 0x03=%x\n",
			 regh);
		tps65200_i2c_read_byte(&regh, 0x02);
		pr_tps_info("Switch charger ON (FAST_HV): regh 0x02=%x\n",
			 regh);
		if (tps65200_chager_check)
			/* set alarm for CHECK_CHG */
			tps65200_set_check_alarm();
		break;
	case ENABLE_LIMITED_CHG:
		pr_tps_info("Switch charger on (LIMITED)\n");
		tps65200_i2c_write_byte(0x8B, 0x03);
		tps65200_low_chg = 1;
		tps65200_i2c_read_byte(&regh, 0x03);
		pr_tps_info("Switch charger ON (LIMITED): regh 0x03=%x\n", regh);
		break;
	case CLEAR_LIMITED_CHG:
		pr_tps_info("Switch charger off (LIMITED)\n");
		tps65200_i2c_write_byte(0x83, 0x03);
		tps65200_low_chg = 0;
		tps65200_i2c_read_byte(&regh, 0x03);
		pr_tps_info("Switch charger OFF (LIMITED): regh 0x03=%x\n", regh);
		break;
	case CHECK_CHG:
		pr_tps_info("Switch charger CHECK \n");
		tps65200_i2c_read_byte(&status, 0x06);
		pr_tps_info("TPS65200 STATUS_A%x\n", status);
		break;
	case SET_ICL500:
		pr_tps_info("Switch charger SET_ICL500 \n");
		tps65200_i2c_write_byte(0xA3, 0x02);
		break;
	case SET_ICL100:
		pr_tps_info("Switch charger SET_ICL100 \n");
		tps65200_i2c_write_byte(0x23, 0x02);
		break;
	case CHECK_INT1:
		tps65200_i2c_read_byte(&status, 0x08);
		pr_tps_info("Switch charger CHECK_INT1: regh 0x08h=%x\n", status);
		result = (int)status;
		break;
	case CHECK_INT2:
		tps65200_i2c_read_byte(&status, 0x09);
		pr_tps_info("TPS65200 INT2 %x\n", status);
		result = (int)status;
		break;
	case CHECK_CONTROL:
		pr_tps_info("Switch charger CHECK_CONTROL \n");
		tps65200_i2c_read_byte(&status, 0x00);
		pr_tps_info("TPS65200 status 0x00=%x\n", status);
		break;
	case OVERTEMP_VREG:
		pr_tps_info("Switch charger OVERTEMP_VREG_4060 \n");
		tps65200_i2c_read_byte(&regh, 0x02);
		regh = (regh & 0xC0) | 0x1C;
		tps65200_i2c_write_byte(regh, 0x02);
		tps65200_i2c_read_byte(&regh, 0x02);
		pr_tps_info("Switch charger OVERTEMP_VREG_4060: regh 0x02=%x\n", regh);
		break;
	case NORMALTEMP_VREG:
#ifdef CONFIG_SUPPORT_DQ_BATTERY
		tps65200_i2c_read_byte(&regh, 0x04);
		pr_tps_info("Switch charger CONFIG_D: regh 0x04=%x\n", regh);
		if (htc_is_dq_pass) {
			pr_tps_info("Switch charger NORMALTEMP_VREG_4340\n");
			tps65200_i2c_read_byte(&regh, 0x02);
			regh = (regh & 0xC0) | 0X2A;
			tps65200_i2c_write_byte(regh, 0x02);
			tps65200_i2c_read_byte(&regh, 0x02);
			pr_tps_info("Switch charger NORMALTEMP_VREG_4340: regh 0x02=%x\n", regh);
			break;
		}
#endif
		pr_tps_info("Switch charger NORMALTEMP_VREG_4200 \n");
		tps65200_i2c_read_byte(&regh, 0x02);
		regh = (regh & 0xC0) | 0X23;
		tps65200_i2c_write_byte(regh, 0x02);
		tps65200_i2c_read_byte(&regh, 0x02);
		pr_tps_info("Switch charger NORMALTEMP_VREG_4200: regh 0x02=%x\n", regh);
		break;
	case NORMALTEMP_VREG_HV:
		pr_tps_info("Switch charger NORMALTEMP_VREG_HV \n");
		tps65200_i2c_read_byte(&regh, 0x02);
		regh = (regh & 0xC0) | 0x2A;
		tps65200_i2c_write_byte(regh, 0x02);
		tps65200_i2c_read_byte(&regh, 0x02);
		pr_tps_info("Switch charger NORMALTEMP_VREG_4200: regh 0x02=%x\n", regh);
		break;
	default:
		pr_tps_info("%s: Not supported battery ctr called.!", __func__);
		result = -EINVAL;
		break;
	}

	return result;
}
EXPORT_SYMBOL(tps_set_charger_ctrl);

#if 0
static int tps65200_detect(struct i2c_client *client, int kind,
			 struct i2c_board_info *info)
{
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA |
				     I2C_FUNC_SMBUS_BYTE))
		return -ENODEV;

	strlcpy(info->type, "tps65200", I2C_NAME_SIZE);

	return 0;
}
#endif

static irqreturn_t chg_stat_handler(int irq, void *data)
{
	pr_tps_info("interrupt chg_stat is triggered.\n");

	tps65200_set_chg_stat(0);

	tps65200_i2c_write_byte(0x29, 0x01);
	tps65200_i2c_write_byte(0x28, 0x00);

	return IRQ_HANDLED;
}

static irqreturn_t chg_int_handler(int irq, void *data)
{
	pr_tps_info("interrupt chg_int is triggered.\n");

	disable_irq_nosync(chg_int_data->gpio_chg_int);
	chg_int_data->tps65200_reg = 0;
	schedule_delayed_work(&chg_int_data->int_work, msecs_to_jiffies(200));
	wake_lock(&chg_int_lock);
	return IRQ_HANDLED;
}

static void tps65200_int_func(struct work_struct *work)
{
	int fault_bit;

	switch (chg_int_data->tps65200_reg) {
	case CHECK_INT1:
		/*  read twice. First read to trigger TPS65200 clear fault bit
		    on INT1. Second read to make sure that fault bit is cleared
		    and call off ovp function.	*/
		fault_bit = tps_set_charger_ctrl(CHECK_INT1);
		fault_bit = tps_set_charger_ctrl(CHECK_INT1);

		if (fault_bit & 0x40) {
			send_tps_chg_int_notify(CHECK_INT1, 1);
			schedule_delayed_work(&chg_int_data->int_work,
						msecs_to_jiffies(5000));
			pr_tps_info("over voltage fault bit "
				"on TPS65200 is raised: %x\n", fault_bit);

		} else {
			send_tps_chg_int_notify(CHECK_INT1, 0);
			wake_unlock(&chg_int_lock);
			cancel_delayed_work(&chg_int_data->int_work);
			enable_irq(chg_int_data->gpio_chg_int);
		}
		break;
	default:
		fault_bit = tps_set_charger_ctrl(CHECK_INT2);
		pr_tps_info("Read register INT2 value: %x\n", fault_bit);
		if (fault_bit & 0x80) {
			fault_bit = tps_set_charger_ctrl(CHECK_INT2);
			fault_bit = tps_set_charger_ctrl(CHECK_INT2);
			send_tps_chg_int_notify(CHECK_INT2, 1);
			wake_lock_timeout(&chg_int_lock, 5 * HZ);
			cancel_delayed_work(&chg_int_data->int_work);
			enable_irq(chg_int_data->gpio_chg_int);
		} else {
			fault_bit = tps_set_charger_ctrl(CHECK_INT1);
			if (fault_bit & 0x40) {
				chg_int_data->tps65200_reg = CHECK_INT1;
				schedule_delayed_work(&chg_int_data->int_work,
							msecs_to_jiffies(200));
			} else {
				pr_tps_err("CHG_INT should not be triggered "
					"without fault bit!\n");
				wake_unlock(&chg_int_lock);
				enable_irq(chg_int_data->gpio_chg_int);
			}
		}
	}
}

static void tps65200_check_alarm_handler(struct alarm *alarm)
{
	queue_work(tps65200_wq, &tps65200_work);
}

static void tps65200_work_func(struct work_struct *work)
{
       tps_set_charger_ctrl(CHECK_CHG);
       tps65200_set_check_alarm();
}

static int tps65200_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int rc = 0;
	struct tps65200_i2c_client   *data = &tps65200_i2c_module;
	struct tps65200_platform_data *pdata =
					client->dev.platform_data;

	if (i2c_check_functionality(client->adapter, I2C_FUNC_I2C) == 0) {
		pr_tps_err("[TPS65200]:I2C fail\n");
		return -EIO;
		}

	if (pdata->charger_check) {
		tps65200_chager_check = 1;
		pr_tps_info("for battery driver 8x60.\n");
		INIT_WORK(&tps65200_work, tps65200_work_func);
		tps65200_wq = create_singlethread_workqueue("tps65200");
		alarm_init(&tps65200_check_alarm,
			ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
			tps65200_check_alarm_handler);
	}

#ifdef CONFIG_SUPPORT_DQ_BATTERY
	htc_is_dq_pass = pdata->dq_result;
#endif
	/*  For chg_stat interrupt initialization. */
	chg_stat_int = 0;
	if (pdata->gpio_chg_stat > 0) {
		set_irq_flags(pdata->gpio_chg_stat,
					IRQF_VALID | IRQF_NOAUTOEN);
		rc = request_threaded_irq(pdata->gpio_chg_stat,
					NULL,
					chg_stat_handler,
					IRQF_TRIGGER_RISING,
					"chg_stat", NULL);

		if (rc)
			pr_tps_err("request chg_stat irq failed!\n");
		else
			chg_stat_int = pdata->gpio_chg_stat;
	}

	/*  For chg_int interrupt initialization. */
	if (pdata->gpio_chg_int > 0) {
		chg_int_data = (struct tps65200_chg_int_data *)
				kmalloc(sizeof(struct tps65200_chg_int_data),
					GFP_KERNEL);
		if (!chg_int_data) {
			pr_tps_err("No memory for chg_int_data!\n");
			return -1;
		}

		chg_int_data->gpio_chg_int = 0;
		wake_lock_init(&chg_int_lock, WAKE_LOCK_SUSPEND, "tps65200");
		INIT_DELAYED_WORK(&chg_int_data->int_work,
				tps65200_int_func);

		rc = request_threaded_irq(
				pdata->gpio_chg_int,
				NULL,
				chg_int_handler,
				IRQF_TRIGGER_FALLING,
				"chg_int", NULL);
		if (rc)
			pr_tps_err("request chg_int irq failed!\n");
		else {
			pr_tps_info("init chg_int interrupt.\n");
			chg_int_data->gpio_chg_int =
				pdata->gpio_chg_int;
		}
	}

	data->address = client->addr;
	data->client = client;
	mutex_init(&data->xfer_lock);
	tps65200_initial = 1;
	pr_tps_info("[TPS65200]: Driver registration done\n");
	return 0;
}

static int tps65200_remove(struct i2c_client *client)
{
	struct tps65200_i2c_client   *data = i2c_get_clientdata(client);
	if (data->client && data->client != client)
		i2c_unregister_device(data->client);
	tps65200_i2c_module.client = NULL;
	return 0;
}

static void tps65200_shutdown(struct i2c_client *client)
{
	u8 regh;

	pr_tps_info("TPS65200 shutdown\n");
	tps65200_i2c_read_byte(&regh, 0x00);
	/* disable shunt monitor to decrease 0.035mA of current */
	regh &= 0xDF;
	tps65200_i2c_write_byte(regh, 0x00);
}

static const struct i2c_device_id tps65200_id[] = {
	{ "tps65200", 0 },
	{  },
};
static struct i2c_driver tps65200_driver = {
	.driver.name    = "tps65200",
	.id_table   = tps65200_id,
	.probe      = tps65200_probe,
	.remove     = tps65200_remove,
	.shutdown   = tps65200_shutdown,
};

static int __init sensors_tps65200_init(void)
{
	int res;

	tps65200_low_chg = 0;
	tps65200_chager_check = 0;
	chg_stat_enabled = 0;
	spin_lock_init(&chg_stat_lock);
	res = i2c_add_driver(&tps65200_driver);
	if (res)
		pr_tps_err("[TPS65200]: Driver registration failed \n");

	return res;
}

static void __exit sensors_tps65200_exit(void)
{
	kfree(chg_int_data);
	i2c_del_driver(&tps65200_driver);
}

MODULE_AUTHOR("Josh Hsiao <Josh_Hsiao@htc.com>");
MODULE_DESCRIPTION("tps65200 driver");
MODULE_LICENSE("GPL");

fs_initcall(sensors_tps65200_init);
module_exit(sensors_tps65200_exit);
