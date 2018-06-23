cmake_minimum_required(VERSION 3.6)
project(rtsp)

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -std=gnu99")

include_directories(./kcp)
set(KCP_FILES kcp/ikcp.c kcp/kcp_server.c kcp/kcp.c buffer.c)

set(SERVER_FILES server.c ${KCP_FILES})
set(CLIENT_FILES client.c ${KCP_FILES})

add_executable(server ${SERVER_FILES})
TARGET_LINK_LIBRARIES(server vlc pthread uv)



add_executable(client ${CLIENT_FILES})
TARGET_LINK_LIBRARIES(client pthread uv)

