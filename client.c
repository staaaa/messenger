#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 7992
#define BUFFER_SIZE 4096

// Struktura przechowująca informacje o połączeniu
typedef struct
{
    int socket_fd;
    bool running;
} connection_info;

// Globalne zmienne
connection_info conn = {-1, true};
pthread_t receive_thread_id;

// Prototypy funkcji
void *receive_messages(void *arg);
void handle_signal(int sig);
void cleanup_resources();

// Obsługa sygnałów (CTRL+C)
void handle_signal(int sig)
{
    printf("\nPrzerwanie działania klienta...\n");
    conn.running = false;
    cleanup_resources();
    exit(0);
}

// Funkcja zwalniająca zasoby
void cleanup_resources()
{
    if (conn.socket_fd != -1)
    {
        close(conn.socket_fd);
        conn.socket_fd = -1;
    }
}

// Wątek odbierający wiadomości od serwera
void *receive_messages(void *arg)
{
    connection_info *connection = (connection_info *)arg;
    char buffer[BUFFER_SIZE];

    while (connection->running)
    {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(connection->socket_fd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0)
        {
            if (connection->running)
            {
                printf("\nUtracono połączenie z serwerem.\n");
                connection->running = false;
                break;
            }
        }
        else
        {
            // Wyświetl otrzymaną wiadomość
            printf("%s", buffer);
            fflush(stdout);
        }
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    // Obsługa argumentów linii poleceń
    char *server_ip = SERVER_IP;
    int server_port = SERVER_PORT;

    if (argc >= 2)
    {
        server_ip = argv[1];
    }

    if (argc >= 3)
    {
        server_port = atoi(argv[2]);
    }

    // Ustawienie obsługi sygnałów
    signal(SIGINT, handle_signal);

    // Utworzenie gniazda
    conn.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn.socket_fd < 0)
    {
        perror("Błąd tworzenia gniazda");
        return 1;
    }

    // Konfiguracja adresu serwera
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
    {
        perror("Nieprawidłowy adres IP serwera");
        cleanup_resources();
        return 1;
    }

    // Nawiązanie połączenia z serwerem
    printf("Łączenie z serwerem %s:%d...\n", server_ip, server_port);
    if (connect(conn.socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Błąd podczas łączenia z serwerem");
        cleanup_resources();
        return 1;
    }

    printf("Połączono z serwerem!\n");

    // Utworzenie wątku do odbierania wiadomości
    if (pthread_create(&receive_thread_id, NULL, receive_messages, &conn) != 0)
    {
        perror("Nie można utworzyć wątku odbierającego");
        cleanup_resources();
        return 1;
    }

    char input[BUFFER_SIZE];

    // Główna pętla klienta
    while (conn.running)
    {
        // Oczekiwanie na wejście użytkownika
        if (fgets(input, BUFFER_SIZE, stdin) == NULL)
        {
            break;
        }

        // Wysłanie wiadomości do serwera
        if (send(conn.socket_fd, input, strlen(input), 0) < 0)
        {
            perror("Błąd wysyłania danych");
            break;
        }

        // Sprawdzenie, czy użytkownik chce zakończyć
        if (strlen(input) == 2 && input[0] == 'q' && input[1] == '\n')
        {
            printf("Trwa zamykanie klienta...\n");
            sleep(1); // Daj serwerowi czas na przetworzenie zamknięcia
            conn.running = false;
            break;
        }
    }

    // Oczekiwanie na zakończenie wątku odbierającego
    if (conn.running == false)
    {
        pthread_join(receive_thread_id, NULL);
    }

    cleanup_resources();
    return 0;
}