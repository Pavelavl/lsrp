#include "lsrp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

static void uint32_to_be(uint32_t val, uint8_t *bytes) {
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
}

static uint32_t be_to_uint32(uint8_t *bytes) {
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

int lsrp_client_send(const char *host, int port, const char *params, lsrp_response_t *response) {
    if (!response) return -1;

    size_t params_len = strlen(params);
    if (params_len > LSRP_MAX_PARAMS_LEN) return -2;

    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -3;

    // Resolve host
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        close(sock);
        return -4;
    }

    // Connect
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sock);
        return -5;
    }
    freeaddrinfo(res);

    // Prepare request
    uint8_t *request = malloc(LSRP_MAGIC_LEN + 4 + params_len);
    if (!request) {
        close(sock);
        return -6;
    }
    memcpy(request, LSRP_MAGIC, LSRP_MAGIC_LEN);
    uint32_to_be((uint32_t)params_len, request + LSRP_MAGIC_LEN);
    memcpy(request + LSRP_MAGIC_LEN + 4, params, params_len);

    // Send
    if (send(sock, request, LSRP_MAGIC_LEN + 4 + params_len, 0) < 0) {
        free(request);
        close(sock);
        return -7;
    }
    free(request);

    // Read header: magic (4) + status (1) + len (4)
    uint8_t header[LSRP_MAGIC_LEN + 1 + 4];
    ssize_t recv_len = recv(sock, header, sizeof(header), 0);
    if (recv_len != sizeof(header)) {
        close(sock);
        return -8;
    }

    // Check magic
    if (memcmp(header, LSRP_MAGIC, LSRP_MAGIC_LEN) != 0) {
        close(sock);
        return -9;
    }

    response->status = header[LSRP_MAGIC_LEN];
    uint32_t data_len = be_to_uint32(header + LSRP_MAGIC_LEN + 1);
    if (data_len > LSRP_MAX_DATA_LEN) {
        close(sock);
        return -10;
    }

    // Read data
    response->data = malloc(data_len);
    if (!response->data) {
        close(sock);
        return -11;
    }
    recv_len = recv(sock, response->data, data_len, 0);
    if (recv_len != data_len) {
        free(response->data);
        close(sock);
        return -12;
    }
    response->data_len = data_len;

    close(sock);
    return 0;
}
