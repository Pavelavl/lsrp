#include "lsrp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s host:port \"params\" [-o output_file]\n", argv[0]);
        return 1;
    }

    char *host_port = argv[1];
    char *params = argv[2];
    char *output_file = NULL;
    if (argc > 3 && strcmp(argv[3], "-o") == 0 && argc > 4) {
        output_file = argv[4];
    }

    // Parse host:port
    char *host = strtok(host_port, ":");
    char *port_str = strtok(NULL, ":");
    int port = port_str ? atoi(port_str) : 8080;

    lsrp_response_t response;
    int ret = lsrp_client_send(host, port, params, &response);
    if (ret < 0) {
        fprintf(stderr, "Error: %d\n", ret);
        return 1;
    }

    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) {
            perror("fopen");
            free(response.data);
            return 1;
        }
    }

    if (response.status == 0) {
        fwrite(response.data, 1, response.data_len, out);
    } else {
        fprintf(stderr, "Error: ");
        fwrite(response.data, 1, response.data_len, stderr);
        fprintf(stderr, "\n");
    }

    free(response.data);
    if (output_file) fclose(out);

    return response.status == 0 ? 0 : 1;
}
