//Marcos Gomez
//cproxy
// The two customized proxy programs detect and handle address changes
// Should listen to TCP port 5200, and take one argument that is the 
// eth1 IP address of the Server, i.e., cproxy w.x.y.z
// 
// Telnet into cproxy using "telnet localhost 5200"

#include <stdio.h>
#include <assert.h>

#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <netdb.h>
//int poll(struct pollfd *ufds, unsigned int nfds, int timeout);
 // struct pollfd {
 //         int    fd;       /* file descriptor */
 //         short  events;   /* events to look for */
 //         short  revents;  /* events returned */
 //     };
// struct pollfd {
//     int fd;         // the socket descriptor
//     short events;   // bitmap of events we're interested in
//     short revents;  // when poll() returns, bitmap of events that occurred
// };

// #include <netinet/in.h>

// struct sockaddr_in {
//     short            sin_family;   // e.g. AF_INET
//     unsigned short   sin_port;     // e.g. htons(3490)
//     struct in_addr   sin_addr;     // see struct in_addr, below
//     char             sin_zero[8];  // zero this if you want to
// };
// struct sockaddr {
//     unsigned short    sa_family;    // address family, AF_xxx
//     char              sa_data[14];  // 14 bytes of protocol address
// }; 

// struct in_addr {
//     unsigned long s_addr;  // load with inet_aton()
// };


// #include <sys/types.h>
// #include <sys/socket.h>
// #include <netdb.h>

// int getaddrinfo(const char *node,     // e.g. "www.example.com" or IP
//                 const char *service,  // e.g. "http" or port number
//                 const struct addrinfo *hints,
//                 struct addrinfo **res);

// struct addrinfo {
//     int              ai_flags;     // AI_PASSIVE, AI_CANONNAME, etc.
//     int              ai_family;    // AF_INET, AF_INET6, AF_UNSPEC
//     int              ai_socktype;  // SOCK_STREAM, SOCK_DGRAM
//     int              ai_protocol;  // use 0 for "any"
//     size_t           ai_addrlen;   // size of ai_addr in bytes
//     struct sockaddr *ai_addr;      // struct sockaddr_in or _in6
//     char            *ai_canonname; // full canonical hostname

//     struct addrinfo *ai_next;      // linked list, next node
// };

#include <sys/ioctl.h>

#define DEBUG 1
#define INCOMING_PORT 5200
#define OUTGOING_PORT 6200
#define BACKLOG 10  //how many pending connections queue will hold

void usage(char *argv[]);
void error(char *msg);

//Using telnet localhost 5200 to connect here
int main( int argc, char *argv[] ){
    char * inPort = "5200";
    char *serverEth1IPAddress;
    int localSockFD, proxySockFD;
    int returnValue;
    struct pollfd uniformFileDescriptors[2];
    socklen_t clilen;
    char buffer[256];
    struct sockaddr_in localAddr, cli_addr;
    struct sockaddr_storage connectingAddr;
    socklen_t addrLen;

    struct addrinfo hints, *res, *serverInfo, *p;

    if(argc < 2){
        usage(argv);
    }

    serverEth1IPAddress = argv[1];

    printf("Starting up the client...\n");
    //Set up sockets
    // Create a socket with the socket() system call
    // Bind the socket to an address using the bind() system call. For a server socket on the Internet, an address consists of a port number on the host machine.
    // Listen for connections with the listen() system call
    // Accept a connection with the accept() system call. This call typically blocks until a client connects with the server.
    // Send and receive data

    

    // memset(&hints, 0, sizeof(hints));
    // hints.ai_family = AF_INET;
    // hints.ai_socktype = SOCK_STREAM;
    // hints.ai_flags = AI_PASSIVE;

    // returnValue = getaddrinfo(serverEth1IPAddress, inPort, &hints, &serverInfo);
    // if(returnValue != 0 ){
    //     fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(returnValue));
    //     exit(1);
    // }

    // printf("IP addresses for %s:\n\n", serverEth1IPAddress);

    // char ipstr[INET6_ADDRSTRLEN];
    // for(p = serverInfo;p != NULL; p = p->ai_next) {
    //     void *addr;
    //     char *ipver;

    //     // get the pointer to the address itself,
    //     // different fields in IPv4 and IPv6:
    //     if (p->ai_family == AF_INET) { // IPv4
    //         struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
    //         addr = &(ipv4->sin_addr);
    //         ipver = "IPv4";
    //     } else { // IPv6
    //         struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
    //         addr = &(ipv6->sin6_addr);
    //         ipver = "IPv6";
    //     }

    //     // convert the IP to a string and print it:
    //     inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
    //     printf("  %s: %s\n", ipver, ipstr);
    // }

    

    

    //localSockFD = socket(serverInfo->ai_family, serverInfo->ai_socktype, serverInfo->ai_protocol);
    localSockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(localSockFD < 0){
        error("Error opening socket\n");
    }
    // proxySockFD = socket(AF_INET, SOCK_STREAM, 0);
    // if(proxySockFD < 0){
    //     error("Error opening socket\n");
    // }

    //bzero((char *) &localAddr, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    localAddr.sin_port = htons(INCOMING_PORT);
    memset(localAddr.sin_zero, '\0', sizeof(localAddr.sin_zero));

    // if(bind(localSockFD, serverInfo->ai_addr, serverInfo->ai_addrlen) < 0){
    //     error("Error on binding\n");
    // }
    if(bind(localSockFD, (struct sockaddr *) &localAddr, sizeof(localAddr)) < 0){
        error("Error on binding\n");
    }

    // uniformFileDescriptors[0].fd = localSockFD;
    // uniformFileDescriptors[0].events = POLLIN;

    //Listen to TCP port 5200 for incoming connection
    if(DEBUG){
        printf("Listening for connections...\n");
    }
    if(listen(localSockFD, BACKLOG) < 0){
        error("Error when listening\n");
    }

    //Accept connection from local telnet
    if(DEBUG){
        printf("Accepting connections\n");
    }
    addrLen = sizeof(connectingAddr);
    proxySockFD = accept(localSockFD, (struct sockaddr *) &connectingAddr,  &addrLen);
    if(proxySockFD < 0){
        error("Error accepting connection\n");
    }

    if(DEBUG){
        printf("Found a connection to accept!!!!!\n");
    }
    //Make a TCP connection to server port 6200(connect to sproxy)
    if(DEBUG){
        printf("Now trying to connect to server with eth1 IP addr: %s\n", serverEth1IPAddress);
    }

    //Keep relaying data between 2 sockets using select() or poll()



    //freeaddrinfo(serverInfo); // free the linked list

    if(DEBUG){
        printf("cproxy is finished\n");
    }
    
    return 0;
}

void usage(char *argv[]){
    fprintf(stdout, "Usage: %s w.x.y.z   ~eth1 IP address of the Server(was 192.168.8.2)\n", argv[0]);
    exit(1);
}

void error(char *msg){
    perror(msg);
    exit(1);
}