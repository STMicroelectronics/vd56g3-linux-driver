ifneq ($(KERNELRELEASE),)
include Kbuild

else
KVERSION ?= `uname -r`
KDIR ?= /lib/modules/${KVERSION}/build
default:
	CONFIG_DRIVER_VD56G3=m $(MAKE) -C $(KDIR) M=$$PWD

clean:
	CONFIG_DRIVER_VD56G3=m $(MAKE) -C $(KDIR) M=$$PWD clean

endif
