#include "nida_conf.h"
#include <infiniband/verbs.h>
#include <log.h>
#include <rdma_cma.h>
#include <stdlib.h>
#include <unistd.h>

struct ibv_context *nida_get_rdma_dev_context(char *devname)
{
	int num_devices;
	struct ibv_context **dev_list = rdma_get_devices(&num_devices);
	struct ibv_context *rdma_dev = NULL;

	if (num_devices < 1) {
		log_fatal("No RDMA device found. Exiting..");
		_exit(EXIT_FAILURE);
	}

	if (!devname) {
		log_info("Using default RDMA device %s", dev_list[0]->device->name);
		return dev_list[0];
	}

	for (int i = 0; i < num_devices; ++i)
		if (!strncmp(dev_list[i]->device->name, devname, strlen(devname))) {
			rdma_dev = dev_list[i];
			break;
		}

	if (!rdma_dev) {
		log_fatal("Cannot find RDMA device %s", devname);
		_exit(EXIT_FAILURE);
	}

	rdma_free_devices(dev_list);

	return rdma_dev;
}
