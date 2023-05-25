#define _GNU_SOURCE
#include "gpoller.h"
#include "server_conn.h"
#include "tpoller_listen.h"
#include <assert.h>
#include <infiniband/verbs.h>
#include <log.h>
#include <numa.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdlib.h>
#include <unistd.h>
#include <uthash.h>

#define TPOLLER_MUTEX_INIT(L) pthread_mutex_init(L)
#define TPOLLER_MUTEX_LOCK(L) pthread_mutex_lock(L)
#define TPOLLER_MUTEX_UNLOCK(L) pthread_mutex_unlock(L)
#define TPOLLER_BUF_SIZE (256UL * 1024)
#define TPOLLER_CQ_SIZE 256
#define TPOLLER_LOOP_START do {
#define TPOLLER_LOOP_END \
	}                \
	while (1)        \
		;

struct tpoller_thread {
	pthread_mutex_t conn_map_lock;
	int port;
	int max_work_req;
	struct ibv_cq *send_cq;
	struct ibv_cq *recv_cq;
	char *rdma_dev_name;
	struct ibv_context *rdma_context;
	struct ibv_comp_channel *rdma_channel;
	struct tpoller_connection *conn_map;
};

typedef void (*tpoller_notify_t)(void *);
struct tpoller_completion_cnxt {
	volatile int completion_flag;
	tpoller_notify_t callback;
};

struct tpoller_connection {
	uint64_t num_bytes;
	struct ibv_mr *recv_mem_region;
	struct ibv_qp *qp;
	struct rdma_cm_id *conn_id;
	int qp_num;
	UT_hash_handle hh;
};

struct tpoller_connection *tpoller_create_conn(size_t comm_buf_size, struct rdma_cm_id *conn_id)
{
	struct tpoller_connection *conn = calloc(1UL, sizeof(*conn));
	conn->qp = conn_id->qp;

	char *comm_buffer = numa_alloc_local(comm_buf_size);
	conn->recv_mem_region = rdma_reg_write(conn_id, comm_buffer, comm_buf_size);
	conn->conn_id = conn_id;
	return conn;
}

void tpoller_completion_notify(void *args)
{
	struct tpoller_completion_cnxt *completion_cnxt = (struct tpoller_completion_cnxt *)args;
	completion_cnxt->completion_flag = 1;
}

tpoller_thread_t tpoller_create(char *rdma_dev_name)
{
	tpoller_thread_t tpoller = calloc(1UL, sizeof(struct tpoller_thread));
	tpoller->rdma_dev_name = strdup(rdma_dev_name);
	tpoller->rdma_context = tpoller_get_rdma_device_context(tpoller->rdma_dev_name);
	tpoller->rdma_channel = ibv_create_comp_channel(tpoller->rdma_context);
	tpoller->send_cq =
		ibv_create_cq(tpoller->rdma_context, TPOLLER_CQ_SIZE, (void *)tpoller, tpoller->rdma_channel, 0);
	tpoller->recv_cq =
		ibv_create_cq(tpoller->rdma_context, TPOLLER_CQ_SIZE, (void *)tpoller, tpoller->rdma_channel, 0);

	return tpoller;
}

#define TPOLLER_CQ_ENTRIES 16
typedef enum { TPOLLER_SEND_CQ = 1, TPOLLER_RECV_CQ } tpoller_cq_type_e;

static void tpoller_poll_cq(struct tpoller_thread *tpoller, tpoller_cq_type_e cq_type, bool detect_message)
{
	if (cq_type == TPOLLER_SEND_CQ && !tpoller->send_cq)
		return;

	if (!tpoller->recv_cq)
		return;

	struct ibv_wc wc[TPOLLER_CQ_ENTRIES];
	int rc = ibv_poll_cq(cq_type == TPOLLER_SEND_CQ ? tpoller->send_cq : tpoller->recv_cq, TPOLLER_CQ_ENTRIES, wc);

	if (rc < 0) {
		log_fatal("poll of completion queue failed!");
		_exit(EXIT_FAILURE);
	}
	for (int i = 0; i < rc; i++) {
		struct tpoller_completion_cnxt *msg_ctx = (struct tpoller_completion_cnxt *)wc[i].wr_id;

		if (wc[i].status != IBV_WC_SUCCESS) {
			log_fatal("Reason %s", ibv_wc_status_str(wc[i].status));
		}

		if (msg_ctx && msg_ctx->callback) {
			msg_ctx->callback(msg_ctx);
		}
		if (!detect_message)
			continue;
		/*figure out which qp caused the completion event*/
		struct tpoller_connection *conn;
		TPOLLER_MUTEX_LOCK(&tpoller->conn_map_lock);
		HASH_FIND_PTR(tpoller->conn_map, &wc[i].qp_num, conn);
		if (!conn) {
			log_fatal("Got a receive event from a connection that does not exist?");
			_exit(EXIT_FAILURE);
		}
		TPOLLER_MUTEX_UNLOCK(&tpoller->conn_map_lock);
		/*new message starts at w[c]*/
		conn->num_bytes += wc[i].byte_len;
	}
}

void *tpoller_worker(void *args)
{
	struct tpoller_thread *tpoller = (struct tpoller_thread *)args;
	TPOLLER_LOOP_START
	tpoller_poll_cq(tpoller, TPOLLER_SEND_CQ, false);
	tpoller_poll_cq(tpoller, TPOLLER_RECV_CQ, true);
	TPOLLER_LOOP_END
}

#define TPOLLER_PORT_BUF_SIZE 16
void *tpoller_listener(void *args)
{
	struct tpoller_thread *tpoller = (struct tpoller_thread *)args;

	pthread_setname_np(pthread_self(), "tpoller");

	log_info("Starting listener for new rdma connections thread at port %d", tpoller->port);

	struct ibv_qp_init_attr qp_init_attr = { 0 };
	qp_init_attr.cap.max_send_wr = qp_init_attr.cap.max_recv_wr = tpoller->max_work_req;
	qp_init_attr.cap.max_send_sge = qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_inline_data = 16;
	qp_init_attr.sq_sig_all = 1;
	qp_init_attr.qp_type = IBV_QPT_RC;

	struct rdma_addrinfo hints = { 0 };
	struct rdma_addrinfo *res = NULL;

	char port[TPOLLER_PORT_BUF_SIZE] = { 0 };
	snprintf(port, TPOLLER_PORT_BUF_SIZE, "%d", tpoller->port);

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

	TPOLLER_LOOP_START
	//  Block until a new connection request arrives
	struct rdma_cm_id *request_id = NULL;
	struct rdma_cm_id *new_conn_id = NULL;
	ret = rdma_get_request(listen_id, &request_id);
	if (ret) {
		log_fatal("rdma_get_request: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	new_conn_id = request_id->event->id;
	qp_init_attr.send_cq = tpoller->send_cq;
	qp_init_attr.send_cq = tpoller->recv_cq;

	ibv_req_notify_cq(qp_init_attr.send_cq, 0);
	assert(qp_init_attr.send_cq);

	ret = rdma_create_qp(new_conn_id, NULL, &qp_init_attr);
	if (ret) {
		log_fatal("rdma_create_qp: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}

	int protocol_version = 0;
	struct ibv_mr *recv_mr = rdma_reg_msgs(new_conn_id, &protocol_version, sizeof(protocol_version));
	struct tpoller_completion_cnxt completion_cnxt = { 0 };
	completion_cnxt.callback = tpoller_completion_notify;
	ret = rdma_post_recv(new_conn_id, &completion_cnxt, &protocol_version, sizeof(protocol_version), recv_mr);
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
	while (0 == completion_cnxt.completion_flag)
		;
	log_debug("We have a new connection request protocol version requested is: %d", protocol_version);
	rdma_dereg_mr(recv_mr);

	struct tpoller_connection *conn = tpoller_create_conn(TPOLLER_BUF_SIZE, new_conn_id);
	TPOLLER_MUTEX_LOCK(&tpoller->conn_map_lock);
	HASH_ADD_PTR(tpoller->conn_map, qp_num, conn);
	TPOLLER_MUTEX_UNLOCK(&tpoller->conn_map_lock);

	log_debug("All good sending info about region to send me messages");
	struct ibv_mr *send_mr = rdma_reg_msgs(new_conn_id, conn->recv_mem_region, sizeof(struct ibv_mr));

	completion_cnxt.completion_flag = 0;
	ret = rdma_post_send(new_conn_id, &completion_cnxt, conn->recv_mem_region, sizeof(struct ibv_mr), send_mr, 0);
	if (ret) {
		log_fatal("rdma_post_send: %s", strerror(errno));
		_exit(EXIT_FAILURE);
	}
	// Block until client sends memory region information
	// sem_wait(&msg_ctx.wait_for_completion);
	while (0 == completion_cnxt.completion_flag)
		;
	rdma_dereg_mr(send_mr);

	TPOLLER_LOOP_END
	return NULL;
}
