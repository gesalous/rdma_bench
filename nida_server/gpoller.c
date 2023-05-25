#include "gpoller.h"
#include <infiniband/verbs.h>
#include <log.h>
#include <rdma/rdma_cma.h>
#include <stdlib.h>
#include <unistd.h>

struct ibv_context *tpoller_get_rdma_device_context(char *dev_name)
{
	int num_devices = 0;
	struct ibv_context **dev_list = rdma_get_devices(&num_devices);
	struct ibv_context *rdma_dev = NULL;

	if (num_devices < 1) {
		log_fatal("No RDMA device found. Exiting..");
		_exit(EXIT_FAILURE);
	}

	if (NULL == dev_name) {
		log_info("Using default RDMA device %s", dev_list[0]->device->name);
		return dev_list[0];
	}

	for (int i = 0; i < num_devices; ++i)
		if (!strncmp(dev_list[i]->device->name, dev_name, strlen(dev_name))) {
			rdma_dev = dev_list[i];
			break;
		}

	if (!rdma_dev) {
		log_fatal("Cannot find RDMA device %s", dev_name);
		_exit(EXIT_FAILURE);
	}

	rdma_free_devices(dev_list);

	return rdma_dev;
}
