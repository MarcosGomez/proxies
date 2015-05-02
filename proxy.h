#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <netdb.h>
#include <unistd.h> //close

#define HEARTBEAT 0
#define INIT 1
#define DATA 2
struct customHdr{
    uint8_t type; //heartbeat, new connection initiation, app data
    uint32_t sequencNumber;
    uint32_t acknowledgementNumber;
    uint32_t payloadLength;//Can be 0 for heartbeat and initiation
} __attribute__ ((packed)); //13 bytes