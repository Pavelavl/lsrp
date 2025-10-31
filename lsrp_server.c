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

static inline void uint32_to_be(uint32_t val, uint8_t *bytes) {
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
}

static inline uint32_t be_to_uint32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | 
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
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

    pthread_detach(pthread_self());

    // Отключаем Nagle для минимальной latency
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Устанавливаем размеры буферов
    int buf_size = 131072; // 128KB
    setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    // Обрабатываем только ОДИН запрос (как HTTP без keep-alive)
    uint8_t header[LSRP_MAGIC_LEN + 4];
    ssize_t recv_len = recv(client_sock, header, sizeof(header), MSG_WAITALL);

    if (recv_len != sizeof(header)) {
        close(client_sock);
        return NULL;
    }

    // Check magic
    if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
        close(client_sock);
        return NULL;
    }

    uint32_t params_len = be_to_uint32(header + LSRP_MAGIC_LEN);
    if (params_len > LSRP_MAX_PARAMS_LEN) {
        close(client_sock);
        return NULL;
    }

    // Read params
    char *params = malloc(params_len + 1);
    if (!params) {
        close(client_sock);
        return NULL;
    }
    
    recv_len = recv(client_sock, params, params_len, MSG_WAITALL);
    if (recv_len != (ssize_t)params_len) {
        free(params);
        close(client_sock);
        return NULL;
    }
    params[params_len] = '\0';

    // Call handler
    lsrp_request_t req = { .params = params, .params_len = params_len };
    lsrp_response_t resp = { .status = 1, .data = NULL, .data_len = 0 };
    handler(&req, &resp);

    if (resp.data == NULL) {
        resp.data = strdup("Internal error");
        resp.data_len = 14;
    }

    // Prepare and send response (одна аллокация, одна отправка)
    size_t total_len = LSRP_MAGIC_LEN + 1 + 4 + resp.data_len;
    uint8_t *response = malloc(total_len);
    
    if (response) {
        memcpy(response, LSRP_MAGIC, LSRP_MAGIC_LEN);
        response[LSRP_MAGIC_LEN] = resp.status;
        uint32_to_be((uint32_t)resp.data_len, response + LSRP_MAGIC_LEN + 1);
        if (resp.data_len > 0) {
            memcpy(response + LSRP_MAGIC_LEN + 5, resp.data, resp.data_len);
        }
        
        // Отправляем весь ответ одним вызовом
        send(client_sock, response, total_len, MSG_NOSIGNAL);
        free(response);
    }

    free(params);
    if (resp.data) free(resp.data);
    close(client_sock);
    return NULL;
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) {
        fprintf(stderr, "No handler provided\n");
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);

    // Increase file descriptor limit
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = (rl.rlim_max < 65536) ? rl.rlim_max : 65536;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -2;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)); // Для параллельности

    // Буферы для сервера
    int buf_size = 131072;
    setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind: %s\n", strerror(errno));
        close(server_sock);
        return -3;
    }

    if (listen(server_sock, LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "Failed to listen: %s\n", strerror(errno));
        close(server_sock);
        return -4;
    }

    fprintf(stderr, "LSRP server listening on port %d\n", port);

    // Атрибуты потоков (создаем один раз)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024); // 256KB stack
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_sock < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        thread_args_t* args = malloc(sizeof(thread_args_t));
        if (!args) {
            close(client_sock);
            continue;
        }
        args->client_sock = client_sock;
        args->handler = handler;

        pthread_t thread;
        if (pthread_create(&thread, &attr, handle_client, args) != 0) {
            free(args);
            close(client_sock);
        }
    }

    pthread_attr_destroy(&attr);
    close(server_sock);
    return 0;
}