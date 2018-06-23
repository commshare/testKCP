#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <vlc/vlc.h>

#include "common.h"
#include "buffer.h"
#include "kcp_server.h"

int stream_fd[2];
int vlc_player_initialized = 0;
int do_run_vlc_thread_ = 0;
struct evbuffer* streambuf = NULL;
// 保存正确的流数据
pthread_t vlc_player_tid = 0;
pthread_cond_t vlc_player_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t vlc_player_lock;

/****************************************************/

static const char * test_defaults_args[] = {
    "-v",
    "--ignore-config",
    "-I", "dummy",
    "--no-media-library"
};

static const int test_defaults_nargs = sizeof(test_defaults_args) / sizeof(test_defaults_args[0]);


#define FRAME_BYTE_SIZE 188
#define MAX_BUFFER_SIZE 1024*1024*10

static void stream_buffer_cb(struct evbuffer* buf, size_t old, size_t now, void* arg)
{
    // 这里回调有锁
    evbuffer_setcb(buf, NULL, NULL);
    if (EVBUFFER_LENGTH(buf) > 0)
    pthread_cond_signal(&vlc_player_cond);
    evbuffer_setcb(buf, stream_buffer_cb, NULL);
}


static void stream_buffer_water_cb(struct evbuffer* buf, size_t old, size_t now, void* arg)
{
    if (old > now) return;
    else if (EVBUFFER_LENGTH(buf) > MAX_BUFFER_SIZE) {
        evbuffer_setcb(buf, stream_buffer_cb, NULL);
    }
}


void *do_run_vlc_thread(void *data)
{
    libvlc_instance_t *instance;
    libvlc_media_t *media;
    libvlc_media_player_t *player;
    int duration, curtime;
    struct evbuffer* cache = evbuffer_new();
    instance = libvlc_new(test_defaults_nargs, test_defaults_args);
    assert(instance);

    media = libvlc_media_new_fd(instance, stream_fd[0]);
    assert(media);

    player = libvlc_media_player_new_from_media(media);
    assert(player);

    libvlc_media_player_play(player);

    while (do_run_vlc_thread_) {
        pthread_mutex_lock(&vlc_player_lock);

        while (do_run_vlc_thread_ && EVBUFFER_LENGTH(streambuf) == 0)
            pthread_cond_wait(&vlc_player_cond, &vlc_player_lock);

        evbuffer_add_buffer(cache, streambuf);

        pthread_mutex_unlock(&vlc_player_lock);
        printf("server cache size %lfmb\n", EVBUFFER_LENGTH(cache)*1.0/(1024*1024));
        while(EVBUFFER_LENGTH(cache) > 0) {
            evbuffer_write(cache, stream_fd[1], FRAME_BYTE_SIZE);
        }
    }

    // 写剩余的内存
    pthread_mutex_lock(&vlc_player_lock);

    evbuffer_add_buffer(cache, streambuf);

    pthread_mutex_unlock(&vlc_player_lock);

    while(EVBUFFER_LENGTH(cache) > 0) {
        evbuffer_write(cache, stream_fd[1], FRAME_BYTE_SIZE);
    }

    printf("out loop %d bytes\n", EVBUFFER_LENGTH(streambuf));
    libvlc_media_release(media);


    do {
        fd_set rfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&rfds);
        FD_SET(stream_fd[0], &rfds);
        tv.tv_sec = 3;
        tv.tv_usec = 0;

        retval = select(stream_fd[0]+1, &rfds, NULL, NULL, &tv);
        if (retval > 0) {
            printf("sleep...\n");
            sleep(3);
            continue;
        }
    } while(0);
    printf("exit vlc thread\n");
    evbuffer_free(cache);
    libvlc_media_player_stop(player);
    libvlc_media_player_release(player);
    libvlc_release(instance);
    return (void *)1;
}


static void s_kcp_on_open(sg_kcp_client_t *client)
{
    char* addr = NULL;

    addr = sg_kcp_server_get_client_addr(client);
    printf("conn from %s\n", addr);

    free(addr);
}


static int s_kcp_start_vlc_player()
{

    if (vlc_player_initialized)
        return  -1;
    assert(!streambuf);

    if (pipe(stream_fd) != 0) {
        return -1;
    }

    if ((streambuf = evbuffer_new()) == NULL) {
        return -1;
    }
    evbuffer_setcb(streambuf, stream_buffer_water_cb, NULL);


    if (pthread_cond_init(&vlc_player_cond, NULL) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&vlc_player_lock, NULL) != 0) {
        return -1;
    }

    do_run_vlc_thread_ = 1;
    if (pthread_create(&vlc_player_tid, NULL, do_run_vlc_thread, NULL/* reserve*/) != 0) {
        printf("create vlc play thread fail\n");
        return -1;
    }
    printf("start vlc play thread successful\n");
    return 0;
}


static int s_kcp_start_video_stream()
{
    if (s_kcp_start_vlc_player() != 0) {
        return -1;
    }

    vlc_player_initialized = 1;
    return  0;
}

static void s_kcp_close_video_stream()
{
    printf("closing thread\n");
    pthread_mutex_lock(&vlc_player_lock);
    do_run_vlc_thread_ = 0;
    pthread_cond_signal(&vlc_player_cond);
    pthread_mutex_unlock(&vlc_player_lock);

    pthread_join(vlc_player_tid, NULL);
    printf("closed thread\n");
    pthread_mutex_destroy(&vlc_player_lock);
    pthread_cond_destroy(&vlc_player_cond);
    // release resource
    if (stream_fd[0] && stream_fd[1]) {
        close(stream_fd[0]);
        close(stream_fd[1]);
    }

    evbuffer_free(streambuf);
    streambuf = NULL;
    vlc_player_initialized = 0;
}

// 接收到客户端发送的数据流
static int kcp_recv_video_stream(sg_kcp_client_t* client, ftp_t* input)
{
    pthread_mutex_lock(&vlc_player_lock);
    evbuffer_add(streambuf, input->data, input->len);
    pthread_cond_signal(&vlc_player_cond);
    pthread_mutex_unlock(&vlc_player_lock);
    return 0;
}


static void s_kcp_on_message(sg_kcp_client_t *client, char *data, size_t size)
{
    ftp_t * input = (ftp_t *)data;
    ftp_t output;
    switch (input->payload) {
    case PT_PUT:
        s_kcp_start_video_stream();
        kcp_recv_video_stream(client, input);
        break;
    case PT_BYE:
        printf("save bye\n");
        output.payload = PT_BYE;
        output.len = 0;
        sg_kcp_server_send_data(client, &output, output.len + 2 * sizeof(int));
        sg_kcp_server_close_client(client);
        break;
    }
}

static void s_kcp_on_close(sg_kcp_client_t *client, int code, const char *reason)
{
    char* addr = NULL;
    addr = sg_kcp_server_get_client_addr(client);

    if (vlc_player_initialized) {
        printf("close video stream from %s\n", addr);
        s_kcp_close_video_stream();
    } else {
        printf("close conn from %s\n", addr);
    }
    free(addr);
}


int main(int argc, char * argv[])
{
    int  port;
    if (argc < 2) {
        printf("usage: %s port\n", argv[0]);
        return 0;
    }

    if (argc > 1) sscanf(argv[1], "%d", &port);

    printf("listen @ %d\n", port);

    sg_kcp_server_t * server = sg_kcp_server_open("0.0.0.0", port, 2, 10, s_kcp_on_open, s_kcp_on_message, s_kcp_on_close);

    sg_kcp_server_run(server, 10);

    sg_kcp_server_close(server);

    return 0;
}

