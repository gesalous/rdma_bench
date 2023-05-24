#include "server_conn.h"
#include <stdlib.h>

struct serv_conn {
	int qp_num;
};

serv_conn_t serv_conn_create(void)
{
	return calloc(1UL, sizeof(struct serv_conn));
}

int serv_conn_get_qp_num(serv_conn_t server_conn)
{
	return server_conn->qp_num;
}
