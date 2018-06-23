#ifndef LIBSG_KCP_H
#define LIBSG_KCP_H

#if defined(linux) || defined(__linux) || defined(__linux__)
#   ifndef PLATFORM_LINUX
#       define PLATFORM_LINUX
#   endif
#elif defined(WIN32) || defined(_WIN32) || defined(_WIN64)
#   ifndef PLATFORM_WINDOWS
#       define PLATFORM_WINDOWS
#   endif
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
#   ifndef PLATFORM_MACOS
#       define PLATFORM_MACOS
#   endif
#else
#   error Unsupported platform.
#endif

#if defined(PLATFORM_WINDOWS)
#   include <winsock2.h>
#   include <windows.h>
#   pragma comment(lib ,"ws2_32.lib")
#   pragma comment(lib, "psapi.lib")
#   pragma comment(lib, "Iphlpapi.lib")
#   pragma comment(lib, "userenv.lib")
#elif defined(PLATFORM_LINUX)
#   include <sys/types.h>
#   include <sys/socket.h>
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/epoll.h>
#   include <sys/time.h>
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_BSD)
#   include <sys/event.h>
#   include <sys/socket.h>
#   include <arpa/inet.h>
#   include <sys/types.h>
#   include <sys/event.h>
#   include <sys/time.h>
#endif
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct sg_kcp_real sg_kcp_t; /*struct sg_kcp_real的真实定义请放在kcp.c中*/

typedef void (*sg_kcp_on_open)(sg_kcp_t *client);
typedef void (*sg_kcp_on_message)(sg_kcp_t *client, char *data, size_t size);
typedef void (*sg_kcp_on_close)(sg_kcp_t *client, int code, const char *reason);
typedef void (*sg_kcp_on_send_done)(sg_kcp_t *client, int status);
/* 初始化，返回0表示成功, 如果不必要，可以撤销此接口 */
int sg_kcp_init();

sg_kcp_t *sg_kcp_open(const char *server_addr, int server_port,
                     sg_kcp_on_open     on_open,
                     sg_kcp_on_message  on_message,
                     sg_kcp_on_send_done on_done,
                     sg_kcp_on_close    on_close);

int sg_kcp_loop(sg_kcp_t *client, int interval_ms);

int sg_kcp_send(sg_kcp_t *client, const void *data, uint64_t size);

uint32_t sg_kcp_now(sg_kcp_t * client);

void sg_kcp_close(sg_kcp_t *client);

/* 如果不必要，可以撤销此接口 */
void sg_kcp_free(void);

/* 限制客户端发送速度, kbps为0不做任何限制 */
void sg_kcp_set_max_send_speed(sg_kcp_t *client, size_t kbps);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LIBSG_KCP_H */
