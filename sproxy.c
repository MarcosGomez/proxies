//Marcos Gomez
//sproxy.c
//Should listen on TCP port 6200, and takes no command-line argument

#include <stdio.h>
#include <assert.h>

#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <sys/ioctl.h>

#define DEBUG 1
#define INCOMING_PORT 6200
#define OUTGOING_PORT 23

int main( void ){
    printf("Starting up the server...\n");

    //Listen on port 6200 for incoming connection

    //Accept the connection

    //Make a TCP connection to localhost(127.0.0.1)port 23 (Where telnet daemon is listening on)
    
    //Keep relaying data between 2 sockets using select() or poll()


    printf("sproxy is finished\n")
    return 0;
}