#include "lsrp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>
#include <pthread.h>
#include <signal.h>

#define LISTEN_BACKLOG 1024
#define KEEPALIVE_TIMEOUT 30  // Таймаут keep-alive в секундах

static void uint32_to_be(uint32_t val, uint8_t *bytes) {
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
}

static uint32_t be_to_uint32(uint8_t *bytes) {
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

typedef struct {
    int client_sock;
    lsrp_handler_t handler;
} thread_args_t;

static void* handle_client(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    int client_sock = args->client_sock;
    lsrp_handler_t handler = args->handler;
    free(args);

    // Отключаемся от главного потока
    pthread_detach(pthread_self());

    // Оптимизация: отключаем алгоритм Nagle для снижения latency
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set socket buffer sizes
    int buf_size = 65536;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    // Set keep-alive timeout на чтение
    struct timeval timeout;
    timeout.tv_sec = KEEPALIVE_TIMEOUT;
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Обрабатываем несколько запросов на одном соединении
    while (1) {
        // Read header: magic (4) + len (4)
        uint8_t header[LSRP_MAGIC_LEN + 4];
        ssize_t recv_len = recv(client_sock, header, sizeof(header), MSG_WAITALL);

        // Клиент закрыл соединение или таймаут - нормальный выход
        if (recv_len == 0) {
            break;
        }

        if (recv_len != sizeof(header)) {
            if (recv_len < 0) {
                // Таймаут - это нормально для keep-alive
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                fprintf(stderr, "Error reading header from socket %d: %s\n", client_sock, strerror(errno));
            } else {
                fprintf(stderr, "Incomplete header on socket %d: %zd bytes\n", client_sock, recv_len);
            }
            break;
        }

        // Check magic
        if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
            fprintf(stderr, "Invalid magic on socket %d\n", client_sock);
            break;
        }

        uint32_t params_len = be_to_uint32(header + LSRP_MAGIC_LEN);
        if (params_len > LSRP_MAX_PARAMS_LEN) {
            fprintf(stderr, "Parameters length %u exceeds maximum %d on socket %d\n", 
                    params_len, LSRP_MAX_PARAMS_LEN, client_sock);
            break;
        }

        // Read params
        char *params = malloc(params_len + 1);
        if (!params) {
            fprintf(stderr, "Memory allocation failed for params on socket %d\n", client_sock);
            break;
        }
        
        recv_len = recv(client_sock, params, params_len, MSG_WAITALL);
        if (recv_len != params_len) {
            if (recv_len < 0) {
                fprintf(stderr, "Error reading params on socket %d: %s\n", client_sock, strerror(errno));
            } else if (recv_len == 0) {
                fprintf(stderr, "Client closed connection while reading params on socket %d\n", client_sock);
            } else {
                fprintf(stderr, "Incomplete params on socket %d: %zd/%u bytes\n", 
                        client_sock, recv_len, params_len);
            }
            free(params);
            break;
        }
        params[params_len] = '\0';

        lsrp_request_t req = { .params = params, .params_len = params_len };
        lsrp_response_t resp = { .status = 1, .data = NULL, .data_len = 0 };

        // Call handler
        int handler_ret = handler(&req, &resp);

        // If handler failed and no data, set default error
        if (handler_ret < 0 && resp.data == NULL) {
            const char *err_msg = "Internal error";
            resp.data = strdup(err_msg);
            resp.data_len = strlen(err_msg);
        }

        // Prepare response
        size_t resp_header_len = LSRP_MAGIC_LEN + 1 + 4;
        uint8_t *resp_header = malloc(resp_header_len + resp.data_len);
        if (!resp_header) {
            fprintf(stderr, "Memory allocation failed for response on socket %d\n", client_sock);
            free(params);
            if (resp.data) free(resp.data);
            break;
        }
        
        memcpy(resp_header, LSRP_MAGIC, LSRP_MAGIC_LEN);
        resp_header[LSRP_MAGIC_LEN] = resp.status;
        uint32_to_be((uint32_t)resp.data_len, resp_header + LSRP_MAGIC_LEN + 1);
        if (resp.data_len > 0) {
            memcpy(resp_header + resp_header_len, resp.data, resp.data_len);
        }

        // Send response
        ssize_t total_sent = 0;
        size_t to_send = resp_header_len + resp.data_len;
        int send_failed = 0;
        while (total_sent < to_send) {
            ssize_t sent = send(client_sock, resp_header + total_sent, to_send - total_sent, MSG_NOSIGNAL);
            if (sent <= 0) {
                fprintf(stderr, "Failed to send response on socket %d: %s\n", client_sock, strerror(errno));
                send_failed = 1;
                break;
            }
            total_sent += sent;
        }

        // Cleanup request/response
        free(resp_header);
        free(params);
        if (resp.data) free(resp.data);

        // If send failed, close connection
        if (send_failed) {
            break;
        }

        // Продолжаем обрабатывать следующий запрос на том же соединении
    }

    close(client_sock);
    return NULL;
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) {
        fprintf(stderr, "No handler provided\n");
        return -1;
    }

    // Ignore SIGPIPE - предотвращает падение при обрыве соединения
    signal(SIGPIPE, SIG_IGN);

    // Increase file descriptor limit
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        fprintf(stderr, "Failed to get file descriptor limit: %s\n", strerror(errno));
        return -1;
    }
    rlim_t target_limit = 65536;
    rl.rlim_cur = target_limit < rl.rlim_max ? target_limit : rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        fprintf(stderr, "Failed to set file descriptor limit to %ld: %s\n", rl.rlim_cur, strerror(errno));
        return -1;
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -2;
    }

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "Failed to set SO_REUSEADDR: %s\n", strerror(errno));
        close(server_sock);
        return -2;
    }

    // Set socket buffer sizes
    int buf_size = 65536;
    setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind socket: %s\n", strerror(errno));
        close(server_sock);
        return -3;
    }

    if (listen(server_sock, LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "Failed to listen on socket: %s\n", strerror(errno));
        close(server_sock);
        return -4;
    }

    fprintf(stderr, "LSRP server listening on port %d (keep-alive enabled, timeout=%ds)\n", 
            port, KEEPALIVE_TIMEOUT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Failed to accept connection: %s\n", strerror(errno));
            continue;
        }

        // Создаем поток для обработки клиента
        thread_args_t* args = malloc(sizeof(thread_args_t));
        if (!args) {
            fprintf(stderr, "Failed to allocate thread args\n");
            close(client_sock);
            continue;
        }
        args->client_sock = client_sock;
        args->handler = handler;

        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // Уменьшаем размер стека до 512KB (по умолчанию 8MB)
        pthread_attr_setstacksize(&attr, 512 * 1024);
        
        if (pthread_create(&thread, &attr, handle_client, args) != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(errno));
            free(args);
            close(client_sock);
        }
        
        pthread_attr_destroy(&attr);
    }

    close(server_sock);
    return 0;
}