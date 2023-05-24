typedef struct serv_conn *serv_conn_t;

serv_conn_t serv_conn_create(void);

int serv_conn_get_qp_num(serv_conn_t server_conn);
