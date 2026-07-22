CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../file_p2p -I..

.PHONY: all clean

all: crc32_verify c_api_test mem_registration_test

crc32_verify: crc32_verify.c
	$(CC) $(CFLAGS) -o $@ $<

c_api_test: c_api_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/file_p2p_api.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		c_api_test.c ../file_p2p/file_p2p_api.c

mem_registration_test: mem_registration_test.c ../file_p2p/file_p2p_api.c \
		../file_p2p/file_p2p_api.h ../p2p_dev_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ \
		mem_registration_test.c ../file_p2p/file_p2p_api.c

clean:
	rm -f crc32_verify c_api_test mem_registration_test
