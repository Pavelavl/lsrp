#include "lsrp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>

#define LISTEN_BACKLOG 1024 // High backlog for concurrent connections

static void uint32_to_be(uint32_t val, uint8_t *bytes) {
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
}

static uint32_t be_to_uint32(uint8_t *bytes) {
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) {
        fprintf(stderr, "No handler provided\n");
        return -1;
    }

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

    if (listen(server_sock, LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "Failed to listen on socket: %s\n", strerror(errno));
        close(server_sock);
        return -4;
    }

    fprintf(stderr, "Server listening on port %d with backlog %d\n", port, LISTEN_BACKLOG);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            fprintf(stderr, "Failed to accept connection: %s\n", strerror(errno));
            continue;
        }

        // Set client socket buffer sizes
        setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

        // Read header: magic (4) + len (4)
        uint8_t header[LSRP_MAGIC_LEN + 4];
        ssize_t recv_len = recv(client_sock, header, sizeof(header), MSG_WAITALL);
        if (recv_len != sizeof(header)) {
            if (recv_len < 0) {
                fprintf(stderr, "Error reading header from socket %d: %s\n", client_sock, strerror(errno));
            } else if (recv_len == 0) {
                fprintf(stderr, "Client closed connection on socket %d\n", client_sock);
            } else {
                fprintf(stderr, "Incomplete header on socket %d: %zd bytes received\n", client_sock, recv_len);
            }
            close(client_sock);
            continue;
        }

        // Check magic
        if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
            fprintf(stderr, "Invalid magic in request on socket %d\n", client_sock);
            close(client_sock);
            continue;
        }

        uint32_t params_len = be_to_uint32(header + LSRP_MAGIC_LEN);
        if (params_len > LSRP_MAX_PARAMS_LEN) {
            fprintf(stderr, "Parameters length %u exceeds maximum %d on socket %d\n", params_len, LSRP_MAX_PARAMS_LEN, client_sock);
            close(client_sock);
            continue;
        }

        // Read params
        char *params = malloc(params_len + 1);
        if (!params) {
            fprintf(stderr, "Memory allocation failed for params on socket %d\n", client_sock);
            close(client_sock);
            continue;
        }
        recv_len = recv(client_sock, params, params_len, MSG_WAITALL);
        if (recv_len != params_len) {
            if (recv_len < 0) {
                fprintf(stderr, "Error reading params on socket %d: %s\n", client_sock, strerror(errno));
            } else if (recv_len == 0) {
                fprintf(stderr, "Client closed connection on socket %d\n", client_sock);
            } else {
                fprintf(stderr, "Failed to read params on socket %d: %zd bytes received, expected %u\n", client_sock, recv_len, params_len);
            }
            free(params);
            close(client_sock);
            continue;
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
            close(client_sock);
            continue;
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
                fprintf(stderr, "Failed to send response on socket %d: %s\n", client_sock, strerror(errno));
                break;
            }
            total_sent += sent;
        }

        // Cleanup
        free(resp_header);
        free(params);
        if (resp.data) free(resp.data);
        close(client_sock);
    }

    close(server_sock);
    return 0;
}