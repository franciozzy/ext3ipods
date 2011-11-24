obj-m	:= ifm_lkm.o

KDIR	:= /usr/src/linux-source-2.6.32
PWD	:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install:
	cp ifm_lkm.ko /lib/modules/2.6.32-5-686/kernel/fs/ext3ipods/
	depmod -a

clean:
	rm -rf	*.ko *.o *.mod.c *.cmd .*.cmd Module.symvers .tmp_versions modules.order
