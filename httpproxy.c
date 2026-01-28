#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <ctype.h>

#define DEFAULT_PROXY_PORT 80
#define BUFFER_SIZE 4096
#define MAX_HOST_LEN 256
#define MAX_HEADER_SIZE 8192

// Статические ответы для клиента
static const char RESP_400_NO_HOST[] =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "400 Bad Request: No Host header specified\n";

static const char RESP_502_BAD_GATEWAY[] =
    "HTTP/1.0 502 Bad Gateway\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "502 Bad Gateway: Cannot connect to target server\n";

static const char RESP_500_INTERNAL_ERROR[] =
    "HTTP/1.0 500 Internal Server Error\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "500 Internal Server Error\n";

// Структура для передачи данных в поток
typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
} thread_data_t;

// Функция для извлечения хоста из HTTP-запроса
int extract_host_from_request(const char *request, char *host, size_t host_len) {
    char *host_start = strstr(request, "Host: ");
    if (!host_start) {
        return -1;
    }
    
    host_start += 6; // Пропускаем "Host: "
    char *host_end = strstr(host_start, "\r\n");
    if (!host_end) {
        return -1;
    }
    
    size_t length = host_end - host_start;
    if (length >= host_len) {
        length = host_len - 1;
    }
    
    strncpy(host, host_start, length);
    host[length] = '\0';
    
    // Удаляем порт, если он указан
    char *port_ptr = strchr(host, ':');
    if (port_ptr) {
        *port_ptr = '\0';
    }
    
    return 0;
}

// Функция для извлечения порта из HTTP-запроса
void extract_port_from_request(const char *request, int *port) {
    // extract_host_from_request успешно сработал до этого, нет смысла заново проверять
    char *host_start = strstr(request, "Host: ");
    host_start += 6;
    char *host_end = strstr(host_start, "\r\n");
    
    char *port_ptr = strchr(host_start, ':');
    if (!port_ptr || port_ptr >= host_end) {
        // Нет порта - используем по умолчанию
        *port = DEFAULT_PROXY_PORT;
        return;
    }
    
    port_ptr++;
    while (port_ptr < host_end && (*port_ptr == ' ' || *port_ptr == '\t')) {
        port_ptr++;
    }
    
    // Проверяем что остались символы для порта
    if (port_ptr >= host_end) {
        *port = DEFAULT_PROXY_PORT;
        return;
    }
    
    char port_str[6] = {0};
    size_t port_len = 0;
    while (port_len < 5 && 
           port_ptr + port_len < host_end && 
           isdigit(port_str[port_len] = port_ptr[port_len])) {
        port_len++;
    }
    
    if (port_len == 0) {
        // Нет цифр - невалидный порт
        *port = DEFAULT_PROXY_PORT;
        return;
    }
    
    port_str[port_len] = '\0';
    
    *port = atoi(port_str);
    if (*port <= 0 || *port > 65535) {
        *port = DEFAULT_PROXY_PORT;  // Невалидный порт → по умолчанию
    }
    
    return;
}

// Функция для нормализации запроса до HTTP/1.0
void normalize_to_http10(char *request) {
    // Заменяем HTTP/1.1 на HTTP/1.0 в строке запроса
    char *version_ptr = strstr(request, "HTTP/1.1");
    if (version_ptr) {
        // Проверяем, что это действительно версия в строке запроса
        char *line_end = strstr(request, "\r\n");
        if (version_ptr < line_end) {
            memcpy(version_ptr, "HTTP/1.0", 8);
        }
    }
    
    // Добавляем или изменяем заголовок Connection
    char *headers_end = strstr(request, "\r\n\r\n");
    if (!headers_end) return;
    
    char *connection_ptr = strstr(request, "Connection:");
    if (connection_ptr && connection_ptr < headers_end) {
        // Находим конец строки с Connection
        char *line_end = strstr(connection_ptr, "\r\n");
        if (line_end) {
            // Заменяем значение на "close"
            char *value_start = connection_ptr + 11; // "Connection:"
            while (value_start < line_end && (*value_start == ' ' || *value_start == '\t')) {
                value_start++;
            }
            
            // Проверяем достаточно ли места
            if (line_end - value_start >= 5) {
                memcpy(value_start, "close", 5);
                // Заполняем оставшееся пространство пробелами
                for (char *p = value_start + 5; p < line_end; p++) {
                    *p = ' ';
                }
            }
        }
    } else {
        // Добавляем заголовок Connection: close перед концом заголовков
        size_t request_len = strlen(request);
        size_t headers_len = headers_end - request;
        
        if (request_len + 20 < BUFFER_SIZE) {
            memmove(headers_end + 20, headers_end, request_len - headers_len + 1);
            memcpy(headers_end, "Connection: close\r\n", 20);
        }
    }
}

// Функция для чтения полного HTTP-запроса от клиента
ssize_t read_full_request(int fd, char *buffer, size_t buffer_size) {
    ssize_t total_read = 0;
    ssize_t bytes_read;
    
    while (total_read < (ssize_t)buffer_size - 1) {
        bytes_read = recv(fd, buffer + total_read, buffer_size - total_read - 1, 0);
        
        if (bytes_read <= 0) {
            return bytes_read;
        }
        
        total_read += bytes_read;
        buffer[total_read] = '\0';
        
        // Проверяем, достигли ли конца заголовков
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
    }
    
    return total_read;
}

// Обработчик соединения
void *handle_client(void *arg) {
    thread_data_t *data = (thread_data_t *)arg;
    int client_fd = data->client_fd;
    struct sockaddr_in client_addr = data->client_addr;
    free(data);
    
    char request[BUFFER_SIZE];
    memset(request, 0, sizeof(request));
    
    // Читаем HTTP-запрос от клиента
    ssize_t request_len = read_full_request(client_fd, request, sizeof(request));
    
    if (request_len <= 0) {
        if (request_len < 0) {
            perror("read from client failed");
        }
        close(client_fd);
        return NULL;
    }
    
    // Извлекаем информацию о целевом сервере
    char target_host[MAX_HOST_LEN];
    int target_port;
    
    if (extract_host_from_request(request, target_host, sizeof(target_host)) < 0) {
        send(client_fd, RESP_400_NO_HOST, strlen(RESP_400_NO_HOST), 0);
        close(client_fd);
        return NULL;
    }
    
    extract_port_from_request(request, &target_port);
    
    printf("[PROXY] Request to %s:%d from %s:%d\n",
           target_host, target_port,
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    
    // Нормализуем запрос до HTTP/1.0
    normalize_to_http10(request);
    
    // Разрешаем DNS-имя
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", target_port);
    
    int gai_result = getaddrinfo(target_host, port_str, &hints, &res);
    if (gai_result != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(gai_result));
        send(client_fd, RESP_502_BAD_GATEWAY, strlen(RESP_502_BAD_GATEWAY), 0);
        close(client_fd);
        return NULL;
    }
    
    // Пробуем подключиться к целевому серверу
    int server_fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_fd < 0) {
            continue;
        }
        
        // Устанавливаем таймаут на подключение
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
        setsockopt(server_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        
        if (connect(server_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // Успешное подключение
        }
        
        close(server_fd);
        server_fd = -1;
    }
    
    freeaddrinfo(res);
    
    if (server_fd < 0) {
        send(client_fd, RESP_502_BAD_GATEWAY, strlen(RESP_502_BAD_GATEWAY), 0);
        close(client_fd);
        return NULL;
    }
    
    // Отправляем запрос на целевой сервер
    ssize_t sent = send(server_fd, request, strlen(request), 0);
    if (sent < 0) {
        perror("send to server failed");
        send(client_fd, RESP_500_INTERNAL_ERROR, strlen(RESP_500_INTERNAL_ERROR), 0);
        close(server_fd);
        close(client_fd);
        return NULL;
    }
    
    // Проксируем данные между клиентом и сервером
    fd_set read_fds;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        FD_SET(server_fd, &read_fds);
        
        int max_fd = (client_fd > server_fd) ? client_fd : server_fd;
        
        // Используем select для мультиплексирования
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select failed");
            break;
        }
        
        // Данные от клиента к серверу
        if (FD_ISSET(client_fd, &read_fds)) {
            bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) {
                break; // Клиент закрыл соединение
            }
            
            ssize_t bytes_sent = send(server_fd, buffer, bytes_read, 0);
            if (bytes_sent <= 0) {
                break; // Ошибка отправки на сервер
            }
        }
        
        // Данные от сервера к клиенту
        if (FD_ISSET(server_fd, &read_fds)) {
            bytes_read = recv(server_fd, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) {
                break; // Сервер закрыл соединение
            }
            
            ssize_t bytes_sent = send(client_fd, buffer, bytes_read, 0);
            if (bytes_sent <= 0) {
                break; // Ошибка отправки клиенту
            }
        }
    }
    
    printf("[PROXY] Connection closed for %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    
    close(server_fd);
    close(client_fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    int proxy_port = DEFAULT_PROXY_PORT;
    
    // Проверяем аргументы командной строки
    if (argc == 2) {
        proxy_port = atoi(argv[1]);
        if (proxy_port <= 0 || proxy_port > 65535) {
            fprintf(stderr, "Invalid port number. Using default port 80.\n");
            proxy_port = DEFAULT_PROXY_PORT;
        }
    } else if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    // Создаем сокет для прокси-сервера
    int proxy_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_fd < 0) {
        perror("socket creation failed");
        return EXIT_FAILURE;
    }
    
    // Устанавливаем опцию для повторного использования адреса
    int opt = 1;
    if (setsockopt(proxy_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(proxy_fd);
        return EXIT_FAILURE;
    }
    
    // Настраиваем адрес прокси-сервера
    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof(proxy_addr));
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(proxy_port);
    
    // Привязываем сокет к адресу
    if (bind(proxy_fd, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) < 0) {
        perror("bind failed");
        close(proxy_fd);
        return EXIT_FAILURE;
    }
    
    // Начинаем слушать соединения
    if (listen(proxy_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        close(proxy_fd);
        return EXIT_FAILURE;
    }
    
    printf("HTTP Proxy Server started on port %d\n", proxy_port);
    printf("Press Ctrl+C to stop the server\n\n");
    
    // Настраиваем атрибуты потоков
    pthread_attr_t thread_attr;
    if (pthread_attr_init(&thread_attr) != 0) {
        perror("pthread_attr_init");
        close(proxy_fd);
        exit(EXIT_FAILURE);
    }

    if (pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED) != 0) {
        perror("pthread_attr_setdetachstate");
        pthread_attr_destroy(&thread_attr);
        close(proxy_fd);
        exit(EXIT_FAILURE);
    }
    
    // Основной цикл принятия соединений
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Принимаем новое соединение
        int client_fd = accept(proxy_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }
        
        // Создаем структуру данных для потока
        thread_data_t *data = malloc(sizeof(thread_data_t));
        if (!data) {
            fprintf(stderr, "malloc failed for thread data\n");
            close(client_fd);
            continue;
        }
        
        data->client_fd = client_fd;
        memcpy(&data->client_addr, &client_addr, sizeof(client_addr));
        
        // Создаем поток для обработки соединения
        pthread_t thread_id;
        if (pthread_create(&thread_id, &thread_attr, handle_client, data) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            free(data);
            close(client_fd);
            continue;
        }
        
        printf("[PROXY] New connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
    
    pthread_attr_destroy(&thread_attr);
    close(proxy_fd);
    
    return EXIT_SUCCESS;
}