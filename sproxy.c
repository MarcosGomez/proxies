//Marcos Gomez
//sproxy.c
//Should listen on TCP port 6200, and takes no command-line argument

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

#define DEBUG 1
#define INCOMING_PORT 6200
#define OUTGOING_PORT 23
#define BACKLOG 10  //how many pending connections queue will hold
#define LOCAL_POLL 0
#define PROXY_POLL 1
#define NUM_OF_SOCKS 2
#define TIMEOUT 1000
#define MAX_BUFFER_SIZE 4096

#define HEARTBEAT 0
#define INIT 1
#define DATA 2
struct customHdr{
    uint8_t type; //heartbeat, new connection initiation, app data
    uint32_t seqNum;
    uint32_t ackNum;
    uint32_t payloadLength;//Can be 0 for heartbeat and initiation
} __attribute__ ((packed)); //13 bytes

void usage(char *argv[]);
void error(char *msg);
void setUpConnections(int *localSock, int *proxySock, int *listenSock);
int sendall(int s, char *buf, int *len, int flags);
void sendHeartBeat(int pSockFD);
void processReceivedHeader(char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag);
int removeHeader(char *buffer, int *nBytes);
int receiveProxyPacket(int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB);
void addHeader(void *buffer, int *nBytes, uint8_t type);

int main( void ){
    int localSockFD, proxySockFD, listenSockFD;
    int returnValue;
    int nBytesLocal, nBytesProxy;
    struct pollfd pollFDs[NUM_OF_SOCKS];
    char bufProxy[MAX_BUFFER_SIZE], bufLocal[MAX_BUFFER_SIZE];

    int sendToProxy, sendToLocal; //booleans
    int isOOBProxy, isOOBLocal; //bool, is out-of-band
    int notSentProxy, notSentLocal; //bool

    int numTimeouts;

    printf("Starting up the server...\n");

    setUpConnections(&localSockFD, &proxySockFD, &listenSockFD);
    
    //Keep relaying data between 2 sockets using select() or poll()
    //Keep proxy up until connection is dead

    pollFDs[LOCAL_POLL].fd = localSockFD;
    pollFDs[LOCAL_POLL].events = POLLIN | POLLPRI | POLLOUT;

    pollFDs[PROXY_POLL].fd = proxySockFD;
    pollFDs[PROXY_POLL].events = POLLIN | POLLPRI | POLLOUT;

    sendToProxy = sendToLocal = isOOBProxy = isOOBLocal = notSentLocal = notSentProxy 
    = numTimeouts = 0; //Initalize to false
    //Mainloop
    while(1){
        //Only check for POLLOUT when necessary to use timeouts as hearbeats
        if(sendToLocal){
            pollFDs[LOCAL_POLL].events = POLLIN | POLLPRI | POLLOUT;
        }else{
            pollFDs[LOCAL_POLL].events = POLLIN | POLLPRI;
        }
        if(sendToProxy){
            pollFDs[PROXY_POLL].events = POLLIN | POLLPRI | POLLOUT;
        }else{
            pollFDs[PROXY_POLL].events = POLLIN | POLLPRI;
        }

        returnValue = poll(pollFDs, NUM_OF_SOCKS, TIMEOUT);
        if(returnValue == -1){
            error("poll Error\n");
        }else if(returnValue == 0){
            numTimeouts++;
            printf("Timeout number occured! No data after %.3f seconds\n", TIMEOUT * numTimeouts/1000.0f);
            //Send out hearbeat message
            sendHeartBeat(proxySockFD);
            
            // if(numTimeouts >= 3){
            //     if(DEBUG){
            //         printf("Lost connection, time to close failed socket\n");
            //     }
            //     close(proxySockFD);
            //     printf("PROGRAM SHOULD KEEP RUNNING. TODO\n");
            //     break;
            // }
        }else{
            //Check proxy events - HEADER MANAGEMENT
            if(notSentProxy){
                if(DEBUG){
                    printf("Skipping recieve to wait to send past data for proxy\n");
                }
            }else{
                //RECEIVE - NEED TO CHECK AND REMOVE HEADER
                if(pollFDs[PROXY_POLL].revents & POLLPRI){
                    if(DEBUG){
                        printf("receiving out-of-band data from proxy!!\n");
                    }
                    nBytesProxy = recv(proxySockFD, bufProxy, sizeof(bufProxy), MSG_OOB); //Receive out-of-band data
                    if(receiveProxyPacket(&nBytesProxy, 1, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal)
                     == -1){
                        break;
                    }  
                }else if(pollFDs[PROXY_POLL].revents & POLLIN){
                    if(DEBUG){
                        printf("receiving normal data from proxy\n");
                    }
                    nBytesProxy = recv(proxySockFD, bufProxy, sizeof(bufProxy), 0); //Receive normal data
                    if(receiveProxyPacket(&nBytesProxy, 0, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal)
                     == -1){
                        break;
                    }  
                }
            }
            //SEND - NEED TO ADD HEADER
            if(sendToProxy){
                if(pollFDs[PROXY_POLL].revents & POLLOUT){
                    if(isOOBProxy){
                        if(DEBUG){
                            printf("Sending out out-of-band data to proxy\n");
                        }
                        addHeader(bufLocal, &nBytesLocal, DATA);
                        if(sendall(proxySockFD, bufLocal, &nBytesLocal, MSG_OOB) == -1){
                            perror("Error with send\n");
                            printf("Only sent %d bytes because of error!\n", nBytesLocal);
                        }
                        isOOBProxy = 0;
                    }else{
                        if(DEBUG){
                            printf("Sending out data to proxy\n");
                        }
                        //Normal
                        addHeader(bufLocal, &nBytesLocal, DATA);
                        if(sendall(proxySockFD, bufLocal, &nBytesLocal, 0) == -1){
                            perror("Error with send\n");
                            printf("Only sent %d bytes because of error!\n", nBytesLocal);
                        }
                    }
                    sendToProxy = 0;
                    notSentProxy = 0;
                }else{
                    notSentProxy = 1;
                }
            }
            if(pollFDs[PROXY_POLL].revents & POLLERR || pollFDs[PROXY_POLL].revents & POLLHUP ||
            pollFDs[PROXY_POLL].revents & POLLNVAL ){
                perror("Poll returned an error from proxy\n");
            }

//---------------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------

            //Check local events
            if(notSentLocal){
                if(DEBUG){
                    printf("Skipping recieve to wait to send past data for local\n");
                }
            }else{
                //RECEIVE
                if(pollFDs[LOCAL_POLL].revents & POLLPRI){
                    if(DEBUG){
                        printf("receiving out-of-band data from local!!\n");
                    }
                    nBytesLocal = recv(localSockFD, bufLocal, sizeof(bufLocal), MSG_OOB); //Receive out-of-band data
                    if(nBytesLocal == -1){
                        perror("recv error\n");
                    }else if(nBytesLocal == 0){
                        printf("The local side closed the connection on you\n");
                        break;
                    }else{
                        if(DEBUG){
                            printf("Just recieved %d bytes\n", nBytesLocal);
                        }
                        sendToProxy = 1;
                        isOOBProxy = 1;
                    } 
                }else if(pollFDs[LOCAL_POLL].revents & POLLIN){
                    if(DEBUG){
                        printf("receiving normal data from local\n");
                    }
                    nBytesLocal = recv(localSockFD, bufLocal, sizeof(bufLocal), 0); //Receive normal data
                    if(nBytesLocal == -1){
                        perror("recv error\n");
                    }else if(nBytesLocal == 0){
                        printf("The local side closed the connection on you\n");
                        break;
                    }else{
                        if(DEBUG){
                            printf("Just recieved %d bytes\n", nBytesLocal);
                        }
                        sendToProxy = 1;
                    } 
                }
            }
            //SEND
            if(sendToLocal){
                if(pollFDs[LOCAL_POLL].revents & POLLOUT){
                    if(isOOBLocal){
                        if(DEBUG){
                            printf("Sending out out-of-band data to local\n");
                        }
                        if(sendall(localSockFD, bufProxy, &nBytesProxy, MSG_OOB) == -1){
                            perror("Error with send\n");
                            printf("Only sent %d bytes because of error!\n", nBytesProxy);
                        }
                        isOOBLocal = 0;
                    }else{
                        if(DEBUG){
                            printf("Sending out data to local\n");
                        }
                        //Normal
                        if(sendall(localSockFD, bufProxy, &nBytesProxy, 0) == -1){
                            perror("Error with send\n");
                            printf("Only sent %d bytes because of error!\n", nBytesProxy);
                        }
                    }
                    sendToLocal = 0;
                    notSentLocal = 0;
                }else{
                    notSentLocal = 1;
                }
            }
            if(pollFDs[LOCAL_POLL].revents & POLLERR || pollFDs[LOCAL_POLL].revents & POLLHUP ||
            pollFDs[LOCAL_POLL].revents & POLLNVAL ){
                perror("Poll returned an error from local\n");
            }
        }
    }
    //Close sockets
    close(proxySockFD);
    close(localSockFD);
    close(listenSockFD);



    printf("sproxy is finished\n");
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


void setUpConnections(int *localSock, int *proxySock, int *listenSock){
    int localSockFD, proxySockFD, listenSockFD;
    struct sockaddr_in localAddr, proxyAddr;
    struct sockaddr_storage connectingAddr;
    socklen_t addrLen;
    

    listenSockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(listenSockFD < 0){
        error("Error opening socket\n");
    }
    
    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    proxyAddr.sin_port = htons(INCOMING_PORT);
    memset(proxyAddr.sin_zero, '\0', sizeof(proxyAddr.sin_zero));

    if(bind(listenSockFD, (struct sockaddr *) &proxyAddr, sizeof(proxyAddr)) < 0){
        error("Error on binding\n");
    }

    //Listen on port 6200 for incoming connection
    if(DEBUG){
        printf("Listening for connections...(Use \"telnet 192.168.8.2 6200\" for debugging)\n");
    }
    if(listen(listenSockFD, BACKLOG) < 0){
        error("Error when listening\n");
    }

    //Accept the connection from telnet/cproxy
    addrLen = sizeof(connectingAddr);
    proxySockFD = accept(listenSockFD, (struct sockaddr *) &connectingAddr,  &addrLen); //This actually waits
    if(proxySockFD < 0){
        error("Error accepting connection\n");
    }

    if(DEBUG){
        printf("Accepted a connection\n");
    }
    //Make a TCP connection to localhost(127.0.0.1)port 23 (Where telnet daemon is listening on)
    if(DEBUG){
        printf("Now trying to connect to telnet on server\n");
    }
    localSockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(localSockFD < 0){
        error("Error opening socket\n");
    }

    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    localAddr.sin_port = htons(OUTGOING_PORT);
    memset(localAddr.sin_zero, '\0', sizeof(localAddr.sin_zero));

    if(connect(localSockFD, (struct sockaddr *) &localAddr, sizeof(localAddr)) < 0){
        error("Error connecting\n");
    }
    if(DEBUG){
        printf("Now connected to server side\n");
    }

    //Assign all file descriptors
    *localSock = localSockFD;
    *proxySock = proxySockFD;
    *listenSock = listenSockFD;
}

int sendall(int s, char *buf, int *len, int flags)
{
    int total = 0;        // how many bytes we've sent
    int bytesleft = *len; // how many we have left to send
    int n;

    while(total < *len) {
        n = send(s, buf+total, bytesleft, flags);
        if (n == -1) { break; }
        total += n;
        bytesleft -= n;
    }

    *len = total; // return number actually sent here

    return n==-1?-1:0; // return -1 on failure, 0 on success
}

void sendHeartBeat(int pSockFD){
    if(DEBUG){
        printf("Sending heartbeat\n");
    }
    char bufHeart[MAX_BUFFER_SIZE];
    int nBytesHeart = 0;
    addHeader(bufHeart, &nBytesHeart, HEARTBEAT);
    if(sendall(pSockFD, bufHeart, &nBytesHeart, 0) == -1){
        perror("Error with send\n");
        printf("Only sent %d bytes because of error!\n", nBytesHeart);
    }
}

void processReceivedHeader(char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag){
    int type;
    type = removeHeader(buffer, nBytes);
    //type = DATA;
    if(type == HEARTBEAT){
        if(DEBUG){
            printf("Recieved a heartbeat\n");
        }
        *numTimeouts = 0;
    }else if(type == INIT){
        if(DEBUG){
            printf("Recieved a new connection initiation, which shouldn't happen on client\n");
        }
    }else if(type == DATA){
        if(DEBUG){
            printf("Received normal data\n");
        }
        *sendTo = 1;
        if(flag){
            *isOOB = 1;
        }else{
            *isOOB = 0;
        }
        
    }else{
        perror("Received unknown type of header\n");
    }
}

int removeHeader(char *buffer, int *nBytes){
    struct customHdr *cHdr;
    int type;
    char tempBuf[MAX_BUFFER_SIZE];

    cHdr = (struct customHdr *) buffer;
    
    //Process Header
    type = cHdr->type;
    if(DEBUG){
        printf("Type was found out to be %d\n", type);
    }

    //Remove header
    memcpy(tempBuf, buffer, *nBytes);
    (*nBytes) -= sizeof(struct customHdr);
    memcpy(buffer, tempBuf + sizeof(struct customHdr), *nBytes);
    
    return type;
}

int receiveProxyPacket(int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB){
    // if(flag){
    //     //Then OOB
    //     if(DEBUG){
    //         printf("receiving out-of-band data from proxy!!\n");
    //     }
    //     *nBytes = recv(sockFD, *buffer, MAX_BUFFER_SIZE, MSG_OOB); //Receive out-of-band data
    // }else{
    //     //Normal
    //     if(DEBUG){
    //         printf("receiving normal data from proxy!!\n");
    //     }
    //     printf("sockFD = %d, sizeof(buffer) = %d", sockFD, sizeof(buffer));
    //     *nBytes = recv(sockFD, *buffer, MAX_BUFFER_SIZE, 0); //Receive out-of-band data
    // }

    if(*nBytes == -1){
        perror("recv error\n");
    }else if(*nBytes == 0){
        printf("The proxy side closed the connection on you\n");
        return -1;
    }else{
        if(DEBUG){
            printf("Just recieved %d bytes\n", *nBytes);
        }
        processReceivedHeader(buffer, numTimeouts, sendTo, isOOB, nBytes, flag);                  
    }
    
    return 0;
}

void addHeader(void *buffer, int *nBytes, uint8_t type){
    struct customHdr cHdr;
    char tempBuf[MAX_BUFFER_SIZE];

    //Set header values
    cHdr.type = type;
    cHdr.seqNum = 0;
    cHdr.ackNum = 0;
    cHdr.payloadLength = *nBytes;

    //Copy header to buffer
    if(*nBytes + sizeof(struct customHdr) > MAX_BUFFER_SIZE){
        printf("ERROR: BUFFER OVERFLOW. YOU NEED TO DYNAMICALLY CHANGE BUFFER SIZE\n");
    }
    memcpy(tempBuf, &cHdr, sizeof(struct customHdr));
    memcpy(tempBuf + sizeof(struct customHdr), buffer, *nBytes);

    (*nBytes) += sizeof(struct customHdr); 
    memcpy(buffer, tempBuf, *nBytes);
    if(DEBUG){
        unsigned char *temp = (unsigned char*)buffer;
        printf("Just added a header of type %d\n", *temp);
    }
}