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
#include <sys/epoll.h>

#define LISTEN_BACKLOG 4096
#define RECV_BUFFER_SIZE 8192
#define THREAD_POOL_SIZE 16
#define MAX_EVENTS 512

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
    int sock;
    char buffer[RECV_BUFFER_SIZE];
    size_t buffer_pos;
    size_t expected_len;
} client_state_t;

typedef struct work_item {
    int client_sock;
    lsrp_handler_t handler;
    struct work_item *next;
} work_item_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    work_item_t *head;
    work_item_t *tail;
    int shutdown;
} work_queue_t;

static work_queue_t work_queue = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .head = NULL,
    .tail = NULL,
    .shutdown = 0
};

static void enqueue_work(int client_sock, lsrp_handler_t handler) {
    work_item_t *item = malloc(sizeof(work_item_t));
    if (!item) {
        close(client_sock);
        return;
    }
    
    item->client_sock = client_sock;
    item->handler = handler;
    item->next = NULL;
    
    pthread_mutex_lock(&work_queue.lock);
    if (work_queue.tail) {
        work_queue.tail->next = item;
    } else {
        work_queue.head = item;
    }
    work_queue.tail = item;
    pthread_cond_signal(&work_queue.cond);
    pthread_mutex_unlock(&work_queue.lock);
}

static work_item_t* dequeue_work(void) {
    pthread_mutex_lock(&work_queue.lock);
    
    while (!work_queue.head && !work_queue.shutdown) {
        pthread_cond_wait(&work_queue.cond, &work_queue.lock);
    }
    
    if (work_queue.shutdown) {
        pthread_mutex_unlock(&work_queue.lock);
        return NULL;
    }
    
    work_item_t *item = work_queue.head;
    work_queue.head = item->next;
    if (!work_queue.head) {
        work_queue.tail = NULL;
    }
    
    pthread_mutex_unlock(&work_queue.lock);
    return item;
}

static void process_request(int client_sock, lsrp_handler_t handler) {
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    #ifdef TCP_QUICKACK
    setsockopt(client_sock, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
    #endif

    int buf_size = 262144;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    char buffer[RECV_BUFFER_SIZE];
    ssize_t total_recv = recv(client_sock, buffer, sizeof(buffer), 0);
    
    if (total_recv < (ssize_t)(LSRP_MAGIC_LEN + 4)) {
        close(client_sock);
        return;
    }

    if (memcmp(buffer, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
        close(client_sock);
        return;
    }

    uint32_t params_len = be_to_uint32((uint8_t*)buffer + LSRP_MAGIC_LEN);
    if (params_len > LSRP_MAX_PARAMS_LEN) {
        close(client_sock);
        return;
    }

    size_t expected_total = LSRP_MAGIC_LEN + 4 + params_len;
    
    while ((size_t)total_recv < expected_total) {
        ssize_t n = recv(client_sock, buffer + total_recv, 
                        sizeof(buffer) - total_recv, 0);
        if (n <= 0) {
            close(client_sock);
            return;
        }
        total_recv += n;
    }

    char *params = buffer + LSRP_MAGIC_LEN + 4;
    char params_copy[LSRP_MAX_PARAMS_LEN + 1];
    memcpy(params_copy, params, params_len);
    params_copy[params_len] = '\0';

    lsrp_request_t req = { .params = params_copy, .params_len = params_len };
    lsrp_response_t resp = { .status = 1, .data = NULL, .data_len = 0 };
    handler(&req, &resp);

    if (resp.data == NULL) {
        resp.data = strdup("Internal error");
        resp.data_len = 14;
    }

    size_t total_len = LSRP_MAGIC_LEN + 1 + 4 + resp.data_len;
    uint8_t *response = malloc(total_len);
    
    if (response) {
        memcpy(response, LSRP_MAGIC, LSRP_MAGIC_LEN);
        response[LSRP_MAGIC_LEN] = resp.status;
        uint32_to_be((uint32_t)resp.data_len, response + LSRP_MAGIC_LEN + 1);
        if (resp.data_len > 0) {
            memcpy(response + LSRP_MAGIC_LEN + 5, resp.data, resp.data_len);
        }
        
        send(client_sock, response, total_len, MSG_NOSIGNAL);
        free(response);
    }

    if (resp.data) free(resp.data);
    close(client_sock);
}

static void* worker_thread(void* arg) {
    (void)arg;
    
    while (1) {
        work_item_t *item = dequeue_work();
        if (!item) break;
        
        process_request(item->client_sock, item->handler);
        free(item);
    }
    
    return NULL;
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) {
        fprintf(stderr, "No handler provided\n");
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = (rl.rlim_max < 65536) ? rl.rlim_max : 65536;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    pthread_t workers[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            return -1;
        }
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -2;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    #ifdef TCP_DEFER_ACCEPT
    int timeout = 1;
    setsockopt(server_sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, &timeout, sizeof(timeout));
    #endif

    int buf_size = 262144;
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

    fprintf(stderr, "LSRP server with thread pool (%d workers) listening on port %d\n", 
            THREAD_POOL_SIZE, port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_sock < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        enqueue_work(client_sock, handler);
    }

    work_queue.shutdown = 1;
    pthread_cond_broadcast(&work_queue.cond);
    
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(workers[i], NULL);
    }

    close(server_sock);
    return 0;
}