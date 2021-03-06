/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * Author: Mattias Wallin <mattias.wallin@stericsson.com> for ST-Ericsson.
 * License Terms: GNU General Public License v2
 */
/*
 * AB8500 register access
 * ======================
 *
 * read:
 * # echo BANK  >  <debugfs>/ab8500/register-bank
 * # echo ADDR  >  <debugfs>/ab8500/register-address
 * # cat <debugfs>/ab8500/register-value
 *
 * write:
 * # echo BANK  >  <debugfs>/ab8500/register-bank
 * # echo ADDR  >  <debugfs>/ab8500/register-address
 * # echo VALUE >  <debugfs>/ab8500/register-value
 *
 * read all registers from a bank:
 * # echo BANK  >  <debugfs>/ab8500/register-bank
 * # cat <debugfs>/ab8500/all-bank-register
 *
 * BANK   target AB8500 register bank
 * ADDR   target AB8500 register address
 * VALUE  decimal or 0x-prefixed hexadecimal
 *
 *
 * User Space notification on AB8500 IRQ
 * =====================================
 *
 * Allows user space entity to be notified when target AB8500 IRQ occurs.
 * When subscribed, a sysfs entry is created in ab8500.i2c platform device.
 * One can pool this file to get target IRQ occurence information.
 *
 * subscribe to an AB8500 IRQ:
 * # echo IRQ  >  <debugfs>/ab8500/irq-subscribe
 *
 * unsubscribe from an AB8500 IRQ:
 * # echo IRQ  >  <debugfs>/ab8500/irq-unsubscribe
 *
 *
 * AB8500 register formated read/write access
 * ==========================================
 *
 * Read:  read data, data>>SHIFT, data&=MASK, output data
 *        [0xABCDEF98] shift=12 mask=0xFFF => 0x00000CDE
 * Write: read data, data &= ~(MASK<<SHIFT), data |= (VALUE<<SHIFT), write data
 *        [0xABCDEF98] shift=12 mask=0xFFF value=0x123 => [0xAB123F98]
 *
 * Usage:
 * # echo "CMD [OPTIONS] BANK ADRESS [VALUE]" > $debugfs/ab8500/hwreg
 *
 * CMD      read      read access
 *          write     write access
 *
 * BANK     target reg bank
 * ADDRESS  target reg address
 * VALUE    (write) value to be updated
 *
 * OPTIONS
 *  -d|-dec            (read) output in decimal
 *  -h|-hexa           (read) output in 0x-hexa (default)
 *  -l|-w|-b           32bit (default), 16bit or 8bit reg access
 *  -m|-mask MASK      0x-hexa mask (default 0xFFFFFFFF)
 *  -s|-shift SHIFT    bit shift value (read:left, write:right)
 *  -o|-offset OFFSET  address offset to add to ADDRESS value
 *
 * Warning: bit shift operation is applied to bit-mask.
 * Warning: bit shift direction depends on read or right command.
 */

#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/slab.h>

#include <linux/mfd/abx500.h>
#include <linux/mfd/ab8500.h>
#include <linux/mfd/ab8500/ab8500-gpadc.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/string.h>
#include <linux/ctype.h>
#endif

static u32 debug_bank;
static u32 debug_address;

static int irq_first;
static int irq_last;
static u32 irq_count[AB8500_NR_IRQS];

static struct device_attribute *dev_attr[AB8500_NR_IRQS];
static char *event_name[AB8500_NR_IRQS];

/**
 * struct ab8500_reg_range
 * @first: the first address of the range
 * @last: the last address of the range
 * @perm: access permissions for the range
 */
struct ab8500_reg_range {
	u8 first;
	u8 last;
	u8 perm;
};

/**
 * struct ab8500_i2c_ranges
 * @num_ranges: the number of ranges in the list
 * @bankid: bank identifier
 * @range: the list of register ranges
 */
struct ab8500_i2c_ranges {
	u8 num_ranges;
	u8 bankid;
	const struct ab8500_reg_range *range;
};

/* hwreg- "mask" and "shift" entries ressources */
struct hwreg_cfg {
	u32  bank;      /* target bank */
	u32  addr;      /* target address */
	uint fmt;       /* format */
	uint mask;      /* read/write mask, applied before any bit shift */
	int  shift;     /* bit shift (read:right shift, write:left shift */
};
/* fmt bit #0: 0=hexa, 1=dec */
#define REG_FMT_DEC(c) ((c)->fmt & 0x1)
#define REG_FMT_HEX(c) (!REG_FMT_DEC(c))

static struct hwreg_cfg hwreg_cfg = {
	.addr = 0,			/* default: invalid phys addr */
	.fmt = 0,			/* default: 32bit access, hex output */
	.mask = 0xFFFFFFFF,	/* default: no mask */
	.shift = 0,			/* default: no bit shift */
};

#define AB8500_NAME_STRING "ab8500"
#define AB8500_ADC_NAME_STRING "gpadc"
#define AB8500_NUM_BANKS 22

#define AB8500_REV_REG 0x80

static struct ab8500_i2c_ranges debug_ranges[AB8500_NUM_BANKS] = {
	[0x0] = {
		.num_ranges = 0,
		.range = NULL,
	},
	[AB8500_SYS_CTRL1_BLOCK] = {
		.num_ranges = 3,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x02,
			},
			{
				.first = 0x42,
				.last = 0x42,
			},
			{
				.first = 0x80,
				.last = 0x81,
			},
		},
	},
	[AB8500_SYS_CTRL2_BLOCK] = {
		.num_ranges = 4,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x0D,
			},
			{
				.first = 0x0F,
				.last = 0x17,
			},
			{
				.first = 0x30,
				.last = 0x30,
			},
			{
				.first = 0x32,
				.last = 0x33,
			},
		},
	},
	[AB8500_REGU_CTRL1] = {
		.num_ranges = 3,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x00,
			},
			{
				.first = 0x03,
				.last = 0x10,
			},
			{
				.first = 0x80,
				.last = 0x84,
			},
		},
	},
	[AB8500_REGU_CTRL2] = {
		.num_ranges = 5,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x15,
			},
			{
				.first = 0x17,
				.last = 0x19,
			},
			{
				.first = 0x1B,
				.last = 0x1D,
			},
			{
				.first = 0x1F,
				.last = 0x22,
			},
			{
				.first = 0x40,
				.last = 0x44,
			},
			/* 0x80-0x8B is SIM registers and should
			 * not be accessed from here */
		},
	},
	[AB8500_USB] = {
		.num_ranges = 2,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x80,
				.last = 0x83,
			},
			{
				.first = 0x87,
				.last = 0x8A,
			},
		},
	},
	[AB8500_TVOUT] = {
		.num_ranges = 9,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x12,
			},
			{
				.first = 0x15,
				.last = 0x17,
			},
			{
				.first = 0x19,
				.last = 0x21,
			},
			{
				.first = 0x27,
				.last = 0x2C,
			},
			{
				.first = 0x41,
				.last = 0x41,
			},
			{
				.first = 0x45,
				.last = 0x5B,
			},
			{
				.first = 0x5D,
				.last = 0x5D,
			},
			{
				.first = 0x69,
				.last = 0x69,
			},
			{
				.first = 0x80,
				.last = 0x81,
			},
		},
	},
	[AB8500_DBI] = {
		.num_ranges = 0,
		.range = NULL,
	},
	[AB8500_ECI_AV_ACC] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x80,
				.last = 0x82,
			},
		},
	},
	[0x9] = {
		.num_ranges = 0,
		.range = NULL,
	},
	[AB8500_GPADC] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x08,
			},
		},
	},
	[AB8500_CHARGER] = {
		.num_ranges = 9,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x03,
			},
			{
				.first = 0x05,
				.last = 0x05,
			},
			{
				.first = 0x40,
				.last = 0x40,
			},
			{
				.first = 0x42,
				.last = 0x42,
			},
			{
				.first = 0x44,
				.last = 0x44,
			},
			{
				.first = 0x50,
				.last = 0x55,
			},
			{
				.first = 0x80,
				.last = 0x82,
			},
			{
				.first = 0xC0,
				.last = 0xC2,
			},
			{
				.first = 0xf5,
				.last = 0xf6,
			},
		},
	},
	[AB8500_GAS_GAUGE] = {
		.num_ranges = 3,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x00,
			},
			{
				.first = 0x07,
				.last = 0x0A,
			},
			{
				.first = 0x10,
				.last = 0x14,
			},
		},
	},
	[AB8500_AUDIO] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x6F,
			},
		},
	},
	[AB8500_INTERRUPT] = {
		.num_ranges = 0,
		.range = NULL,
	},
	[AB8500_RTC] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x0F,
			},
		},
	},
	[AB8500_MISC] = {
		.num_ranges = 8,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x05,
			},
			{
				.first = 0x10,
				.last = 0x15,
			},
			{
				.first = 0x20,
				.last = 0x25,
			},
			{
				.first = 0x30,
				.last = 0x35,
			},
			{
				.first = 0x40,
				.last = 0x45,
			},
			{
				.first = 0x50,
				.last = 0x50,
			},
			{
				.first = 0x60,
				.last = 0x67,
			},
			{
				.first = 0x80,
				.last = 0x80,
			},
		},
	},
	[AB8500_DEVELOPMENT] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x00,
				.last = 0x00,
			},
		},
	},
	[AB8500_DEBUG] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x05,
				.last = 0x07,
			},
		},
	},
	[0x13] = {
		.num_ranges = 0,
		.range = NULL,
	},
	[0x14] = {
		.num_ranges = 0,
		.range = NULL,
	},
	[AB8500_OTP_EMUL] = {
		.num_ranges = 1,
		.range = (struct ab8500_reg_range[]) {
			{
				.first = 0x01,
				.last = 0x0F,
			},
		},
	},
};

static irqreturn_t ab8500_debug_handler(int irq, void *data)
{
	char buf[16];
	struct kobject *kobj = (struct kobject *)data;
	unsigned int irq_abb = irq - irq_first;

	if (irq_abb < AB8500_NR_IRQS)
		irq_count[irq_abb]++;
	/*
	 * This makes it possible to use poll for events (POLLPRI | POLLERR)
	 * from userspace on sysfs file named <irq-nr>
	 */
	sprintf(buf, "%d", irq);
	sysfs_notify(kobj, NULL, buf);

	return IRQ_HANDLED;
}

static int ab8500_registers_print(struct seq_file *s, void *p)
{
	struct device *dev = s->private;
	unsigned int i;
	u32 bank = debug_bank;

	seq_printf(s, AB8500_NAME_STRING " register values:\n");

	seq_printf(s, " bank %u:\n", bank);
	for (i = 0; i < debug_ranges[bank].num_ranges; i++) {
		u32 reg;

		for (reg = debug_ranges[bank].range[i].first;
			reg <= debug_ranges[bank].range[i].last;
			reg++) {
			u8 value;
			int err;

			err = abx500_get_register_interruptible(dev,
				(u8)bank, (u8)reg, &value);
			if (err < 0) {
				dev_err(dev, "ab->read fail %d\n", err);
				return err;
			}

			err = seq_printf(s, "  [%u/0x%02X]: 0x%02X\n", bank,
				reg, value);
			if (err < 0) {
				dev_err(dev, "seq_printf overflow\n");
				/* Error is not returned here since
				 * the output is wanted in any case */
				return 0;
			}
		}
	}
	return 0;
}

static int ab8500_registers_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_registers_print, inode->i_private);
}

static const struct file_operations ab8500_registers_fops = {
	.open = ab8500_registers_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_bank_print(struct seq_file *s, void *p)
{
	return seq_printf(s, "%d\n", debug_bank);
}

static int ab8500_bank_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_bank_print, inode->i_private);
}

static ssize_t ab8500_bank_write(struct file *file,
	const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	struct device *dev = ((struct seq_file *)(file->private_data))->private;
	char buf[32];
	int buf_size;
	unsigned long user_bank;
	int err;

	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf) - 1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	err = strict_strtoul(buf, 0, &user_bank);
	if (err)
		return -EINVAL;

	if (user_bank >= AB8500_NUM_BANKS) {
		dev_err(dev, "debugfs error input > number of banks\n");
		return -EINVAL;
	}

	debug_bank = user_bank;

	return buf_size;
}

static int ab8500_address_print(struct seq_file *s, void *p)
{
	return seq_printf(s, "0x%02X\n", debug_address);
}

static int ab8500_address_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_address_print, inode->i_private);
}

static ssize_t ab8500_address_write(struct file *file,
	const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	struct device *dev = ((struct seq_file *)(file->private_data))->private;
	char buf[32];
	int buf_size;
	unsigned long user_address;
	int err;

	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf) - 1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	err = strict_strtoul(buf, 0, &user_address);
	if (err)
		return -EINVAL;
	if (user_address > 0xff) {
		dev_err(dev, "debugfs error input > 0xff\n");
		return -EINVAL;
	}
	debug_address = user_address;
	return buf_size;
}

static int ab8500_val_print(struct seq_file *s, void *p)
{
	struct device *dev = s->private;
	int ret;
	u8 regvalue;

	ret = abx500_get_register_interruptible(dev,
		(u8)debug_bank, (u8)debug_address, &regvalue);
	if (ret < 0) {
		dev_err(dev, "abx500_get_reg fail %d, %d\n",
			ret, __LINE__);
		return -EINVAL;
	}
	seq_printf(s, "0x%02X\n", regvalue);

	return 0;
}

static int ab8500_val_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_val_print, inode->i_private);
}

static ssize_t ab8500_val_write(struct file *file,
	const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	struct device *dev = ((struct seq_file *)(file->private_data))->private;
	char buf[32];
	int buf_size;
	unsigned long user_val;
	int err;

	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	err = strict_strtoul(buf, 0, &user_val);
	if (err)
		return -EINVAL;
	if (user_val > 0xff) {
		dev_err(dev, "debugfs error input > 0xff\n");
		return -EINVAL;
	}
	err = abx500_set_register_interruptible(dev,
		(u8)debug_bank, debug_address, (u8)user_val);
	if (err < 0) {
		printk(KERN_ERR "abx500_set_reg failed %d, %d", err, __LINE__);
		return -EINVAL;
	}
	return buf_size;
}

/*
 * - HWREG DB8500 formated routines
 */
static int ab8500_hwreg_print(struct seq_file *s, void *d)
{
	struct device *dev = s->private;
	int ret;
	u8 regvalue;

	ret = abx500_get_register_interruptible(dev,
		(u8)hwreg_cfg.bank, (u8)hwreg_cfg.addr, &regvalue);
	if (ret < 0) {
		dev_err(dev, "abx500_get_reg fail %d, %d\n",
			ret, __LINE__);
		return -EINVAL;
	}

	if (hwreg_cfg.shift >= 0)
		regvalue >>= hwreg_cfg.shift;
	else
		regvalue <<= -hwreg_cfg.shift;
	regvalue &= hwreg_cfg.mask;

	if (REG_FMT_DEC(&hwreg_cfg))
		seq_printf(s, "%d\n", regvalue);
	else
		seq_printf(s, "0x%02X\n", regvalue);
	return 0;
}

static int ab8500_hwreg_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_hwreg_print, inode->i_private);
}

static int ab8500_gpadc_bat_ctrl_print(struct seq_file *s, void *p)
{
	int bat_ctrl_raw;
	int bat_ctrl_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	bat_ctrl_raw = ab8500_gpadc_read_raw(gpadc, BAT_CTRL);
	bat_ctrl_convert = ab8500_gpadc_ad_to_voltage(gpadc,
			BAT_CTRL, bat_ctrl_raw);

	return seq_printf(s, "%d,0x%X\n",
			bat_ctrl_convert, bat_ctrl_raw);
}

static int ab8500_gpadc_bat_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_gpadc_bat_ctrl_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_bat_ctrl_fops = {
	.open = ab8500_gpadc_bat_ctrl_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_btemp_ball_print(struct seq_file *s, void *p)
{
	int btemp_ball_raw;
	int btemp_ball_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	btemp_ball_raw = ab8500_gpadc_read_raw(gpadc, BTEMP_BALL);
	btemp_ball_convert = ab8500_gpadc_ad_to_voltage(gpadc, BTEMP_BALL,
			btemp_ball_raw);

	return seq_printf(s,
			"%d,0x%X\n", btemp_ball_convert, btemp_ball_raw);
}

static int ab8500_gpadc_btemp_ball_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_btemp_ball_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_btemp_ball_fops = {
	.open = ab8500_gpadc_btemp_ball_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_main_charger_v_print(struct seq_file *s, void *p)
{
	int main_charger_v_raw;
	int main_charger_v_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	main_charger_v_raw = ab8500_gpadc_read_raw(gpadc, MAIN_CHARGER_V);
	main_charger_v_convert = ab8500_gpadc_ad_to_voltage(gpadc,
			MAIN_CHARGER_V, main_charger_v_raw);

	return seq_printf(s, "%d,0x%X\n",
			main_charger_v_convert, main_charger_v_raw);
}

static int ab8500_gpadc_main_charger_v_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_main_charger_v_print,
			inode->i_private);
}

static const struct file_operations ab8500_gpadc_main_charger_v_fops = {
	.open = ab8500_gpadc_main_charger_v_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_acc_detect1_print(struct seq_file *s, void *p)
{
	int acc_detect1_raw;
	int acc_detect1_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	acc_detect1_raw = ab8500_gpadc_read_raw(gpadc, ACC_DETECT1);
	acc_detect1_convert = ab8500_gpadc_ad_to_voltage(gpadc, ACC_DETECT1,
			acc_detect1_raw);

	return seq_printf(s, "%d,0x%X\n",
			acc_detect1_convert, acc_detect1_raw);
}

static int ab8500_gpadc_acc_detect1_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_acc_detect1_print,
			inode->i_private);
}

static const struct file_operations ab8500_gpadc_acc_detect1_fops = {
	.open = ab8500_gpadc_acc_detect1_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_acc_detect2_print(struct seq_file *s, void *p)
{
	int acc_detect2_raw;
	int acc_detect2_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	acc_detect2_raw = ab8500_gpadc_read_raw(gpadc, ACC_DETECT2);
	acc_detect2_convert = ab8500_gpadc_ad_to_voltage(gpadc,
	    ACC_DETECT2, acc_detect2_raw);

	return seq_printf(s, "%d,0x%X\n",
			acc_detect2_convert, acc_detect2_raw);
}

static int ab8500_gpadc_acc_detect2_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_acc_detect2_print,
	    inode->i_private);
}

static const struct file_operations ab8500_gpadc_acc_detect2_fops = {
	.open = ab8500_gpadc_acc_detect2_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_aux1_print(struct seq_file *s, void *p)
{
	int aux1_raw;
	int aux1_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	aux1_raw = ab8500_gpadc_read_raw(gpadc, ADC_AUX1);
	aux1_convert = ab8500_gpadc_ad_to_voltage(gpadc, ADC_AUX1,
			aux1_raw);

	return seq_printf(s, "%d,0x%X\n",
			aux1_convert, aux1_raw);
}

static int ab8500_gpadc_aux1_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_gpadc_aux1_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_aux1_fops = {
	.open = ab8500_gpadc_aux1_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_aux2_print(struct seq_file *s, void *p)
{
	int aux2_raw;
	int aux2_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	aux2_raw = ab8500_gpadc_read_raw(gpadc, ADC_AUX2);
	aux2_convert = ab8500_gpadc_ad_to_voltage(gpadc, ADC_AUX2,
			aux2_raw);

	return seq_printf(s, "%d,0x%X\n",
			aux2_convert, aux2_raw);
}

static int ab8500_gpadc_aux2_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_gpadc_aux2_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_aux2_fops = {
	.open = ab8500_gpadc_aux2_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_main_bat_v_print(struct seq_file *s, void *p)
{
	int main_bat_v_raw;
	int main_bat_v_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	main_bat_v_raw = ab8500_gpadc_read_raw(gpadc, MAIN_BAT_V);
	main_bat_v_convert = ab8500_gpadc_ad_to_voltage(gpadc, MAIN_BAT_V,
			main_bat_v_raw);

	return seq_printf(s, "%d,0x%X\n",
			main_bat_v_convert, main_bat_v_raw);
}

static int ab8500_gpadc_main_bat_v_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_main_bat_v_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_main_bat_v_fops = {
	.open = ab8500_gpadc_main_bat_v_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_vbus_v_print(struct seq_file *s, void *p)
{
	int vbus_v_raw;
	int vbus_v_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	vbus_v_raw = ab8500_gpadc_read_raw(gpadc, VBUS_V);
	vbus_v_convert = ab8500_gpadc_ad_to_voltage(gpadc, VBUS_V,
			vbus_v_raw);

	return seq_printf(s, "%d,0x%X\n",
			vbus_v_convert, vbus_v_raw);
}

static int ab8500_gpadc_vbus_v_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_gpadc_vbus_v_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_vbus_v_fops = {
	.open = ab8500_gpadc_vbus_v_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_main_charger_c_print(struct seq_file *s, void *p)
{
	int main_charger_c_raw;
	int main_charger_c_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	main_charger_c_raw = ab8500_gpadc_read_raw(gpadc, MAIN_CHARGER_C);
	main_charger_c_convert = ab8500_gpadc_ad_to_voltage(gpadc,
			MAIN_CHARGER_C, main_charger_c_raw);

	return seq_printf(s, "%d,0x%X\n",
			main_charger_c_convert, main_charger_c_raw);
}

static int ab8500_gpadc_main_charger_c_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_main_charger_c_print,
			inode->i_private);
}

static const struct file_operations ab8500_gpadc_main_charger_c_fops = {
	.open = ab8500_gpadc_main_charger_c_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_usb_charger_c_print(struct seq_file *s, void *p)
{
	int usb_charger_c_raw;
	int usb_charger_c_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	usb_charger_c_raw = ab8500_gpadc_read_raw(gpadc, USB_CHARGER_C);
	usb_charger_c_convert = ab8500_gpadc_ad_to_voltage(gpadc,
	    USB_CHARGER_C, usb_charger_c_raw);

	return seq_printf(s, "%d,0x%X\n",
			usb_charger_c_convert, usb_charger_c_raw);
}

static int ab8500_gpadc_usb_charger_c_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, ab8500_gpadc_usb_charger_c_print,
	    inode->i_private);
}

static const struct file_operations ab8500_gpadc_usb_charger_c_fops = {
	.open = ab8500_gpadc_usb_charger_c_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_bk_bat_v_print(struct seq_file *s, void *p)
{
	int bk_bat_v_raw;
	int bk_bat_v_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	bk_bat_v_raw = ab8500_gpadc_read_raw(gpadc, BK_BAT_V);
	bk_bat_v_convert = ab8500_gpadc_ad_to_voltage(gpadc,
			BK_BAT_V, bk_bat_v_raw);

	return seq_printf(s, "%d,0x%X\n",
			bk_bat_v_convert, bk_bat_v_raw);
}

static int ab8500_gpadc_bk_bat_v_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_gpadc_bk_bat_v_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_bk_bat_v_fops = {
	.open = ab8500_gpadc_bk_bat_v_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int ab8500_gpadc_die_temp_print(struct seq_file *s, void *p)
{
	int die_temp_raw;
	int die_temp_convert;
	struct ab8500_gpadc *gpadc;

	gpadc = ab8500_gpadc_get();
	die_temp_raw = ab8500_gpadc_read_raw(gpadc, DIE_TEMP);
	die_temp_convert = ab8500_gpadc_ad_to_voltage(gpadc, DIE_TEMP,
			die_temp_raw);

	return seq_printf(s, "%d,0x%X\n",
			die_temp_convert, die_temp_raw);
}

static int ab8500_gpadc_die_temp_open(struct inode *inode, struct file *file)
{
	return single_open(file, ab8500_gpadc_die_temp_print, inode->i_private);
}

static const struct file_operations ab8500_gpadc_die_temp_fops = {
	.open = ab8500_gpadc_die_temp_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

/*
 * return length of an ASCII numerical value, 0 is string is not a
 * numerical value.
 * string shall start at value 1st char.
 * string can be tailed with \0 or space or newline chars only.
 * value can be decimal or hexadecimal (prefixed 0x or 0X).
 */
static int strval_len(char *b)
{
	char *s = b;
	if ((*s == '0') && ((*(s+1) == 'x') || (*(s+1) == 'X'))) {
		s += 2;
		for (; *s && (*s != ' ') && (*s != '\n'); s++) {
			if (!isxdigit(*s))
				return 0;
		}
	} else {
		if (*s == '-')
			s++;
		for (; *s && (*s != ' ') && (*s != '\n'); s++) {
			if (!isdigit(*s))
				return 0;
		}
	}
	return (int) (s-b);
}

/*
 * parse hwreg input data.
 * update global hwreg_cfg only if input data syntax is ok.
 */
static ssize_t hwreg_common_write(char *b, struct hwreg_cfg *cfg,
		struct device *dev)
{
	uint write, val = 0;
	struct hwreg_cfg loc = {
		.bank = 0,          /* default: invalid phys addr */
		.addr = 0,          /* default: invalid phys addr */
		.fmt = 0,           /* default: 32bit access, hex output */
		.mask = 0xFFFFFFFF, /* default: no mask */
		.shift = 0,         /* default: no bit shift */
	};

	/* read or write ? */
	if (!strncmp(b, "read ", 5)) {
		write = 0;
		b += 5;
	} else if (!strncmp(b, "write ", 6)) {
		write = 1;
		b += 6;
	} else
		return -EINVAL;

	/* OPTIONS -l|-w|-b -s -m -o */
	while ((*b == ' ') || (*b == '-')) {
		if (*(b-1) != ' ') {
			b++;
			continue;
		}
		if ((!strncmp(b, "-d ", 3)) ||
				(!strncmp(b, "-dec ", 5))) {
			b += (*(b+2) == ' ') ? 3 : 5;
			loc.fmt |= (1<<0);
		} else if ((!strncmp(b, "-h ", 3)) ||
				(!strncmp(b, "-hex ", 5))) {
			b += (*(b+2) == ' ') ? 3 : 5;
			loc.fmt &= ~(1<<0);
		} else if ((!strncmp(b, "-m ", 3)) ||
				(!strncmp(b, "-mask ", 6))) {
			b += (*(b+2) == ' ') ? 3 : 6;
			if (strval_len(b) == 0)
				return -EINVAL;
			loc.mask = simple_strtoul(b, &b, 0);
		} else if ((!strncmp(b, "-s ", 3)) ||
				(!strncmp(b, "-shift ", 7))) {
			b += (*(b+2) == ' ') ? 3 : 7;
			if (strval_len(b) == 0)
				return -EINVAL;
			loc.shift = simple_strtol(b, &b, 0);
		} else {
			return -EINVAL;
		}
	}
	/* get arg BANK and ADDRESS */
	if (strval_len(b) == 0)
		return -EINVAL;
	loc.bank = simple_strtoul(b, &b, 0);
	while (*b == ' ')
		b++;
	if (strval_len(b) == 0)
		return -EINVAL;
	loc.addr = simple_strtoul(b, &b, 0);

	if (write) {
		while (*b == ' ')
			b++;
		if (strval_len(b) == 0)
			return -EINVAL;
		val = simple_strtoul(b, &b, 0);
	}

	/* args are ok, update target cfg (mainly for read) */
	*cfg = loc;

#ifdef ABB_HWREG_DEBUG
	pr_warn("HWREG request: %s, %s, addr=0x%08X, mask=0x%X, shift=%d"
		"value=0x%X\n", (write) ? "write" : "read",
		REG_FMT_DEC(cfg) ? "decimal" : "hexa",
		cfg->addr, cfg->mask, cfg->shift, val);
#endif

	if (write) {
		u8  regvalue;
		int ret = abx500_get_register_interruptible(dev,
			(u8)cfg->bank, (u8)cfg->addr, &regvalue);
		if (ret < 0) {
			dev_err(dev, "abx500_get_reg fail %d, %d\n",
				ret, __LINE__);
			return -EINVAL;
		}

		if (cfg->shift >= 0) {
			regvalue &= ~(cfg->mask << (cfg->shift));
			val = (val & cfg->mask) << (cfg->shift);
		} else {
			regvalue &= ~(cfg->mask >> (-cfg->shift));
			val = (val & cfg->mask) >> (-cfg->shift);
		}
		val = val | regvalue;

		ret = abx500_set_register_interruptible(dev,
			(u8)cfg->bank, (u8)cfg->addr, (u8)val);
		if (ret < 0) {
			pr_err("abx500_set_reg failed %d, %d", ret, __LINE__);
			return -EINVAL;
		}

	}
	return 0;
}

static ssize_t ab8500_hwreg_write(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct device *dev = ((struct seq_file *)(file->private_data))->private;
	char buf[128];
	int buf_size, ret;

	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	/* get args and process */
	ret = hwreg_common_write(buf, &hwreg_cfg, dev);
	return (ret) ? ret : buf_size;
}

/*
 * - irq subscribe/unsubscribe stuff
 */
static int ab8500_subscribe_unsubscribe_print(struct seq_file *s, void *p)
{
	seq_printf(s, "%d\n", irq_first);

	return 0;
}

static int ab8500_subscribe_unsubscribe_open(struct inode *inode,
	struct file *file)
{
	return single_open(file, ab8500_subscribe_unsubscribe_print,
		inode->i_private);
}

/*
 * Userspace should use poll() on this file. When an event occur
 * the blocking poll will be released.
 */
static ssize_t show_irq(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	unsigned long name;
	unsigned int irq_index;
	int err;

	err = strict_strtoul(attr->attr.name, 0, &name);
	if (err)
		return err;

	irq_index = name - irq_first;
	if (irq_index >= AB8500_NR_IRQS)
		return -EINVAL;
	else
		return sprintf(buf, "%u\n", irq_count[irq_index]);
}

static ssize_t ab8500_subscribe_write(struct file *file,
	const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	struct device *dev = ((struct seq_file *)(file->private_data))->private;
	char buf[32];
	int buf_size;
	unsigned long user_val;
	int err;
	unsigned int irq_index;

	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	err = strict_strtoul(buf, 0, &user_val);
	if (err)
		return -EINVAL;
	if (user_val < irq_first) {
		dev_err(dev, "debugfs error input < %d\n", irq_first);
		return -EINVAL;
	}
	if (user_val > irq_last) {
		dev_err(dev, "debugfs error input > %d\n", irq_last);
		return -EINVAL;
	}

	irq_index = user_val - irq_first;
	if (irq_index >= AB8500_NR_IRQS)
		return -EINVAL;

	/*
	 * This will create a sysfs file named <irq-nr> which userspace can
	 * use to select or poll and get the AB8500 events
	 */
	dev_attr[irq_index] = kmalloc(sizeof(struct device_attribute),
		GFP_KERNEL);
	event_name[irq_index] = kmalloc(buf_size, GFP_KERNEL);
	sprintf(event_name[irq_index], "%lu", user_val);
	dev_attr[irq_index]->show = show_irq;
	dev_attr[irq_index]->store = NULL;
	dev_attr[irq_index]->attr.name = event_name[irq_index];
	dev_attr[irq_index]->attr.mode = S_IRUGO;
	err = sysfs_create_file(&dev->kobj, &dev_attr[irq_index]->attr);
	if (err < 0) {
		printk(KERN_ERR "sysfs_create_file failed %d\n", err);
		return err;
	}

	err = request_threaded_irq(user_val, NULL, ab8500_debug_handler,
		IRQF_SHARED | IRQF_NO_SUSPEND, "ab8500-debug", &dev->kobj);
	if (err < 0) {
		printk(KERN_ERR "request_threaded_irq failed %d, %lu\n",
			err, user_val);
		sysfs_remove_file(&dev->kobj, &dev_attr[irq_index]->attr);
		return err;
	}

	return buf_size;
}

static ssize_t ab8500_unsubscribe_write(struct file *file,
	const char __user *user_buf,
	size_t count, loff_t *ppos)
{
	struct device *dev = ((struct seq_file *)(file->private_data))->private;
	char buf[32];
	int buf_size;
	unsigned long user_val;
	int err;
	unsigned int irq_index;

	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	err = strict_strtoul(buf, 0, &user_val);
	if (err)
		return -EINVAL;
	if (user_val < irq_first) {
		dev_err(dev, "debugfs error input < %d\n", irq_first);
		return -EINVAL;
	}
	if (user_val > irq_last) {
		dev_err(dev, "debugfs error input > %d\n", irq_last);
		return -EINVAL;
	}

	irq_index = user_val - irq_first;
	if (irq_index >= AB8500_NR_IRQS)
		return -EINVAL;

	/* Set irq count to 0 when unsubscribe */
	irq_count[irq_index] = 0;

	if (dev_attr[irq_index])
		sysfs_remove_file(&dev->kobj, &dev_attr[irq_index]->attr);


	free_irq(user_val, &dev->kobj);
	kfree(event_name[irq_index]);
	kfree(dev_attr[irq_index]);

	return buf_size;
}

/*
 * - several deubgfs nodes fops
 */

static const struct file_operations ab8500_bank_fops = {
	.open = ab8500_bank_open,
	.write = ab8500_bank_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static const struct file_operations ab8500_address_fops = {
	.open = ab8500_address_open,
	.write = ab8500_address_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static const struct file_operations ab8500_val_fops = {
	.open = ab8500_val_open,
	.write = ab8500_val_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static const struct file_operations ab8500_subscribe_fops = {
	.open = ab8500_subscribe_unsubscribe_open,
	.write = ab8500_subscribe_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static const struct file_operations ab8500_unsubscribe_fops = {
	.open = ab8500_subscribe_unsubscribe_open,
	.write = ab8500_unsubscribe_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static const struct file_operations ab8500_hwreg_fops = {
	.open = ab8500_hwreg_open,
	.write = ab8500_hwreg_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static struct dentry *ab8500_dir;
static struct dentry *ab8500_gpadc_dir;

static int __devinit ab8500_debug_probe(struct platform_device *plf)
{
	struct dentry *file;
	debug_bank = AB8500_MISC;
	debug_address = AB8500_REV_REG & 0x00FF;

	irq_first = platform_get_irq_byname(plf, "IRQ_FIRST");
	if (irq_first < 0) {
		dev_err(&plf->dev, "First irq not found, err %d\n",
				irq_first);
		return irq_first;
	}

	irq_last = platform_get_irq_byname(plf, "IRQ_LAST");
	if (irq_last < 0) {
		dev_err(&plf->dev, "Last irq not found, err %d\n",
				irq_last);
		return irq_last;
	}

	ab8500_dir = debugfs_create_dir(AB8500_NAME_STRING, NULL);
	if (!ab8500_dir)
		goto err;

	ab8500_gpadc_dir = debugfs_create_dir(AB8500_ADC_NAME_STRING,
	    ab8500_dir);
	if (!ab8500_gpadc_dir)
		goto err;

	file = debugfs_create_file("all-bank-registers", S_IRUGO,
	    ab8500_dir, &plf->dev, &ab8500_registers_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("register-bank", (S_IRUGO | S_IWUGO),
	    ab8500_dir, &plf->dev, &ab8500_bank_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("register-address", (S_IRUGO | S_IWUGO),
	    ab8500_dir, &plf->dev, &ab8500_address_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("register-value", (S_IRUGO | S_IWUGO),
	    ab8500_dir, &plf->dev, &ab8500_val_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("irq-subscribe", (S_IRUGO | S_IWUGO),
	    ab8500_dir, &plf->dev, &ab8500_subscribe_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("irq-unsubscribe", (S_IRUGO | S_IWUGO),
	    ab8500_dir, &plf->dev, &ab8500_unsubscribe_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("hwreg", (S_IRUGO | S_IWUGO),
	    ab8500_dir, &plf->dev, &ab8500_hwreg_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("bat_ctrl", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_bat_ctrl_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("btemp_ball", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_btemp_ball_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("main_charger_v", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_main_charger_v_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("acc_detect1", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_acc_detect1_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("acc_detect2", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_acc_detect2_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("adc_aux1", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_aux1_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("adc_aux2", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_aux2_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("main_bat_v", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_main_bat_v_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("vbus_v", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_vbus_v_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("main_charger_c", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_main_charger_c_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("usb_charger_c", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_usb_charger_c_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("bk_bat_v", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_bk_bat_v_fops);
	if (!file)
		goto err;

	file = debugfs_create_file("die_temp", (S_IRUGO | S_IWUGO),
	    ab8500_gpadc_dir, &plf->dev, &ab8500_gpadc_die_temp_fops);
	if (!file)
		goto err;

	return 0;

err:
	if (ab8500_dir)
		debugfs_remove_recursive(ab8500_dir);
	dev_err(&plf->dev, "failed to create debugfs entries.\n");
	return -ENOMEM;
}

static int __devexit ab8500_debug_remove(struct platform_device *plf)
{
	debugfs_remove_recursive(ab8500_dir);
	return 0;
}

static struct platform_driver ab8500_debug_driver = {
	.driver = {
		.name = "ab8500-debug",
		.owner = THIS_MODULE,
	},
	.probe  = ab8500_debug_probe,
	.remove = __devexit_p(ab8500_debug_remove)
};

static int __init ab8500_debug_init(void)
{
	return platform_driver_register(&ab8500_debug_driver);
}

static void __exit ab8500_debug_exit(void)
{
	platform_driver_unregister(&ab8500_debug_driver);
}
subsys_initcall(ab8500_debug_init);
module_exit(ab8500_debug_exit);

MODULE_AUTHOR("Mattias WALLIN <mattias.wallin@stericsson.com");
MODULE_DESCRIPTION("AB8500 DEBUG");
MODULE_LICENSE("GPL v2");
