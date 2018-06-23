#include <stdio.h>
#include <string.h>
#include "uv.h"
#include "ikcp.h"
#include "kcp_server.h"

typedef unsigned char bool;
#define true    1
#define false   0

#define OK      0
#define ERROR (-1)

#define SG_ASSERT(exp, prmpt)          if (exp) {} else { printf("%d:%s() " prmpt "\n", __LINE__, __FUNCTION__); return; }
#define SG_ASSERT_RET(exp, prmpt, ret) if (exp) {} else { printf("%d:%s() " prmpt "\n", __LINE__, __FUNCTION__); return(ret); }
#define SG_ASSERT_BRK(exp, prmpt)      if (exp) {} else { printf("%d:%s() " prmpt "\n", __LINE__, __FUNCTION__); break; }

typedef struct DBLL_Entry_T
{
    struct DBLL_Entry_T *next;  /**< Next entry in list */
    struct DBLL_Entry_T *prev;  /**< Previous entry in list */
}
DBLL_Entry_T;

typedef struct DBLL_List_T
{
  DBLL_Entry_T *first;  /**< Pointer to first entry in list; NULL if list is empty. */
  DBLL_Entry_T *last;   /**< Pointer to last entry in list; NULL if list is empty. */
  uint32_t count;       /**< Number of entries in list. */
}
DBLL_List_T;

void DBLL_Init(DBLL_List_T * pLL);
void DBLL_Destroy(DBLL_List_T * pLL);
void DBLL_Add_Entry(DBLL_List_T * pLL, DBLL_Entry_T * pEntry);
void DBLL_Insert_After(DBLL_List_T * pLL, DBLL_Entry_T * pEntry, DBLL_Entry_T * pNeighbor);
void *DBLL_Remove_Entry(DBLL_List_T * pLL, DBLL_Entry_T * pEntry);
uint32_t DBLL_Get_Count(DBLL_List_T const *pLL);
void *DBLL_Get_First(DBLL_List_T const *pLL);
void *DBLL_Get_Next(DBLL_Entry_T const *pEntry);
void * DBLL_Find(DBLL_List_T * pLL, uint32_t conv);

/* 定义越大越接近真实速度 */
#define _SMOOTH_SPEED_SIZE 16
/* 多长统计一次发送速度 */
#define _CALC_SPEED_INTERVAL 2000

typedef struct sg_kcp_client_real
{
    DBLL_Entry_T        le;
    IUINT32             conv;
    uv_loop_t         * loop;
    uv_udp_t          * udp;
    uv_timer_t          timer;
    uv_idle_t           idle;
    struct sockaddr     addr;
    ikcpcb            * kcp;
    IUINT32             next_update;
    int                 to_close;
    sg_kcp_server_t   * server;
    // 统计速率相关
    uint64_t            last_time; /* 上一次统计时间 */
    int                 last_send_byte;
    int                 max_speed_limit;
    int                 head, tail;
    int                 last_speed[_SMOOTH_SPEED_SIZE];
    double                 current_speed;
    sg_kcp_server_on_open_func_t    on_open;
    sg_kcp_server_on_message_func_t on_message;
    sg_kcp_server_on_close_func_t   on_close;
    void * data;
}sg_kcp_client_t;

typedef struct sg_kcp_server_real
{
    uv_loop_t         * loop;
    uv_udp_t            udp;
    uv_timer_t          timer;
    uv_idle_t           idle;
    struct sockaddr     addr;
    int                 backlog;
    int                 max_conn;
    int                 interval;
    sg_kcp_server_on_open_func_t    on_open;
    sg_kcp_server_on_message_func_t on_message;
    sg_kcp_server_on_close_func_t   on_close;
    DBLL_List_T         clients;
    void * data;
}sg_kcp_server_t;

typedef struct send_req_s
{
	uv_udp_send_t req;
	uv_buf_t buf;
    sg_kcp_client_t* client;
}send_req_t;


static void on_uv_alloc_buffer(uv_handle_t* handle, size_t size, uv_buf_t* buf);
static int on_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user);
static void on_udp_send_done(uv_udp_send_t* req, int status);
static void on_server_recv_udp(uv_udp_t* handle,
    ssize_t nread,
    const uv_buf_t* rcvbuf,
    const struct sockaddr* addr,
    unsigned flags);
static void on_uv_timer_cb(uv_timer_t* handle);
static void on_uv_idle_cb(uv_idle_t* handle);
static void on_uv_close_done(uv_handle_t* handle);
static void sg_kcp_client_update_speed(sg_kcp_client_t* client, uint64_t now);

/* for libuv */
static void on_uv_alloc_buffer(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
	buf->len = (unsigned long)size;
	buf->base = malloc(sizeof(char) * size);
}

/* for kcp callback */
static int on_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user)
{
	sg_kcp_client_t * client = (sg_kcp_client_t *)user;
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

        req->client = client;
    	req->buf.base = malloc(sizeof(char) * len);
        SG_ASSERT_BRK(NULL != req->buf.base, "create buf failed");
    	req->buf.len = len;

    	memcpy(req->buf.base, buf, len);

    	ret = uv_udp_send((uv_udp_send_t*)req, client->udp, &req->buf, 1, &client->addr, on_udp_send_done);
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

static void on_udp_send_done(uv_udp_send_t* req, int status)
{
	send_req_t * send_req = (send_req_t *)req;
    send_req->client->last_send_byte += send_req->buf.len;
	free(send_req->buf.base); /* TODO: ensure free*/
	free(send_req);	/** TODO: ensure free */
}

static void on_server_recv_udp(uv_udp_t* handle,
    ssize_t nread,
    const uv_buf_t* rcvbuf,
    const struct sockaddr* addr,
    unsigned flags)
{
    IUINT32 conv = 0;
	int ret = 0;
    sg_kcp_server_t * server = handle->data;
    sg_kcp_client_t * client = NULL;

    /*printf("recv udp %d\n", nread);*/

    do
    {
        /*SG_ASSERT_BRK(nread > 0, "no data read");*/
        if (nread <= 0) break;

        ret = ikcp_get_conv(rcvbuf->base, (long)nread, (IUINT32 *)&conv);
        SG_ASSERT_BRK(1 == ret, "get conv by ikcp failed");

        /* TODO: find client */
        client = DBLL_Find(&(server->clients), conv);
        if (NULL == client)
        {
            if (DBLL_Get_Count(&(server->clients)) >= server->max_conn)
            {
                printf("meet max connection, ignore\n");
                break;
            }
        
            client = (sg_kcp_client_t *)malloc(sizeof(sg_kcp_client_t));
            SG_ASSERT_BRK(NULL != client, "create client failed");
            /* link client */
            DBLL_Add_Entry(&(server->clients), (DBLL_Entry_T *)client);
            client->conv        = conv;
            client->loop        = server->loop;
            client->udp         = &(server->udp);
            client->on_open     = server->on_open;
            client->on_message  = server->on_message;
            client->on_close    = server->on_close;
            client->server      = server;
            memcpy(&(client->addr), addr, sizeof(struct sockaddr));

            client->kcp = ikcp_create(conv, (void*)client);
            SG_ASSERT_BRK(client->kcp != NULL, "create ikcp failed");

        	client->kcp->output = on_kcp_output;

        	ret = ikcp_nodelay(client->kcp, 1, server->interval, 2, 1);

        	printf("conn from %u\n", (unsigned int)client->conv);

        	client->on_open(client);
        }

        client->next_update = 0; /* clear next update */
        ikcp_input(client->kcp, rcvbuf->base, nread);
    }
    while (0);

    free(rcvbuf->base);
}

static void on_uv_timer_cb(uv_timer_t* handle)
{
    sg_kcp_server_t * server = handle->data;
    sg_kcp_client_t * client = NULL;
    sg_kcp_client_t * prev = NULL;
    IUINT32 now = (IUINT32)uv_now(server->loop);

    /*printf("update %d\n", DBLL_Get_Count(&(server->clients)));*/

    /* traverse client list */
    client = DBLL_Get_First(&(server->clients));
    while (NULL != client)
    {
        /*printf("update %d\n", client->conv);*/
        sg_kcp_client_update_speed(client, now);
        if (now >= client->next_update)
        {
            ikcp_update(client->kcp, now);
            client->next_update = ikcp_check(client->kcp, now);
        }

        prev = client;
        client = DBLL_Get_Next((DBLL_Entry_T const *)client);

        if (prev->to_close)
        {
            sg_kcp_server_close_client(prev);
        }
    }
}


static void on_uv_idle_cb(uv_idle_t* handle)
{
    int ret = 0;
    int len = 0;
    char * data = NULL;
    sg_kcp_server_t * server = handle->data;
    sg_kcp_client_t * client = NULL;

    /* traverse client list */
    client = DBLL_Get_First(&(server->clients));
    while (NULL != client)
    {
        len = ikcp_peeksize(client->kcp);
    	if (len < 0)
        {
    		return;
    	}

    	data = (char *)malloc(len);
    	ret = ikcp_recv(client->kcp, data, len);
    	if (ret < 0)
        {
    		free(data);
    		return;
    	}

        client->on_message(client, data, len);

        free(data);
        data = NULL;

        client = DBLL_Get_Next((DBLL_Entry_T const *)client);
    }
}

static void on_uv_close_done(uv_handle_t* handle)
{

}


int sg_kcp_server_init(void)
{
    return 0;
}

sg_kcp_server_t *sg_kcp_server_open(
    const char *server_addr, int server_port,
    int                             backlog,
    int                             max_conn_count,
    sg_kcp_server_on_open_func_t    on_open,
    sg_kcp_server_on_message_func_t on_message,
    sg_kcp_server_on_close_func_t   on_close
)
{
    sg_kcp_server_t * server = NULL;
    struct sockaddr_in addr;
    int ret = 0;

    do
    {
        /* create the client object */
        server = (sg_kcp_server_t *)malloc(sizeof(sg_kcp_server_t));
        SG_ASSERT_BRK(NULL != server, "create sg_kcp_server_t");

        server->backlog     = backlog;
        server->max_conn    = max_conn_count;
        server->on_open     = on_open;
        server->on_message  = on_message;
        server->on_close    = on_close;

        DBLL_Init(&(server->clients));

    	/* get address */
    	ret = uv_ip4_addr(server_addr, server_port, &addr);
      	SG_ASSERT_BRK(ret >= 0, "get address failed");
        memcpy(&(server->addr), &addr, sizeof(struct sockaddr));

    	server->loop = uv_default_loop();

        return server;
    }
    while (0);

    if (NULL != server)
    {
        free(server);
        server = NULL;
    }

    return server;
}

int sg_kcp_server_send_data(sg_kcp_client_t * client, void *data, size_t size)
{
    client->next_update = 0; /* clear next update */
    return ikcp_send(client->kcp, data, size);
}

void sg_kcp_server_close_client(sg_kcp_client_t * client)
{
    sg_kcp_server_t * server = client->server;

    if (ikcp_waitsnd(client->kcp) > 0 || ikcp_peeksize(client->kcp) > 0)
    {
        client->to_close = true; /* mark for close later */
        return;
    }

    client->on_close(client, OK, "");
    
    ikcp_release(client->kcp);
    DBLL_Remove_Entry(&(server->clients), (DBLL_Entry_T *)client);
    free(client);
}

char *sg_kcp_server_get_client_addr(sg_kcp_client_t * client)
{
    char * addr = NULL;

    addr = malloc(256);

    uv_ip4_name((const struct sockaddr_in*)&(client->addr), addr, 256);

    return addr;
}

void sg_kcp_server_run(sg_kcp_server_t * server, int interval_ms)
{
    int ret = 0;

    server->interval = interval_ms;

    /* init udp */
	ret = uv_udp_init(server->loop, &(server->udp));
    SG_ASSERT(ret >= 0, "init udp failed");
    server->udp.data = server;
    ret = uv_udp_bind(&(server->udp), &(server->addr), 0);
	SG_ASSERT(ret >= 0, "bind udp failed");
	ret = uv_udp_recv_start(&(server->udp), on_uv_alloc_buffer, on_server_recv_udp);
	SG_ASSERT(ret >= 0, "start udp recv failed");

    /* start a timer for kcp update and receiving */
    ret = uv_timer_init(server->loop, &(server->timer));
    SG_ASSERT(ret >= 0, "init timer failed");
    server->timer.data = server; /* link client pointer to timer */
    ret = uv_timer_start(&(server->timer), on_uv_timer_cb, interval_ms, interval_ms);
    SG_ASSERT(ret >= 0, "start timer failed");

    /* reg idle for data process */
    ret = uv_idle_init(server->loop, &(server->idle));
    SG_ASSERT(ret >= 0, "init idle failed");
    server->idle.data = server;
    ret = uv_idle_start(&(server->idle), on_uv_idle_cb);
    SG_ASSERT(ret >= 0, "start idle failed");

    /* network run */
    uv_run(server->loop, UV_RUN_DEFAULT);
}

void sg_kcp_server_close(sg_kcp_server_t * server)
{
    sg_kcp_client_t * client = NULL;
    sg_kcp_client_t * to_del = NULL;

    /* traverse client list */
    client = DBLL_Get_First(&(server->clients));
    while (NULL != client)
    {
        to_del = client;
        client = DBLL_Get_Next((DBLL_Entry_T *)client);

        sg_kcp_server_close_client(to_del);
    }

    uv_close((uv_handle_t*)&(server->udp), on_uv_close_done);

    DBLL_Destroy(&(server->clients));

    free(server);
}

static void sg_kcp_client_update_speed(sg_kcp_client_t* client, uint64_t now)
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

void sg_kcp_server_set_max_send_speed(sg_kcp_client_t *client, size_t kbps)
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


void sg_kcp_server_free(void)
{}


void DBLL_Init(DBLL_List_T * pLL)
{
    SG_ASSERT(pLL != NULL, "DBLL_Init cannot be called with a NULL pLL pointer");

    memset(pLL, 0, sizeof(*pLL));
}

void DBLL_Destroy(DBLL_List_T * pLL)
{
    SG_ASSERT(pLL != NULL, "DBLL_Destroy cannot be called with a NULL pLL pointer");

    memset(pLL, 0, sizeof(*pLL));
}

void DBLL_Add_Entry(DBLL_List_T * pLL, DBLL_Entry_T * pEntry)
{
   SG_ASSERT(pLL != NULL, "DBLL_Add_Entry cannot be called with a NULL pLL pointer");
   SG_ASSERT(pEntry != NULL, "DBLL_Add_Entry cannot be called with a NULL pEntry pointer");
   SG_ASSERT((NULL == pLL->first) || (NULL == pLL->first->prev), "DBLL_Add_Entry cannot be called with a NULL first or prev pointer");
   SG_ASSERT((NULL == pLL->last) || (NULL == pLL->last->next), "DBLL_Add_Entry cannot be called with a NULL last or next pointer");

   /* insert at the end of the list */
   DBLL_Insert_After(pLL, pEntry, NULL);
}

void DBLL_Insert_After(DBLL_List_T * pLL, DBLL_Entry_T * pEntry, DBLL_Entry_T * pNeighbor)
{
   bool only_one = false;

   SG_ASSERT(pLL != NULL, "DBLL_Insert_After cannot be called with a NULL pLL pointer");
   SG_ASSERT(pEntry != NULL, "DBLL_Insert_After cannot be called with a NULL pEntry pointer");
   SG_ASSERT((NULL == pLL->first) || (NULL == pLL->first->prev),"DBLL_Insert_After cannot be called with a NULL first or prev pointer");
   SG_ASSERT((NULL == pLL->last) || (NULL == pLL->last->next), "DBLL_Insert_After cannot be called with a NULL last or next pointer");

   if (NULL == pNeighbor)       /* place entry at end of list */
   {
      pNeighbor = pLL->last;

      if (NULL == pNeighbor)    /* list is currently empty! */
      {
         pLL->first = pLL->last = pEntry;
         pLL->count++;
         pEntry->next = pEntry->prev = NULL;
         SG_ASSERT(NULL == pLL->first->prev, "DBLL_Insert_After cannot have a null prev pointer");
         SG_ASSERT(NULL == pLL->last->next, "DBLL_Insert_After cannot have a null next pointer");

         only_one = true;       /* all done (bypass extra logic below) */
      }
   }

   if (!only_one)
   {
      DBLL_Entry_T *oldNext;

      oldNext = pNeighbor->next;
      pNeighbor->next = pEntry;
      pEntry->prev = pNeighbor;
      pEntry->next = oldNext;

      if (oldNext != NULL)      /* not last in list */
      {
         oldNext->prev = pEntry;
      }
      else                      /* is the last in the list */
      {
            pLL->last = pEntry;
            SG_ASSERT(NULL == pLL->last->next, "DBLL_Insert_After next pointer is not valid");
      }
      pLL->count++;

      SG_ASSERT((NULL == pLL->first) || (NULL == pLL->first->prev), "DBLL_Insert_After first or prev pointer is not valid");
      SG_ASSERT((NULL == pLL->last) || (NULL == pLL->last->next), "DBLL_Insert_After first or prev pointer is not valid");
   }                            /* if (!only_one) */
}

void *DBLL_Remove_Entry(DBLL_List_T * pLL, DBLL_Entry_T * pEntry)
{
    DBLL_Entry_T *next;
    DBLL_Entry_T *prev;

    SG_ASSERT_RET(pLL != NULL, "DBLL_Remove_Entry cannot be called with a NULL bdb pointer", NULL);
    SG_ASSERT_RET(pEntry != NULL, "DBLL_Remove_Entry cannot be called with a NULL bdb pointer", NULL);
    SG_ASSERT_RET((NULL == pLL->first) || (NULL == pLL->first->prev), "DBLL_Remove_Entry cannot be called with a NULL first or prev pointer", NULL);
    SG_ASSERT_RET((NULL == pLL->last) || (NULL == pLL->last->next), "DBLL_Remove_Entry cannot be called with a NULL last or next pointer", NULL);

    prev = pEntry->prev;
    next = pEntry->next;
    pEntry->next = pEntry->prev = NULL;  /* disassociate entry from list */

    if (NULL == prev)            /* removing 1st one from list */
    {
        pLL->first = next;
    }
    else
    {
        prev->next = next;
    }

    if (next != NULL)            /* an entry follows the one being removed */
    {
        next->prev = prev;
    }
    else                         /* the last entry was just removed */
    {
        pLL->last = prev;
    }
    pLL->count--;

    SG_ASSERT_RET((NULL == pLL->first) || (NULL == pLL->first->prev), "DBLL_Remove_Entry cannot have a null first or prev pointer", NULL);
    SG_ASSERT_RET((NULL == pLL->last) || (NULL == pLL->last->next), "DBLL_Remove_Entry cannot have a null last or next pointer", NULL);

    return next;
}


uint32_t DBLL_Get_Count(DBLL_List_T const *pLL)
{
    uint32_t count;

    SG_ASSERT_RET(pLL != NULL, "DBLL_Get_Count cannot be called with a NULL pLL pointer", 0);

    count = pLL->count;

    return count;
}

void *DBLL_Get_First(DBLL_List_T const *pLL)
{
    SG_ASSERT_RET(pLL != NULL, "DBLL_Get_First cannot be called with a NULL pLL pointer", NULL);
    SG_ASSERT_RET((NULL == pLL->first) || (NULL == pLL->first->prev), "DBLL_Get_First cannot be called with a NULL first and prev pointer", NULL);
    SG_ASSERT_RET((NULL == pLL->last) || (NULL == pLL->last->next), "DBLL_Get_First cannot be called with a NULL last and next pointer", NULL);

    return pLL->first;
}

void *DBLL_Get_Next(DBLL_Entry_T const *pEntry)
{
    SG_ASSERT_RET(pEntry != NULL, "DBLL_Get_Next cannot be called with a NULL pEntry pointer", NULL);

    return pEntry->next;
}



void * DBLL_Find(DBLL_List_T * pLL, uint32_t conv)
{
    SG_ASSERT_RET(pLL != NULL, "DBLL_Find cannot be called with a NULL pLL pointer", NULL);

    /* TODO: find */
    sg_kcp_client_t * client;

    client = DBLL_Get_First(pLL);
    while (NULL != client)
    {
        if (client->conv == conv)
        {
            return client;
        }

        client = DBLL_Get_Next((DBLL_Entry_T const *)client);
    }

    return NULL;
}



