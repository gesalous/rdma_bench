typedef struct clisten_thread *clisten_thread_t;

clisten_thread_t clisten_create(char *rdma_dev_name);
enum clisten_recv_type { CLISTEN_POLL_DIFFERENT_CQ = 1, CLISTEN_POLL_SAME_CQ };
