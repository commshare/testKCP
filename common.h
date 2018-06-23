//
// Created by twist on 8/13/16.
//

#ifndef _COMMON_H
#define _COMMON_H

enum
{
    PT_GET,
    PT_PUT,
    PT_CHK,
    PT_BYE,
    PT_ACK,
};



#define FTP_MAX_BUF_SIZE 4096

typedef struct
{
    int payload;
    int len;
    char data[FTP_MAX_BUF_SIZE];
}ftp_t;

#endif //_COMMON_H
