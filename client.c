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

// struct that keeps information about current connection
typedef struct
{
    int socket_fd;
    bool running;
} connection_info;

// global vars:
connection_info conn = {-1, true};
pthread_t receive_thread_id;

// funciton prototypes
void *receive_messages(void *arg);
void handle_signal(int sig);
void cleanup_resources();

// handling signals ( killing the app)
void handle_signal(int sig)
{
    printf("\nPrzerwanie działania klienta...\n");
    conn.running = false;
    cleanup_resources();
    exit(0);
}

// free the resources
void cleanup_resources()
{
    if (conn.socket_fd != -1)
    {
        close(conn.socket_fd);
        conn.socket_fd = -1;
    }
}

// thread that recives messages from the server
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
            // print recived message
            printf("%s", buffer);
            fflush(stdout);
        }
    }

    return NULL;
}

int main(int argc, char *argv[])
{
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

    signal(SIGINT, handle_signal);

    // create socket
    conn.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn.socket_fd < 0)
    {
        perror("Błąd tworzenia gniazda");
        return 1;
    }

    // config addr
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

    // connect to the server
    printf("Łączenie z serwerem %s:%d...\n", server_ip, server_port);
    if (connect(conn.socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Błąd podczas łączenia z serwerem");
        cleanup_resources();
        return 1;
    }

    printf("Połączono z serwerem!\n");

    // create reciving thread
    if (pthread_create(&receive_thread_id, NULL, receive_messages, &conn) != 0)
    {
        perror("Nie można utworzyć wątku odbierającego");
        cleanup_resources();
        return 1;
    }

    char input[BUFFER_SIZE];

    // main client loop
    while (conn.running)
    {
        // wait for input
        if (fgets(input, BUFFER_SIZE, stdin) == NULL)
        {
            break;
        }

        // send message to server
        if (send(conn.socket_fd, input, strlen(input), 0) < 0)
        {
            perror("Błąd wysyłania danych");
            break;
        }

        // check if client wants to disconnect
        if (strlen(input) == 2 && input[0] == 'q' && input[1] == '\n')
        {
            printf("Trwa zamykanie klienta...\n");
            sleep(1); // give server time to handle the disconnection
            conn.running = false;
            break;
        }
    }

    // wait for the reciving thread to end
    if (conn.running == false)
    {
        pthread_join(receive_thread_id, NULL);
    }

    cleanup_resources();
    return 0;
}