#include "tclient.h"
#include "../nida_conf.h"
#include <assert.h>
#include <infiniband/verbs.h>
#include <log.h>
#include <rdma_cma.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

struct tclient {
	struct ibv_context *rdma_cnxt;
	char *dev_name;
	struct ibv_comp_channel *comp_channel;
};

struct tclient_conn {
	struct rdma_cm_id *conn_id;
	int id;
};

tclient_t tclient_create(char *dev_name)
{
	tclient_t new_client = calloc(1UL, sizeof(struct tclient));
	new_client->rdma_cnxt = nida_get_rdma_dev_context(dev_name);
	new_client->dev_name = strdup(dev_name);
	new_client->comp_channel = ibv_create_comp_channel(new_client->rdma_cnxt);
	return new_client;
}

bool tclient_connect(tclient_t client, tclient_conn_t conn, char *hostname)
{
	struct ibv_qp_init_attr qp_init_attr = { 0 };

	qp_init_attr.cap.max_send_wr = qp_init_attr.cap.max_recv_wr = NIDA_CQ_SIZE;
	qp_init_attr.cap.max_send_sge = qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_inline_data = 16;
	qp_init_attr.sq_sig_all = 1;
	qp_init_attr.send_cq = qp_init_attr.recv_cq =
		ibv_create_cq(client->rdma_cnxt, NIDA_CQ_SIZE, (void *)conn, client->comp_channel, 0);
	ibv_req_notify_cq(qp_init_attr.send_cq, 0);
	assert(qp_init_attr.send_cq);
	qp_init_attr.qp_type = IBV_QPT_RC;

	struct rdma_addrinfo hints, *res;
	memset(&hints, 0, sizeof hints);
	hints.ai_port_space = RDMA_PS_TCP;

	char host[1024] = { 0 };
	strcpy(host, hostname);
	char *ip = host;
	char *port;
	char *colon;
	for (colon = host; *colon != ':'; ++colon)
		;
	*colon = '\0';
	port = colon + 1;

	log_info("Connecting to %s at port %s", ip, port);
	int ret = rdma_getaddrinfo(ip, port, &hints, &res);
	if (ret) {
		log_fatal("Cannot get addr: %s info,reason: %s\n", hostname, strerror(errno));
		_exit(EXIT_FAILURE);
	}

	struct rdma_cm_id *rdma_cm_id;
	// FIXME Need to use channel->pd here instead of NULL
	ret = rdma_create_ep(&rdma_cm_id, res, NULL, &qp_init_attr);
	if (ret) {
		log_fatal("rdma_create_ep: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	conn->conn_id = rdma_cm_id;

	// TODO Check the private data functionality of the connection parameters!!!
	struct rdma_conn_param conn_param = { 0 };
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.flow_control = 1;
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;
	int tries = 0;

	while (tries < 100) {
		ret = rdma_connect(rdma_cm_id, &conn_param);
		if (0 == ret)
			break;
		log_warn("rdma_connect failed reconnecting: %s", strerror(errno));
		usleep(50000);
		++tries;
	}
	if (ret) {
		log_fatal("rdma_connect failed: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}

	conn->peer_mr = calloc(1UL, sizeof(struct ibv_mr));
	struct ibv_mr *recv_mr = rdma_reg_msgs(rdma_cm_id, conn->peer_mr, sizeof(struct ibv_mr));
	struct rdma_message_context msg_ctx = { 0 };
	client_rdma_init_message_context(&msg_ctx, NULL);
	msg_ctx.on_completion_callback = on_completion_client;
	ret = rdma_post_recv(rdma_cm_id, &msg_ctx, conn->peer_mr, sizeof(struct ibv_mr), recv_mr);
	if (ret) {
		log_fatal("rdma_post_recv: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	struct ibv_mr *send_mr = rdma_reg_msgs(rdma_cm_id, &type, sizeof(type));
	ret = rdma_post_send(rdma_cm_id, NULL, &type, sizeof(type), send_mr, 0);
	if (ret) {
		log_fatal("rdma_post_send: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
	/*free(host_copy);*/
	switch (type) {
	case MASTER_TO_REPLICA_CONNECTION:
		log_debug("Remote side accepted created a new MASTER_TO_REPLICA_CONNECTION");
		conn->type = MASTER_TO_REPLICA_CONNECTION;
		conn->rdma_memory_regions = mrpool_allocate_memory_region(channel->dynamic_pool, rdma_cm_id);
		break;
	case CLIENT_TO_SERVER_CONNECTION:
		// log_info("Remote side accepted created a new
		// CLIENT_TO_SERVER_CONNECTION");
		conn->type = CLIENT_TO_SERVER_CONNECTION;
		conn->rdma_memory_regions = mrpool_allocate_memory_region(channel->dynamic_pool, rdma_cm_id);
		break;
	case REPLICA_TO_MASTER_CONNECTION:
	case SERVER_TO_CLIENT_CONNECTION:
		log_warn("Should not handle this kind of connection here");
		break;
	default:
		log_fatal("BAD connection type");
		_exit(EXIT_FAILURE);
	}
	conn->remaining_bytes_in_remote_rdma_region = conn->rdma_memory_regions->memory_region_length;
	conn->rendezvous = conn->rdma_memory_regions->remote_memory_buffer;

	// Block until server sends memory region information
	//sem_wait(&msg_ctx.wait_for_completion);
	while (0 == msg_ctx.completion_flag)
		;
	rdma_dereg_mr(send_mr);
	rdma_dereg_mr(recv_mr);

	send_mr = rdma_reg_msgs(rdma_cm_id, conn->rdma_memory_regions->remote_memory_region, sizeof(struct ibv_mr));

	// Send memory region information
	ret = rdma_post_send(rdma_cm_id, NULL, conn->rdma_memory_regions->remote_memory_region, sizeof(struct ibv_mr),
			     send_mr, 0);
	if (ret) {
		log_fatal("rdma_post_send: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}

	conn->status = CONNECTION_OK;
	/*zero all memory*/
	memset(conn->rdma_memory_regions->local_memory_buffer, 0x00, conn->rdma_memory_regions->memory_region_length);
	if (sem_init(&conn->congestion_control, 0, 0) != 0) {
		log_fatal("failed to initialize semaphore reason follows");
		perror("Reason: ");
	}
	conn->sleeping_workers = 0;
	// conn->pending_sent_messages = 0;
	// conn->pending_received_messages = 0;
	conn->offset = 0;
	conn->qp = conn->rdma_cm_id->qp;

#ifdef CONNECTION_BUFFER_WITH_MUTEX_LOCK
	pthread_mutex_init(&conn->buffer_lock, NULL);
	pthread_mutex_init(&conn->allocation_lock, NULL);
#else
	pthread_spin_init(&conn->buffer_lock, PTHREAD_PROCESS_PRIVATE);
#endif
	switch (conn->type) {
	case CLIENT_TO_SERVER_CONNECTION:
		log_debug("Initializing client communication circular buffer");
		conn->send_circular_buf =
			create_and_init_circular_buffer(conn->rdma_memory_regions->local_memory_buffer,
							conn->peer_mr->length, MESSAGE_SEGMENT_SIZE, SEND_BUFFER);
		conn->recv_circular_buf =
			create_and_init_circular_buffer(conn->rdma_memory_regions->remote_memory_buffer,
							conn->peer_mr->length, MESSAGE_SEGMENT_SIZE, RECEIVE_BUFFER);
		conn->reset_point = 0;
		/*Inform the server that you are a client, patch but now I am in a hurry*/
		// log_info("CLIENT: Informing server that I am a client and about my
		// control location\n");
		/*control info*/
		break;

	case MASTER_TO_REPLICA_CONNECTION:
		log_debug("Initializing master to replica communication circular buffer");
		conn->send_circular_buf =
			create_and_init_circular_buffer(conn->rdma_memory_regions->local_memory_buffer,
							conn->peer_mr->length, MESSAGE_SEGMENT_SIZE, SC_SEND_BUFFER);
		conn->recv_circular_buf =
			create_and_init_circular_buffer(conn->rdma_memory_regions->remote_memory_buffer,
							conn->peer_mr->length, MESSAGE_SEGMENT_SIZE, SC_RECEIVE_BUFFER);
		conn->reset_point = 0;
		break;

	default:
		conn->send_circular_buf = NULL;
		conn->recv_circular_buf = NULL;
		conn->reset_point = 0;
		crdma_add_connection_channel(channel, conn);
		conn->send_circular_buf = NULL;
		conn->recv_circular_buf = NULL;
		conn->reset_point = 0;
		crdma_add_connection_channel(channel, conn);
	}
	__sync_fetch_and_add(&channel->nused, 1);
}

tclient_conn_t tclient_create_conn(char *IP, int port)
{
	tclient_conn_t conn = calloc(1UL, sizeof(struct tclient_conn));
	return conn;
}
