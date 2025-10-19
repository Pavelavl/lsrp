#include "lsrp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>

static volatile int keep_running = 1;
static lsrp_handler_t global_handler = NULL; // Store handler for threaded connections

#define MAX_CONNECTIONS 100
static int active_connections[MAX_CONNECTIONS];
static int connection_count = 0;
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;

// Signal handler for SIGINT/SIGTERM
static void signal_handler(int sig) {
    keep_running = 0;
}

static void add_connection(int client_sock) {
    pthread_mutex_lock(&connections_mutex);
    if (connection_count < MAX_CONNECTIONS) {
        active_connections[connection_count++] = client_sock;
    } else {
        fprintf(stderr, "Max connections reached, dropping socket %d\n", client_sock);
        close(client_sock);
    }
    pthread_mutex_unlock(&connections_mutex);
}

static void remove_connection(int client_sock) {
    pthread_mutex_lock(&connections_mutex);
    for (int i = 0; i < connection_count; i++) {
        if (active_connections[i] == client_sock) {
            active_connections[i] = active_connections[--connection_count];
            break;
        }
    }
    pthread_mutex_unlock(&connections_mutex);
}

static void close_all_connections() {
    pthread_mutex_lock(&connections_mutex);
    for (int i = 0; i < connection_count; i++) {
        close(active_connections[i]);
    }
    connection_count = 0;
    pthread_mutex_unlock(&connections_mutex);
}

static void uint32_to_be(uint32_t val, uint8_t *bytes) {
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
}

static uint32_t be_to_uint32(uint8_t *bytes) {
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

static void *handle_connection_thread(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);

    // Set SO_LINGER to ensure clean closure
    struct linger linger = { .l_onoff = 1, .l_linger = 5 }; // Wait up to 5s for data
    setsockopt(client_sock, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

    add_connection(client_sock);

    // Read header: magic (4) + len (4)
    uint8_t header[LSRP_MAGIC_LEN + 4];
    ssize_t recv_len = recv(client_sock, header, sizeof(header), 0);
    if (recv_len != sizeof(header)) {
        fprintf(stderr, "Failed to read header from socket %d: %zd bytes\n", client_sock, recv_len);
        remove_connection(client_sock);
        close(client_sock);
        return NULL;
    }

    // Check magic
    if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
        fprintf(stderr, "Invalid magic header on socket %d\n", client_sock);
        remove_connection(client_sock);
        close(client_sock);
        return NULL;
    }

    uint32_t params_len = be_to_uint32(header + LSRP_MAGIC_LEN);
    if (params_len > LSRP_MAX_PARAMS_LEN) {
        fprintf(stderr, "Params too large (%u) on socket %d\n", params_len, client_sock);
        remove_connection(client_sock);
        close(client_sock);
        return NULL;
    }

    // Read params
    char *params = malloc(params_len + 1);
    if (!params) {
        fprintf(stderr, "Memory allocation failed for params on socket %d\n", client_sock);
        remove_connection(client_sock);
        close(client_sock);
        return NULL;
    }
    recv_len = recv(client_sock, params, params_len, 0);
    if (recv_len != params_len) {
        fprintf(stderr, "Failed to read params (%zd/%u) on socket %d\n", recv_len, params_len, client_sock);
        free(params);
        remove_connection(client_sock);
        close(client_sock);
        return NULL;
    }
    params[params_len] = '\0';

    lsrp_request_t req = { .params = params, .params_len = params_len };
    lsrp_response_t resp = { .status = 1, .data = NULL, .data_len = 0 };

    // Call handler
    int handler_ret = global_handler(&req, &resp);

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
        remove_connection(client_sock);
        close(client_sock);
        return NULL;
    }
    memcpy(resp_header, LSRP_MAGIC, LSRP_MAGIC_LEN);
    resp_header[LSRP_MAGIC_LEN] = resp.status;
    uint32_to_be((uint32_t)resp.data_len, resp_header + LSRP_MAGIC_LEN + 1);
    if (resp.data_len > 0) {
        memcpy(resp_header + resp_header_len, resp.data, resp.data_len);
    }

    // Send
    send(client_sock, resp_header, resp_header_len + resp.data_len, 0);

    // Cleanup
    free(resp_header);
    free(params);
    if (resp.data) free(resp.data);
    remove_connection(client_sock);
    close(client_sock);
    return NULL;
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) {
        fprintf(stderr, "No handler provided\n");
        return -1;
    }

    // Validate port
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return -1;
    }

    // Store handler for threads
    global_handler = handler;

    // Set signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        fprintf(stderr, "Socket creation failed: %s (errno=%d)\n", strerror(errno), errno);
        return -2;
    }

    // Set non-blocking mode for graceful shutdown
    int flags = fcntl(server_sock, F_GETFL, 0);
    fcntl(server_sock, F_SETFL, flags | O_NONBLOCK);

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Optional: Enable SO_REUSEPORT for multiple servers (uncomment if needed)
    // setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Retry bind up to 5 times with 1-second delay
    int attempts = 5;
    int delay_ms = 1000;
    for (int i = 0; i < attempts; i++) {
        if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "Bind failed on port %d: %s (errno=%d), attempt %d/%d\n", 
                    port, strerror(errno), errno, i + 1, attempts);
            if (i < attempts - 1) {
                usleep(delay_ms * 1000);
                continue;
            }
            close(server_sock);
            return -3;
        }
        break;
    }

    if (listen(server_sock, 10) < 0) {
        fprintf(stderr, "Listen failed on port %d: %s (errno=%d)\n", port, strerror(errno), errno);
        close(server_sock);
        return -4;
    }

    printf("Server listening on port %d\n", port);

    while (keep_running) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000); // 100ms sleep
                continue;
            }
            fprintf(stderr, "Accept failed: %s (errno=%d)\n", strerror(errno), errno);
            continue;
        }

        // Start new thread for connection
        int *sock_ptr = malloc(sizeof(int));
        if (!sock_ptr) {
            fprintf(stderr, "Memory allocation failed for client socket %d\n", client_sock);
            close(client_sock);
            continue;
        }
        *sock_ptr = client_sock;
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_connection_thread, sock_ptr) != 0) {
            fprintf(stderr, "Failed to create thread for client socket %d\n", client_sock);
            free(sock_ptr);
            close(client_sock);
            continue;
        }
        pthread_detach(thread); // Detach to avoid joining
    }

    printf("Initiating graceful shutdown...\n");

    close(server_sock);
    close_all_connections();
    pthread_mutex_destroy(&connections_mutex);

    printf("Server shutdown complete.\n");
    return 0;
}
