#ifndef LSRP_CLIENT_H
#define LSRP_CLIENT_H

#include "lsrp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

/**
 * Send LSRP request to server and get response.
 * @param host Hostname or IP.
 * @param port Port number.
 * @param params Query string params (e.g., "file=example.rrd&start=now-1h").
 * @param response Pointer to response struct (caller must free response->data).
 * @return 0 on success, <0 on error.
 */
int lsrp_client_send(const char *host, int port, const char *params, lsrp_response_t *response);

#endif // LSRP_CLIENT_H
