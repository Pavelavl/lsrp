#include "lsrp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <errno.h>

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
    struct handle_args *args = (struct handle_args *)arg;
    int client_sock = args->client_sock;
    lsrp_handler_t handler = args->handler;
    free(args);

    // Set socket linger to ensure data is sent before closing
    struct linger linger_opt = { .l_onoff = 1, .l_linger = 5 }; // Wait up to 5 seconds
    setsockopt(client_sock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt));

    // Increase socket buffer sizes
    int buf_size = 65536; // 64KB
    setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    // Handle multiple requests on the same connection
    while (1) {
        // Read header: magic (4) + len (4)
        uint8_t header[LSRP_MAGIC_LEN + 4];
        ssize_t recv_len = recv(client_sock, header, sizeof(header), MSG_WAITALL);
        if (recv_len == 0) {
            fprintf(stderr, "Client closed connection\n");
            break;
        }
        if (recv_len != sizeof(header)) {
            fprintf(stderr, "Failed to read header: %zd bytes received, errno: %s\n", recv_len, strerror(errno));
            break;
        }

        // Check magic
        if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
            fprintf(stderr, "Invalid magic in request\n");
            break;
        }

        uint32_t params_len = be_to_uint32(header + LSRP_MAGIC_LEN);
        if (params_len > LSRP_MAX_PARAMS_LEN) {
            fprintf(stderr, "Parameters length %u exceeds maximum %d\n", params_len, LSRP_MAX_PARAMS_LEN);
            break;
        }

        // Read params
        char *params = malloc(params_len + 1);
        if (!params) {
            fprintf(stderr, "Memory allocation failed for params\n");
            break;
        }
        recv_len = recv(client_sock, params, params_len, MSG_WAITALL);
        if (recv_len != params_len) {
            fprintf(stderr, "Failed to read params: %zd bytes received, expected %u, errno: %s\n", recv_len, params_len, strerror(errno));
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
            fprintf(stderr, "Memory allocation failed for response\n");
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
        while (total_sent < to_send) {
            ssize_t sent = send(client_sock, resp_header + total_sent, to_send - total_sent, 0);
            if (sent <= 0) {
                fprintf(stderr, "Failed to send response: %s\n", strerror(errno));
                break;
            }
            total_sent += sent;
        }
        if (total_sent != to_send) {
            fprintf(stderr, "Incomplete send: %zd bytes sent, expected %zu\n", total_sent, to_send);
        }

        // Cleanup
        free(resp_header);
        free(params);
        if (resp.data) free(resp.data);

        // Continue to handle next request on the same connection
    }

    close(client_sock);
    return NULL;
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) {
        fprintf(stderr, "No handler provided\n");
        return -1;
    }

    // Increase file descriptor limit
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = 4096;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        fprintf(stderr, "Failed to set file descriptor limit: %s\n", strerror(errno));
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -2;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Increase server socket buffer sizes
    int buf_size = 65536; // 64KB
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

    if (listen(server_sock, 10) < 0) {
        fprintf(stderr, "Failed to listen on socket: %s\n", strerror(errno));
        close(server_sock);
        return -4;
    }

    fprintf(stderr, "Server listening on port %d\n", port);

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) {
            fprintf(stderr, "Failed to accept connection: %s\n", strerror(errno));
            continue;
        }

        struct handle_args *args = malloc(sizeof(struct handle_args));
        if (!args) {
            fprintf(stderr, "Memory allocation failed for handle_args\n");
            close(client_sock);
            continue;
        }
        args->client_sock = client_sock;
        args->handler = handler;

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_connection_thread, args) != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(errno));
            free(args);
            close(client_sock);
            continue;
        }
        pthread_detach(thread);
    }

    close(server_sock);
    return 0;
}
