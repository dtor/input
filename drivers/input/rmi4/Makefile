obj-$(CONFIG_RMI4_BUS) += rmi_bus.o
obj-$(CONFIG_RMI4_I2C) += rmi_i2c.o
obj-$(CONFIG_RMI4_GENERIC) += rmi_driver.o rmi_f01.o
obj-$(CONFIG_RMI4_F11) += rmi_f11.o

ccflags-$(CONFIG_RMI4_DEBUG) += -DDEBUG

ifeq ($(KERNELRELEASE),)

# KERNELDIR ?= /home/<AndroidKernelDirectory>
PWD := $(shell pwd)

.PHONY: build clean

build:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c

endif

