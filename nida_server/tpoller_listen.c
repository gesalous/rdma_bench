#define _GNU_SOURCE
#include "connection_listener.h"
#include "server_conn.h"
#include <assert.h>
#include <infiniband/verbs.h>
#include <log.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdlib.h>
#include <unistd.h>

#define CLISTEN_CQ_SIZE 256
#define CLISTEN_LOOP_START do {
#define CLISTEN_LOOP_END \
	}                \
	while (1)        \
		;

struct clisten_thread {
	int port;
	int max_work_req;
	struct ibv_cq *send_cq;
	struct ibv_cq *recv_cq;
	char *rdma_dev_name;
	struct ibv_context *rdma_context;
	struct ibv_comp_channel *rdma_channel;
};

struct clisten_completion_cnxt {
	volatile int completion_flag;
};

void clisten_completion_notify(v)
{
}

static struct ibv_context *clisten_get_rdma_device_context(struct clisten_thread *clisten_thread)
{
	int num_devices = 0;
	struct ibv_context **dev_list = rdma_get_devices(&num_devices);
	struct ibv_context *rdma_dev = NULL;

	if (num_devices < 1) {
		log_fatal("No RDMA device found. Exiting..");
		_exit(EXIT_FAILURE);
	}

	if (NULL == clisten_thread->rdma_dev_name) {
		log_info("Using default RDMA device %s", dev_list[0]->device->name);
		return dev_list[0];
	}

	for (int i = 0; i < num_devices; ++i)
		if (!strncmp(dev_list[i]->device->name, clisten_thread->rdma_dev_name,
			     strlen(clisten_thread->rdma_dev_name))) {
			rdma_dev = dev_list[i];
			break;
		}

	if (!rdma_dev) {
		log_fatal("Cannot find RDMA device %s", clisten_thread->rdma_dev_name);
		_exit(EXIT_FAILURE);
	}

	rdma_free_devices(dev_list);

	return rdma_dev;
}

clisten_thread_t clisten_create(char *rdma_dev_name)
{
	clisten_thread_t clistener = calloc(1UL, sizeof(struct clisten_thread));
	clistener->rdma_dev_name = strdup(rdma_dev_name);
	clistener->rdma_context = clisten_get_rdma_device_context(clistener);
	clistener->rdma_channel = ibv_create_comp_channel(clistener->rdma_context);
	clistener->send_cq =
		ibv_create_cq(clistener->rdma_context, CLISTEN_CQ_SIZE, (void *)clistener, clistener->rdma_channel, 0);
	clistener->recv_cq =
		ibv_create_cq(clistener->rdma_context, CLISTEN_CQ_SIZE, (void *)clistener, clistener->rdma_channel, 0);

	return clistener;
}

#define CLISTEN_PORT_BUF_SIZE 16
void *clisten_thread(void *args)
{
	struct clisten_thread *clistener = (struct clisten_thread *)args;

	pthread_setname_np(pthread_self(), "clistener");

	log_info("Starting listener for new rdma connections thread at port %d", clistener->port);

	struct ibv_qp_init_attr qp_init_attr = { 0 };
	qp_init_attr.cap.max_send_wr = qp_init_attr.cap.max_recv_wr = clistener->max_work_req;
	qp_init_attr.cap.max_send_sge = qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_inline_data = 16;
	qp_init_attr.sq_sig_all = 1;
	qp_init_attr.qp_type = IBV_QPT_RC;

	struct rdma_addrinfo hints = { 0 };
	struct rdma_addrinfo *res = NULL;

	char port[CLISTEN_PORT_BUF_SIZE] = { 0 };
	snprintf(port, CLISTEN_PORT_BUF_SIZE, "%d", clistener->port);

	memset(&hints, 0, sizeof hints);
	hints.ai_flags = RAI_PASSIVE; // Passive side, awaiting incoming connections
	hints.ai_port_space = RDMA_PS_TCP; // Supports Reliable Connections
	int ret = rdma_getaddrinfo(NULL, port, &hints, &res);
	if (ret) {
		log_fatal("rdma_getaddrinfo: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}

	struct rdma_cm_id *listen_id = NULL;
	ret = rdma_create_ep(&listen_id, res, NULL, NULL);
	if (ret) {
		log_fatal("rdma_create_ep: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// Listen for incoming connections on available RDMA devices
	ret = rdma_listen(listen_id, 0); // called with backlog = 0
	if (ret) {
		log_fatal("rdma_listen: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	CLISTEN_LOOP_START
	//  Block until a new connection request arrives
	struct rdma_cm_id *request_id = NULL;
	struct rdma_cm_id *new_conn_id = NULL;
	ret = rdma_get_request(listen_id, &request_id);
	if (ret) {
		log_fatal("rdma_get_request: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	new_conn_id = request_id->event->id;
	serv_conn_t conn = serv_conn_create();
	qp_init_attr.send_cq = clistener->send_cq;
	qp_init_attr.send_cq = clistener->recv_cq;

	ibv_req_notify_cq(qp_init_attr.send_cq, 0);
	assert(qp_init_attr.send_cq);

	ret = rdma_create_qp(new_conn_id, NULL, &qp_init_attr);
	if (ret) {
		log_fatal("rdma_create_qp: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}

	int protocol_version;
	struct ibv_mr *recv_mr = rdma_reg_msgs(new_conn_id, &protocol_version, sizeof(protocol_version));
	struct clisten_completion_cnxt completion_cnxt = { 0 };
	msg_ctx.on_completion_callback = on_completion_notify;
	ret = rdma_post_recv(new_conn_id, &msg_ctx, &incoming_connection_type, sizeof(connection_type), recv_mr);
	if (ret) {
		log_fatal("rdma_post_recv: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// Accept incomming connection TODO look into rdma_conn_param a bit more
	ret = rdma_accept(new_conn_id, NULL);
	if (ret) {
		log_fatal("rdma_accept: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}

	// Block until client sends connection type
	// sem_wait(&msg_ctx.wait_for_completion);
	while (0 == msg_ctx.completion_flag)
		;
	rdma_dereg_mr(recv_mr);

	if (incoming_connection_type == CLIENT_TO_SERVER_CONNECTION) {
		incoming_connection_type = SERVER_TO_CLIENT_CONNECTION;
		log_info("We have a new client connection request");
	} else if (incoming_connection_type == MASTER_TO_REPLICA_CONNECTION) {
		incoming_connection_type = REPLICA_TO_MASTER_CONNECTION;
		log_debug("We have a new replica connection request");
	} else {
		log_fatal("bad connection type");
		exit(EXIT_FAILURE);
	}
	/*
   * Important note to future self: klist.h used for keeping
   * channels and connections used by spinning thread is NOT thread
   * safe. Patch: all operations adding new connections and
   * removing connections take place from the context of the
   * spinning thread
   */

	/* I assume global state for the connections is already kept in the
   * system?*/
	/*!!! follow this path to add this connection to the appropriate connection
   * list !!!*/
	crdma_server_create_connection_inuse(conn, channel,
					     incoming_connection_type); // TODO not sure if it's needed with rdma_cm
	conn->rdma_cm_id = new_conn_id;

	switch (conn->type) {
	case SERVER_TO_CLIENT_CONNECTION:
	case REPLICA_TO_MASTER_CONNECTION:
		conn->rdma_memory_regions = mrpool_allocate_memory_region(channel->dynamic_pool, new_conn_id);
		break;
	case MASTER_TO_REPLICA_CONNECTION:
		assert(0);
		break;
	default:
		log_fatal("bad connection type %d", conn->type);
		_exit(EXIT_FAILURE);
	}
	assert(conn->rdma_memory_regions);

	struct ibv_mr *send_mr =
		rdma_reg_msgs(new_conn_id, conn->rdma_memory_regions->remote_memory_region, sizeof(struct ibv_mr));

	// Receive memory region information
	conn->peer_mr = calloc(1UL, sizeof(struct ibv_mr));
	recv_mr = rdma_reg_msgs(new_conn_id, conn->peer_mr, sizeof(struct ibv_mr));
	msg_ctx.completion_flag = 0;
	ret = rdma_post_recv(new_conn_id, &msg_ctx, conn->peer_mr, sizeof(struct ibv_mr), recv_mr);
	if (ret) {
		log_fatal("rdma_post_recv: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	// Send memory region information
	ret = rdma_post_send(new_conn_id, NULL, conn->rdma_memory_regions->remote_memory_region, sizeof(struct ibv_mr),
			     send_mr, 0);
	if (ret) {
		log_fatal("rdma_post_send: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	// Block until client sends memory region information
	// sem_wait(&msg_ctx.wait_for_completion);
	while (0 == msg_ctx.completion_flag)
		;
	rdma_dereg_mr(send_mr);
	rdma_dereg_mr(recv_mr);

	conn->status = CONNECTION_OK;
	conn->qp = conn->rdma_cm_id->qp;
	conn->rendezvous = conn->rdma_memory_regions->remote_memory_buffer;
	/*zerop all rdma memory*/
	memset(conn->rdma_memory_regions->local_memory_buffer, 0x00, conn->rdma_memory_regions->memory_region_length);

	if (sem_init(&conn->congestion_control, 0, 0) != 0) {
		log_fatal("failed to initialize semaphore reason follows");
		perror("Reason: ");
	}
	conn->sleeping_workers = 0;

	conn->offset = 0;
	conn->worker_id = -1;
#ifdef CONNECTION_BUFFER_WITH_MUTEX_LOCK
	pthread_mutex_init(&conn->buffer_lock, NULL);
#else
	pthread_spin_init(&conn->buffer_lock, PTHREAD_PROCESS_PRIVATE);
#endif
	// Add connection to spinner's connection list
	pthread_mutex_lock(&my_server->spinner.conn_list_lock);
	add_last_in_simple_concurrent_list(my_server->spinner.conn_list, conn);
	conn->responsible_spin_list = my_server->spinner.conn_list;
	conn->responsible_spinning_thread_id = my_server->spinner.spinner_id;
	pthread_mutex_unlock(&my_server->spinner.conn_list_lock);

	log_debug("Built new connection successfully for Server at port %d", rdma_port);
	CLISTEN_LOOP_END
	return NULL;
}
