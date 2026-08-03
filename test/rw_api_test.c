#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_p2p_api.h"

#define TEST_OP_MIXED 2U
#define MIXED_READ_CMB_OFFSET (4UL << 20)

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --topology <bdev> --target <path> "
		"--op <read|write|mixed> --offset <bytes> --cmb-va <bytes> "
		"--length <bytes> [--source-offset <bytes>] "
		"[--registered-mem]\n",
		program);
}

static int parse_ulong(const char *text, unsigned long *value)
{
	unsigned long parsed;
	char *end;

	if (!text || !*text || *text == '-')
		return -EINVAL;
	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno == ERANGE)
		return -ERANGE;
	if (end == text || *end)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static unsigned int make_iovs(struct p2p_iov iov[3], unsigned long cmb_va,
			      unsigned long length)
{
	unsigned long first = 512;
	unsigned long second = 128UL << 10;

	memset(iov, 0, sizeof(*iov) * 3);
	if (length < first + second + 512) {
		iov[0].addr = cmb_va;
		iov[0].size = length;
		return 1;
	}
	iov[0].addr = cmb_va;
	iov[0].size = first;
	iov[1].addr = cmb_va + first;
	iov[1].size = second;
	iov[2].addr = cmb_va + first + second;
	iov[2].size = length - first - second;
	return 3;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "target", required_argument, NULL, 'f' },
		{ "op", required_argument, NULL, 'o' },
		{ "offset", required_argument, NULL, 'x' },
		{ "cmb-va", required_argument, NULL, 'a' },
		{ "length", required_argument, NULL, 'l' },
		{ "source-offset", required_argument, NULL, 's' },
		{ "registered-mem", no_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct p2p_mem_unregister_param unreg = { };
	struct p2p_mem_register_param reg = { };
	struct io_parameter read_param = { };
	const char *topology = NULL;
	const char *target = NULL;
	struct p2p_iov read_iov[3];
	struct p2p_iov iov[3];
	struct io_parameter param = { };
	unsigned long source_offset = 0;
	unsigned long offset = 0;
	unsigned long cmb_va = 0;
	unsigned long length = 0;
	unsigned int op = UINT32_MAX;
	int registered = 0;
	int dev_fd = -1;
	int option;
	int err = 0;

	while ((option = getopt_long(argc, argv, "t:f:o:x:a:l:s:rh", options,
				     NULL)) != -1) {
		switch (option) {
		case 't':
			topology = optarg;
			break;
		case 'f':
			target = optarg;
			break;
		case 'o':
			if (!strcmp(optarg, "read"))
				op = P2P_IO_READ;
			else if (!strcmp(optarg, "write"))
				op = P2P_IO_WRITE;
			else if (!strcmp(optarg, "mixed"))
				op = TEST_OP_MIXED;
			else
				goto invalid;
			break;
		case 'x':
			if (parse_ulong(optarg, &offset))
				goto invalid;
			break;
		case 's':
			if (parse_ulong(optarg, &source_offset))
				goto invalid;
			break;
		case 'a':
			if (parse_ulong(optarg, &cmb_va))
				goto invalid;
			break;
		case 'l':
			if (parse_ulong(optarg, &length))
				goto invalid;
			break;
		case 'r':
			registered = 1;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			goto invalid;
		}
	}
	if (!topology || !target || op == UINT32_MAX || !length ||
	    (op == TEST_OP_MIXED && !source_offset) || optind != argc)
		goto invalid;

	dev_fd = new_p2p_fd();
	if (dev_fd < 0) {
		err = dev_fd;
		goto out;
	}
	err = add_topo(dev_fd, topology);
	if (err)
		goto out;

	param.op = op == TEST_OP_MIXED ? P2P_IO_WRITE : op;
	param.file_name = target;
	param.file_offset = offset;
	param.iov = iov;
	param.iov_nr = make_iovs(iov, cmb_va, length);
	if (registered) {
		unsigned long reg_size = length;

		if (op == TEST_OP_MIXED) {
			if (ULONG_MAX - MIXED_READ_CMB_OFFSET < length) {
				err = -EOVERFLOW;
				goto out;
			}
			reg_size = MIXED_READ_CMB_OFFSET + length;
		}
		reg.addr = cmb_va;
		reg.size = reg_size;
		err = register_mem(dev_fd, &reg);
		if (err)
			goto out;
		param.flags = P2P_IO_F_REGISTERED_MEM;
		param.mem_handle = reg.mem_handle;
	}

	if (op == TEST_OP_MIXED) {
		int drain_err;

		read_param = param;
		read_param.op = P2P_IO_READ;
		read_param.file_offset = source_offset;
		read_param.iov = read_iov;
		read_param.iov_nr = make_iovs(read_iov,
					     cmb_va + MIXED_READ_CMB_OFFSET,
					     length);

		err = rw_file(dev_fd, &param);
		if (!err)
			err = rw_file(dev_fd, &read_param);
		drain_err = drain_io(dev_fd);
		if (!err)
			err = drain_err;
	} else {
		err = rw_file(dev_fd, &param);
		if (!err)
			err = drain_io(dev_fd);
	}

	if (registered) {
		int unreg_err;

		unreg.mem_handle = reg.mem_handle;
		unreg_err = unregister_mem(dev_fd, &unreg);
		if (!err)
			err = unreg_err;
	}

out:
	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	if (err)
		fprintf(stderr, "%s failed: %s (%d)\n",
			op == TEST_OP_MIXED ? "mixed" :
			op == P2P_IO_WRITE ? "write" : "read",
			strerror(-err), err);
	else
		printf("api=c case=%s ret=0 expected=0\n",
		       op == TEST_OP_MIXED ? "mixed" :
		       op == P2P_IO_WRITE ? "write" : "read");
	return err ? EXIT_FAILURE : EXIT_SUCCESS;

invalid:
	usage(argv[0]);
	return EXIT_FAILURE;
}
