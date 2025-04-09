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

// Struktura wiadomości do kolejki
struct entry
{
  char *from_login;
  char *to_login;
  char *message;
  STAILQ_ENTRY(entry)
  entries;
};

// Definicja głowy kolejki
STAILQ_HEAD(stailhead, entry);

// Struktura klienta
struct client
{
  int socket;
  char login[LOGIN_SIZE];
  bool is_logged_in;
  struct stailhead queue;
};

// Lista klientów i liczba klientów
struct client *client_list[MAX_CLIENTS];
int num_of_clients = 0;
struct stailhead message_queue;

// Mutexy do synchronizacji dostępu
pthread_mutex_t mutex_cl = PTHREAD_MUTEX_INITIALIZER; // Mutex dla listy klientów
pthread_mutex_t mutex_mq = PTHREAD_MUTEX_INITIALIZER; // Mutex dla kolejki wiadomości
pthread_cond_t cond_mq = PTHREAD_COND_INITIALIZER;    // Warunek dla kolejki wiadomości

// Flaga zakończenia programu
volatile bool server_running = true;

// Funkcja usuwająca klienta z listy
void removeClient(struct client *client)
{
  pthread_mutex_lock(&mutex_cl);

  // Szukaj klienta w liście
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
    // Przesuń wszystkich klientów o jeden w dół
    for (int i = index; i < num_of_clients - 1; i++)
    {
      client_list[i] = client_list[i + 1];
    }
    num_of_clients--;
    printf("Klient '%s' został usunięty. Liczba aktywnych klientów: %d\n", client->login, num_of_clients);
  }

  // Zamknij socket i zwolnij pamięć
  close(client->socket);
  free(client);

  pthread_mutex_unlock(&mutex_cl);
}

// Funckja wylogowująca klienta z listy
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
  // free(client);

  pthread_mutex_unlock(&mutex_cl);
}

// Funkcja znajdująca klienta po loginie
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

// Funkcja dodająca wiadomość do kolejki
void addMessageToQueue(const char *from_login, const char *to_login, const char *message)
{
  struct entry *new_entry = (struct entry *)malloc(sizeof(struct entry));
  if (!new_entry)
  {
    printf("BŁĄD: Nie można zaalokować pamięci dla wiadomości\n");
    return;
  }

  new_entry->from_login = strdup(from_login);
  new_entry->to_login = strdup(to_login);
  new_entry->message = strdup(message);

  struct client *client_to = findClientByLogin(to_login);
  if (!client_to->is_logged_in)
  {
    STAILQ_INSERT_TAIL(&client_to->queue, new_entry, entries);
    return;
  }

  pthread_mutex_lock(&mutex_mq);
  STAILQ_INSERT_TAIL(&message_queue, new_entry, entries);
  pthread_cond_signal(&cond_mq); // Sygnalizuj, że nowa wiadomość jest dostępna
  pthread_mutex_unlock(&mutex_mq);
}

// Funkcja dostarczajaca zalegle wiadomosci
void deliverPastMessages(struct client *client)
{
  while (!STAILQ_EMPTY(&client->queue))
  {
    struct entry *message = STAILQ_FIRST(&client->queue);
    addMessageToQueue(message->from_login, message->to_login, message->message);
    STAILQ_REMOVE_HEAD(&client->queue, entries);
  }
}

// Funkcja dodająca nowego klienta
bool addNewClient(int *client_socket, struct client **new_client)
{
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
    // Alokacja struktury klienta
    struct client *cl = (struct client *)malloc(sizeof(struct client));
    if (!cl)
    {
      printf("BŁĄD: Nie można zaalokować pamięci dla klienta\n");
      close(*client_socket);
      free(cl);
      return false;
    }

    cl->socket = *client_socket;
    cl->is_logged_in = false;
    memset(cl->login, 0, LOGIN_SIZE);

    // Poproś o login
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
    // Usuń znak nowej linii, jeśli istnieje
    char *newline = strchr(login_buffer, '\n');
    if (newline)
      *newline = '\0';

    // Sprawdź, czy użytkownik o podanym loginie już istnieje
    struct client *client = findClientByLogin(login_buffer);
    // Jesli istnieje
    if (client != NULL)
    {
      // Jesli jest zalogowany juz ktos na ten login
      if (client->is_logged_in)
      {
        printf("Klient %s jest juz zalogowany. Odmowa nowego logowania na ten login.\n", client->login);
        close(*client_socket);
        free(cl);
        return false;
      }
      // Jesli nikt nie jest obecnie zalgoowany na ten login
      else
      {
        pthread_mutex_lock(&mutex_cl);
        client->is_logged_in = true;
        client->socket = cl->socket;
        pthread_mutex_unlock(&mutex_cl);

        // Ustawiamy *new_client na istniejącego klienta, aby wywołujący mógł poprawnie z niego korzystać
        *new_client = client;

        char *msg = "Pomyślnie zalogowano!\n";
        send(client->socket, msg, strlen(msg), 0);
        deliverPastMessages(client);
        return true;
      }
    }
    // Jeśli login jest unikalny, dodaj klienta do listy
    strncpy(cl->login, login_buffer, LOGIN_SIZE - 1);
    cl->is_logged_in = true;
    STAILQ_INIT(&cl->queue);
    client_list[num_of_clients] = cl;
    num_of_clients++;
    *new_client = cl;

    printf("Nowy klient zalogowany jako '%s'. Aktywni klienci: %d\n", cl->login, num_of_clients);

    const char *welcome_msg = "Pomyślnie zalogowano! Dostępne komendy:\n"
                              " m <login> <wiadomość> : wyślij wiadomość do użytkownika\n"
                              " l : lista zalogowanych użytkowników\n"
                              " q : wyloguj (rozłącz)\n";
    send(*client_socket, welcome_msg, strlen(welcome_msg), 0);
    return true;
  }
}

// Wątek obsługi klienta
void *clientHandler(void *args)
{
  // Sparsuj argument i utworz lokalnego klienta
  struct client *client = (struct client *)args;
  char buffer[BUFFER_SIZE];
  bool running = true;

  while (running && server_running)
  {
    // wyzeruj bufor i oczekuj komendy od klienta
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_read <= 0)
    {
      printf("Klient '%s' rozłączony\n", client->login);
      running = false;
      break;
    }

    buffer[bytes_read] = '\0';
    // Usuń znak nowej linii, jeśli istnieje
    char *newline = strchr(buffer, '\n');
    if (newline)
      *newline = '\0';

    // Parsowanie komendy - message
    if (buffer[0] == 'm' && buffer[1] == ' ')
    {
      // Komenda wysłania wiadomości: m <login> <wiadomość>
      char to_login[LOGIN_SIZE] = {0};
      char message[BUFFER_SIZE] = {0};

      // Znajdź pierwszy odstęp po 'm '
      char *space_after_command = strchr(buffer + 2, ' ');
      if (!space_after_command)
      {
        const char *error_msg = "Błędny format komendy. Użyj: m <login> <wiadomość>\n";
        send(client->socket, error_msg, strlen(error_msg), 0);
        continue;
      }

      // Wyodrębnij login (wszystko między 'm ' a następnym spacją)
      int login_len = space_after_command - (buffer + 2);
      if (login_len >= LOGIN_SIZE || login_len <= 0)
      {
        const char *error_msg = "Nieprawidłowa długość loginu\n";
        send(client->socket, error_msg, strlen(error_msg), 0);
        continue;
      }
      strncpy(to_login, buffer + 2, login_len);
      to_login[login_len] = '\0';

      // Wyodrębnij wiadomość (wszystko po loginie)
      strncpy(message, space_after_command + 1, BUFFER_SIZE - 1);

      // Sprawdź, czy odbiorca istnieje
      struct client *recipient = findClientByLogin(to_login);
      if (!recipient)
      {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, BUFFER_SIZE, "Użytkownik '%s' nie jest zalogowany\n", to_login);
        send(client->socket, error_msg, strlen(error_msg), 0);
      }
      else
      {
        // Dodaj wiadomość do kolejki
        addMessageToQueue(client->login, to_login, message);
        const char *confirm_msg = "Wiadomość została dodana do kolejki\n";
        send(client->socket, confirm_msg, strlen(confirm_msg), 0);
      }
    }
    // Parsowanie komendy - q
    else if (strcmp(buffer, "q") == 0)
    {
      // Komenda wylogowania
      const char *logout_msg = "Wylogowywanie...\n";
      send(client->socket, logout_msg, strlen(logout_msg), 0);
      running = false;
    }
    else if (strcmp(buffer, "l") == 0)
    {
      // Komenda wyświetlenia listy zalogowanych użytkowników
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
      // Nieznana komenda
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

// Wątek wysyłający wiadomości z kolejki
void *messageDeliveryThread(void *args)
{
  while (server_running)
  {
    struct entry *message = NULL;

    pthread_mutex_lock(&mutex_mq);
    while (STAILQ_EMPTY(&message_queue) && server_running)
    {
      // Czekaj na sygnał o nowej wiadomości lub zakończeniu serwera
      // Czekanie na sygnał automatycznie zwalnia mutex_mq do momentu pojawienia sie sygnalu
      // O nowej wiadomości
      pthread_cond_wait(&cond_mq, &mutex_mq);
    }

    if (!server_running)
    {
      pthread_mutex_unlock(&mutex_mq);
      break;
    }

    // Pobierz pierwszą wiadomość z kolejki
    message = STAILQ_FIRST(&message_queue);
    STAILQ_REMOVE_HEAD(&message_queue, entries);
    pthread_mutex_unlock(&mutex_mq);

    if (message)
    {
      // Znajdź odbiorcę wiadomości
      struct client *recipient = findClientByLogin(message->to_login);
      if (recipient)
      {
        // Sformatuj i wyślij wiadomość
        char formatted_message[BUFFER_SIZE];
        snprintf(formatted_message, BUFFER_SIZE, "Wiadomość od %s: %s\n",
                 message->from_login, message->message);

        send(recipient->socket, formatted_message, strlen(formatted_message), 0);
        printf("Dostarczono wiadomość od '%s' do '%s'\n", message->from_login, message->to_login);
      }
      // Zwolnij pamięć zajmowaną przez wiadomość
      free(message->from_login);
      free(message->to_login);
      free(message->message);
      free(message);
    }
  }
  return NULL;
}

// Obsługa sygnału zakończenia programu
void cleanup()
{
  printf("Zamykanie serwera...\n");
  server_running = false;

  // Sygnalizuj wątek obsługujący wiadomości, aby zakończył pracę
  pthread_cond_signal(&cond_mq);

  // Zamknij wszystkie połączenia klientów
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

  // Zwolnij elementy pozostałe w kolejce wiadomości
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
  // Inicjalizacja kolejki wiadomości
  STAILQ_INIT(&message_queue);

  // Utworzenie socketu serwera
  int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0)
  {
    perror("Nie można utworzyć socketu serwera");
    exit(1);
  }

  // Ustawienie opcji socketu dla szybkiego ponownego użycia adresu i portu
  int opt = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
  {
    perror("Błąd przy ustawianiu opcji socketu");
    close(server_socket);
    exit(1);
  }

  // Konfiguracja adresu serwera
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(PORT);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  // Powiązanie socketu z adresem
  if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    perror("Nie można powiązać socketu z adresem");
    close(server_socket);
    exit(1);
  }

  // Nasłuchiwanie na połączenia
  if (listen(server_socket, 5) < 0)
  {
    perror("Błąd podczas nasłuchiwania");
    close(server_socket);
    exit(1);
  }

  printf("Serwer uruchomiony i nasłuchuje na porcie: %d...\n", PORT);

  // Uruchomienie wątku dostarczającego wiadomości
  pthread_t delivery_thread;
  if (pthread_create(&delivery_thread, NULL, messageDeliveryThread, NULL) != 0)
  {
    perror("Nie można utworzyć wątku dostarczającego wiadomości");
    close(server_socket);
    exit(1);
  }

  // Pętla akceptująca połączenia
  while (server_running)
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Akceptuj nowe połączenie
    int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
    if (client_socket < 0)
    {
      if (server_running)
      {
        perror("Błąd podczas akceptowania połączenia");
      }
      continue;
    }

    // Wypisz informacje o nowym połączeniu
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Nowe połączenie z %s:%d\n", client_ip, ntohs(client_addr.sin_port));

    // Dodaj klienta i utwórz wątek obsługujący
    struct client *new_client = NULL;
    if (addNewClient(&client_socket, &new_client))
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

  // Poczekaj na zakończenie wątku dostarczającego wiadomości
  pthread_join(delivery_thread, NULL);

  // Zamknij socket serwera
  close(server_socket);

  // Zniszcz mutexy i zmienną warunkową
  pthread_mutex_destroy(&mutex_cl);
  pthread_mutex_destroy(&mutex_mq);
  pthread_cond_destroy(&cond_mq);

  printf("Serwer zakończył działanie\n");
  return 0;
}