/*
 * Simple HTTP/1.1 Server Implementation
 * Single-threaded, blocking I/O model for simplicity and longevity
 */

#include "server.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include "../auth/session.h"
#include "../utils/ratelimit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

#define BUFFER_SIZE 65536
#define LISTEN_BACKLOG 128

static int g_server_socket = -1;
static volatile int g_running = 0;

/* Get client IP address from socket */
static void get_client_address(int socket, char *ip_buffer, size_t buffer_size,
                               int *port) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getpeername(socket, (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, ip_buffer, buffer_size);
        *port = ntohs(addr.sin_port);
    } else {
        strncpy(ip_buffer, "unknown", buffer_size);
        *port = 0;
    }
}

/* Send response to client */
static int send_response(int client_socket, http_response_t *response) {
    char *response_str = response_build(response);
    if (response_str == NULL) {
        LOG_ERROR("server", "Failed to build response");
        return -1;
    }

    size_t response_len = strlen(response_str);
    ssize_t sent = send(client_socket, response_str, response_len, 0);

    free(response_str);

    if (sent < 0) {
        LOG_ERROR("server", "Failed to send response: %s", strerror(errno));
        return -1;
    }

    if ((size_t)sent != response_len) {
        LOG_WARN("server", "Incomplete send: %zd of %zu bytes", sent, response_len);
        return -1;
    }

    return 0;
}

/* Send error response */
static void send_error_response(int client_socket, int status_code,
                                const char *message) {
    http_response_t *resp = response_new();
    response_set_status(resp, status_code);
    response_set_content_type(resp, "text/html");

    char body[1024];
    snprintf(body, sizeof(body),
             "<html><head><title>%d Error</title></head>"
             "<body><h1>%d Error</h1><p>%s</p></body></html>",
             status_code, status_code, message);

    response_set_body(resp, body);
    send_response(client_socket, resp);
    response_free(resp);
}

/* Handle single client connection */
void server_handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    char client_ip[64];
    int client_port;

    /* Get client address */
    get_client_address(client_socket, client_ip, sizeof(client_ip), &client_port);

    /* Read request */
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        if (bytes_read < 0) {
            LOG_ERROR("server", "Failed to read from client %s:%d: %s",
                     client_ip, client_port, strerror(errno));
        }
        close(client_socket);
        return;
    }

    buffer[bytes_read] = '\0';
    LOG_DEBUG("server", "Received %zd bytes from %s:%d", bytes_read,
              client_ip, client_port);

    /* Parse request */
    http_request_t req;
    memset(&req, 0, sizeof(req));

    if (request_parse(buffer, bytes_read, &req) != 0) {
        LOG_WARN("server", "Failed to parse request from %s:%d",
                 client_ip, client_port);
        send_error_response(client_socket, HTTP_400_BAD_REQUEST,
                          "Bad Request");
        close(client_socket);
        return;
    }

    /* Set client info */
    safe_strncpy(req.client_ip, client_ip, sizeof(req.client_ip));
    req.client_port = client_port;

    /* Check rate limit */
    ratelimit_result_t rate_result = ratelimit_check_ip(client_ip);
    if (rate_result == RATELIMIT_EXCEEDED) {
        LOG_WARN("server", "Rate limit exceeded for %s", client_ip);
        send_error_response(client_socket, HTTP_429_TOO_MANY_REQUESTS,
                          "Too Many Requests");
        request_free(&req);
        close(client_socket);
        return;
    }

    /* Validate session (if session cookie present) */
    const char *session_token = request_get_cookie(&req, "session_token");
    if (session_token != NULL) {
        if (session_validate(session_token, &req) != 0) {
            /* Session is valid, user context populated in req */
            LOG_DEBUG("server", "Valid session for user %d", req.user_id);
        }
    }

    if (req.is_authenticated) {
        ratelimit_result_t user_rate_result = ratelimit_check_user(req.user_id);
        if (user_rate_result == RATELIMIT_EXCEEDED) {
            LOG_WARN("server", "User rate limit exceeded for user %d", req.user_id);
            send_error_response(client_socket, HTTP_429_TOO_MANY_REQUESTS,
                              "Too Many Requests");
            request_free(&req);
            close(client_socket);
            return;
        }
    }

    /* Route request to handler */
    http_response_t *response = router_handle_request(&req);
    if (response == NULL) {
        LOG_ERROR("server", "Handler returned NULL response");
        send_error_response(client_socket, HTTP_500_INTERNAL_SERVER_ERROR,
                          "Internal Server Error");
    } else {
        /* Send response */
        send_response(client_socket, response);
        response_free(response);
    }

    /* Cleanup */
    request_free(&req);
    close(client_socket);

    LOG_DEBUG("server", "Connection closed: %s:%d", client_ip, client_port);
}

/* Start HTTP server */
int server_start(const char *host, int port) {
    struct sockaddr_in server_addr;

    /* Create socket */
    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket < 0) {
        LOG_ERROR("server", "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    /* Set socket options */
    int opt = 1;
    if (setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        LOG_WARN("server", "Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    /* Setup server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (strcmp(host, "0.0.0.0") == 0) {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
            LOG_ERROR("server", "Invalid host address: %s", host);
            close(g_server_socket);
            return -1;
        }
    }

    /* Bind socket */
    if (bind(g_server_socket, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        LOG_ERROR("server", "Failed to bind to %s:%d: %s",
                 host, port, strerror(errno));
        close(g_server_socket);
        return -1;
    }

    /* Listen */
    if (listen(g_server_socket, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("server", "Failed to listen: %s", strerror(errno));
        close(g_server_socket);
        return -1;
    }

    LOG_INFO("server", "Server listening on %s:%d", host, port);
    g_running = 1;

    /* Accept loop */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(g_server_socket,
                                   (struct sockaddr *)&client_addr,
                                   &client_len);

        if (client_socket < 0) {
            if (errno == EINTR || !g_running) {
                /* Interrupted by signal or shutdown */
                break;
            }
            LOG_ERROR("server", "Failed to accept connection: %s",
                     strerror(errno));
            continue;
        }

        /* Handle client (blocking) */
        server_handle_client(client_socket);
    }

    LOG_INFO("server", "Server stopped");
    return 0;
}

/* Stop HTTP server */
void server_stop(void) {
    LOG_INFO("server", "Stopping server");
    g_running = 0;

    if (g_server_socket >= 0) {
        shutdown(g_server_socket, SHUT_RDWR);
        close(g_server_socket);
        g_server_socket = -1;
    }
}
