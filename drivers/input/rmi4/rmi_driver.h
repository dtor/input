/*
 * Copyright (c) 2011, 2012 Synaptics Incorporated
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
#ifndef _RMI_DRIVER_H
#define _RMI_DRIVER_H

#include <linux/ctype.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#define RMI_DRIVER_VERSION "1.6"

#define SYNAPTICS_INPUT_DEVICE_NAME "Synaptics RMI4 Touch Sensor"
#define SYNAPTICS_VENDOR_ID 0x06cb

#define GROUP(_attrs) { \
	.attrs = _attrs,  \
}

#define attrify(nm) (&dev_attr_##nm.attr)

#define PDT_PROPERTIES_LOCATION 0x00EF
#define BSR_LOCATION 0x00FE

struct pdt_properties {
	u8 reserved_1:6;
	u8 has_bsr:1;
	u8 reserved_2:1;
} __attribute__((__packed__));

struct rmi_driver_data {
	struct rmi_function_container rmi_functions;
	struct rmi_device *rmi_dev;

	struct rmi_function_container *f01_container;
	bool f01_bootloader_mode;

	atomic_t attn_count;
	bool irq_debug;
	int irq;
	int irq_flags;
	int num_of_irq_regs;
	int irq_count;
	unsigned long *irq_status;
	unsigned long *current_irq_mask;
	unsigned long *irq_mask_store;
	bool irq_stored;
	struct mutex irq_mutex;

	/* Following are used when polling. */
	struct hrtimer poll_timer;
	struct work_struct poll_work;
	ktime_t poll_interval;

	struct mutex pdt_mutex;
	struct pdt_properties pdt_props;
	u8 bsr;

	int board;
	int rev;

	bool enabled;
#ifdef CONFIG_PM
	bool suspended;
	struct mutex suspend_mutex;

	void *pm_data;
	int (*pre_suspend) (const void *pm_data);
	int (*post_suspend) (const void *pm_data);
	int (*pre_resume) (const void *pm_data);
	int (*post_resume) (const void *pm_data);
#endif

#ifdef CONFIG_RMI4_DEBUG
	struct dentry *debugfs_delay;
	struct dentry *debugfs_phys;
	struct dentry *debugfs_reg_ctl;
	struct dentry *debugfs_reg;
	struct dentry *debugfs_irq;
	struct dentry *debugfs_attn_count;
	u16 reg_debug_addr;
	u8 reg_debug_size;
#endif

	void *data;
};

#define PDT_START_SCAN_LOCATION 0x00e9
#define PDT_END_SCAN_LOCATION	0x0005
#define RMI4_END_OF_PDT(id) ((id) == 0x00 || (id) == 0xff)

struct pdt_entry {
	u8 query_base_addr:8;
	u8 command_base_addr:8;
	u8 control_base_addr:8;
	u8 data_base_addr:8;
	u8 interrupt_source_count:3;
	u8 bits3and4:2;
	u8 function_version:2;
	u8 bit7:1;
	u8 function_number:8;
} __attribute__((__packed__));

static inline void copy_pdt_entry_to_fd(struct pdt_entry *pdt,
				 struct rmi_function_descriptor *fd,
				 u16 page_start)
{
	fd->query_base_addr = pdt->query_base_addr + page_start;
	fd->command_base_addr = pdt->command_base_addr + page_start;
	fd->control_base_addr = pdt->control_base_addr + page_start;
	fd->data_base_addr = pdt->data_base_addr + page_start;
	fd->function_number = pdt->function_number;
	fd->interrupt_source_count = pdt->interrupt_source_count;
	fd->function_version = pdt->function_version;
}

#ifdef	CONFIG_RMI4_FWLIB
extern void rmi4_fw_update(struct rmi_device *rmi_dev,
		struct pdt_entry *f01_pdt, struct pdt_entry *f34_pdt);
#else
#define rmi4_fw_update(rmi_dev, f01_pdt, f34_pdt)
#endif

#endif
