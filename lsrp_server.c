#include "lsrp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static void uint32_to_be(uint32_t val, uint8_t *bytes) {
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
}

static uint32_t be_to_uint32(uint8_t *bytes) {
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

static void handle_connection(int client_sock, lsrp_handler_t handler) {
    // Read header: magic (4) + len (4)
    uint8_t header[LSRP_MAGIC_LEN + 4];
    ssize_t recv_len = recv(client_sock, header, sizeof(header), 0);
    if (recv_len != sizeof(header)) {
        close(client_sock);
        return;
    }

    // Check magic
    if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
        close(client_sock);
        return;
    }

    uint32_t params_len = be_to_uint32(header + LSRP_MAGIC_LEN);
    if (params_len > LSRP_MAX_PARAMS_LEN) {
        close(client_sock);
        return;
    }

    // Read params
    char *params = malloc(params_len + 1);
    if (!params) {
        close(client_sock);
        return;
    }
    recv_len = recv(client_sock, params, params_len, 0);
    if (recv_len != params_len) {
        free(params);
        close(client_sock);
        return;
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
        free(params);
        if (resp.data) free(resp.data);
        close(client_sock);
        return;
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
    close(client_sock);
}

int lsrp_server_start(int port, lsrp_handler_t handler) {
    if (!handler) return -1;

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) return -2;

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_sock);
        return -3;
    }

    if (listen(server_sock, 10) < 0) {
        close(server_sock);
        return -4;
    }

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) continue;

        handle_connection(client_sock, handler);
    }

    close(server_sock);
    return 0;
}
