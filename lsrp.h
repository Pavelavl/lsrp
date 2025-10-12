#ifndef LSRP_H
#define LSRP_H

#include <stdint.h>
#include <stddef.h>

#define LSRP_MAGIC "LSRP"
#define LSRP_MAGIC_LEN 4
#define LSRP_MAX_PARAMS_LEN 4096
#define LSRP_MAX_DATA_LEN (1024 * 1024)  // 1MB max for SVG/error

typedef struct {
    char *params;
    size_t params_len;
} lsrp_request_t;

typedef struct {
    uint8_t status;  // 0 = OK, 1 = Error
    char *data;
    size_t data_len;
} lsrp_response_t;

#endif // LSRP_H
