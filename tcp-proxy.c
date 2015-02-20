/*
 * Copyright (C) 2014 Ki Suh Lee (kslee@cs.cornell.edu)
 *
 * TCP Proxy skeleton.
 * 
 * Adithya Venkatesh - av445
 * I discussed designs for the code with Remya, Zhihong and Amarinder.
 * No code was copied!
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include "list.h"

#define MAX_LISTEN_BACKLOG 5
#define MAX_ADDR_NAME 32
#define ONE_K 1024

/* Data that can pile up to be sent before reading must stop temporarily */
#define MAX_CONN_BACKLOG  (8*ONE_K)
#define GRACE_CONN_BACKLOG  (MAX_CONN_BACKLOG / 2)

/* Watermarks for number of active connections. Lower to 2 for testing */
#define MAX_CONN_HIGH_WATERMARK (256)
#define MAX_CONN_LOW_WATERMARK  (MAX_CONN_HIGH_WATERMARK - 1)

#define MAX_THREAD_NUM 4

#define BUF_SIZE 8192

struct sockaddr_in remote_addr; /* The address of the target server */

typedef struct connections {
  int client_fd, server_fd;
  char s1[BUF_SIZE], s2[BUF_SIZE];
  int cLen, sLen;
  int cflag, sflag;
  struct list_head mylist ;
}avar;

typedef struct connections conn;

struct list_head listNode[MAX_THREAD_NUM]; 
pthread_t threads[MAX_THREAD_NUM];

sem_t readyToSend[MAX_THREAD_NUM];
sem_t createConnection;

pthread_mutex_t mutexList;

struct list_head *closeConnection(struct connections *connection){
    close(connection->client_fd);
    close(connection->server_fd);
    return &connection->mylist;
}

void *transmit(void *threadid){
  pthread_t id = pthread_self();
  
  int tid;
  int i=0;

  for(i =0; i<MAX_THREAD_NUM; i++){
    if(pthread_equal(id,threads[i]))
      tid = i;
  }

  struct timeval tv;

  struct list_head *connList=(struct list_head *)threadid;
  struct list_head *connArray[MAX_CONN_HIGH_WATERMARK];
  
  int bytesRead,bytesWritten;
  fd_set readfd, writefd;
  int numConnections = 0;
  
  sem_wait(&readyToSend[tid]);  //SEMAPHORE_WAIT

  while(1){
    numConnections = 0;

    // start to add read fd
    FD_ZERO(&readfd);

    struct connections *connpos;
    int numReadConn = 0;
    int numWriteConn = 0;
    
    //Counting the number of readers and writers
    list_for_each_entry(connpos, connList, mylist){
      if(connpos->cLen < BUF_SIZE && connpos->cflag != 1){
        FD_SET(connpos->client_fd, &readfd);
        numReadConn++;
      }
      if(connpos->sLen < BUF_SIZE && connpos->sflag != 1){
        FD_SET(connpos->server_fd, &readfd);
        numReadConn++;
      }
    }
  
    int selectval;
    tv.tv_sec =  1;
    tv.tv_usec = 0;
    bytesRead = 0;

    if(numReadConn > 0){
      selectval = select(FD_SETSIZE, &readfd, NULL, NULL, &tv);
      if(selectval){
        list_for_each_entry(connpos, connList, mylist){
          if(FD_ISSET(connpos->client_fd, &readfd)){
            bytesRead = recv(connpos->client_fd, connpos->s1, BUF_SIZE, 0);
            if(bytesRead == 0){
              connpos->cflag = 1;
              shutdown(connpos->client_fd, SHUT_RD);
              if((connpos->cflag && connpos->cLen == 0))
                connArray[numConnections++] = closeConnection(connpos);
            }
            connpos->cLen += bytesRead;
          }

          if(FD_ISSET(connpos->server_fd, &readfd)){
            bytesRead=recv(connpos->server_fd, connpos->s2, BUF_SIZE-connpos->sLen, 0);
              if(bytesRead == 0){
                connpos->sflag = 1;
                shutdown(connpos->server_fd, SHUT_RD);
                if((connpos->sflag && connpos->sLen == 0))
                  connArray[numConnections++] = closeConnection(connpos);
              }
              connpos->sLen += bytesRead;
          }
        }
      }
    }
  
    list_for_each_entry(connpos, connList, mylist){
      numWriteConn = 0;
      FD_ZERO(&writefd);
      if(connpos->cLen > 0){
        FD_SET(connpos->server_fd, &writefd);
        numWriteConn++;
      }
      if(connpos->sLen > 0){
        FD_SET(connpos->client_fd, &writefd);
        numWriteConn++;
      } 

      tv.tv_sec = 10;
      tv.tv_usec = 0;
      bytesWritten = 0;

      if(numWriteConn > 0){
        selectval=select(FD_SETSIZE, NULL, &writefd, NULL, &tv);
        if(selectval > 0){
          if(FD_ISSET(connpos->server_fd, &writefd)){
            bytesWritten=send(connpos->server_fd, connpos->s1, connpos->cLen, 0);
            connpos->cLen -= bytesWritten;
            if((connpos->cflag && connpos->cLen == 0))
              connArray[numConnections++] = closeConnection(connpos);
          }

          if(FD_ISSET(connpos->client_fd, &writefd)){
            bytesWritten=send(connpos->client_fd, connpos->s2, connpos->sLen, 0);
            connpos->sLen -= bytesWritten;
            if((connpos->sflag&&connpos->sLen == 0))
              connArray[numConnections++] = closeConnection(connpos);
          }
        }
        else{
          if(connpos->cLen>0 || connpos->sLen > 0)
            connArray[numConnections++] = closeConnection(connpos);
        }
      } 
    }

    for(i=0; i<numConnections; i++){
      pthread_mutex_lock(&mutexList);
      list_del(connArray[i]);
      pthread_mutex_unlock(&mutexList);
      free(list_entry(connArray[i],conn,mylist));
      sem_post(&createConnection);
      sem_wait(&readyToSend[tid]);  //SEMAPHORE_WAIT  
    }
  }
}

void __loop(int proxy_fd)
{
  struct sockaddr_in client_addr;
  socklen_t addr_size;
  int client_fd, server_fd;
  struct client *client;
  int currentThread = 0;
  struct worker_thread *thread;
  char client_hname[MAX_ADDR_NAME+1];
  char server_hname[MAX_ADDR_NAME+1];
  int i=0;
  
  int res = sem_init(&createConnection, 0, MAX_CONN_HIGH_WATERMARK);  
    
  if(res == -1)  {  
    printf("Semaphore couldn't be initialized\n");
    exit(1);
  }  

  while(1) {
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    sem_wait(&createConnection);    //SEMAPHORE WAIT
    addr_size = sizeof(client_addr);
    client_fd = accept(proxy_fd, (struct sockaddr *)&client_addr,
          &addr_size);
    if(client_fd == -1) {
      fprintf(stderr, "accept error %s\n", strerror(errno));
      continue;
    }

    // For debugging purpose
    if (getpeername(client_fd, (struct sockaddr *) &client_addr, &addr_size) < 0) {
      fprintf(stderr, "getpeername error %s\n", strerror(errno));
    }

    strncpy(client_hname, inet_ntoa(client_addr.sin_addr), MAX_ADDR_NAME);
    strncpy(server_hname, inet_ntoa(remote_addr.sin_addr), MAX_ADDR_NAME);

    // TODO: Disable following printf before submission
    /*printf("Connection proxied: %s:%d --> %s:%d\n",
        client_hname, ntohs(client_addr.sin_port),
        server_hname, ntohs(remote_addr.sin_port));*/

    // Connect to the server
    if ((server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
      fprintf(stderr, "socket error %s\n", strerror(errno));
      close(client_fd);
      continue;
    }

    if (connect(server_fd, (struct sockaddr *) &remote_addr,
      sizeof(struct sockaddr_in)) <0) {
      if (errno != EINPROGRESS) {
        fprintf(stderr, "connect error %s\n", strerror(errno));
        close(client_fd);
        close(server_fd);
          continue;
      }
    }
    
    //Received a new connection
    conn* node=(conn*)malloc(sizeof(conn)) ;
    INIT_LIST_HEAD( & node->mylist);
    
    node->client_fd=client_fd;
    node->server_fd=server_fd;
    
    memset(node->s1,0,sizeof(node->s1));  
    memset(node->s2,0,sizeof(node->s1));
    
    node->cLen = 0;        //Initialize positions and flags for the new connection.
    node->sLen = 0;
    node->cflag = 0;
    node->sflag = 0;  
    
    //Add client to the shared list of connections
    pthread_mutex_lock(&mutexList);
    list_add_tail(&node->mylist,&listNode[currentThread]);
    pthread_mutex_unlock(&mutexList);
    
    sem_post(&readyToSend[currentThread]);
    currentThread=(currentThread+1)%MAX_THREAD_NUM; //Round robin on threads
  }
}

int main(int argc, char **argv)
{
  char *remote_name;
  struct sockaddr_in proxy_addr;
  unsigned short local_port, remote_port;
  struct hostent *h;
  int arg_idx = 1, proxy_fd;
  int i;

  if (argc != 4)
  {
    fprintf(stderr, "Usage %s <remote-target> <remote-target-port> "
      "<local-port>\n", argv[0]);
    exit(1);
  }

  remote_name = argv[arg_idx++];
  remote_port = atoi(argv[arg_idx++]);
  local_port = atoi(argv[arg_idx++]);

  /* Lookup server name and establish control connection */
  if ((h = gethostbyname(remote_name)) == NULL) {
    fprintf(stderr, "gethostbyname(%s) failed %s\n", remote_name,
      strerror(errno));
    exit(1);
  }

  memset(&remote_addr, 0, sizeof(struct sockaddr_in));
  remote_addr.sin_family = AF_INET;
  memcpy(&remote_addr.sin_addr.s_addr, h->h_addr_list[0], sizeof(in_addr_t));
  remote_addr.sin_port = htons(remote_port);

  /* open up the TCP socket the proxy listens on */
  if ((proxy_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    fprintf(stderr, "socket error %s\n", strerror(errno));
    exit(1);
  }
  
  /* bind the socket to all local addresses */
  memset(&proxy_addr, 0, sizeof(struct sockaddr_in));
  proxy_addr.sin_family = AF_INET;
  proxy_addr.sin_addr.s_addr = INADDR_ANY; /* bind to all local addresses */
  proxy_addr.sin_port = htons(local_port);
  if (bind(proxy_fd, (struct sockaddr *) &proxy_addr,
    sizeof(proxy_addr)) < 0) {
    fprintf(stderr, "bind error %s\n", strerror(errno));
    exit(1);
  }

  listen(proxy_fd, MAX_LISTEN_BACKLOG);

  pthread_mutex_init(&mutexList,NULL);
  
  for(i=0; i<MAX_THREAD_NUM; i++){
    INIT_LIST_HEAD(&listNode[i]);
    int err = pthread_create(&(threads[i]), NULL, &transmit, &listNode[i]);
    if(err)
      printf("Can't create thread :[%s]\n", strerror(err));
    //else
    //  printf("Created thread\n");
    int rc = sem_init(&readyToSend[i], 0, 0);  
    if(rc == -1)  
    {  
      printf("Sem initialization error\n");
      exit(1);
    }  
  }

  __loop(proxy_fd);

  return 0;
}  
