// Created by twist on 8/12/16.
//

#include <unistd.h>
#include <stdio.h>
#include "common.h"
#include "kcp.h"

#define ASSERT assert

static char s_path[256] = {0};
static uint32_t now;
FILE* video_fp = NULL;

static int s_kcp_send_put(sg_kcp_t *client, const char *filename)
{
    ftp_t output;
    memset(&output, 0, sizeof(output));
    output.payload = PT_PUT;
    strncpy(output.data, s_path, sizeof(output.data));
    output.len = strlen(output.data) + 1;
    sg_kcp_send(client, &output, output.len + 2 * sizeof(int));
    return 0;
}

static int s_kcp_send_file(sg_kcp_t *client, const char *filename)
{
    ftp_t output;
    if (video_fp) {
        return -1;
    }
    printf("send %s\n", filename);
    if (!(video_fp = fopen(filename, "rb"))) {
        printf("open %s failed\n", filename);
        return -1;
    }
    printf("open %s successful\n", filename);
    memset(&output, 0, sizeof(output));
    // TODO:发送一个小包唤醒客户端，实现有点不好
    output.payload = PT_PUT;
    output.len = fread(output.data, 1, sizeof(output.data), video_fp);
    sg_kcp_send(client, &output, output.len + 2 * sizeof(int));
    return 0;
}


static void s_kcp_on_open(sg_kcp_t *client)
{
    ftp_t output;

    printf("start to put file %s\n", s_path);

    if (s_kcp_send_file(client, s_path)) {
        printf("send file fail!\n");
    }
}

static void s_kcp_on_message(sg_kcp_t *client, char *data, size_t size)
{
    ftp_t *input = (ftp_t *) data;
    ftp_t output;
    FILE *fp;

    switch (input->payload) {
    case PT_BYE:
        // client bye msg send successful, just close
        printf("bye\n");
        sg_kcp_close(client);
        break;
    }
}



static void s_kcp_on_done(sg_kcp_t* client, int status)
{
    ftp_t output;
    if (!video_fp)
        return;

    memset(&output, 0, sizeof(output));
    output.payload = PT_PUT;

    while ((output.len = fread(output.data, 1, sizeof(output.data), video_fp)) > 0) {
        sg_kcp_send(client, &output, output.len + 2 * sizeof(int));
    }

    if (feof(video_fp)) {
        output.payload = PT_BYE;
        output.len = 0;
        sg_kcp_send(client, &output, output.len + 2 * sizeof(int));
        printf("send bye\n");
        fclose(video_fp);
        video_fp = NULL;
    }
}

static void s_kcp_on_close(sg_kcp_t *client, int code, const char *reason)
{
    printf("conn closed\n");
}

int main(int argc, char * argv[])
{
    char ip[32]     = "127.0.0.1";
    char sport[16]  = "3333";
    char path[256]  = "/disk2/hello.ts";
    char sspeed[256] = "0";
    int  port;
    int  speed = 0;

    if (argc < 4) {
        printf("usage: %s host port file_path [speed]\n", argv[0]);
        return 0;
    }

    if (argc > 1) strncpy(ip,    argv[1], sizeof(ip));
    if (argc > 2) strncpy(sport, argv[2], sizeof(sport));
    if (argc > 3) strncpy(path,  argv[3], sizeof(path));
    if (argc > 4) strncpy(sspeed,  argv[4], sizeof(sspeed));

    sscanf(sport, "%d", &port);
    sscanf(sspeed, "%d", &speed);


    printf("  ip: %s\n",   ip);
    printf("port: %d\n", port);
    printf("file: %s\n", path);

    strncpy(s_path, path, sizeof(s_path));

    sg_kcp_t * client = sg_kcp_open(ip, port, s_kcp_on_open, s_kcp_on_message, s_kcp_on_done, s_kcp_on_close);
    sg_kcp_set_max_send_speed(client, speed);
    sg_kcp_loop(client, 3);


    return 0;
}




