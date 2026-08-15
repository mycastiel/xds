KSRC ?= /lib/modules/$(shell uname -r)/build

.PHONY: all mod lib clean test

all: mod lib

mod:
	$(MAKE) M=$(shell pwd) -C $(KSRC) modules

lib:
	$(MAKE) -C file_p2p

obj-m := stub.o
obj-m += p2p_dev.o

p2p_dev-objs := dev.o topo.o debugfs.o mem.o

test:
	$(MAKE) -C test

clean:
	rm -rf *.o *.ko *.mod.c *.mod.o *.mod modules.* Module.* .*.ko.cmd .*.mod.o.cmd .*.o.cmd
	rm -rf .*.mod.cmd .tmp_versions/
	$(MAKE) -C file_p2p clean
	$(MAKE) -C test clean
