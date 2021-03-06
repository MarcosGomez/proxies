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
#include <sys/time.h>

#define DEBUG 1
#define INCOMING_PORT 6200
#define OUTGOING_PORT 23
#define BACKLOG 10  //how many pending connections queue will hold
#define LOCAL_POLL 0
#define PROXY_POLL 1
#define NUM_OF_SOCKS 2
#define TIMEOUT 1000
#define MAX_BUFFER_SIZE 16384

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

//int gettimeofday(struct timeval *tv, struct timezone *tz);
// struct timeval {
//     time_t      tv_sec;     // seconds 
//     suseconds_t tv_usec;    // microseconds 
// };
// struct timezone {
//     int tz_minuteswest;     ///* minutes west of Greenwich */
//     int tz_dsttime;         ///* type of DST correction */
// };

void usage(char *argv[]);
void error(char *msg);
void setUpConnections(int *localSock, int *proxySock, int *listenSock);
int sendall(int s, char *buf, int *len, int flags);
void sendHeartBeat(int pSockFD);
void sendAck(int pSockFD);
void processReceivedHeader(int sockFD, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag, uint32_t *ackNum);
int removeHeader(char *buffer, int *nBytes, int *rType, uint32_t *ackNum);
int receiveProxyPacket(int sockFD, int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, struct timeval *receiveTime, uint32_t *ackNum);
void addHeader(void *buffer, int *nBytes, uint8_t type, uint32_t seqNum, uint32_t ackNum);
void reconnectToProxy(int *listenSock, int *proxySock);
void rememberData(struct packetData **startPacket, void *buffer, uint32_t id, int nBytes);
void addData(struct packetData *pData, void *buffer, uint32_t id, int nBytes);
void eraseData(struct packetData **startPacket, uint32_t id);
struct packetData *deleteData(struct packetData *pData, uint32_t id);
int receiveLocalPacket(int sockFD, int *nBytes, int flag, char *buffer, int *sendTo, int *isOOB, uint32_t *seqNum, struct packetData **startPacket);
void retransmitUnAckedData(int sockFD, struct packetData *pData);
int retryProxyConnection(int *listenSock, int *proxySock);
void listenForReconnect(int *listenSock);
int checkIfInit(int sockFD, int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, struct timeval *receiveTime, uint32_t *ackNum);
int processHeaderForInit(int sockFD, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag, uint32_t *ackNum);
void eraseAllData(struct packetData **startPacket);
struct packetData *deleteAllData(struct packetData *pData);
void setUpLocal(int *localSock);

int main( void ){
    int localSockFD, proxySockFD, listenSockFD;
    int returnValue;
    int nBytesLocal, nBytesProxy;
    struct pollfd pollFDs[NUM_OF_SOCKS];
    char bufProxy[MAX_BUFFER_SIZE], bufLocal[MAX_BUFFER_SIZE];

    int sendToProxy, sendToLocal; //booleans
    int isOOBProxy, isOOBLocal; //bool, is out-of-band
    int notSentProxy, notSentLocal; //bool
    int closeSession;//bool

    int numTimeouts;
    struct timeval receiveTime;
    struct timeval timeNow;

    struct packetData *storedPackets;
    uint32_t sequenceNum;
    uint32_t receivedAckNum;

    int isProxyConnection; //bool
    int startWithProxy; //bool

    startWithProxy = 0;
    while(1){
        printf("Starting up the server...\n");

        if(startWithProxy){
            setUpLocal(&localSockFD);
        }else{
            setUpConnections(&localSockFD, &proxySockFD, &listenSockFD);
            close(listenSockFD);
        }
        
        startWithProxy = 0;
        isProxyConnection = 1;
        storedPackets = NULL;
        sequenceNum = 1;
        receivedAckNum = 0;
        closeSession = 0;
        
        //Keep relaying data between 2 sockets using select() or poll()
        //Keep proxy up until connection is dead
        for(;;){
            //Reconnect again
            pollFDs[LOCAL_POLL].fd = localSockFD;
            pollFDs[LOCAL_POLL].events = POLLIN | POLLPRI | POLLOUT;

            pollFDs[PROXY_POLL].fd = proxySockFD;
            pollFDs[PROXY_POLL].events = POLLIN | POLLPRI | POLLOUT;

            sendToProxy = sendToLocal = isOOBProxy = isOOBLocal = notSentLocal = notSentProxy 
            = numTimeouts = 0; //Initalize to false
            gettimeofday(&receiveTime, NULL);
            gettimeofday(&timeNow, NULL);
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
                        sendHeartBeat(proxySockFD);
                    }
                }else{

                    if(isProxyConnection){
                        //Check proxy events - HEADER MANAGEMENT
                        if(notSentProxy){
                            if(DEBUG){
                                printf("Skipping recieve to wait to send past data for proxy\n");
                            }
                        }else{
                            //RECEIVE - NEED TO CHECK AND REMOVE HEADER
                            if(pollFDs[PROXY_POLL].revents & POLLPRI){
                                if(receiveProxyPacket(proxySockFD, &nBytesProxy, 1, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal, &receiveTime, &receivedAckNum)
                                 == -1){
                                    closeSession = 1;
                                    break;
                                }
                                if(receivedAckNum != 0){
                                    eraseData(&storedPackets, receivedAckNum);
                                }
                            }else if(pollFDs[PROXY_POLL].revents & POLLIN){
                                if(receiveProxyPacket(proxySockFD, &nBytesProxy, 0, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal, &receiveTime, &receivedAckNum)
                                 == -1){
                                    closeSession = 1;
                                    break;
                                }
                                if(receivedAckNum != 0){
                                    eraseData(&storedPackets, receivedAckNum);
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
                            if(pollFDs[PROXY_POLL].revents & POLLNVAL){
                                perror("The socket file desc is not open\n");
                                closeSession = 1;
                                break;
                            }
                            
                        }
                    }
                    
                    


                    //Check local events
                    if(notSentLocal){
                        if(DEBUG){
                            printf("Skipping recieve to wait to send past data for local\n");
                        }
                    }else{
                        //RECEIVE
                        if(pollFDs[LOCAL_POLL].revents & POLLPRI){
                            if(receiveLocalPacket(localSockFD, &nBytesLocal, 1, bufLocal, &sendToProxy, &isOOBProxy, &sequenceNum, &storedPackets) == -1){
                                closeSession = 1;
                                break;
                            }
                        }else if(pollFDs[LOCAL_POLL].revents & POLLIN){
                            if(receiveLocalPacket(localSockFD, &nBytesLocal, 0, bufLocal, &sendToProxy, &isOOBProxy, &sequenceNum, &storedPackets) == -1){
                                closeSession = 1;
                                break;
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
                        if(pollFDs[LOCAL_POLL].revents & POLLNVAL){
                            perror("The socket file desc is not open\n");
                        }
                    }


                    //Check proxy connection when high traffic
                    gettimeofday(&timeNow, NULL);
                    if(timeNow.tv_sec - receiveTime.tv_sec >= 1){
                        numTimeouts = (int) timeNow.tv_sec - receiveTime.tv_sec;
                        if(DEBUG)
                            printf("Timeout occured by gettimeofday! No data after %d seconds\n", numTimeouts);

                        if(numTimeouts >= 3 && numTimeouts < 9999){
                            if(DEBUG){
                                printf("Lost connection, time to close failed socket\n");
                            }
                            break;
                        }else{
                            //Send out hearbeat message
                            sendHeartBeat(proxySockFD);
                        }
                    }
                }
                //Only do this loop once
                if(!isProxyConnection){
                    break;
                }
            }//end while



            if(isProxyConnection){
                //Do this once for each connection loss
                if(DEBUG){
                    printf("SHOULD ONLY HAPPEN ONCE PER CONNECTION.\nclosing proxySockFD\n");
                }
                close(proxySockFD);

                listenForReconnect(&listenSockFD);
            }
            
            if(closeSession){
                if(DEBUG){
                    printf("Breaking because session closed\n");
                }
                eraseAllData(&storedPackets);
                break;
            }else{
                //Only retransmit after known connection loss
                if(DEBUG){
                    printf("receivedAckNum = %d and sequenceNum = %d\n", receivedAckNum, sequenceNum);
                }
                if( retryProxyConnection(&listenSockFD, &proxySockFD) == 0 ){
                    isProxyConnection = 1;
                    //close(listenSockFD);//EDIT retryProxyConnection so same listenSock
                    if(checkIfInit(proxySockFD, &nBytesProxy, 0, bufProxy, &numTimeouts, &sendToLocal, &isOOBLocal, &receiveTime, &receivedAckNum) == 0){
                        if(DEBUG){
                            printf("Restarting telnet connection with server\n");
                        }
                        //Need to reset everything (eg linked list) and reconnect to local side
                        eraseAllData(&storedPackets);
                        closeSession = 1;
                        startWithProxy = 1;
                        break;
                    }else{
                        if(DEBUG){
                            printf("Sending out data to local because not an Init\n");
                        }
                        //Send out data it received from checking if init
                        if(sendToLocal){
                            //Normal
                            if(sendall(localSockFD, bufProxy, &nBytesProxy, 0) == -1){
                                perror("Error with send\n");
                                printf("Only sent %d bytes because of error!\n", nBytesProxy);
                            }
                        }
                        retransmitUnAckedData(proxySockFD, storedPackets);
                    }
                }else{
                    isProxyConnection = 0;
                    if(DEBUG){
                        printf("Cannot connect to proxy now, checking for data from local\n");
                    }
                }
            }
            //eraseAllData(&storedPackets);
            
        }//End for(;;)
        close(localSockFD);
        close(listenSockFD);
    }//while(1)
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
    int localSockFD;
    struct sockaddr_in localAddr;
    
    reconnectToProxy(listenSock, proxySock);
    
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
        error("Error connecting to local\n");
    }else{
        if(DEBUG){
            printf("Now connected to server side after restarting\n");
        }
    }
    

    //Assign all file descriptors
    *localSock = localSockFD;
    
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

void sendHeartBeat(int pSockFD){
    if(DEBUG){
        printf("Sending heartbeat\n");
    }
    char bufHeart[MAX_BUFFER_SIZE];
    int nBytesHeart = 0;
    addHeader(bufHeart, &nBytesHeart, HEARTBEAT, 0, 0);
    if(sendall(pSockFD, bufHeart, &nBytesHeart, 0) == -1){
        perror("Error with send\n");
        printf("Only sent %d bytes because of error!\n", nBytesHeart);
    }
}

void sendAck(int pSockFD){
    if(DEBUG){
        printf("Sending ack to heartbeat\n");
    }
    char bufAck[MAX_BUFFER_SIZE];
    int nBytesAck = 0;
    addHeader(bufAck, &nBytesAck, ACK, 0, 0);
    if(sendall(pSockFD, bufAck, &nBytesAck, 0) == -1){
        perror("Error with send\n");
        printf("Only sent %d bytes because of error!\n", nBytesAck);
    }
}

void processReceivedHeader(int sockFD, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag, uint32_t *ackNum){

    int type;
    int pastType = -1;
    //uint32_t pastSeqNum = *seqNum;
    int tempNBytes = *nBytes;
    uint32_t pLoadLength;
    int rVal;

    //Remove multiple headers on same buffer
    do{
        rVal = removeHeader(buffer, &tempNBytes, &type, ackNum);
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
            sendAck(sockFD);
        }else if(type == ACK){
            if(DEBUG){
                printf("received ACK\n");
            }
            *numTimeouts = 0;
        }else if(type == INIT){
            if(DEBUG){
                printf("RECEIVED A NEW CONNECTION INITIATION\n");
            }
        }else{
            perror("Received unknown type of header!\n");
            pastType = -1;
            //*seqNum = pastSeqNum;
        }
    }while(rVal >= 0);
    
    if(pastType == DATA){
        if(DEBUG){
            printf("Received normal data\n");
        }
        // if(pastSeqNum >= *seqNum){
        //     //Then don't send it out
        //     if(DEBUG){
        //         printf("Not sending out this packet because seqNum too low!\n");
        //     }
        // }else{
            *sendTo = 1;
            if(flag){
                *isOOB = 1;
            }else{
                *isOOB = 0;
            }
        //}
    }
}

int removeHeader(char *buffer, int *nBytes, int *rType, uint32_t *ackNum){
    struct customHdr *cHdr;
    int type;
    char tempBuf[MAX_BUFFER_SIZE];
    uint32_t pLength;

    cHdr = (struct customHdr *) buffer;
    
    //Process Header
    type = cHdr->type;
    pLength = ntohl(cHdr->payloadLength);
    if(ntohl(cHdr->ackNum) > *ackNum){
        *ackNum = ntohl(cHdr->ackNum);
    }else{
        if(DEBUG){
            printf("ignored ackNum because too small!\n");
        }
    }

    // if(ntohl(cHdr->seqNum) != ((*seqNum) + 1) && type == DATA){
    //     if(DEBUG){
    //         printf("Missing a packet!\n");
    //     }
    // }
    
    if(DEBUG){
        printf("Received packet of type %d with seqNum %d and payload size %d\n", type, *ackNum, pLength);
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

int receiveProxyPacket(int sockFD, int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, struct timeval *receiveTime, uint32_t *ackNum){
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

    gettimeofday(receiveTime, NULL);
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
        processReceivedHeader(sockFD, buffer, numTimeouts, sendTo, isOOB, nBytes, flag, ackNum);                  
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

void reconnectToProxy(int *listenSock, int *proxySock){
    int proxySockFD, listenSockFD;
    struct sockaddr_in proxyAddr;
    struct sockaddr_storage connectingAddr;
    socklen_t addrLen;
    int option = 1;

    if(DEBUG){
        printf("Now trying to reconnect to listen and proxy socket\n");
    }

    listenSockFD = socket(PF_INET, SOCK_STREAM, 0);
    if(listenSockFD < 0){
        error("Error opening listen socket\n");
    }
    if(setsockopt(listenSockFD,SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR),(char*)&option,sizeof(option)) < 0)
    {
        printf("setsockopt failed\n");
        close(listenSockFD);
        exit(2);
    }   
    
    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    proxyAddr.sin_port = htons(INCOMING_PORT);
    memset(proxyAddr.sin_zero, '\0', sizeof(proxyAddr.sin_zero));

    if(bind(listenSockFD, (struct sockaddr *) &proxyAddr, sizeof(proxyAddr)) < 0){
        error("Error on binding listen\n");
    }

    //Listen on port 6200 for incoming connection
    printf("Listening for connections...");
    if(DEBUG){
        printf("(Use \"telnet 192.168.8.2 6200\" for debugging)\n");
    }else{
        printf("\n");
    }
    if(listen(listenSockFD, BACKLOG) < 0){
        error("Error when listening\n");
    }

    if(DEBUG){
        printf("Now trying to accept connection\n");
    }

    // struct pollfd pollFD;
    // printf("Now trying to attempting to connect to server\n");
    // fcntl(proxySockFD, F_SETFL, O_NONBLOCK);
    
    // int rv;
    // int i = 0;
    // for(rv = -1; rv < 0; ){
    //     rv = accept(proxySockFD, (struct sockaddr *) &connectingAddr, sizeof(connectingAddr));
    //     if( rv == -1 ){
    //         perror("Error connecting\n");
    //     }
    //     //Wait one sec
    //     pollFD.fd = proxySockFD;
    //     pollFD.events = POLLIN;
        
    //     poll(&pollFD, 1, WAITTIME);
    //     i += WAITTIME/1000;
    //     if(DEBUG){
    //         printf("Has been trying to reconnect for %d seconds (Can take up to 3 min)\n", i);
    //     }
        
    // }

    //Accept the connection from telnet/cproxy
    addrLen = sizeof(connectingAddr);
    proxySockFD = accept(listenSockFD, (struct sockaddr *) &connectingAddr,  &addrLen); //This actually waits
    if(proxySockFD < 0){
        error("Error accepting connection to proxy\n");
    }else{
        if(DEBUG){
            printf("Accepted a connection\n");
        }
    }
    

    *listenSock = listenSockFD;
    *proxySock = proxySockFD;
}

void rememberData(struct packetData **startPacket, void *buffer, uint32_t id, int nBytes){
    if(DEBUG){
        printf("Remembering data with id: %d\n", id);
    }
    if(*startPacket == NULL){
        if(DEBUG){
            printf("Adding first data\n");
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

void eraseData(struct packetData **startPacket, uint32_t id){
    if(DEBUG){
        printf("Erasing all data up to id: %d\n", id);
    }
    if(*startPacket == NULL){
        if(DEBUG){
            perror("Trying to erase stored packets from an empty list!\n");
        }
    }else{
        *startPacket = deleteData(*startPacket, id);
    }
}

struct packetData *deleteData(struct packetData *pData, uint32_t id){
    struct packetData *tempData;
    tempData = pData->next;
    if(pData->id > id){
        if(DEBUG){
            printf("Received an id to remove that is smaller than first!\n");
        }
        return pData;
    }else if(pData->id == id){
        if(DEBUG){
            printf("Deleted last packet\n");
        }
        free(pData);
        return tempData;
    }else if(tempData == NULL){
        perror("Packet to delete doesn't exit in the list!!\n");
        return NULL;
    }else{
        if(DEBUG){
            printf("Deleted a packet\n");
        }
        free(pData);
        return deleteData(tempData, id);
    }
}

int receiveLocalPacket(int sockFD, int *nBytes, int flag, char *buffer, int *sendTo, int *isOOB, uint32_t *seqNum, struct packetData **startPacket){
    if(flag){
        if(DEBUG){
            printf("receiving out-of-band data from local\n");
        }
        *nBytes = recv(sockFD, buffer, MAX_BUFFER_SIZE - sizeof(struct customHdr), MSG_OOB); //Receive normal data
    }else{
        if(DEBUG){
            printf("receiving normal data from local\n");
        }
        *nBytes = recv(sockFD, buffer, MAX_BUFFER_SIZE - sizeof(struct customHdr), 0); //Receive normal data
    }
    
    if(*nBytes == -1){
        perror("recv error\n");
    }else if(*nBytes == 0){
        printf("The local side closed the connection on you\n");
        return -1;
    }else{
        if(DEBUG){
            printf("Just recieved %d bytes\n", *nBytes);
        }
        *sendTo = 1;
        if(flag){
            *isOOB = 1;
        }else{
            *isOOB = 0;
        }

        //Store
        addHeader(buffer, nBytes, DATA, *seqNum, 0);
        rememberData(startPacket, buffer, *seqNum, *nBytes);
        (*seqNum)++;
    }

    return 0;
}

void retransmitUnAckedData(int sockFD, struct packetData *pData){
    if(pData != NULL){
        
        int nBytes = pData->size;
        if(sendall(sockFD, pData->data, &nBytes, 0) == -1){
            perror("Error with send\n");
            printf("Only sent %d bytes because of error!\n", nBytes);
        }
        if(DEBUG){
            printf("Retransmitted a packet of size %d and id of %d\n", nBytes, pData->id);
        }

        retransmitUnAckedData(sockFD, pData->next);
    }
    //else return
}

int retryProxyConnection(int *listenSock, int *proxySock){
    int proxySockFD, listenSockFD;
    struct sockaddr_storage connectingAddr;
    socklen_t addrLen;
    struct pollfd pollFD;
    int retVal;

    if(DEBUG){
        printf("Now trying to accept connection\n");
    }
    listenSockFD = *listenSock;

    pollFD.fd = listenSockFD;
    pollFD.events = POLLIN;

    retVal = poll(&pollFD, 1, TIMEOUT);
    if(retVal == -1){
        error("poll Error\n");
        return -1;
    }else if(retVal == 0){
        if(DEBUG){
            printf("No incoming connection :(\n");
        }
        return -1;
    }else{
        //Accept the connection from telnet/cproxy
        addrLen = sizeof(connectingAddr);
        proxySockFD = accept(listenSockFD, (struct sockaddr *) &connectingAddr,  &addrLen); //This actually waits
        if(proxySockFD < 0){
            error("Error accepting connection\n");
        }

        if(DEBUG){
            printf("Accepted a connection!!!!!!!!!!\n");
        }

        *proxySock = proxySockFD;
        return 0;
    }
}

void listenForReconnect(int *listenSock){
    int listenSockFD;
    struct sockaddr_in proxyAddr;
    int option = 1;

    if(DEBUG){
        printf("Now trying to reconnect to listen and proxy socket\n");
    }

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
    
    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    proxyAddr.sin_port = htons(INCOMING_PORT);
    memset(proxyAddr.sin_zero, '\0', sizeof(proxyAddr.sin_zero));

    if(bind(listenSockFD, (struct sockaddr *) &proxyAddr, sizeof(proxyAddr)) < 0){
        error("Error on binding\n");
    }

    //Listen on port 6200 for incoming connection
    printf("Listening for connections...");
    if(DEBUG){
        printf("(Use \"telnet 192.168.8.2 6200\" for debugging)\n");
    }else{
        printf("\n");
    }
    if(listen(listenSockFD, BACKLOG) < 0){
        error("Error when listening\n");
    }

    *listenSock = listenSockFD;
}

//Recieve first packet and check if it's INIT
int checkIfInit(int sockFD, int *nBytes, int flag, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, struct timeval *receiveTime, uint32_t *ackNum){
    //First send ack to establish connection
    sendAck(sockFD);
    if(flag){
        //Then OOB
        if(DEBUG){
            printf("receiving out-of-band data from proxy NOT HERE\n");
        }
        *nBytes = recv(sockFD, buffer, MAX_BUFFER_SIZE - sizeof(struct customHdr), MSG_OOB); //Receive out-of-band data
    }else{
        //Normal
        if(DEBUG){
            printf("receiving data from proxy to check if init\n");
        }
        *nBytes = recv(sockFD, buffer, MAX_BUFFER_SIZE - sizeof(struct customHdr), 0); //Receive out-of-band data
    }

    gettimeofday(receiveTime, NULL);
    *numTimeouts = 0;
    if(*nBytes == -1){
        perror("recv ERROR\n");
        return -2;
    }else if(*nBytes == 0){
        printf("The proxy side closed the connection on you\n");
        return -1;
    }else{
        if(DEBUG){
            printf("Just recieved %d bytes\n", *nBytes);
        }
        return processHeaderForInit(sockFD, buffer, numTimeouts, sendTo, isOOB, nBytes, flag, ackNum);                  
    }
}

int processHeaderForInit(int sockFD, char *buffer, int *numTimeouts, int *sendTo, int *isOOB, int *nBytes, int flag, uint32_t *ackNum){
    int foundINIT = 0;
    int type;
    int pastType = -1;
    int tempNBytes = *nBytes;
    uint32_t pLoadLength;
    int rVal;

    //Remove multiple headers on same buffer
    do{
        rVal = removeHeader(buffer, &tempNBytes, &type, ackNum);
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
            sendAck(sockFD);
        }else if(type == ACK){
            if(DEBUG){
                printf("received ACK\n");
            }
            *numTimeouts = 0;
        }else if(type == INIT){
            if(DEBUG){
                printf("RECEIVED A NEW CONNECTION INITIATION\n");
            }
            foundINIT = 1;
        }else{
            perror("Received unknown type of header!\n");
            pastType = -1;
            //*seqNum = pastSeqNum;
        }
    }while(rVal >= 0);
    
    if(pastType == DATA){
        if(DEBUG){
            printf("Received normal data\n");
        }
        // if(pastSeqNum >= *seqNum){
        //     //Then don't send it out
        //     if(DEBUG){
        //         printf("Not sending out this packet because seqNum too low!\n");
        //     }
        // }else{
            *sendTo = 1;
            if(flag){
                *isOOB = 1;
            }else{
                *isOOB = 0;
            }
        //}
    }

    if(foundINIT){
        return 0;
    }else{
        return -3;
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

void setUpLocal(int *localSock){
    int localSockFD;
    struct sockaddr_in localAddr;
    
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
        printf("Now connected to server side (again with setUpLocal)\n");
    }

    //Assign all file descriptors
    *localSock = localSockFD;
}