/* 
 * kcp_server.h
 */

#ifndef LIBSG_KCP_SERVER_H
#define	LIBSG_KCP_SERVER_H

#ifdef	__cplusplus
extern "C"
{
#endif

typedef struct sg_kcp_client_real sg_kcp_client_t; /*struct sg_kcp_client_real的真实定义请放在kcp_server.c中*/
typedef struct sg_kcp_server_real sg_kcp_server_t; /*struct sg_kcp_server_real的真实定义请放在kcp_server.c中*/

typedef void (*sg_kcp_server_on_open_func_t)(sg_kcp_client_t *client);
typedef void (*sg_kcp_server_on_close_func_t)(sg_kcp_client_t *client, int code, const char *reason);
typedef void (*sg_kcp_server_on_message_func_t)(sg_kcp_client_t *client, char *data, size_t len);

/* 初始化，返回0表示成功，如果不必要，可以撤销此接口 */
int sg_kcp_server_init(void);

sg_kcp_server_t *sg_kcp_server_open(
    const char                     *server_addr, 
    int                             server_port,
    int                             backlog,
    int                             max_conn_count,
    sg_kcp_server_on_open_func_t    on_open,
    sg_kcp_server_on_message_func_t on_message,
    sg_kcp_server_on_close_func_t   on_close
);

int sg_kcp_server_send_data(sg_kcp_client_t *, void *data, size_t size);

void sg_kcp_server_close_client(sg_kcp_client_t *);

/* 调用者需要释放返回的char * */
char *sg_kcp_server_get_client_addr(sg_kcp_client_t *);

/* 实际上就是调用了uv_run之类的接口，让线程保持阻塞，直至kcp_server结束时此接口才返回 */
void sg_kcp_server_run(sg_kcp_server_t *, int interval_ms);

void sg_kcp_server_close(sg_kcp_server_t *);

/* 限制对此客户端的发送速度 */
void sg_kcp_server_set_max_send_speed(sg_kcp_client_t *client, size_t kbps);

/* 如果不必要，可以撤销此接口 */
void sg_kcp_server_free(void);

#ifdef	__cplusplus
}
#endif

#endif	/* LIBSG_KCP_SERVER_H */
