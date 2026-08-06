CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../file_p2p -I..

.PHONY: all clean

TEST_BINS := crc32_verify c_api_test mem_registration_test direct_io_test \
	rw_api_test uapi_rw_test nds_api_test nds_example

all: $(TEST_BINS)

crc32_verify: crc32_verify.c
	$(CC) $(CFLAGS) -o $@ $<

c_api_test: c_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		c_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

mem_registration_test: mem_registration_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/p2p_common.c ../file_p2p/file_p2p_api.h \
		../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		mem_registration_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/p2p_common.c

direct_io_test: direct_io_test.c
	$(CC) $(CFLAGS) -o $@ $<

rw_api_test: rw_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		rw_api_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

uapi_rw_test: uapi_rw_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c \
		../file_p2p/file_p2p_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		uapi_rw_test.c ../file_p2p/file_p2p_api.c ../file_p2p/p2p_common.c

nds_api_test: nds_api_test.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		../file_p2p/nds_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		nds_api_test.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c

nds_example: nds_example.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c \
		../file_p2p/nds_api.h ../file_p2p/p2p_common.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		nds_example.c ../file_p2p/nds_api.c ../file_p2p/p2p_common.c

clean:
	rm -f $(TEST_BINS)
