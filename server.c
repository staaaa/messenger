#include <netinet/in.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>

#define PORT 5000
#define MAX_CLIENTS 20
#define BUFFER_SIZE 4096
#define LOGIN_SIZE 256

// We are defining the type which will be held in queue
struct entry{
  char* message;
  STAILQ_ENTRY(entry) entries;
};

// Here we are creating a structure for head
STAILQ_HEAD(stailhead, entry);

// here we are creating structs to resemble a client
struct client{
  int socket;
  char login[LOGIN_SIZE];
};

// List of clients and number of clients
struct client* client_list[MAX_CLIENTS];
int num_of_clients = 0;
struct stailhead message_queue;

// To lock client list
pthread_mutex_t mutex_cl = PTHREAD_MUTEX_INITIALIZER;

// To lock message_queue
pthread_mutex_t mutex_mq = PTHREAD_MUTEX_INITIALIZER; 

// Function that adds new client to array of clients
void addNewClient(int* socket){
  pthread_mutex_lock(&mutex_cl);
  if(num_of_clients >= MAX_CLIENTS){
    printf("ERRR: Reached max number of clients");
    close(*socket);
  }
  else{
    struct client* cl;
    cl->socket = *socket;
    client_list[num_of_clients] = cl;
    num_of_clients++;
  }
  pthread_mutex_unlock(&mutex_cl);
}


void* clientHandler(void* args){
  int client_socket = *(int*) args;
  free(args);
  char* buffer[BUFFER_SIZE];
  char* login_buffer[LOGIN_SIZE];
  char* buffer_server;

  //ask for user login
  buffer_server = strdup("Insert your login.");
  send(client_socket, &buffer_server, sizeof(buffer_server), 0);
  
  //get a login
  int bytes_read = recv(client_socket, &login_buffer, sizeof(buffer), 0);
  if(bytes_read <= 0){
    printf("Client disconnected. \n");
    close(client_socket);
    remove_client(client_socket);
    return NULL;
  }
  for(int i = 0; i < num_of_clients; i++){
    if(strcmp(client_list[i]->login, login_buffer)){
      buffer_server = strdup("Client with given login already connected. Disconnecting...");
      send(
    }
  }
  //aks who he wants to write to (login)
  //wait for client to write the message
  //create a new message (from, to, body)
  //add the message to queue
  while(1){
    send(client_socket, &buffer, sizeof(buffer), 0);
    int bytes_read = recv(client_socket, &buffer, sizeof(buffer), 0);
    if(bytes_read <= 0){
      printf("Client disconnected. \n");
      close(client_socket);
      remove_client(client_socket);
      break;
    }

  }
  return NULL;
}


int main(){
  // Init of queue
  // To copy - elem->str = strdup("string")
  // To insert - STAILQ_INSERT_TAIL(&head, elem, entries); to insert to queue
  STAILQ_INIT(&message_queue);

  //create server_socket
  int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0)
  {
    printf("Couldn't create a server socket. Exiting app... \n");
    exit(1);
  }

  //sockaddr_in - describes IPv4 Internet domain socket address 
  struct sockaddr_in server_addr, client_addr;
  memset(server_addr.sin_zero, '\0', sizeof(server_addr.sin_zero));
  server_addr.sin_family = AF_INET;         // IPv4 family
  server_addr.sin_port = htons(PORT);       // setting port, htons converts short to network byte order (to big endian)
  server_addr.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY sets the IP address to the machine address

  // try binding the socket to the address
  if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    printf("Couldn't bind the socket to the address");
    exit(1);
  }
  listen(server_socket, 5);
  printf("Server is listening on port: %d...\n", PORT);

  // Buffer for reading and sending data
  char buffer[BUFFER_SIZE];
  
  int* client_socket;
  while(1){
    int clilen = sizeof(client_addr);
    client_socket = malloc(sizeof(int));

    if(!client_socket){
      printf("Couldnt allocate memory for descriptor of client socket");
      exit(1);
    }

    *client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &clilen);
    if (*client_socket < 0){
      printf("Couldnt accept the connection from client. \n");
      continue;
    }
    addNewClient(client_socket);
    pthread_t tid;
    if(pthread_create(&tid, NULL, clientHandler, client_socket) != 0)
    {
      printf("There was an error creating client thread. \n");
      free(client_socket);
      continue;
    }
    printf("New client connected. \n");
    pthread_detach(tid);
  }
}
