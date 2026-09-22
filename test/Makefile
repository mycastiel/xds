CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../file_p2p -I..

.PHONY: all clean

TEST_BINS := crc32_verify c_api_test mem_registration_test direct_io_test \
	topo_test rw_api_test uapi_rw_test nds_api_test nds_example nds_bw_bench \
	nds_crc_verify coverage_errors topo_devt_test prepare_extents_test \
	file_raw_write_test nds_file_prepare_test

all: $(TEST_BINS)

topo_devt_test: topo_devt_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/nds_api.h \
		../file_p2p/nds_api_internal.h ../file_p2p/p2p_common.h \
		../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		topo_devt_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		-Wl,--wrap=stat,--wrap=fstat,--wrap=open,--wrap=openat \
		-Wl,--wrap=read,--wrap=close,--wrap=ioctl

coverage_errors: coverage_errors.c ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -D_GNU_SOURCE -o $@ $<

prepare_extents_test: prepare_extents_test.c ../file_p2p/p2p_common.c \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -D_GNU_SOURCE -o $@ \
		prepare_extents_test.c ../file_p2p/p2p_common.c \
		-Wl,--wrap=ioctl,--wrap=fallocate,--wrap=fcntl

file_raw_write_test: file_raw_write_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/p2p_common.c ../file_p2p/file_p2p_api.h \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		file_raw_write_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/p2p_common.c

nds_file_prepare_test: nds_file_prepare_test.c \
		../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		../file_p2p/nds_api.h ../file_p2p/nds_api_internal.h \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		nds_file_prepare_test.c ../file_p2p/nds_api.c \
		../file_p2p/p2p_common.c \
		-Wl,--wrap=ioctl,--wrap=p2p_open_dev

crc32_verify: crc32_verify.c
	$(CC) $(CFLAGS) -o $@ $<

c_api_test: c_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		c_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

mem_registration_test: mem_registration_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/p2p_common.c ../file_p2p/file_p2p_api.h \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h debugfs_test.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		mem_registration_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/p2p_common.c

direct_io_test: direct_io_test.c
	$(CC) $(CFLAGS) -o $@ $<

topo_test: topo_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h \
		../p2p_dev_uapi.h debugfs_test.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		topo_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

rw_api_test: rw_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		rw_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

uapi_rw_test: uapi_rw_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		uapi_rw_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

nds_api_test: nds_api_test.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		../file_p2p/nds_api.h ../file_p2p/nds_api_internal.h \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		nds_api_test.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c

nds_example: nds_example.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		../file_p2p/nds_api.h ../file_p2p/nds_api_internal.h \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		nds_example.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c

nds_bw_bench: nds_bw_bench.c hbm_acl.c hbm_acl.h ../file_p2p/nds_api.c \
		../file_p2p/p2p_common.c ../file_p2p/nds_api.h \
		../file_p2p/nds_api_internal.h ../file_p2p/p2p_common.h \
		../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		nds_bw_bench.c hbm_acl.c ../file_p2p/nds_api.c \
		../file_p2p/p2p_common.c -ldl

nds_crc_verify: nds_crc_verify.c hbm_acl.c hbm_acl.h ../file_p2p/nds_api.c \
		../file_p2p/p2p_common.c ../file_p2p/nds_api.h \
		../file_p2p/nds_api_internal.h ../file_p2p/p2p_common.h \
		../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		nds_crc_verify.c hbm_acl.c ../file_p2p/nds_api.c \
		../file_p2p/p2p_common.c -ldl

clean:
	rm -f $(TEST_BINS)
