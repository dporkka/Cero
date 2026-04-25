# Implementation Guide

This document provides detailed guidance for implementing the remaining components of the Core SaaS Platform.

## Current Status

### ✅ Fully Implemented
- Database schema and utilities (`src/utils/db.c`)
- Configuration parser (`src/utils/config.c`)
- Logging system (`src/utils/log.c`)
- Time utilities (`src/utils/time_utils.c`)
- HTTP server core (`src/core/server.c`)
- Main entry point (`src/main.c`)
- HTML templates (`templates/`)
- Build system (`Makefile`)
- String utilities (`src/utils/string_utils.c`)
- Rate limiting (`src/utils/ratelimit.c`)
- HTTP request parsing (`src/core/request.c`)
- HTTP response building (`src/core/response.c`)
- Routing system (`src/core/router.c`)
- Authentication (`src/auth/auth.c`)
- Session management (`src/auth/session.c`)
- Subscription logic (`src/billing/subscription.c`)
- Entitlement checking (`src/billing/entitlement.c`)
- Admin billing operations (`src/billing/admin.c`)
- Reports generation (`src/reports/reports.c`)
- CSV export (`src/reports/csv.c`)
- Template rendering (`src/templates/template.c`)
- Smoke test coverage (`tests/smoke.sh`)

### ✅ Current Delivered State

The remaining components described in the original guide are now implemented and wired into the running application. The project now includes:

1. Template-rendered login, dashboard, billing, reports, and admin billing flows
2. Bcrypt-compatible password hashing and verification via the system `crypt` implementation
3. SQLite-backed sessions with activity-based expiry and secure cookies
4. SQLite-backed token-bucket rate limiting for IP and user identities
5. Subscription, entitlement, billing event, and CSV/report behavior tied to the documented schema
6. End-to-end smoke coverage through `make test`

---

## Implementation Priority

Implement in this order for incremental functionality:

### Phase 1: Core HTTP (Required to run)
1. String utilities
2. Request parsing
3. Response building
4. Router
5. Template engine

**Milestone**: Server can respond to HTTP requests with rendered HTML

### Phase 2: Authentication (Required for security)
6. Authentication (with bcrypt integration)
7. Session management

**Milestone**: Users can login and maintain sessions

### Phase 3: Billing (Core business logic)
8. Subscription logic
9. Entitlement checking
10. Admin billing operations

**Milestone**: Manual billing workflow complete

### Phase 4: Features (Value-add)
11. Rate limiting
12. Reports generation
13. CSV export

**Milestone**: Full feature set complete

---

## Detailed Implementation Guide

### 1. String Utilities (`src/utils/string_utils.c`)

**Purpose**: Safe string operations and encoding/decoding

**Key Functions**:

```c
#include "string_utils.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void safe_strncpy(char *dest, const char *src, size_t dest_size) {
    if (dest_size == 0) return;
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

char *html_escape(const char *str) {
    // Allocate buffer (worst case: every char needs escaping)
    size_t len = strlen(str);
    char *escaped = malloc(len * 6 + 1); // &quot; = 6 chars
    if (!escaped) return NULL;

    char *p = escaped;
    for (const char *s = str; *s; s++) {
        switch (*s) {
            case '<': strcpy(p, "&lt;"); p += 4; break;
            case '>': strcpy(p, "&gt;"); p += 4; break;
            case '&': strcpy(p, "&amp;"); p += 5; break;
            case '"': strcpy(p, "&quot;"); p += 6; break;
            case '\'': strcpy(p, "&#39;"); p += 5; break;
            default: *p++ = *s; break;
        }
    }
    *p = '\0';
    return escaped;
}

char *generate_random_hex(size_t length) {
    char *hex = malloc(length + 1);
    if (!hex) return NULL;

    FILE *urandom = fopen("/dev/urandom", "r");
    if (!urandom) {
        free(hex);
        return NULL;
    }

    for (size_t i = 0; i < length; i++) {
        unsigned char byte;
        fread(&byte, 1, 1, urandom);
        sprintf(&hex[i], "%02x", byte & 0x0F);
    }
    hex[length] = '\0';
    fclose(urandom);
    return hex;
}

// Implement remaining functions following similar patterns
```

**Testing**:
```c
// Test HTML escaping
char *escaped = html_escape("<script>alert('XSS')</script>");
// Should produce: &lt;script&gt;alert(&#39;XSS&#39;)&lt;/script&gt;

// Test random hex generation
char *token = generate_random_hex(64);
// Should produce 64-character hex string
```

---

### 2. Request Parsing (`src/core/request.c`)

**Purpose**: Parse incoming HTTP requests

**Key Functions**:

```c
#include "request.h"
#include "../utils/string_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int request_parse(const char *buffer, size_t buffer_len, http_request_t *req) {
    memset(req, 0, sizeof(*req));

    char *buf_copy = strndup(buffer, buffer_len);
    if (!buf_copy) return -1;

    // Parse request line: "GET /path HTTP/1.1"
    char *line = strtok(buf_copy, "\r\n");
    if (!line) {
        free(buf_copy);
        return -1;
    }

    // Parse method
    char method_str[16];
    char path[1024];
    char version[16];

    if (sscanf(line, "%15s %1023s %15s", method_str, path, version) != 3) {
        free(buf_copy);
        return -1;
    }

    // Convert method string to enum
    if (strcmp(method_str, "GET") == 0) req->method = HTTP_GET;
    else if (strcmp(method_str, "POST") == 0) req->method = HTTP_POST;
    else req->method = HTTP_UNKNOWN;

    // Split path and query string
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
        safe_strncpy(req->query_string, query + 1, sizeof(req->query_string));
    }
    safe_strncpy(req->path, path, sizeof(req->path));
    safe_strncpy(req->http_version, version, sizeof(req->http_version));

    // Parse headers
    while ((line = strtok(NULL, "\r\n")) != NULL && line[0] != '\0') {
        if (req->header_count >= MAX_HEADERS) break;

        char *colon = strchr(line, ':');
        if (!colon) continue;

        *colon = '\0';
        req->headers[req->header_count].name = strdup(line);
        req->headers[req->header_count].value = strdup(colon + 1);

        // Parse cookies from Cookie header
        if (strcasecmp(line, "Cookie") == 0) {
            // Parse cookie_name=value pairs
            // Store in req->cookies array
        }

        req->header_count++;
    }

    // Parse body (for POST requests)
    const char *body_start = strstr(buffer, "\r\n\r\n");
    if (body_start && req->method == HTTP_POST) {
        body_start += 4;
        size_t body_len = buffer_len - (body_start - buffer);
        req->body = malloc(body_len + 1);
        memcpy(req->body, body_start, body_len);
        req->body[body_len] = '\0';
        req->body_length = body_len;
    }

    free(buf_copy);
    return 0;
}

const char *request_get_header(http_request_t *req, const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

void request_free(http_request_t *req) {
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i].name);
        free(req->headers[i].value);
    }
    if (req->body) {
        free(req->body);
    }
}
```

**Testing**: Parse sample HTTP request and verify all fields populated correctly.

---

### 3. Response Building (`src/core/response.c`)

**Purpose**: Build HTTP responses

**Implementation**:

```c
#include "response.h"
#include "../utils/time_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

http_response_t *response_new(void) {
    http_response_t *resp = calloc(1, sizeof(http_response_t));
    resp->status_code = 200;
    strcpy(resp->status_message, "OK");
    resp->body_capacity = 4096;
    resp->body = malloc(resp->body_capacity);
    resp->body[0] = '\0';
    return resp;
}

void response_set_status(http_response_t *resp, int status_code) {
    resp->status_code = status_code;

    switch (status_code) {
        case 200: strcpy(resp->status_message, "OK"); break;
        case 301: strcpy(resp->status_message, "Moved Permanently"); break;
        case 302: strcpy(resp->status_message, "Found"); break;
        case 400: strcpy(resp->status_message, "Bad Request"); break;
        case 401: strcpy(resp->status_message, "Unauthorized"); break;
        case 403: strcpy(resp->status_message, "Forbidden"); break;
        case 404: strcpy(resp->status_message, "Not Found"); break;
        case 500: strcpy(resp->status_message, "Internal Server Error"); break;
        default: strcpy(resp->status_message, "Unknown"); break;
    }
}

void response_add_header(http_response_t *resp, const char *name, const char *value) {
    if (resp->header_count >= MAX_RESPONSE_HEADERS) return;

    size_t len = strlen(name) + strlen(value) + 4; // ": \r\n"
    resp->headers[resp->header_count] = malloc(len);
    snprintf(resp->headers[resp->header_count], len, "%s: %s", name, value);
    resp->header_count++;
}

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

void response_set_body(http_response_t *resp, const char *body) {
    size_t needed = strlen(body) + 1;
    if (needed > resp->body_capacity) {
        resp->body_capacity = needed * 2;
        resp->body = realloc(resp->body, resp->body_capacity);
    }
    strcpy(resp->body, body);
    resp->body_length = needed - 1;
}

char *response_build(http_response_t *resp) {
    // Build status line
    char status_line[256];
    snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
             resp->status_code, resp->status_message);

    // Calculate total size
    size_t total_size = strlen(status_line);
    for (int i = 0; i < resp->header_count; i++) {
        total_size += strlen(resp->headers[i]) + 2; // +2 for \r\n
    }
    total_size += 2; // blank line
    total_size += resp->body_length;
    total_size += 1; // null terminator

    // Build response
    char *response = malloc(total_size);
    strcpy(response, status_line);

    for (int i = 0; i < resp->header_count; i++) {
        strcat(response, resp->headers[i]);
        strcat(response, "\r\n");
    }

    strcat(response, "\r\n");
    if (resp->body_length > 0) {
        strcat(response, resp->body);
    }

    return response;
}

void response_free(http_response_t *resp) {
    if (!resp) return;

    for (int i = 0; i < resp->header_count; i++) {
        free(resp->headers[i]);
    }
    free(resp->body);
    free(resp);
}
```

---

### 4. Template Engine (`src/templates/template.c`)

**Purpose**: Simple Mustache-style template rendering

**Implementation**:

```c
#include "template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

template_ctx_t *template_ctx_new(void) {
    template_ctx_t *ctx = calloc(1, sizeof(template_ctx_t));
    return ctx;
}

void template_set(template_ctx_t *ctx, const char *key, const char *value) {
    if (ctx->var_count >= MAX_TEMPLATE_VARS) return;

    ctx->vars[ctx->var_count].key = key;
    ctx->vars[ctx->var_count].value = value;
    ctx->var_count++;
}

char *template_load(const char *template_name) {
    char path[512];
    snprintf(path, sizeof(path), "templates/%s", template_name);

    FILE *file = fopen(path, "r");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = malloc(size + 1);
    fread(content, 1, size, file);
    content[size] = '\0';

    fclose(file);
    return content;
}

char *template_render(const char *template_content, template_ctx_t *ctx) {
    size_t result_size = strlen(template_content) * 2; // Estimate
    char *result = malloc(result_size);
    result[0] = '\0';

    const char *p = template_content;
    char *out = result;

    while (*p) {
        if (*p == '{' && *(p+1) == '{') {
            // Find closing }}
            const char *end = strstr(p + 2, "}}");
            if (!end) {
                *out++ = *p++;
                continue;
            }

            // Extract variable name
            size_t key_len = end - (p + 2);
            char key[256];
            strncpy(key, p + 2, key_len);
            key[key_len] = '\0';

            // Remove leading/trailing spaces from key
            char *key_trimmed = key;
            while (*key_trimmed == ' ') key_trimmed++;
            char *key_end = key_trimmed + strlen(key_trimmed) - 1;
            while (key_end > key_trimmed && *key_end == ' ') *key_end-- = '\0';

            // Find value in context
            const char *value = NULL;
            for (int i = 0; i < ctx->var_count; i++) {
                if (strcmp(ctx->vars[i].key, key_trimmed) == 0) {
                    value = ctx->vars[i].value;
                    break;
                }
            }

            // Insert value (or empty string if not found)
            if (value) {
                strcpy(out, value);
                out += strlen(value);
            }

            p = end + 2;
        } else {
            *out++ = *p++;
        }
    }

    *out = '\0';
    return result;
}

char *template_render_file(const char *template_name, template_ctx_t *ctx) {
    char *content = template_load(template_name);
    if (!content) return NULL;

    char *result = template_render(content, ctx);
    free(content);
    return result;
}

void template_ctx_free(template_ctx_t *ctx) {
    free(ctx);
}
```

---

### 5. Router (`src/core/router.c`)

**Purpose**: Route HTTP requests to handlers

**Implementation**:

```c
#include "router.h"
#include "../utils/log.h"
#include <string.h>

#define MAX_ROUTES 100

static route_t routes[MAX_ROUTES];
static int route_count = 0;

void router_init(void) {
    route_count = 0;
}

void router_add_route(http_method_t method, const char *path,
                     route_handler_t handler,
                     int requires_auth, int requires_admin) {
    if (route_count >= MAX_ROUTES) {
        LOG_ERROR("router", "Too many routes");
        return;
    }

    routes[route_count].method = method;
    routes[route_count].path = path;
    routes[route_count].handler = handler;
    routes[route_count].requires_auth = requires_auth;
    routes[route_count].requires_admin = requires_admin;
    route_count++;
}

http_response_t *router_handle_request(http_request_t *req) {
    // Find matching route
    for (int i = 0; i < route_count; i++) {
        if (routes[i].method == req->method &&
            strcmp(routes[i].path, req->path) == 0) {

            // Check authentication
            if (routes[i].requires_auth && !req->is_authenticated) {
                http_response_t *resp = response_new();
                response_set_status(resp, 401);
                response_redirect(resp, "/login", 0);
                return resp;
            }

            // Check admin access
            if (routes[i].requires_admin &&
                strcmp(req->user_role, "admin") != 0) {
                http_response_t *resp = response_new();
                response_set_status(resp, 403);
                response_set_body(resp, "Forbidden");
                return resp;
            }

            // Call handler
            return routes[i].handler(req);
        }
    }

    // No route found - 404
    http_response_t *resp = response_new();
    response_set_status(resp, 404);
    response_set_body(resp, "<h1>404 Not Found</h1>");
    return resp;
}

// Register all application routes
void routes_register_all(void) {
    // Public routes
    router_add_route(HTTP_GET, "/", handle_home_page, 0, 0);
    router_add_route(HTTP_GET, "/login", handle_login_page, 0, 0);
    router_add_route(HTTP_POST, "/login", handle_login_submit, 0, 0);
    router_add_route(HTTP_GET, "/logout", handle_logout, 0, 0);

    // Authenticated routes
    router_add_route(HTTP_GET, "/dashboard", handle_dashboard, 1, 0);
    router_add_route(HTTP_GET, "/billing", handle_billing_page, 1, 0);
    router_add_route(HTTP_GET, "/reports", handle_reports_page, 1, 0);
    router_add_route(HTTP_POST, "/reports/generate", handle_reports_generate, 1, 0);

    // Admin routes
    router_add_route(HTTP_GET, "/admin/billing", handle_admin_billing_page, 1, 1);
    router_add_route(HTTP_POST, "/admin/billing/mark-paid", handle_admin_mark_paid, 1, 1);
}
```

---

## Implementation Tips

### Use Existing Code as Reference
- `src/utils/db.c` - Good example of C style
- `src/utils/config.c` - Shows file parsing
- `src/core/server.c` - Shows error handling

### Memory Management
- Always free allocated memory
- Use `valgrind` to check for leaks
- Prefer stack allocation for small buffers
- Use `calloc` for zero-initialized memory

### Error Handling
- Check all return values
- Log errors using LOG_ERROR
- Return -1 or NULL on error
- Clean up resources before returning

### Security
- Always use parameterized SQL queries
- HTML-escape all user input in templates
- Validate all inputs
- Use secure random for tokens

### Testing Strategy
1. Unit test each function
2. Integration test with database
3. End-to-end test with HTTP requests
4. Load test with many concurrent requests

---

## Build and Test Cycle

```bash
# After implementing a component

# 1. Update Makefile to include new source file
# Add to appropriate *_SOURCES variable

# 2. Rebuild
make clean
make

# 3. Run basic test
./cero

# 4. Check for memory leaks
valgrind --leak-check=full ./cero

# 5. Test HTTP endpoint
curl -v http://localhost:8080/
```

---

## Bcrypt Integration

For password hashing, use an existing bcrypt library:

```bash
# Install bcrypt library
sudo apt-get install libbcrypt-dev

# Or use portable implementation
# Download bcrypt.c and bcrypt.h from:
# https://github.com/rg3/bcrypt

# Add to Makefile
LDFLAGS += -lbcrypt
```

Example usage in `src/auth/auth.c`:

```c
#include <bcrypt.h>

int auth_hash_password(const char *password, char *hash, size_t hash_size) {
    return bcrypt_hashpw(password, bcrypt_gensalt(12), hash);
}

int auth_verify_password(const char *password, const char *hash) {
    return bcrypt_checkpw(password, hash) == 0 ? 1 : 0;
}
```

---

## Completion Checklist

- [x] All .c files compile without warnings
- [x] `make` builds successfully
- [x] Database initialization is exercised by the smoke test
- [x] Server starts and listens on the configured port
- [x] Login page renders
- [x] Login succeeds with test credentials
- [x] Dashboard renders after login
- [x] Billing page renders
- [x] Reports page renders and generates HTML output
- [x] Admin billing page renders
- [x] Admin can mark an account as paid
- [x] Sessions persist across requests
- [x] Rate limiting returns HTTP 429 when limits are exceeded
- [x] CSV export is enforced by entitlements and works after upgrade
- [x] Shared templates render through the application layout
- [x] Database integrity is maintained through SQLite-backed flows
- [x] Logs are written during runtime
- [x] Config and secrets are loaded during startup

---

## Final Notes

- Keep it simple
- Follow the header files exactly
- Use the schema as your guide
- Test incrementally
- Document as you go
- When in doubt, make it boring

**Total estimated implementation time**: 40-80 hours for experienced C developer.

**Result**: Production-ready, 50-year SaaS platform.
