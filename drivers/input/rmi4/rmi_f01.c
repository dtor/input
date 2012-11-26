/*
 * Copyright (c) 2011-2012 Synaptics Incorporated
 * Copyright (c) 2011 Unixphere
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/kconfig.h>
#include <linux/rmi.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "rmi_driver.h"
#include "rmi_f01.h"

#define FUNCTION_NUMBER 0x01

/**
 * @reset - set this bit to force a firmware reset of the sensor.
 */
struct f01_device_commands {
	u8 reset:1;
	u8 reserved:7;
};

/**
 * @ctrl0 - see documentation in rmi_f01.h.
 * @interrupt_enable - A mask of per-function interrupts on the touch sensor.
 * @doze_interval - controls the interval between checks for finger presence
 * when the touch sensor is in doze mode, in units of 10ms.
 * @wakeup_threshold - controls the capacitance threshold at which the touch
 * sensor will decide to wake up from that low power state.
 * @doze_holdoff - controls how long the touch sensor waits after the last
 * finger lifts before entering the doze state, in units of 100ms.
 */
struct f01_device_control {
	struct f01_device_control_0 ctrl0;
	u8 *interrupt_enable;
	u8 doze_interval;
	u8 wakeup_threshold;
	u8 doze_holdoff;
};

/**
 * @has_ds4_queries - if true, the query registers relating to Design Studio 4
 * features are present.
 * @has_multi_phy - if true, multiple physical communications interfaces are
 * supported.
 * @has_guest - if true, a "guest" device is supported.
 */
struct f01_query_42 {
	u8 has_ds4_queries:1;
	u8 has_multi_phy:1;
	u8 has_guest:1;
	u8 reserved:5;
} __attribute__((__packed__));

/**
 * @length - the length of the remaining Query43.* register block, not
 * including the first register.
 * @has_package_id_query -  the package ID query data will be accessible from
 * inside the ProductID query registers.
 * @has_packrat_query -  the packrat query data will be accessible from inside
 * the ProductID query registers.
 * @has_reset_query - the reset pin related registers are valid.
 * @has_maskrev_query - the silicon mask revision number will be reported.
 * @has_i2c_control - the register F01_RMI_Ctrl6 will exist.
 * @has_spi_control - the register F01_RMI_Ctrl7 will exist.
 * @has_attn_control - the register F01_RMI_Ctrl8 will exist.
 * @reset_enabled - the hardware reset pin functionality has been enabled
 * for this device.
 * @reset_polarity - If this bit reports as ‘0’, it means that the reset state
 * is active low. A ‘1’ means that the reset state is active high.
 * @pullup_enabled - If set, it indicates that a built-in weak pull up has
 * been enabled on the Reset pin; clear means that no pull-up is present.
 * @reset_pin_number - This field represents which GPIO pin number has been
 * assigned the reset functionality.
 */
struct f01_ds4_queries {
	u8 length:4;
	u8 reserved_1:4;

	u8 has_package_id_query:1;
	u8 has_packrat_query:1;
	u8 has_reset_query:1;
	u8 has_maskrev_query:1;
	u8 reserved_2:4;

	u8 has_i2c_control:1;
	u8 has_spi_control:1;
	u8 has_attn_control:1;
	u8 reserved_3:5;

	u8 reset_enabled:1;
	u8 reset_polarity:1;
	u8 pullup_enabled:1;
	u8 reserved_4:1;
	u8 reset_pin_number:4;
} __attribute__((__packed__));

struct f01_data {
	struct f01_device_control device_control;
	struct f01_basic_queries basic_queries;
	struct f01_device_status device_status;
	u8 product_id[RMI_PRODUCT_ID_LENGTH + 1];

	u16 interrupt_enable_addr;
	u16 doze_interval_addr;
	u16 wakeup_threshold_addr;
	u16 doze_holdoff_addr;

	int irq_count;
	int num_of_irq_regs;

#ifdef CONFIG_PM
	bool suspended;
	bool old_nosleep;
#endif

#ifdef CONFIG_RMI4_DEBUG
	struct dentry *debugfs_interrupt_enable;
#endif
};

#ifdef CONFIG_RMI4_DEBUG
struct f01_debugfs_data {
	bool done;
	struct rmi_function *fn;
};

static int f01_debug_open(struct inode *inodep, struct file *filp)
{
	struct f01_debugfs_data *data;
	struct rmi_function *fn = inodep->i_private;

	data = kzalloc(sizeof(struct f01_debugfs_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->fn = fn;
	filp->private_data = data;
	return 0;
}

static int f01_debug_release(struct inode *inodep, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static ssize_t interrupt_enable_read(struct file *filp, char __user *buffer,
				     size_t size, loff_t *offset) {
	int i;
	int len;
	int total_len = 0;
	char local_buf[size];
	char *current_buf = local_buf;
	struct f01_debugfs_data *data = filp->private_data;
	struct f01_data *f01 = data->fn->data;

	if (data->done)
		return 0;

	data->done = 1;

	/* loop through each irq value and copy its
	 * string representation into buf */
	for (i = 0; i < f01->irq_count; i++) {
		int irq_reg;
		int irq_shift;
		int interrupt_enable;

		irq_reg = i / 8;
		irq_shift = i % 8;
		interrupt_enable =
		    ((f01->device_control.interrupt_enable[irq_reg]
			>> irq_shift) & 0x01);

		/* get next irq value and write it to buf */
		len = snprintf(current_buf, size - total_len,
			"%u ", interrupt_enable);
		/* bump up ptr to next location in buf if the
		 * snprintf was valid.  Otherwise issue an error
		 * and return. */
		if (len > 0) {
			current_buf += len;
			total_len += len;
		} else {
			dev_err(&data->fn->dev, "Failed to build interrupt_enable buffer, code = %d.\n",
						len);
			return snprintf(local_buf, size, "unknown\n");
		}
	}
	len = snprintf(current_buf, size - total_len, "\n");
	if (len > 0)
		total_len += len;
	else
		dev_warn(&data->fn->dev, "%s: Failed to append carriage return.\n",
			 __func__);

	if (copy_to_user(buffer, local_buf, total_len))
		return -EFAULT;

	return total_len;
}

static ssize_t interrupt_enable_write(struct file *filp,
		const char __user *buffer, size_t size, loff_t *offset) {
	int retval;
	char buf[size];
	char *local_buf = buf;
	int i;
	int irq_count = 0;
	int irq_reg = 0;
	struct f01_debugfs_data *data = filp->private_data;
	struct f01_data *f01 = data->fn->data;

	retval = copy_from_user(buf, buffer, size);
	if (retval)
		return -EFAULT;

	for (i = 0; i < f01->irq_count && *local_buf != 0;
	     i++, local_buf += 2) {
		int irq_shift;
		int interrupt_enable;
		int result;

		irq_reg = i / 8;
		irq_shift = i % 8;

		/* get next interrupt mapping value and store and bump up to
		 * point to next item in local_buf */
		result = sscanf(local_buf, "%u", &interrupt_enable);
		if ((result != 1) ||
			(interrupt_enable != 0 && interrupt_enable != 1)) {
			dev_err(&data->fn->dev, "Interrupt enable[%d] is not a valid value 0x%x.\n",
				i, interrupt_enable);
			return -EINVAL;
		}
		if (interrupt_enable == 0) {
			f01->device_control.interrupt_enable[irq_reg] &=
				(1 << irq_shift) ^ 0xFF;
		} else
			f01->device_control.interrupt_enable[irq_reg] |=
				(1 << irq_shift);
		irq_count++;
	}

	/* Make sure the irq count matches */
	if (irq_count != f01->irq_count) {
		dev_err(&data->fn->dev, "Interrupt enable count of %d doesn't match device count of %d.\n",
			 irq_count, f01->irq_count);
		return -EINVAL;
	}

	/* write back to the control register */
	retval = rmi_write_block(data->fn->rmi_dev, f01->interrupt_enable_addr,
			f01->device_control.interrupt_enable,
			f01->num_of_irq_regs);
	if (retval < 0) {
		dev_err(&data->fn->dev, "Could not write interrupt_enable mask to %#06x\n",
			f01->interrupt_enable_addr);
		return retval;
	}

	return size;
}

static const struct file_operations interrupt_enable_fops = {
	.owner = THIS_MODULE,
	.open = f01_debug_open,
	.release = f01_debug_release,
	.read = interrupt_enable_read,
	.write = interrupt_enable_write,
};

static int setup_debugfs(struct rmi_function *fn)
{
	struct f01_data *data = fn->data;

	if (!fn->debugfs_root)
		return -ENODEV;

	data->debugfs_interrupt_enable = debugfs_create_file("interrupt_enable",
		RMI_RW_ATTR, fn->debugfs_root, fn, &interrupt_enable_fops);
	if (!data->debugfs_interrupt_enable)
		dev_warn(&fn->dev,
			 "Failed to create debugfs interrupt_enable.\n");

	return 0;
}

static void teardown_debugfs(struct f01_data *f01)
{
	if (f01->debugfs_interrupt_enable)
		debugfs_remove(f01->debugfs_interrupt_enable);
}

#else

static inline int setup_debugfs(struct rmi_function *fn)
{
	return 0;
}

static inline void teardown_debugfs(struct f01_data *f01)
{
}

#endif

static ssize_t rmi_fn_01_productinfo_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "0x%02x 0x%02x\n",
			data->basic_queries.productinfo_1,
			data->basic_queries.productinfo_2);
}

static ssize_t rmi_fn_01_productid_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%s\n", data->product_id);
}

static ssize_t rmi_fn_01_manufacturer_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n",
			data->basic_queries.manufacturer_id);
}

static ssize_t rmi_fn_01_datecode_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "20%02u-%02u-%02u\n",
			data->basic_queries.year,
			data->basic_queries.month,
			data->basic_queries.day);
}

static ssize_t rmi_fn_01_reset_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	unsigned int reset;
	int retval = 0;


	if (sscanf(buf, "%u", &reset) != 1)
		return -EINVAL;
	if (reset < 0 || reset > 1)
		return -EINVAL;

	/* Per spec, 0 has no effect, so we skip it entirely. */
	if (reset) {
		/* Command register always reads as 0, so just use a local. */
		struct f01_device_commands commands = {
			.reset = 1
		};
		retval = rmi_write_block(fn->rmi_dev, fn->fd.command_base_addr,
				&commands, sizeof(commands));
		if (retval < 0) {
			dev_err(dev, "Failed to issue reset command, code = %d.",
						retval);
			return retval;
		}
	}

	return count;
}

static ssize_t rmi_fn_01_sleepmode_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE,
			"%d\n", data->device_control.ctrl0.sleep_mode);
}

static ssize_t rmi_fn_01_sleepmode_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || !RMI_IS_VALID_SLEEPMODE(new_value)) {
		dev_err(dev, "%s: Invalid sleep mode %s.", __func__, buf);
		return -EINVAL;
	}

	dev_dbg(dev, "Setting sleep mode to %ld.", new_value);
	data->device_control.ctrl0.sleep_mode = new_value;
	retval = rmi_write_block(fn->rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write sleep mode, code %d.\n", retval);
	return retval;
}

static ssize_t rmi_fn_01_nosleep_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.ctrl0.nosleep);
}

static ssize_t rmi_fn_01_nosleep_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || new_value > 1) {
		dev_err(dev, "%s: Invalid nosleep bit %s.", __func__, buf);
		return -EINVAL;
	}

	data->device_control.ctrl0.nosleep = new_value;
	retval = rmi_write_block(fn->rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write nosleep bit.\n");

	return retval;
}

static ssize_t rmi_fn_01_chargerinput_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.ctrl0.charger_input);
}

static ssize_t rmi_fn_01_chargerinput_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || new_value > 1) {
		dev_err(dev, "%s: Invalid chargerinput bit %s.", __func__, buf);
		return -EINVAL;
	}

	data->device_control.ctrl0.charger_input = new_value;
	retval = rmi_write_block(fn->rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write chargerinput bit.\n");

	return retval;
}

static ssize_t rmi_fn_01_reportrate_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.ctrl0.report_rate);
}

static ssize_t rmi_fn_01_reportrate_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || new_value > 1) {
		dev_err(dev, "%s: Invalid reportrate bit %s.", __func__, buf);
		return -EINVAL;
	}

	data->device_control.ctrl0.report_rate = new_value;
	retval = rmi_write_block(fn->rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write reportrate bit.\n");

	return retval;
}

static ssize_t rmi_fn_01_interrupt_enable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	int i, len, total_len = 0;
	char *current_buf = buf;

	/* loop through each irq value and copy its
	 * string representation into buf */
	for (i = 0; i < data->irq_count; i++) {
		int irq_reg;
		int irq_shift;
		int interrupt_enable;

		irq_reg = i / 8;
		irq_shift = i % 8;
		interrupt_enable =
		    ((data->device_control.interrupt_enable[irq_reg]
			>> irq_shift) & 0x01);

		/* get next irq value and write it to buf */
		len = snprintf(current_buf, PAGE_SIZE - total_len,
			"%u ", interrupt_enable);
		/* bump up ptr to next location in buf if the
		 * snprintf was valid.  Otherwise issue an error
		 * and return. */
		if (len > 0) {
			current_buf += len;
			total_len += len;
		} else {
			dev_err(dev, "Failed to build interrupt_enable buffer, code = %d.\n",
						len);
			return snprintf(buf, PAGE_SIZE, "unknown\n");
		}
	}
	len = snprintf(current_buf, PAGE_SIZE - total_len, "\n");
	if (len > 0)
		total_len += len;
	else
		dev_warn(dev, "%s: Failed to append carriage return.\n",
			 __func__);
	return total_len;

}

static ssize_t rmi_fn_01_doze_interval_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.doze_interval);

}

static ssize_t rmi_fn_01_doze_interval_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;
	u16 ctrl_base_addr;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || new_value > 255) {
		dev_err(dev, "%s: Invalid doze interval %s.", __func__, buf);
		return -EINVAL;
	}

	data->device_control.doze_interval = new_value;
	ctrl_base_addr = fn->fd.control_base_addr + sizeof(u8) +
			(sizeof(u8)*(data->num_of_irq_regs));
	dev_dbg(dev, "doze_interval store address %x, value %d",
		ctrl_base_addr, data->device_control.doze_interval);

	retval = rmi_write_block(fn->rmi_dev, data->doze_interval_addr,
			&data->device_control.doze_interval,
			sizeof(u8));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write doze interval.\n");

	return retval;

}

static ssize_t rmi_fn_01_wakeup_threshold_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.wakeup_threshold);
}

static ssize_t rmi_fn_01_wakeup_threshold_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || new_value > 255) {
		dev_err(dev, "%s: Invalid wakeup threshold %s.", __func__, buf);
		return -EINVAL;
	}

	data->device_control.doze_interval = new_value;
	retval = rmi_write_block(fn->rmi_dev, data->wakeup_threshold_addr,
			&data->device_control.wakeup_threshold,
			sizeof(u8));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write wakeup threshold.\n");
	return retval;

}

static ssize_t rmi_fn_01_doze_holdoff_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.doze_holdoff);

}


static ssize_t rmi_fn_01_doze_holdoff_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	unsigned long new_value;
	int retval;

	retval = strict_strtoul(buf, 10, &new_value);
	if (retval < 0 || new_value > 255) {
		dev_err(dev, "%s: Invalid doze holdoff %s.", __func__, buf);
		return -EINVAL;
	}

	data->device_control.doze_interval = new_value;
	retval = rmi_write_block(fn->rmi_dev, data->doze_holdoff_addr,
			&data->device_control.doze_holdoff,
			sizeof(u8));
	if (retval >= 0)
		retval = count;
	else
		dev_err(dev, "Failed to write doze holdoff.\n");

	return retval;

}

static ssize_t rmi_fn_01_configured_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_control.ctrl0.configured);
}

static ssize_t rmi_fn_01_unconfigured_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_status.unconfigured);
}

static ssize_t rmi_fn_01_flashprog_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			data->device_status.flash_prog);
}

static ssize_t rmi_fn_01_statuscode_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n",
			data->device_status.status_code);
}

#define RMI_F01_ATTR(_name)			\
	DEVICE_ATTR(_name, RMI_RW_ATTR,		\
		    rmi_fn_01_##_name##_show,	\
		    rmi_fn_01_##_name##_store)

#define RMI_F01_RO_ATTR(_name)			\
	DEVICE_ATTR(_name, RMI_RO_ATTR,		\
		    rmi_fn_01_##_name##_show,	\
		    NULL)

#define RMI_F01_WO_ATTR(_name)			\
	DEVICE_ATTR(_name, RMI_RO_ATTR,		\
		    NULL,			\
		    rmi_fn_01_##_name##_store)


static RMI_F01_RO_ATTR(productinfo);
static RMI_F01_RO_ATTR(productid);
static RMI_F01_RO_ATTR(manufacturer);
static RMI_F01_RO_ATTR(datecode);

/* Control register access */
static RMI_F01_ATTR(sleepmode);
static RMI_F01_ATTR(nosleep);
static RMI_F01_ATTR(chargerinput);
static RMI_F01_ATTR(reportrate);

/*
 * We don't want arbitrary callers changing the interrupt enable mask,
 * so it's read only.
 */
static RMI_F01_RO_ATTR(interrupt_enable);
static RMI_F01_ATTR(doze_interval);
static RMI_F01_ATTR(wakeup_threshold);
static RMI_F01_ATTR(doze_holdoff);

/*
 * We make report rate RO, since the driver uses that to look for
 * resets.  We don't want someone faking us out by changing that
 * bit.
 */
static RMI_F01_RO_ATTR(configured);

/* Command register access. */
static RMI_F01_WO_ATTR(reset);

/* Status register access. */
static RMI_F01_RO_ATTR(unconfigured);
static RMI_F01_RO_ATTR(flashprog);
static RMI_F01_RO_ATTR(statuscode);

static struct attribute *rmi_fn_01_attrs[] = {
	&dev_attr_productinfo.attr,
	&dev_attr_productid.attr,
	&dev_attr_manufacturer.attr,
	&dev_attr_datecode.attr,
	&dev_attr_sleepmode.attr,
	&dev_attr_nosleep.attr,
	&dev_attr_chargerinput.attr,
	&dev_attr_reportrate.attr,
	&dev_attr_interrupt_enable.attr,
	&dev_attr_doze_interval.attr,
	&dev_attr_wakeup_threshold.attr,
	&dev_attr_doze_holdoff.attr,
	&dev_attr_configured.attr,
	&dev_attr_reset.attr,
	&dev_attr_unconfigured.attr,
	&dev_attr_flashprog.attr,
	&dev_attr_statuscode.attr,
	NULL
};

static umode_t rmi_fn_01_attr_visible(struct kobject *kobj,
				      struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj); 
	struct rmi_function *fn = to_rmi_function(dev);
	struct f01_data *data = fn->data;
	umode_t mode = attr->mode;

	if (attr == &dev_attr_doze_interval.attr) {
		if (!data->basic_queries.has_lts)
			mode = 0;
	} else if (attr == &dev_attr_wakeup_threshold.attr) {
		if (!data->basic_queries.has_adjustable_doze)
			mode = 0;
	} else if (attr == &dev_attr_doze_holdoff.attr) {
		if (!data->basic_queries.has_adjustable_doze_holdoff)
			mode = 0;
	}

	return mode;
}

static struct attribute_group rmi_fn_01_attr_group = {
	.is_visible	= rmi_fn_01_attr_visible,
	.attrs		= rmi_fn_01_attrs,
};

static int rmi_f01_alloc_memory(struct rmi_function *fn,
				int num_of_irq_regs)
{
	struct f01_data *f01;

	f01 = devm_kzalloc(&fn->dev, sizeof(struct f01_data), GFP_KERNEL);
	if (!f01) {
		dev_err(&fn->dev, "Failed to allocate fn_01_data.\n");
		return -ENOMEM;
	}

	f01->device_control.interrupt_enable = devm_kzalloc(&fn->dev,
			sizeof(u8)*(num_of_irq_regs),
			GFP_KERNEL);
	if (!f01->device_control.interrupt_enable) {
		dev_err(&fn->dev, "Failed to allocate interrupt enable.\n");
		return -ENOMEM;
	}
	fn->data = f01;

	return 0;
}

static int rmi_f01_initialize(struct rmi_function *fn)
{
	u8 temp;
	int error;
	u16 ctrl_base_addr;
	struct rmi_device *rmi_dev = fn->rmi_dev;
	struct rmi_driver_data *driver_data = dev_get_drvdata(&rmi_dev->dev);
	struct f01_data *data = fn->data;
	struct rmi_device_platform_data *pdata = to_rmi_platform_data(rmi_dev);

	/* Set the configured bit and (optionally) other important stuff
	 * in the device control register. */
	ctrl_base_addr = fn->fd.control_base_addr;
	error = rmi_read_block(rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (error < 0) {
		dev_err(&fn->dev, "Failed to read F01 control.\n");
		return error;
	}
	switch (pdata->power_management.nosleep) {
	case RMI_F01_NOSLEEP_DEFAULT:
		break;
	case RMI_F01_NOSLEEP_OFF:
		data->device_control.ctrl0.nosleep = 0;
		break;
	case RMI_F01_NOSLEEP_ON:
		data->device_control.ctrl0.nosleep = 1;
		break;
	}
	/* Sleep mode might be set as a hangover from a system crash or
	 * reboot without power cycle.  If so, clear it so the sensor
	 * is certain to function.
	 */
	if (data->device_control.ctrl0.sleep_mode != RMI_SLEEP_MODE_NORMAL) {
		dev_warn(&fn->dev,
			 "WARNING: Non-zero sleep mode found. Clearing...\n");
		data->device_control.ctrl0.sleep_mode = RMI_SLEEP_MODE_NORMAL;
	}

	data->device_control.ctrl0.configured = 1;
	error = rmi_write_block(rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (error < 0) {
		dev_err(&fn->dev, "Failed to write F01 control.\n");
		return error;
	}

	data->irq_count = driver_data->irq_count;
	data->num_of_irq_regs = driver_data->num_of_irq_regs;
	ctrl_base_addr += sizeof(struct f01_device_control_0);

	data->interrupt_enable_addr = ctrl_base_addr;
	error = rmi_read_block(rmi_dev, ctrl_base_addr,
			data->device_control.interrupt_enable,
			sizeof(u8)*(data->num_of_irq_regs));
	if (error < 0) {
		dev_err(&fn->dev, "Failed to read F01 control interrupt enable register.\n");
		goto error_exit;
	}
	ctrl_base_addr += data->num_of_irq_regs;

	/* dummy read in order to clear irqs */
	error = rmi_read(rmi_dev, fn->fd.data_base_addr + 1, &temp);
	if (error < 0) {
		dev_err(&fn->dev, "Failed to read Interrupt Status.\n");
		return error;
	}

	error = rmi_read_block(rmi_dev, fn->fd.query_base_addr,
				&data->basic_queries,
				sizeof(data->basic_queries));
	if (error < 0) {
		dev_err(&fn->dev, "Failed to read device query registers.\n");
		return error;
	}

	error = rmi_read_block(rmi_dev,
		fn->fd.query_base_addr + sizeof(data->basic_queries),
		data->product_id, RMI_PRODUCT_ID_LENGTH);
	if (error < 0) {
		dev_err(&fn->dev, "Failed to read product ID.\n");
		return error;
	}
	data->product_id[RMI_PRODUCT_ID_LENGTH] = '\0';
	dev_info(&fn->dev, "found RMI device, manufacturer: %s, product: %s\n",
		 data->basic_queries.manufacturer_id == 1 ?
							"synaptics" : "unknown",
		 data->product_id);

	/* read control register */
	if (data->basic_queries.has_adjustable_doze) {
		data->doze_interval_addr = ctrl_base_addr;
		ctrl_base_addr++;

		if (pdata->power_management.doze_interval) {
			data->device_control.doze_interval =
				pdata->power_management.doze_interval;
			error = rmi_write(rmi_dev, data->doze_interval_addr,
					data->device_control.doze_interval);
			if (error < 0) {
				dev_err(&fn->dev, "Failed to configure F01 doze interval register.\n");
				goto error_exit;
			}
		} else {
			error = rmi_read(rmi_dev, data->doze_interval_addr,
					&data->device_control.doze_interval);
			if (error < 0) {
				dev_err(&fn->dev, "Failed to read F01 doze interval register.\n");
				goto error_exit;
			}
		}

		data->wakeup_threshold_addr = ctrl_base_addr;
		ctrl_base_addr++;

		if (pdata->power_management.wakeup_threshold) {
			data->device_control.wakeup_threshold =
				pdata->power_management.wakeup_threshold;
			error = rmi_write(rmi_dev, data->wakeup_threshold_addr,
					data->device_control.wakeup_threshold);
			if (error < 0) {
				dev_err(&fn->dev, "Failed to configure F01 wakeup threshold register.\n");
				goto error_exit;
			}
		} else {
			error = rmi_read(rmi_dev, data->wakeup_threshold_addr,
					&data->device_control.wakeup_threshold);
			if (error < 0) {
				dev_err(&fn->dev, "Failed to read F01 wakeup threshold register.\n");
				goto error_exit;
			}
		}
	}

	if (data->basic_queries.has_adjustable_doze_holdoff) {
		data->doze_holdoff_addr = ctrl_base_addr;
		ctrl_base_addr++;

		if (pdata->power_management.doze_holdoff) {
			data->device_control.doze_holdoff =
				pdata->power_management.doze_holdoff;
			error = rmi_write(rmi_dev, data->doze_holdoff_addr,
					data->device_control.doze_holdoff);
			if (error < 0) {
				dev_err(&fn->dev, "Failed to configure F01 doze holdoff register.\n");
				goto error_exit;
			}
		} else {
			error = rmi_read(rmi_dev, data->doze_holdoff_addr,
					&data->device_control.doze_holdoff);
			if (error < 0) {
				dev_err(&fn->dev, "Failed to read F01 doze holdoff register.\n");
				goto error_exit;
			}
		}
	}

	error = rmi_read_block(rmi_dev, fn->fd.data_base_addr,
		&data->device_status, sizeof(data->device_status));
	if (error < 0) {
		dev_err(&fn->dev, "Failed to read device status.\n");
		goto error_exit;
	}

	if (data->device_status.unconfigured) {
		dev_err(&fn->dev, "Device reset during configuration process, status: %#02x!\n",
				data->device_status.status_code);
		error = -EINVAL;
		goto error_exit;
	}

	error = setup_debugfs(fn);
	if (error)
		dev_warn(&fn->dev, "Failed to setup debugfs, error: %d.\n",
			 error);

	return 0;

 error_exit:
	kfree(data);
	return error;
}

static int rmi_f01_config(struct rmi_function *fn)
{
	struct f01_data *data = fn->data;
	int retval;

	retval = rmi_write_block(fn->rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval < 0) {
		dev_err(&fn->dev, "Failed to write device_control.reg.\n");
		return retval;
	}

	retval = rmi_write_block(fn->rmi_dev, data->interrupt_enable_addr,
			data->device_control.interrupt_enable,
			sizeof(u8)*(data->num_of_irq_regs));

	if (retval < 0) {
		dev_err(&fn->dev, "Failed to write interrupt enable.\n");
		return retval;
	}
	if (data->basic_queries.has_lts) {
		retval = rmi_write_block(fn->rmi_dev, data->doze_interval_addr,
				&data->device_control.doze_interval,
				sizeof(u8));
		if (retval < 0) {
			dev_err(&fn->dev, "Failed to write doze interval.\n");
			return retval;
		}
	}

	if (data->basic_queries.has_adjustable_doze) {
		retval = rmi_write_block(
				fn->rmi_dev, data->wakeup_threshold_addr,
				&data->device_control.wakeup_threshold,
				sizeof(u8));
		if (retval < 0) {
			dev_err(&fn->dev, "Failed to write wakeup threshold.\n");
			return retval;
		}
	}

	if (data->basic_queries.has_adjustable_doze_holdoff) {
		retval = rmi_write_block(fn->rmi_dev, data->doze_holdoff_addr,
				&data->device_control.doze_holdoff,
				sizeof(u8));
		if (retval < 0) {
			dev_err(&fn->dev, "Failed to write doze holdoff.\n");
			return retval;
		}
	}
	return 0;
}

static int f01_device_init(struct rmi_function *fn)
{
	struct rmi_driver_data *driver_data =
			dev_get_drvdata(&fn->rmi_dev->dev);
	int error;

	error = rmi_f01_alloc_memory(fn, driver_data->num_of_irq_regs);
	if (error)
		return error;

	error = rmi_f01_initialize(fn);
	if (error)
		return error;

	error = sysfs_create_group(&fn->dev.kobj, &rmi_fn_01_attr_group);
	if (error)
		return error;

	return 0;
}

#ifdef CONFIG_PM
static int rmi_f01_suspend(struct rmi_function *fn)
{
	struct rmi_device *rmi_dev = fn->rmi_dev;
	struct f01_data *data = fn->data;
	int retval = 0;

	if (data->suspended)
		return 0;

	data->old_nosleep = data->device_control.ctrl0.nosleep;
	data->device_control.ctrl0.nosleep = 0;
	data->device_control.ctrl0.sleep_mode = RMI_SLEEP_MODE_SENSOR_SLEEP;

	retval = rmi_write_block(rmi_dev,
			fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval < 0) {
		dev_err(&fn->dev, "Failed to write sleep mode. Code: %d.\n",
			retval);
		data->device_control.ctrl0.nosleep = data->old_nosleep;
		data->device_control.ctrl0.sleep_mode = RMI_SLEEP_MODE_NORMAL;
	} else {
		data->suspended = true;
		retval = 0;
	}

	return retval;
}

static int rmi_f01_resume(struct rmi_function *fn)
{
	struct rmi_device *rmi_dev = fn->rmi_dev;
	struct f01_data *data = fn->data;
	int retval = 0;

	if (!data->suspended)
		return 0;

	data->device_control.ctrl0.nosleep = data->old_nosleep;
	data->device_control.ctrl0.sleep_mode = RMI_SLEEP_MODE_NORMAL;

	retval = rmi_write_block(rmi_dev, fn->fd.control_base_addr,
			&data->device_control.ctrl0,
			sizeof(data->device_control.ctrl0));
	if (retval < 0)
		dev_err(&fn->dev,
			"Failed to restore normal operation. Code: %d.\n",
			retval);
	else {
		data->suspended = false;
		retval = 0;
	}

	return retval;
}
#endif /* CONFIG_PM */

static int f01_remove(struct device *dev)
{
	struct rmi_function *fn = to_rmi_function(dev);

	teardown_debugfs(fn->data);
	sysfs_remove_group(&fn->dev.kobj, &rmi_fn_01_attr_group);

	return 0;
}

static int rmi_f01_attention(struct rmi_function *fn,
						unsigned long *irq_bits)
{
	struct rmi_device *rmi_dev = fn->rmi_dev;
	struct f01_data *data = fn->data;
	int retval;

	retval = rmi_read_block(rmi_dev, fn->fd.data_base_addr,
		&data->device_status, sizeof(data->device_status));
	if (retval < 0) {
		dev_err(&fn->dev, "Failed to read device status, code: %d.\n",
			retval);
		return retval;
	}
	if (data->device_status.unconfigured) {
		dev_warn(&fn->dev, "Device reset detected.\n");
		retval = rmi_dev->driver->reset_handler(rmi_dev);
		if (retval < 0)
			return retval;
	}
	return 0;
}

static int f01_probe(struct device *dev)
{
	struct rmi_function *fn;

	if (dev->type != &rmi_function_type)
		return 1;

	fn = to_rmi_function(dev);
	if (fn->fd.function_number != FUNCTION_NUMBER)
		return 1;

	return f01_device_init(fn);
}

static struct rmi_function_handler function_handler = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "rmi_f01",
		.bus = &rmi_bus_type,
		.probe = f01_probe,
		.remove = f01_remove,
	},
	.func = FUNCTION_NUMBER,
	.config = rmi_f01_config,
	.attention = rmi_f01_attention,

#ifdef CONFIG_PM
	.suspend = rmi_f01_suspend,
	.resume = rmi_f01_resume,
#endif  /* CONFIG_PM */
};

static int __init rmi_f01_module_init(void)
{
	int error;

	error = driver_register(&function_handler.driver);
	if (error < 0) {
		pr_err("%s: register failed!\n", __func__);
		return error;
	}

	return 0;
}

static void __exit rmi_f01_module_exit(void)
{
	driver_unregister(&function_handler.driver);
}

module_init(rmi_f01_module_init);
module_exit(rmi_f01_module_exit);

MODULE_AUTHOR("Christopher Heiny <cheiny@synaptics.com>");
MODULE_DESCRIPTION("RMI F01 module");
MODULE_LICENSE("GPL");
MODULE_VERSION(RMI_DRIVER_VERSION);
