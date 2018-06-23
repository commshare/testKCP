#include <sys/time.h>
#include "uv.h"
#include "ikcp.h"
#include "kcp.h"

typedef unsigned char bool;
#define true    1
#define false   0

#define OK      0
#define ERROR (-1)

#define SG_ASSERT(exp, prmpt)          if (exp) {} else { printf("%d:%s() " prmpt "\n", __LINE__, __FUNCTION__); return; }
#define SG_ASSERT_RET(exp, prmpt, ret) if (exp) {} else { printf("%d:%s() " prmpt "\n", __LINE__, __FUNCTION__); return(ret); }
#define SG_ASSERT_BRK(exp, prmpt)      if (exp) {} else { printf("%d:%s() " prmpt "\n", __LINE__, __FUNCTION__); break; }

/* 定义越大越接近真实速度 */
#define _SMOOTH_SPEED_SIZE 16
/* 多长统计一次发送速度 */
#define _CALC_SPEED_INTERVAL 2000
/* as conn_t in kcpuv */
typedef struct sg_kcp_real
{
    uint32_t            conv;
    uv_loop_t         * loop;
    uv_udp_t            udp;
    uv_timer_t          timer;
    uv_idle_t           idle;
    struct sockaddr     addr;
    ikcpcb            * kcp;
    IUINT32             next_update;
    bool                to_close;
    sg_kcp_on_open      on_open;
    sg_kcp_on_message   on_message;
    sg_kcp_on_send_done on_done;
    sg_kcp_on_close     on_close;

    // 统计速率相关,采用滑窗试计算
    uint64_t            last_time; /* 上一次统计时间 */
    int                 last_send_byte;
    int                 max_speed_limit;
    int                 head, tail;
    int                 last_speed[_SMOOTH_SPEED_SIZE];
    double              current_speed;
    void * data;
}sg_kcp_t;

typedef struct send_req_s
{
	uv_udp_send_t req;
	uv_buf_t buf;
    sg_kcp_t* client;
}send_req_t;

static void on_uv_alloc_buffer(uv_handle_t* handle, size_t size, uv_buf_t* buf);
static void on_client_recv_udp(
    uv_udp_t                * handle,
    ssize_t                   nread,
    const uv_buf_t          * rcvbuf,
    const struct sockaddr   * addr,
    unsigned int              flags);
/* for libuv */
static void on_uv_close_done(uv_handle_t* handle);
/* for kcp callback */
static int on_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user);
/* for libuv callback */
static void on_udp_send_done(uv_udp_send_t* req, int status);

/* for update client send speed */
static void sg_kcp_update_speed(sg_kcp_t* client, uint64_t now);

/* for libuv */
static void on_uv_alloc_buffer(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
	buf->len = (unsigned long)size;
	buf->base = malloc(size);
}

/* for libuv */
static void on_client_recv_udp(
    uv_udp_t                * handle,
    ssize_t                   nread,
    const uv_buf_t          * rcvbuf,
    const struct sockaddr   * addr,
    unsigned int              flags)
{
	sg_kcp_t * client = NULL;

	/*printf("recv udp %d\n", nread);*/

    do
    {
        /*SG_ASSERT_BRK(nread > 0, "no data recv");*/
        if (nread <= 0) break;

    	client = (sg_kcp_t *)(handle->data);
        /*memcpy(&(client->addr), addr, sizeof(struct sockaddr));*/
        client->next_update = 0; /* clear next update */
    	ikcp_input(client->kcp, rcvbuf->base, (long)nread);
    } while (0);

	free(rcvbuf->base);
}

/* for libuv */
static void on_uv_close_done(uv_handle_t* handle)
{

}



/* for libuv */
static void on_uv_timer_cb(uv_timer_t* handle)
{
    sg_kcp_t * client = handle->data;
    IUINT32 now = 0;

    /*printf("update %d\n", client->conv);*/
    
    /* update ikcp */
    now = (IUINT32)uv_now(client->loop);
    sg_kcp_update_speed(client, now);
    if (now >= client->next_update)
    {
        ikcp_update(client->kcp, now);
        client->next_update = ikcp_check(client->kcp, now);
    }

    if (client->to_close)
    {
        sg_kcp_close(client);
    }
}

static void on_uv_idle_cb(uv_idle_t* handle)
{
    int ret = 0;
    int len = 0;
    char * data = NULL;
    sg_kcp_t * client = NULL;

    client = handle->data;

    /* recv data from kcp level */
    len = ikcp_peeksize(client->kcp);
	if (len < 0)
    {
		return;
	}

	data = (char *)malloc(len);
	ret = ikcp_recv(client->kcp, data, len);
	if (ret < 0)
    {
        printf("recv ikcp failed, ret = %d\n", ret);
		free(data);
		return;
	}

    client->on_message(client, data, len);

    free(data);
    data = NULL;
}

/* for kcp callback */
static int on_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user)
{
	sg_kcp_t * client = (sg_kcp_t*)user;
    int ret = -1;
    send_req_t * req = NULL;

    /*printf("udp send: %d\n", len);*/

    do
    {
        // 限速
        if (client->max_speed_limit > 0 && client->current_speed > client->max_speed_limit)
            return ret;
    	req = (send_req_t *)malloc(sizeof(send_req_t));
        SG_ASSERT_BRK(NULL != req, "create send_req_t failed");

        memset(req, 0, sizeof(send_req_t));

    	req->buf.base = malloc(sizeof(char) * len);
        SG_ASSERT_BRK(NULL != req->buf.base, "create buf failed");
    	req->buf.len = len;
        req->client = client;

    	memcpy(req->buf.base, buf, len);

    	ret = uv_udp_send((uv_udp_send_t*)req, &(client->udp), &req->buf, 1, &client->addr, on_udp_send_done);
    	if (ret < 0)
        {
    		free(req->buf.base); /* TODO: ensure free */
    		free(req); /* TODO: ensure free ? */
    		return -1;
    	}

    	return ret;
    } while (0);

    if (NULL != req)
    {
        if (NULL != req->buf.base)
        {
            free(req->buf.base);
            req->buf.base = NULL;
        }
        
        free(req);
        req = NULL;
    }

	return ret;
}

/* for libuv callback */
static void on_udp_send_done(uv_udp_send_t* req, int status)
{
	send_req_t * send_req = (send_req_t *)req;
    send_req->client->on_done(send_req->client, status);
    send_req->client->last_send_byte += send_req->buf.len;
	free(send_req->buf.base); /* TODO: ensure free*/
	free(send_req);	/** TODO: ensure free */
}


int sg_kcp_init()
{
    return 0;
}

sg_kcp_t *sg_kcp_open(
    const char *server_addr, int server_port,
    sg_kcp_on_open     on_open,
    sg_kcp_on_message  on_message,
    sg_kcp_on_send_done on_done,
    sg_kcp_on_close    on_close)
{
    sg_kcp_t * client = NULL;
    struct timeval tv;
    struct sockaddr_in addr;
	int ret = -1;

    do
    {
        /* create the client object */
        client = malloc(sizeof(sg_kcp_t));
        SG_ASSERT_BRK(NULL != client, "create client failed");

        client->on_open     = on_open;
        client->on_message  = on_message;
        client->on_done     = on_done;
        client->on_close    = on_close;

        gettimeofday(&tv, NULL);
        client->conv = (uint32_t)(tv.tv_sec*1000000 + tv.tv_usec); /* TODO: alloc the conversation id */

    	/* get address */
    	ret = uv_ip4_addr(server_addr, server_port, &addr);
    	SG_ASSERT_BRK(ret >= 0, "get address failed");
        memcpy(&(client->addr), &addr, sizeof(struct sockaddr));

        /* create the kcp object */
    	client->kcp = ikcp_create(client->conv, (void*)client);
    	SG_ASSERT_BRK(NULL != client->kcp, "create ikcp failed");

    	client->kcp->output = on_kcp_output;

    	client->loop = uv_default_loop();

        client->on_open(client);

        return client;
    } while (0);

    if (NULL != client)
    {
        free(client);
        client = NULL;
    }
    return client;
}

int sg_kcp_loop(sg_kcp_t *client, int interval_ms)
{
    int ret = -1;
    
    ret = ikcp_nodelay(client->kcp, 1, interval_ms, 2, 1);
    SG_ASSERT_RET(ret >= 0, "ikcp nodelay failed", ERROR);

    /* init udp */
	ret = uv_udp_init(client->loop, &(client->udp));
    SG_ASSERT_RET(ret >= 0, "init udp failed", ERROR);
    client->udp.data = client;
	ret = uv_udp_recv_start(&(client->udp), on_uv_alloc_buffer, on_client_recv_udp);
	SG_ASSERT_RET(ret >= 0, "start udp recv failed", ERROR);

    /* start a timer for kcp update and receiving */
    ret = uv_timer_init(client->loop, &(client->timer));
    SG_ASSERT_RET(ret >= 0, "init timer failed", ERROR);
    client->timer.data = client; /* link client pointer to timer */
    ret = uv_timer_start(&(client->timer), on_uv_timer_cb, interval_ms, interval_ms);
    SG_ASSERT_RET(ret >= 0, "start timer failed", ERROR);

    /* reg idle for data process */
    ret = uv_idle_init(client->loop, &(client->idle));
    SG_ASSERT_RET(ret >= 0, "init idle failed", ERROR);
    client->idle.data = client;
    ret = uv_idle_start(&(client->idle), on_uv_idle_cb);
    SG_ASSERT_RET(ret >= 0, "start idle failed", ERROR);

    /* enter loop */
    uv_run(client->loop, UV_RUN_DEFAULT);

    return OK;
}

int sg_kcp_send(sg_kcp_t *client, const void * data, uint64_t size)
{
    client->next_update = 0; /* clear next update */
    return ikcp_send(client->kcp, data, size);
}

uint32_t sg_kcp_now(sg_kcp_t * client)
{
    return (uint32_t)uv_now(client->loop);
}

void sg_kcp_close(sg_kcp_t *client)
{
    if (ikcp_waitsnd(client->kcp) > 0 || ikcp_peeksize(client->kcp) > 0)
    {
        client->to_close = true; /* mark for close later */
        return;
    }

    client->on_close(client, 0, "");

    ikcp_release(client->kcp);

    uv_timer_stop(&(client->timer));
    uv_idle_stop(&(client->idle));

    uv_close((uv_handle_t*)&(client->udp), on_uv_close_done);
   
    free(client);
}

void sg_kcp_free(void)
{

}

/* 跟新发送速度 */
static void sg_kcp_update_speed(sg_kcp_t* client, uint64_t now)
{
    if (client->max_speed_limit <=0 )
        return;

    if (!client->last_time) {
        // start to record
        client->last_time = now;
        return;
    }
    else {
        // update the last_speed, every tow second
        uint64_t msecond = now - client->last_time;
        if (msecond > _CALC_SPEED_INTERVAL) {
            // 平滑处理
            int count = 0;
            double speed = client->last_send_byte * 1.0 / msecond * 1000;

            // 加入队列
            client->last_speed[client->tail++] = speed;
            client->tail &= (_SMOOTH_SPEED_SIZE-1);
            if (client->tail == client->head)
                client->head = (client->head + 1)&(_SMOOTH_SPEED_SIZE-1);

            speed = 0;
            for (int i = client->head; i != client->tail; i=(i+1)&(_SMOOTH_SPEED_SIZE-1)) {
                speed += client->last_speed[i];
                count += 1;
            }
            speed /= count;
            client->current_speed = speed;
            printf("send %lf kib/s %d\n", speed/1024, count);
            client->last_time = now;
            client->last_send_byte = 0;
        }

    }
}

void sg_kcp_set_max_send_speed(sg_kcp_t *client, size_t kbps)
{
    client->max_speed_limit = kbps*1024;
    client->last_send_byte = 0;
    client->last_time = 0;
    if (!kbps){
        client->tail = client->head = 0;
        memset(client->last_speed, 0, sizeof(client->last_speed));
        client->current_speed = 0;
    }

}
