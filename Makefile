KSRC ?= /lib/modules/$(shell uname -r)/build
KSRC = /home/begin/code/oe_knl/


.PHONY: all clean test

CRC32_VERIFY := test/crc32_verify
TEST_CFLAGS ?= -O2 -Wall -Wextra

all:
	$(MAKE) M=$(shell pwd) -C $(KSRC) modules

obj-m := stub.o
obj-m += p2p_dev.o

p2p_dev-objs := dev.o topo.o debugfs.o

test: $(CRC32_VERIFY)

$(CRC32_VERIFY): test/crc32_verify.c
	$(CC) $(TEST_CFLAGS) -o $@ $<

clean:
	rm -rf *.o *.ko *.mod.c *.mod.o *.mod modules.* Module.* .*.ko.cmd .*.mod.o.cmd .*.o.cmd
	rm -rf .*.mod.cmd .tmp_versions/
	rm -f $(CRC32_VERIFY)
