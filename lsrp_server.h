#ifndef LSRP_SERVER_H
#define LSRP_SERVER_H

#include "lsrp.h"

/**
 * Handler callback: Parse req, generate response.
 * @param req Request with params.
 * @param resp Response to fill (malloc data inside handler).
 * @return 0 on success, <0 on error (server will set error response).
 */
typedef int (*lsrp_handler_t)(lsrp_request_t *req, lsrp_response_t *resp);

/**
 * Start LSRP server.
 * @param port Port to listen on.
 * @param handler Callback for each request.
 * @return 0 on success, <0 on error. Runs forever until interrupted.
 */
int lsrp_server_start(int port, lsrp_handler_t handler);

struct handle_args {
    int client_sock;
    lsrp_handler_t handler;
};

#endif // LSRP_SERVER_H
