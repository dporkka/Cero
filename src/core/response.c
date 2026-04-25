/*
 * HTTP response building
 * Handles building HTTP responses
 */

#include "response.h"
#include "../utils/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void response_remove_header(http_response_t *resp, const char *name) {
    size_t name_len = strlen(name);

    for (int i = 0; i < resp->header_count; i++) {
        if (strncmp(resp->headers[i], name, name_len) == 0 &&
            resp->headers[i][name_len] == ':') {
            free(resp->headers[i]);
            for (int j = i; j < resp->header_count - 1; j++) {
                resp->headers[j] = resp->headers[j + 1];
            }
            resp->header_count--;
            i--;
        }
    }
}

static void response_set_or_replace_header(http_response_t *resp,
                                           const char *name,
                                           const char *value) {
    response_remove_header(resp, name);
    response_add_header(resp, name, value);
}

/* Create new response */
http_response_t *response_new(void) {
    http_response_t *resp = calloc(1, sizeof(http_response_t));
    if (!resp) {
        LOG_ERROR("response", "Failed to allocate response");
        return NULL;
    }

    resp->status_code = 200;
    strcpy(resp->status_message, "OK");
    resp->body_capacity = 4096;
    resp->body = malloc(resp->body_capacity);
    if (!resp->body) {
        free(resp);
        return NULL;
    }
    resp->body[0] = '\0';
    resp->body_length = 0;

    return resp;
}

/* Set response status */
void response_set_status(http_response_t *resp, int status_code) {
    if (!resp) return;

    resp->status_code = status_code;

    switch (status_code) {
        case 200: strcpy(resp->status_message, "OK"); break;
        case 201: strcpy(resp->status_message, "Created"); break;
        case 204: strcpy(resp->status_message, "No Content"); break;
        case 301: strcpy(resp->status_message, "Moved Permanently"); break;
        case 302: strcpy(resp->status_message, "Found"); break;
        case 303: strcpy(resp->status_message, "See Other"); break;
        case 304: strcpy(resp->status_message, "Not Modified"); break;
        case 400: strcpy(resp->status_message, "Bad Request"); break;
        case 401: strcpy(resp->status_message, "Unauthorized"); break;
        case 403: strcpy(resp->status_message, "Forbidden"); break;
        case 404: strcpy(resp->status_message, "Not Found"); break;
        case 405: strcpy(resp->status_message, "Method Not Allowed"); break;
        case 429: strcpy(resp->status_message, "Too Many Requests"); break;
        case 500: strcpy(resp->status_message, "Internal Server Error"); break;
        case 503: strcpy(resp->status_message, "Service Unavailable"); break;
        default: strcpy(resp->status_message, "Unknown"); break;
    }
}

/* Add response header */
void response_add_header(http_response_t *resp, const char *name, const char *value) {
    if (!resp || !name || !value) return;

    if (resp->header_count >= MAX_RESPONSE_HEADERS) {
        LOG_WARN("response", "Too many headers");
        return;
    }

    size_t len = strlen(name) + strlen(value) + 4; /* ": \r\n" + null */
    resp->headers[resp->header_count] = malloc(len);
    if (!resp->headers[resp->header_count]) {
        LOG_ERROR("response", "Failed to allocate header");
        return;
    }

    snprintf(resp->headers[resp->header_count], len, "%s: %s", name, value);
    resp->header_count++;
}

/* Set cookie */
void response_set_cookie(http_response_t *resp, const char *name, const char *value,
                        int max_age, int http_only, int secure, const char *same_site) {
    char cookie[1024];
    int len = snprintf(cookie, sizeof(cookie), "%s=%s", name, value);

    if (max_age > 0) {
        len += snprintf(cookie + len, sizeof(cookie) - len, "; Max-Age=%d", max_age);
    }
    if (http_only) {
        len += snprintf(cookie + len, sizeof(cookie) - len, "; HttpOnly");
    }
    if (secure) {
        len += snprintf(cookie + len, sizeof(cookie) - len, "; Secure");
    }
    if (same_site) {
        len += snprintf(cookie + len, sizeof(cookie) - len, "; SameSite=%s", same_site);
    }

    len += snprintf(cookie + len, sizeof(cookie) - len, "; Path=/");

    response_add_header(resp, "Set-Cookie", cookie);
}

/* Delete cookie */
void response_delete_cookie(http_response_t *resp, const char *name) {
    /* Set cookie with past expiration to delete it */
    response_set_cookie(resp, name, "", -1, 1, 0, "Strict");
}

/* Set response body */
void response_set_body(http_response_t *resp, const char *body) {
    const char *safe_body = body ? body : "";
    size_t needed = strlen(safe_body) + 1;

    if (needed > resp->body_capacity) {
        resp->body_capacity = needed * 2;
        char *new_body = realloc(resp->body, resp->body_capacity);
        if (!new_body) {
            LOG_ERROR("response", "Failed to reallocate body");
            return;
        }
        resp->body = new_body;
    }

    strcpy(resp->body, safe_body);
    resp->body_length = needed - 1;

    /* Automatically set Content-Length header */
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%zu", resp->body_length);
    response_set_or_replace_header(resp, "Content-Length", content_length);
}

/* Append to response body */
void response_append_body(http_response_t *resp, const char *data) {
    if (!resp || !data) return;

    size_t data_len = strlen(data);
    size_t needed = resp->body_length + data_len + 1;

    if (needed > resp->body_capacity) {
        resp->body_capacity = needed * 2;
        char *new_body = realloc(resp->body, resp->body_capacity);
        if (!new_body) {
            LOG_ERROR("response", "Failed to reallocate body");
            return;
        }
        resp->body = new_body;
    }

    strcat(resp->body, data);
    resp->body_length += data_len;

    /* Update Content-Length header */
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%zu", resp->body_length);
    response_set_or_replace_header(resp, "Content-Length", content_length);
}

/* Set content type */
void response_set_content_type(http_response_t *resp, const char *content_type) {
    response_set_or_replace_header(resp, "Content-Type", content_type);
}

/* Redirect to URL */
void response_redirect(http_response_t *resp, const char *url, int permanent) {
    response_set_status(resp, permanent ? 301 : 302);
    response_set_or_replace_header(resp, "Location", url);
    response_set_body(resp, "");
}

/* Build response as HTTP string - caller must free result */
char *response_build(http_response_t *resp) {
    /* Build status line */
    char status_line[256];
    snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
             resp->status_code, resp->status_message);

    /* Calculate total size */
    size_t total_size = strlen(status_line);
    for (int i = 0; i < resp->header_count; i++) {
        total_size += strlen(resp->headers[i]) + 2; /* +2 for \r\n */
    }
    total_size += 2; /* blank line */
    total_size += resp->body_length;
    total_size += 1; /* null terminator */

    /* Build response */
    char *response = calloc(1, total_size);
    if (!response) {
        LOG_ERROR("response", "Failed to allocate response buffer");
        return NULL;
    }

    size_t offset = 0;
    memcpy(response + offset, status_line, strlen(status_line));
    offset += strlen(status_line);

    for (int i = 0; i < resp->header_count; i++) {
        size_t header_len = strlen(resp->headers[i]);
        memcpy(response + offset, resp->headers[i], header_len);
        offset += header_len;
        memcpy(response + offset, "\r\n", 2);
        offset += 2;
    }

    memcpy(response + offset, "\r\n", 2);
    offset += 2;

    if (resp->body_length > 0) {
        memcpy(response + offset, resp->body, resp->body_length);
    }

    return response;
}

/* Free response */
void response_free(http_response_t *resp) {
    if (!resp) return;

    for (int i = 0; i < resp->header_count; i++) {
        free(resp->headers[i]);
    }

    free(resp->body);
    free(resp);
}
