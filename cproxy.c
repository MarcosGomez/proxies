//Marcos Gomez
//cproxy
// The two customized proxy programs detect and handle address changes
// Should listen to TCP port 5200, and take one argument that is the 
// eth1 IP address of the Server, i.e., cproxy w.x.y.z
// 
// Telnet into cproxy using "telnet localhost 5200"


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
#include <fcntl.h>


#define DEBUG 1
#define INCOMING_PORT 5200
#define OUTGOING_PORT 6200
#define TELNET_PORT 23
#define BACKLOG 10  //how many pending connections queue will hold
#define LOCAL_POLL 0
#define PROXY_POLL 1
#define NUM_OF_SOCKS 2
#define TIMEOUT 1000
#define MAX_BUFFER_SIZE 16384
#define WAITTIME 5000

#define HEARTBEAT 0
#define INIT 1
#define DATA 2
#define ACK 3

struct customHdr{
    uint8_t type; //heartbeat, new connection initiation, app data
    uint32_t seqNum;
    uint32_t ackNum;
    uint32_t payloadLength;//Can be 0 for heartbeat and initiation
} __attribute__ ((packed)); //13 bytes

struct packetData{
    struct packetData *next;
    uint32_t id;
    uint32_t size;
    char data[MAX_BUFFER_SIZE];
};

void usage(char *argv[]);
void error(char *msg);
void setUpConnections(int *localSock, int *proxySock, int *listenSock, char *serverEth1IPAddress);
int sendall(int s, char *buf, int *len, int flags);
void sendHeartBeat(int pSockFD, uint32_t ackNum);
void sendAck(int pSockFD, uint32_t ackNum);
void sendInit(int pSockFD);
void processReceivedHeader(int sockFD, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag, uint32_t *seqNum);
int removeHeader(char *buffer, int *nBytes, int *rType, uint32_t *seqNum);
int receiveProxyPacket(int sockFD, int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, uint32_t *seqNum);
void addHeader(void *buffer, int *nBytes, uint8_t type, uint32_t seqNum, uint32_t ackNum);
void reconnectToProxy(int *proxySock, char *serverEth1IPAddress);
void setProxySocket(int *proxySock);
int tryToConnect(int proxySockFD, char *serverEth1IPAddress);
void rememberData(struct packetData **startPacket, void *buffer, uint32_t id, int nBytes);
void addData(struct packetData *pData, void *buffer, uint32_t id, int nBytes);
void eraseAllData(struct packetData **startPacket);
struct packetData *deleteAllData(struct packetData *pData);
void sendStoredData(int sockFD, struct packetData *pData);


//Using telnet localhost 5200 to connect here
int main( int argc, char *argv[] ){
    int localSockFD, proxySockFD, listenSockFD;
    int returnValue;
    int nBytesLocal, nBytesProxy;
    struct pollfd pollFDs[NUM_OF_SOCKS];
    char bufProxy[MAX_BUFFER_SIZE], bufLocal[MAX_BUFFER_SIZE];

    int sendToProxy, sendToLocal; //booleans
    int isOOBProxy, isOOBLocal; //bool, is out-of-band
    int notSentProxy, notSentLocal; //bool
    int closeSession;

    int numTimeouts;
    uint32_t receivedSeqNum;

    struct packetData *storedPackets = NULL;
    
    //Make sure IP of server is provided
    if(argc < 2){
        usage(argv);
    }
    while(1){
        printf("Starting up the client...\n");
        //Set up sockets
        setUpConnections(&localSockFD, &proxySockFD, &listenSockFD, argv[1]);//CHange something here
        receivedSeqNum = 0;
        closeSession = 0;
        
        //Keep relaying data between 2 sockets using select() or poll()
        //Keep proxy up until connection is dead
        for(;;){
            pollFDs[LOCAL_POLL].fd = localSockFD;
            pollFDs[LOCAL_POLL].events = POLLIN | POLLPRI | POLLOUT;

            pollFDs[PROXY_POLL].fd = proxySockFD;
            pollFDs[PROXY_POLL].events = POLLIN | POLLPRI | POLLOUT;

            sendToProxy = sendToLocal = isOOBProxy = isOOBLocal = notSentLocal = notSentProxy 
            = numTimeouts = 0; //Initalize to false

            //Mainloop
            while(!closeSession){
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
                    if(DEBUG){
                        printf("Timeout number occured! No data after %.3f seconds\n", TIMEOUT * numTimeouts/1000.0f);
                    }
                    if(numTimeouts >= 3){
                        if(DEBUG){
                            printf("Lost connection, time to close failed socket\n");
                        }
                        break;
                    }else{
                        //Send out hearbeat message
                        sendHeartBeat(proxySockFD, receivedSeqNum);
                    }
                }else{
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
                            nBytesLocal = recv(localSockFD, bufLocal, sizeof(bufLocal) - sizeof(struct customHdr), MSG_OOB); //Receive out-of-band data
                            if(nBytesLocal == -1){
                                perror("recv error\n");
                            }else if(nBytesLocal == 0){
                                printf("The local side closed the connection on you\n");
                                closeSession = 1;
                                break;
                            }else{
                                if(DEBUG){
                                    printf("Just recieved %d bytes\n", nBytesLocal);
                                }
                                sendToProxy = 1;
                                isOOBProxy = 1;
                                addHeader(bufLocal, &nBytesLocal, DATA, 0, receivedSeqNum);
                            } 
                        }else if(pollFDs[LOCAL_POLL].revents & POLLIN){
                            if(DEBUG){
                                printf("receiving normal data from local\n");
                            }
                            nBytesLocal = recv(localSockFD, bufLocal, sizeof(bufLocal) - sizeof(struct customHdr), 0); //Receive normal data
                            if(nBytesLocal == -1){
                                perror("recv error\n");
                            }else if(nBytesLocal == 0){
                                printf("The local side closed the connection on you\n");
                                closeSession = 1;
                                break;
                            }else{
                                if(DEBUG){
                                    printf("Just recieved %d bytes\n", nBytesLocal);
                                }
                                sendToProxy = 1;
                                addHeader(bufLocal, &nBytesLocal, DATA, 0, receivedSeqNum);
                            } 
                        }
                    }
                    //SEND
                    if(sendToLocal){
                        if(pollFDs[LOCAL_POLL].revents & POLLOUT){
                            // if(isOOBLocal){
                            //     if(DEBUG){
                            //         printf("Sending out out-of-band data to local\n");
                            //     }
                            //     if(sendall(localSockFD, bufProxy, &nBytesProxy, MSG_OOB) == -1){
                            //         perror("Error with send\n");
                            //         printf("Only sent %d bytes because of error!\n", nBytesProxy);
                            //     }
                            //     isOOBLocal = 0;
                            // }else{
                                if(DEBUG){
                                    printf("Sending out data to local\n");
                                }
                                //Normal
                                if(sendall(localSockFD, bufProxy, &nBytesProxy, 0) == -1){
                                    perror("Error with send\n");
                                    printf("Only sent %d bytes because of error!\n", nBytesProxy);
                                }
                            //}
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

        //---------------------------------------------------------------------------------------------
        //---------------------------------------------------------------------------------------------
        //---------------------------------------------------------------------------------------------

                    //Check proxy events - HEADER MANAGEMENT
                    if(notSentProxy){
                        if(DEBUG){
                            printf("Skipping recieve to wait to send past data to proxy\n");
                        }
                    }else{
                        //RECEIVE - NEED TO CHECK AND REMOVE HEADER
                        if(pollFDs[PROXY_POLL].revents & POLLPRI){
                            if(receiveProxyPacket(proxySockFD, &nBytesProxy, 1, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal, &receivedSeqNum)
                             == -1){
                                closeSession = 1;
                                break;
                            }  
                        }else if(pollFDs[PROXY_POLL].revents & POLLIN){
                            if(receiveProxyPacket(proxySockFD, &nBytesProxy, 0, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal, &receivedSeqNum)
                             == -1){
                                closeSession = 1;
                                break;
                            }
                        }
                    }
                    //SEND - NEED TO ADD HEADER
                    if(sendToProxy){
                        if(pollFDs[PROXY_POLL].revents & POLLOUT){
                            //if(isOOBProxy){
                                // if(DEBUG){
                                //     printf("Sending out out-of-band data to proxy\n");
                                // }
                                // addHeader(bufLocal, &nBytesLocal, DATA);
                                // if(sendall(proxySockFD, bufLocal, &nBytesLocal, MSG_OOB) == -1){
                                //     perror("Error with send\n");
                                //     printf("Only sent %d bytes because of error!\n", nBytesLocal);
                                // }
                                // isOOBProxy = 0;
                            //}else{
                                if(DEBUG){
                                    printf("Sending out data to proxy\n");
                                }
                                //Normal
                                //addHeader(bufLocal, &nBytesLocal, DATA, 0, receivedSeqNum);
                                if(sendall(proxySockFD, bufLocal, &nBytesLocal, 0) == -1){
                                    perror("Error with send\n");
                                    printf("Only sent %d bytes because of error!\n", nBytesLocal);
                                }
                            //}
                            sendToProxy = 0;
                            notSentProxy = 0;
                        }else{
                            notSentProxy = 1;
                        }
                    }
                    if(pollFDs[PROXY_POLL].revents & POLLERR || pollFDs[PROXY_POLL].revents & POLLHUP ||
                    pollFDs[PROXY_POLL].revents & POLLNVAL ){
                        perror("Poll returned an ERROR from proxy\n");

                    }
                }
            }
            
            //Close sockets
            close(proxySockFD);
            if(closeSession){
                break;
            }else{
                setProxySocket(&proxySockFD);
                while(tryToConnect(proxySockFD, argv[1]) != 0){
                    if(DEBUG){
                        printf("Checking if local socket is closed\n");
                    }
                    
                    nBytesLocal = recv(localSockFD, bufLocal, sizeof(bufLocal) - sizeof(struct customHdr), MSG_DONTWAIT); //Receive normal data
                    if(nBytesLocal == -1){
                        //perror("recv error\n");
                    }else if(nBytesLocal == 0){
                        printf("The local side closed the connection on you\n");
                        closeSession = 1;
                        eraseAllData(&storedPackets);
                        close(proxySockFD);
                        break;
                    }else{
                        if(DEBUG){
                            printf("Just recieved %d bytes\n", nBytesLocal);
                        }
                        //sendToProxy = 1;
                        addHeader(bufLocal, &nBytesLocal, DATA, 0, receivedSeqNum);
                        rememberData(&storedPackets, bufLocal, 0, nBytesLocal);
                    } 
                    
                }
                
            }
            
            if(!closeSession){
                sendStoredData(proxySockFD, storedPackets);
                //Just in case no stored data
                sendAck(proxySockFD, receivedSeqNum);
            }
            eraseAllData(&storedPackets);
            
        }//End for(;;)
        //When local closed connection
        close(localSockFD);
        close(listenSockFD);
    }//End while(1)
    printf("cproxy is finished\n");
    return 0;
}








void usage(char *argv[]){
    fprintf(stdout, "Usage: %s w.x.y.z   ~eth1 IP address of the Server\n", argv[0]);
    exit(1);
}

void error(char *msg){
    perror(msg);
    exit(1);
}

void setUpConnections(int *localSock, int *proxySock, int *listenSock, char *serverEth1IPAddress){
    int localSockFD, listenSockFD;
    struct sockaddr_in localAddr;
    struct sockaddr_storage connectingAddr;
    socklen_t addrLen;
    int option = 1;

    listenSockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(listenSockFD < 0){
        error("Error opening socket\n");
    }
    if(setsockopt(listenSockFD,SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR),(char*)&option,sizeof(option)) < 0)
    {
        printf("setsockopt failed\n");
        close(listenSockFD);
        exit(2);
    } 
    
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    localAddr.sin_port = htons(INCOMING_PORT);
    memset(localAddr.sin_zero, '\0', sizeof(localAddr.sin_zero));

    if(bind(listenSockFD, (struct sockaddr *) &localAddr, sizeof(localAddr)) < 0){
        error("Error on binding\n");
    }

    //Listen to TCP port 5200 for incoming connection
    printf("Listening for connections...(Use \"telnet localhost 5200\")\n");
    if(listen(listenSockFD, BACKLOG) < 0){
        error("Error when listening\n");
    }

    //Accept connection from local telnet
    addrLen = sizeof(connectingAddr);
    localSockFD = accept(listenSockFD, (struct sockaddr *) &connectingAddr,  &addrLen); //This actually waits
    if(localSockFD < 0){
        error("Error accepting connection\n");
    }

    if(DEBUG){
        printf("Accepted a connection\n");
    }
    //Make a TCP connection to server port 6200(connect to sproxy)
    reconnectToProxy(proxySock, serverEth1IPAddress);

    //Send INIT packet
    sendInit(*proxySock);

    //Assign all file descriptors
    *localSock = localSockFD;
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
    if(DEBUG){
        printf("Sent out a total of %d bytes\n", total);
    }

    return n==-1?-1:0; // return -1 on failure, 0 on success
}

void sendHeartBeat(int pSockFD, uint32_t ackNum){
    if(DEBUG){
        printf("Sending heartbeat\n");
    }
    char bufHeart[MAX_BUFFER_SIZE];
    int nBytesHeart = 0;
    addHeader(bufHeart, &nBytesHeart, HEARTBEAT, 0, ackNum);
    if(sendall(pSockFD, bufHeart, &nBytesHeart, 0) == -1){
        perror("Error with send\n");
        printf("Only sent %d bytes because of error!\n", nBytesHeart);
    }
}

void sendAck(int pSockFD, uint32_t ackNum){
    if(DEBUG){
        printf("Sending ack to heartbeat\n");
    }
    char bufAck[MAX_BUFFER_SIZE];
    int nBytesAck = 0;
    addHeader(bufAck, &nBytesAck, ACK, 0, ackNum);
    if(sendall(pSockFD, bufAck, &nBytesAck, 0) == -1){
        perror("Error with send\n");
        printf("Only sent %d bytes because of error!\n", nBytesAck);
    }
}

void sendInit(int pSockFD){
    if(DEBUG){
        printf("Sending Initiation request\n");
    }
    char buf[MAX_BUFFER_SIZE];
    int nBytes = 0;
    addHeader(buf, &nBytes, INIT, 0, 0);
    if(sendall(pSockFD, buf, &nBytes, 0) == -1){
        perror("Error with send\n");
        printf("Only sent %d bytes because of error!\n", nBytes);
    }
}

void processReceivedHeader(int sockFD, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag, uint32_t *seqNum){
    int type;
    int pastType = -1;
    uint32_t pastSeqNum = *seqNum;
    int tempNBytes = *nBytes;
    uint32_t pLoadLength;
    int rVal;

    //Remove multiple headers on same buffer
    do{
        rVal = removeHeader(buffer, &tempNBytes, &type, seqNum);
        pLoadLength = rVal;
        (*nBytes) -= sizeof(struct customHdr);
        tempNBytes -= pLoadLength;
        buffer += pLoadLength;

        if(type == DATA){
            pastType = DATA;
        }else if(type == HEARTBEAT){
            if(DEBUG){
                printf("Recieved a heartbeat, time to send ACK\n");
            }
            sendAck(sockFD, *seqNum);
        }else if(type == ACK){
            if(DEBUG){
                printf("received ACK\n");
            }
            *numTimeouts = 0;
        }else if(type == INIT){
            if(DEBUG){
                printf("Recieved a new connection initiation, which shouldn't happen on client\n");
            }
        }else{
            perror("Received unknown type of header!\n");
            pastType = -1;
            *seqNum = pastSeqNum;
        }
    }while(rVal >= 0);
    
    if(pastType == DATA){
        if(DEBUG){
            printf("Received normal data\n");
        }
        if(pastSeqNum >= *seqNum){
            //Then don't send it out
            if(DEBUG){
                printf("Not sending out this packet because seqNum too low!\n");
            }
        }else{
            *sendTo = 1;
            if(flag){
                *isOOB = 1;
            }else{
                *isOOB = 0;
            }
        }
    }
}

int removeHeader(char *buffer, int *nBytes, int *rType, uint32_t *seqNum){
    struct customHdr *cHdr;
    int type;
    char tempBuf[MAX_BUFFER_SIZE];
    uint32_t pLength;

    cHdr = (struct customHdr *) buffer;
    
    //Process Header
    type = cHdr->type;
    pLength = ntohl(cHdr->payloadLength);
    if(ntohl(cHdr->seqNum) > *seqNum){
        *seqNum = ntohl(cHdr->seqNum);
    }else{
        if(DEBUG){
            printf("Ignored seq number %d because too low\n", ntohl(cHdr->seqNum));
        }
        
    }
    // if(ntohl(cHdr->seqNum) != ((*seqNum) + 1) && type == DATA){
    //     if(DEBUG){
    //         printf("Missing a packet!\n");
    //     }
    // }
    
    if(DEBUG){
        printf("Received packet of type %d with seqNum %d and payload size %d\n", type, *seqNum, pLength);
    }

    //Remove header
    memcpy(tempBuf, buffer, *nBytes);
    (*nBytes) -= sizeof(struct customHdr);
    memcpy(buffer, tempBuf + sizeof(struct customHdr), *nBytes);

    *rType = type;

    if(*nBytes > pLength){
        if(DEBUG){
            printf("This buffer contains another header. payload of size %d but %d bytes left\n", pLength, *nBytes);
        }
        return pLength;
    }else if(*nBytes < pLength){
        fprintf(stderr, "There is something wrong with payloadLength!! Number of bytes left is %d, but payloadLength is %d\n", *nBytes, pLength);
    }
    return -1;
}

int receiveProxyPacket(int sockFD, int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, uint32_t *seqNum){
    if(flag){
        //Then OOB
        if(DEBUG){
            printf("receiving out-of-band data from proxy\n");
        }
        *nBytes = recv(sockFD, buffer, MAX_BUFFER_SIZE - sizeof(struct customHdr), MSG_OOB); //Receive out-of-band data
    }else{
        //Normal
        if(DEBUG){
            printf("receiving normal data from proxy\n");
        }
        *nBytes = recv(sockFD, buffer, MAX_BUFFER_SIZE - sizeof(struct customHdr), 0); //Receive out-of-band data
    }
    *numTimeouts = 0;
    if(*nBytes == -1){
        perror("recv ERROR\n");
    }else if(*nBytes == 0){
        printf("The proxy side closed the connection on you\n");
        return -1;
    }else{
        if(DEBUG){
            printf("Just recieved %d bytes\n", *nBytes);
        }
        processReceivedHeader(sockFD, buffer, numTimeouts, sendTo, isOOB, nBytes, flag, seqNum);                  
    }
    
    return 0;
}

void addHeader(void *buffer, int *nBytes, uint8_t type, uint32_t seqNum, uint32_t ackNum){
    struct customHdr cHdr;
    char tempBuf[MAX_BUFFER_SIZE];

    //Set header values
    cHdr.type = type;
    cHdr.seqNum = htonl(seqNum);
    cHdr.ackNum = htonl(ackNum);
    cHdr.payloadLength = htonl(*nBytes);

    //Copy header to buffer
    if(*nBytes + sizeof(struct customHdr) > MAX_BUFFER_SIZE){
        printf("ERROR: BUFFER OVERFLOW. YOU NEED TO DYNAMICALLY CHANGE BUFFER SIZE\n");
    }
    memcpy(tempBuf, &cHdr, sizeof(struct customHdr));
    memcpy(tempBuf + sizeof(struct customHdr), buffer, *nBytes);

    (*nBytes) += sizeof(struct customHdr); 
    memcpy(buffer, tempBuf, *nBytes);
    if(DEBUG){
        printf("Just added header %d, seq:%d, ack:%d with total size %d\n", type, seqNum, ackNum, *nBytes);
    }
}

//Need to assume proxySock is non-blocking
void reconnectToProxy(int *proxySock, char *serverEth1IPAddress){
    int proxySockFD;
    struct sockaddr_in proxyAddr;
    struct pollfd pollFD;
    int option  = 1;
    //Make a TCP connection to server port 6200(connect to sproxy)
    if(DEBUG){
        printf("Now trying to connect to server with eth1 IP addr: %s\n", serverEth1IPAddress);
    }
    proxySockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(proxySockFD < 0){
        error("Error opening socket\n");
    }
    if(setsockopt(proxySockFD,SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR),(char*)&option,sizeof(option)) < 0)
    {
        printf("setsockopt failed\n");
        close(proxySockFD);
        exit(2);
    }   

    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_addr.s_addr = inet_addr(serverEth1IPAddress);
    proxyAddr.sin_port = htons(OUTGOING_PORT); //CHANGE WHEN DEBUGGING TO TELNET_PORT/OUTGOING_PORT
    memset(proxyAddr.sin_zero, '\0', sizeof(proxyAddr.sin_zero));
    
    printf("Now trying to attempting to connect to server\n");
    fcntl(proxySockFD, F_SETFL, O_NONBLOCK);
    
    int rv;
    int i = 0;
    for(rv = -1; rv < 0; ){
        rv = connect(proxySockFD, (struct sockaddr *) &proxyAddr, sizeof(proxyAddr));
        if( rv == -1 ){
            if(DEBUG)
                perror("Error connecting to proxy\n");
        }
        //Wait one sec
        pollFD.fd = proxySockFD;
        pollFD.events = POLLIN;
        
        poll(&pollFD, 1, WAITTIME);
        i += WAITTIME/1000;
        if(DEBUG){
            printf("Has been trying to reconnect for %d seconds (Can take up to 3 min)\n", i);
        }
        
    }

    if(DEBUG){
        printf("Now connected to server side\n");
    }

    //Assign all file descriptors
    *proxySock = proxySockFD;
}


void setProxySocket(int *proxySock){
    int proxySockFD;
    int option  = 1;

    //Make a TCP connection to server port 6200(connect to sproxy)
    proxySockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(proxySockFD < 0){
        error("Error opening socket\n");
    }
    if(setsockopt(proxySockFD,SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR),(char*)&option,sizeof(option)) < 0)
    {
        printf("setsockopt failed\n");
        close(proxySockFD);
        exit(2);
    }   

    if(DEBUG){
        printf("Now trying to attempting to connect to server\n");
    }
    
    fcntl(proxySockFD, F_SETFL, O_NONBLOCK);

    //Assign all file descriptors
    *proxySock = proxySockFD;
}

int tryToConnect(int proxySockFD, char *serverEth1IPAddress){
    struct sockaddr_in proxyAddr;
    struct pollfd pollFD;
    int rv;

    if(DEBUG){
        printf("Now trying to connect ONCE to server with eth1 IP addr: %s\n", serverEth1IPAddress);
    }

    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_addr.s_addr = inet_addr(serverEth1IPAddress);
    proxyAddr.sin_port = htons(OUTGOING_PORT); //CHANGE WHEN DEBUGGING TO TELNET_PORT/OUTGOING_PORT
    memset(proxyAddr.sin_zero, '\0', sizeof(proxyAddr.sin_zero));
  
    rv = connect(proxySockFD, (struct sockaddr *) &proxyAddr, sizeof(proxyAddr));
    if( rv == -1 ){
        if(DEBUG)
            perror("Error connecting to trying to reconnect to proxy\n");
    }
    //Wait one sec
    pollFD.fd = proxySockFD;
    pollFD.events = POLLIN;
        
    if(DEBUG){
        printf("Will been try to reconnect in %d seconds (Can take up to 3 min)\n", WAITTIME/1000);
    }
    poll(&pollFD, 1, WAITTIME);

    if(rv == -1){
        return -1;
    }else{
        if(DEBUG){
            printf("Now connected to server side\n");
        }
        return 0;
    }
}



void rememberData(struct packetData **startPacket, void *buffer, uint32_t id, int nBytes){
    if(DEBUG){
        printf("Remembering data with id: %d\n", id);
    }
    if(*startPacket == NULL){
        if(DEBUG){
            printf("Remembering first data\n");
        }
        (*startPacket) = (struct packetData *)malloc(sizeof(struct packetData));
        (*startPacket)->next = NULL;
        (*startPacket)->id = id;
        (*startPacket)->size = nBytes;
        memcpy((*startPacket)->data, buffer, nBytes);
        if(DEBUG){
            printf("Just stored packet with id %d of size %d\n", (*startPacket)->id, (*startPacket)->size);
        }
    }else{
        addData(*startPacket, buffer, id, nBytes);
    }
}

//Adds data to end of list recursively
void addData(struct packetData *pData, void *buffer, uint32_t id, int nBytes){
    if(pData->next == NULL){
        pData->next = (struct packetData *)malloc(sizeof(struct packetData));
        struct packetData *newData = pData->next;
        newData->next = NULL;
        newData->id = id;
        newData->size = nBytes;
        memcpy(newData->data, buffer, nBytes);
        if(DEBUG){
            printf("Just stored packet with id %d\n", newData->id);
        }
    }else{
        //Go closer to end of linked list
        addData(pData->next, buffer, id, nBytes);
    }
}

void eraseAllData(struct packetData **startPacket){
    if(DEBUG){
        printf("Erasing all data\n");
    }
    if(*startPacket == NULL){
        if(DEBUG){
            perror("Trying to erase stored packets from an empty list!\n");
        }
    }else{
        *startPacket = deleteAllData(*startPacket);
    }
}

struct packetData *deleteAllData(struct packetData *pData){
    struct packetData *tempData;
    tempData = pData->next;
    if(tempData == NULL){
        if(DEBUG){
            printf("Deleted all packets\n");
        }
        return NULL;
    }else{
        if(DEBUG){
            printf("Deleted a packet\n");
        }
        free(pData);
        return deleteAllData(tempData);
    }
}

void sendStoredData(int sockFD, struct packetData *pData){
    if(pData != NULL){
        
        int nBytes = pData->size;
        if(sendall(sockFD, pData->data, &nBytes, 0) == -1){
            perror("Error with send\n");
            printf("Only sent %d bytes because of error!\n", nBytes);
        }
        if(DEBUG){
            printf("transmitted a packet of size %d and id of %d\n", nBytes, pData->id);
        }

        sendStoredData(sockFD, pData->next);
    }
    //else return
}
