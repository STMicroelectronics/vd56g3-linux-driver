ifneq ($(KERNELRELEASE),)
include Kbuild

else
KDIR ?= /lib/modules/`uname -r`/build
default:
	CONFIG_DRIVER_ST_VD56G3=m $(MAKE) -C $(KDIR) M=$$PWD

clean:
	CONFIG_DRIVER_ST_VD56G3=m $(MAKE) -C $(KDIR) M=$$PWD clean

endif