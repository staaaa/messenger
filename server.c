#include <netinet/in.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>
#include <stdbool.h>

#define PORT 7992
#define MAX_CLIENTS 20
#define BUFFER_SIZE 4096
#define LOGIN_SIZE 256

// queue message struct
struct entry
{
  char *from_login;
  char *to_login;
  char *message;
  // STAILQ - single tail queue (with pointer to tail)
  STAILQ_ENTRY(entry)
  entries;
};

// definition of queue head
STAILQ_HEAD(stailhead, entry);

// client struct (that is kept in a arr)
struct client
{
  int socket;
  char login[LOGIN_SIZE];
  bool is_logged_in;
  // every client has it's own message queue - for delivering message later after
  // they logged out
  struct stailhead queue;
};

// array of clients
struct client *client_list[MAX_CLIENTS];
int num_of_clients = 0;

// global message queue for all messages
struct stailhead message_queue;

pthread_mutex_t mutex_cl = PTHREAD_MUTEX_INITIALIZER; // mutex for client list
pthread_mutex_t mutex_mq = PTHREAD_MUTEX_INITIALIZER; // mutex for message queue
pthread_cond_t cond_mq = PTHREAD_COND_INITIALIZER;    // condition for message queue

// flag that indicates if server is still running
volatile bool server_running = true;

// Function for handling kill signals
void handle_signal(int sig)
{
  printf("\nPrzerwanie działania klienta...\n");
  server_running = false;
  cleanup();
  exit(0);
}

// Function that removes client from clientlist
void removeClient(struct client *client)
{
  pthread_mutex_lock(&mutex_cl);

  // find client in list
  int index = -1;
  for (int i = 0; i < num_of_clients; i++)
  {
    if (client_list[i] == client)
    {
      index = i;
      break;
    }
  }

  if (index != -1)
  {
    // move all clients (after the found one) one space up in list
    for (int i = index; i < num_of_clients - 1; i++)
    {
      client_list[i] = client_list[i + 1];
    }
    num_of_clients--;
    printf("Klient '%s' został usunięty. Liczba aktywnych klientów: %d\n", client->login, num_of_clients);
  }

  // close socket, free memory
  close(client->socket);
  free(client);

  pthread_mutex_unlock(&mutex_cl);
}

// Function that logs out client (sets its logged = false)
void logoutClient(struct client *client)
{
  pthread_mutex_lock(&mutex_cl);
  int index = -1;
  for (int i = 0; i < num_of_clients; i++)
  {
    if (client_list[i] == client)
    {
      index = i;
      break;
    }
  }

  if (index != -1)
  {
    client_list[index]->is_logged_in = false;
    printf("Klient '%s' został rozłączony.\n", client->login);
  }
  close(client->socket);
  pthread_mutex_unlock(&mutex_cl);
}

// Function that finds a client with given login, returns the pointer to that client
struct client *findClientByLogin(const char *login)
{
  pthread_mutex_lock(&mutex_cl);
  struct client *found = NULL;
  for (int i = 0; i < num_of_clients; i++)
  {
    if (strcmp(client_list[i]->login, login) == 0)
    {
      found = client_list[i];
      break;
    }
  }
  pthread_mutex_unlock(&mutex_cl);
  return found;
}

// Function that adds a message to global queue
// every user whenever sends a new message, its being added to global message queue
// whenever it happens, the condition for messagequeue is set to true, and then server
// sents all messages in queue periodicly
void addMessageToQueue(const char *from_login, const char *to_login, const char *message)
{
  // allocate memory for new message
  struct entry *new_entry = (struct entry *)malloc(sizeof(struct entry));
  if (!new_entry)
  {
    printf("BŁĄD: Nie można zaalokować pamięci dla wiadomości\n");
    return;
  }

  // set values of message fields
  new_entry->from_login = strdup(from_login);
  new_entry->to_login = strdup(to_login);
  new_entry->message = strdup(message);

  struct client *client_to = findClientByLogin(to_login);
  // if client isn't logged in right now, add the message to personal queue to be sent later
  if (!client_to->is_logged_in)
  {
    STAILQ_INSERT_TAIL(&client_to->queue, new_entry, entries);
    return;
  }

  // else if client is logged currently, add the new message to the global messege queue
  pthread_mutex_lock(&mutex_mq);
  STAILQ_INSERT_TAIL(&message_queue, new_entry, entries);
  pthread_cond_signal(&cond_mq); // signal that new message is in queue and should be sent out
  pthread_mutex_unlock(&mutex_mq);
}

// Function that delivers messages that are waiting in the personal queues
void deliverPastMessages(struct client *client)
{
  // while queue is not empty
  while (!STAILQ_EMPTY(&client->queue))
  {
    // pop the message from the personal queue to the global queue (to be sent)
    struct entry *message = STAILQ_FIRST(&client->queue);
    addMessageToQueue(message->from_login, message->to_login, message->message);
    STAILQ_REMOVE_HEAD(&client->queue, entries);
  }
}

// Function that handles loggin in
bool handleLoggingIn(int *client_socket, struct client **new_client)
{
  // check if server isn't full
  if (num_of_clients >= MAX_CLIENTS)
  {
    printf("BŁĄD: Osiągnięto maksymalną liczbę klientów\n");
    const char *msg = "Serwer osiągnął maksymalną liczbę klientów. Spróbuj później.";
    send(*client_socket, msg, strlen(msg), 0);
    close(*client_socket);
    return false;
  }
  else
  {
    // allocate client struct
    struct client *cl = (struct client *)malloc(sizeof(struct client));
    if (!cl)
    {
      printf("BŁĄD: Nie można zaalokować pamięci dla klienta\n");
      close(*client_socket);
      free(cl);
      return false;
    }

    // set client values
    cl->socket = *client_socket;
    cl->is_logged_in = false;
    memset(cl->login, 0, LOGIN_SIZE);

    // ask for login
    const char *login_prompt = "Podaj swój login: ";
    send(*client_socket, login_prompt, strlen(login_prompt), 0);

    char login_buffer[LOGIN_SIZE];
    int bytes_read = recv(*client_socket, login_buffer, LOGIN_SIZE - 1, 0);
    if (bytes_read <= 0)
    {
      printf("Klient rozłączony podczas logowania\n");
      close(*client_socket);
      free(cl);
      return false;
    }

    login_buffer[bytes_read] = '\0';
    // delete newline if exists
    char *newline = strchr(login_buffer, '\n');
    if (newline)
      *newline = '\0';

    // check if user with given login already exist
    struct client *client = findClientByLogin(login_buffer);
    // if exist
    if (client != NULL)
    {
      // check if this user is currently logged in
      // if so deny new connection
      if (client->is_logged_in)
      {
        printf("Klient %s jest juz zalogowany. Odmowa nowego logowania na ten login.\n", client->login);
        close(*client_socket);
        free(cl);
        return false;
      }
      // if noone is logged in on that account, log this user onto that account
      else
      {
        pthread_mutex_lock(&mutex_cl);
        client->is_logged_in = true;
        // change the previously saved socket to the new one
        client->socket = cl->socket;
        pthread_mutex_unlock(&mutex_cl);

        // set the new_client pointer to the current client
        *new_client = client;

        char *msg = "Pomyślnie zalogowano!\n";
        send(client->socket, msg, strlen(msg), 0);
        deliverPastMessages(client);
        return true;
      }
    }
    // if login is unique, add the new user to the queue
    strncpy(cl->login, login_buffer, LOGIN_SIZE - 1);
    cl->is_logged_in = true;
    STAILQ_INIT(&cl->queue);
    client_list[num_of_clients] = cl;
    num_of_clients++;
    *new_client = cl;

    // send init message to client
    printf("Nowy klient zalogowany jako '%s'. Aktywni klienci: %d\n", cl->login, num_of_clients);

    const char *welcome_msg = "Pomyślnie zalogowano! Dostępne komendy:\n"
                              " m <login> <wiadomość> : wyślij wiadomość do użytkownika\n"
                              " l : lista zalogowanych użytkowników\n"
                              " q : wyloguj (rozłącz)\n";
    send(*client_socket, welcome_msg, strlen(welcome_msg), 0);
    return true;
  }
}

// thread client handler
void *clientHandler(void *args)
{
  // parse arg to client struct
  struct client *client = (struct client *)args;
  char buffer[BUFFER_SIZE];
  bool running = true;

  while (running && server_running)
  {
    // clear buffer and wait for input from client
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_read <= 0)
    {
      printf("Klient '%s' rozłączony\n", client->login);
      running = false;
      break;
    }

    buffer[bytes_read] = '\0';
    // delete newline if exists
    char *newline = strchr(buffer, '\n');
    if (newline)
      *newline = '\0';

    // parse command - message
    // m <login> <message>
    if (buffer[0] == 'm' && buffer[1] == ' ')
    {
      char to_login[LOGIN_SIZE] = {0};
      char message[BUFFER_SIZE] = {0};

      // find first space after m
      char *space_after_command = strchr(buffer + 2, ' ');
      if (!space_after_command)
      {
        const char *error_msg = "Błędny format komendy. Użyj: m <login> <wiadomość>\n";
        send(client->socket, error_msg, strlen(error_msg), 0);
        continue;
      }

      // extract login
      int login_len = space_after_command - (buffer + 2);
      if (login_len >= LOGIN_SIZE || login_len <= 0)
      {
        const char *error_msg = "Nieprawidłowa długość loginu\n";
        send(client->socket, error_msg, strlen(error_msg), 0);
        continue;
      }
      strncpy(to_login, buffer + 2, login_len);
      to_login[login_len] = '\0';

      // extract message
      strncpy(message, space_after_command + 1, BUFFER_SIZE - 1);

      // check if recipient exists
      struct client *recipient = findClientByLogin(to_login);
      if (!recipient)
      {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, BUFFER_SIZE, "Użytkownik '%s' nie jest zalogowany\n", to_login);
        send(client->socket, error_msg, strlen(error_msg), 0);
      }
      else
      {
        // add message to queue
        addMessageToQueue(client->login, to_login, message);
        const char *confirm_msg = "Wiadomość została dodana do kolejki\n";
        send(client->socket, confirm_msg, strlen(confirm_msg), 0);
      }
    }
    // parse command, quit
    // quit logs user out
    else if (strcmp(buffer, "q") == 0)
    {
      const char *logout_msg = "Wylogowywanie...\n";
      send(client->socket, logout_msg, strlen(logout_msg), 0);
      running = false;
    }
    // parse command, list
    //  sents logged user list to user
    else if (strcmp(buffer, "l") == 0)
    {
      char users_list[BUFFER_SIZE] = "Zalogowani użytkownicy:\n";
      pthread_mutex_lock(&mutex_cl);
      for (int i = 0; i < num_of_clients; i++)
      {
        if (client_list[i]->is_logged_in)
        {
          strcat(users_list, "- ");
          strcat(users_list, client_list[i]->login);
          strcat(users_list, "\n");
        }
      }
      pthread_mutex_unlock(&mutex_cl);
      send(client->socket, users_list, strlen(users_list), 0);
    }
    else
    {
      // handling parsing error
      const char *help_msg = "Nieznana komenda. Dostępne komendy:\n"
                             " m <login> <wiadomość> : wyślij wiadomość do użytkownika\n"
                             " l : lista zalogowanych użytkowników\n"
                             " q : wyloguj (rozłącz)\n";
      send(client->socket, help_msg, strlen(help_msg), 0);
    }
  }

  logoutClient(client);
  return NULL;
}

// thread that is delivering messages from queue
void *messageDeliveryThread(void *args)
{
  while (server_running)
  {
    struct entry *message = NULL;

    pthread_mutex_lock(&mutex_mq);
    while (STAILQ_EMPTY(&message_queue) && server_running)
    {
      // wait for signal about new message or wait for server to stop running
      // reciving the cond_mq automaticlly releases the mutex_mq
      pthread_cond_wait(&cond_mq, &mutex_mq);
    }

    // if server closes release mutex
    if (!server_running)
    {
      pthread_mutex_unlock(&mutex_mq);
      break;
    }

    // get the first message in queue
    message = STAILQ_FIRST(&message_queue);
    STAILQ_REMOVE_HEAD(&message_queue, entries);
    pthread_mutex_unlock(&mutex_mq);

    if (message)
    {
      // find recipient
      struct client *recipient = findClientByLogin(message->to_login);
      if (recipient)
      {
        // format message and send
        char formatted_message[BUFFER_SIZE];
        snprintf(formatted_message, BUFFER_SIZE, "Wiadomość od %s: %s\n",
                 message->from_login, message->message);

        send(recipient->socket, formatted_message, strlen(formatted_message), 0);
        printf("Dostarczono wiadomość od '%s' do '%s'\n", message->from_login, message->to_login);
      }
      // free memory
      free(message->from_login);
      free(message->to_login);
      free(message->message);
      free(message);
    }
  }
  return NULL;
}

// handling the program cleanup
void cleanup()
{
  printf("Zamykanie serwera...\n");
  server_running = false;

  // signal to the message thread that he needs to end his work gracefully
  pthread_cond_signal(&cond_mq);

  // close all connections
  pthread_mutex_lock(&mutex_cl);
  for (int i = 0; i < num_of_clients; i++)
  {
    if (client_list[i])
    {
      const char *shutdown_msg = "Serwer jest zamykany. Rozłączanie...\n";
      send(client_list[i]->socket, shutdown_msg, strlen(shutdown_msg), 0);
      close(client_list[i]->socket);
      free(client_list[i]);
    }
  }
  num_of_clients = 0;
  pthread_mutex_unlock(&mutex_cl);

  // free all elements in memory
  pthread_mutex_lock(&mutex_mq);
  struct entry *entry;
  while (!STAILQ_EMPTY(&message_queue))
  {
    entry = STAILQ_FIRST(&message_queue);
    STAILQ_REMOVE_HEAD(&message_queue, entries);
    free(entry->from_login);
    free(entry->to_login);
    free(entry->message);
    free(entry);
  }
  pthread_mutex_unlock(&mutex_mq);
}

int main()
{
  // init message queue
  STAILQ_INIT(&message_queue);

  // bind the handle_signal method to signals
  signal(SIGINT, handle_signal);

  // create server socket
  int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0)
  {
    perror("Nie można utworzyć socketu serwera");
    exit(1);
  }

  // set socket options
  int opt = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
  {
    perror("Błąd przy ustawianiu opcji socketu");
    close(server_socket);
    exit(1);
  }

  // configure server
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(PORT);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  // bind socket to addr
  if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    perror("Nie można powiązać socketu z adresem");
    close(server_socket);
    exit(1);
  }

  // listen for connections
  if (listen(server_socket, 5) < 0)
  {
    perror("Błąd podczas nasłuchiwania");
    close(server_socket);
    exit(1);
  }

  printf("Serwer uruchomiony i nasłuchuje na porcie: %d...\n", PORT);

  // start the delivery thread
  pthread_t delivery_thread;
  if (pthread_create(&delivery_thread, NULL, messageDeliveryThread, NULL) != 0)
  {
    perror("Nie można utworzyć wątku dostarczającego wiadomości");
    close(server_socket);
    exit(1);
  }

  // loop that is constantly listening for connection
  while (server_running)
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // accept new connection
    int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
    if (client_socket < 0)
    {
      if (server_running)
      {
        perror("Błąd podczas akceptowania połączenia");
      }
      continue;
    }

    // write information about new connection
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Nowe połączenie z %s:%d\n", client_ip, ntohs(client_addr.sin_port));

    // create new client and create new client handling thread
    struct client *new_client = NULL;
    if (handleLoggingIn(&client_socket, &new_client))
    {
      pthread_t client_thread;
      if (pthread_create(&client_thread, NULL, clientHandler, new_client) != 0)
      {
        perror("Nie można utworzyć wątku klienta");
        removeClient(new_client);
        continue;
      }
      pthread_detach(client_thread);
    }
  }

  pthread_join(delivery_thread, NULL);

  // close server socket
  close(server_socket);

  pthread_mutex_destroy(&mutex_cl);
  pthread_mutex_destroy(&mutex_mq);
  pthread_cond_destroy(&cond_mq);

  cleanup();
  printf("Serwer zakończył działanie\n");
  return 0;
}
