/*
 * Authentication system
 * Handles user login, password hashing, and authentication
 */

#define _GNU_SOURCE
#include "auth.h"
#include "session.h"
#include "../utils/db.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include "../templates/template.h"
#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BCRYPT_COST 12
#define BCRYPT_SALT_LENGTH 22
#define BCRYPT_RAW_SALT_BYTES 16

typedef struct {
    char *values[16];
    int count;
} owned_strings_t;

static const char *auth_track_string(owned_strings_t *owned, char *value) {
    if (!value) {
        return "";
    }

    if (owned->count < (int)(sizeof(owned->values) / sizeof(owned->values[0]))) {
        owned->values[owned->count++] = value;
        return value;
    }

    free(value);
    return "";
}

static const char *auth_track_escape(owned_strings_t *owned, const char *value) {
    return auth_track_string(owned, html_escape(value ? value : ""));
}

static void auth_free_owned(owned_strings_t *owned) {
    for (int i = 0; i < owned->count; i++) {
        free(owned->values[i]);
    }
}

static int auth_is_bcrypt_hash(const char *hash) {
    return hash != NULL &&
           (strncmp(hash, "$2a$", 4) == 0 ||
            strncmp(hash, "$2b$", 4) == 0 ||
            strncmp(hash, "$2x$", 4) == 0 ||
            strncmp(hash, "$2y$", 4) == 0);
}

static void auth_secure_zero(void *buffer, size_t length) {
    volatile unsigned char *p = (volatile unsigned char *)buffer;
    while (length-- > 0) {
        *p++ = 0;
    }
}

static int auth_generate_bcrypt_salt(char *salt, size_t salt_size) {
    static const char bcrypt_alphabet[] =
        "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char random_bytes[BCRYPT_RAW_SALT_BYTES];
    char encoded[BCRYPT_SALT_LENGTH + 1];
    FILE *urandom;
    size_t input_index = 0;
    size_t output_index = 0;

    if (salt_size < 8 + BCRYPT_SALT_LENGTH + 1) {
        return -1;
    }

    urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        return -1;
    }

    if (fread(random_bytes, 1, sizeof(random_bytes), urandom) != sizeof(random_bytes)) {
        fclose(urandom);
        return -1;
    }
    fclose(urandom);

    /*
     * Encode 16 random bytes into bcrypt's custom 64-character alphabet.
     * Bcrypt salts store 6-bit groups, so this mirrors the standard bcrypt
     * base64 packing rules rather than MIME/base64.
     */
    while (input_index < sizeof(random_bytes) && output_index < BCRYPT_SALT_LENGTH) {
        unsigned int c1 = random_bytes[input_index++];
        encoded[output_index++] = bcrypt_alphabet[(c1 >> 2) & 0x3F];
        c1 = (c1 & 0x03) << 4;

        if (input_index >= sizeof(random_bytes)) {
            encoded[output_index++] = bcrypt_alphabet[c1 & 0x3F];
            break;
        }

        unsigned int c2 = random_bytes[input_index++];
        c1 |= (c2 >> 4) & 0x0F;
        encoded[output_index++] = bcrypt_alphabet[c1 & 0x3F];
        c1 = (c2 & 0x0F) << 2;

        if (input_index >= sizeof(random_bytes)) {
            encoded[output_index++] = bcrypt_alphabet[c1 & 0x3F];
            break;
        }

        unsigned int c3 = random_bytes[input_index++];
        c1 |= (c3 >> 6) & 0x03;
        encoded[output_index++] = bcrypt_alphabet[c1 & 0x3F];
        encoded[output_index++] = bcrypt_alphabet[c3 & 0x3F];
    }
    encoded[output_index] = '\0';

    snprintf(salt, salt_size, "$2b$%02d$%s", BCRYPT_COST, encoded);
    return 0;
}

static void auth_set_html_response(http_response_t *resp,
                                   int status_code,
                                   const char *html) {
    response_set_status(resp, status_code);
    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, html);
}

static char *auth_render_login_page(http_request_t *req, const char *error_message) {
    template_ctx_t *ctx = template_ctx_new();
    owned_strings_t owned = {{0}, 0};
    char *page;
    char error_html[1024];

    if (!ctx) {
        return NULL;
    }

    if (error_message && error_message[0] != '\0') {
        const char *escaped_error = auth_track_escape(&owned, error_message);
        snprintf(error_html, sizeof(error_html),
                 "<div class=\"alert alert-error\">%s</div>", escaped_error);
    } else {
        error_html[0] = '\0';
    }

    template_set(ctx, "error_html", error_html);
    template_set(ctx, "csrf_token", "");

    page = template_render_page("Login", "login.html", ctx,
                                req ? req->is_authenticated : 0,
                                req ? req->user_email : "",
                                req && strcmp(req->user_role, "admin") == 0);

    auth_free_owned(&owned);
    template_ctx_free(ctx);
    return page;
}

/* Hash password using bcrypt-compatible libcrypt support */
int auth_hash_password(const char *password, char *hash, size_t hash_size) {
    char salt[64];
    char *hashed;

    if (!password || !hash || hash_size == 0) {
        return -1;
    }

    if (auth_generate_bcrypt_salt(salt, sizeof(salt)) != 0) {
        LOG_ERROR("auth", "Failed to generate bcrypt salt");
        return -1;
    }

    hashed = crypt(password, salt);
    if (!hashed || hashed[0] == '\0') {
        LOG_ERROR("auth", "Failed to hash password with bcrypt salt");
        return -1;
    }

    if (!auth_is_bcrypt_hash(hashed)) {
        LOG_ERROR("auth", "System crypt did not return a bcrypt hash");
        return -1;
    }

    safe_strncpy(hash, hashed, hash_size);
    return 0;
}

/* Verify password against hash */
int auth_verify_password(const char *password, const char *hash) {
    char *result;

    if (!password || !hash) {
        return 0;
    }

    result = crypt(password, hash);
    if (!result) {
        LOG_ERROR("auth", "Failed to verify password");
        return 0;
    }

    return strcmp(result, hash) == 0 ? 1 : 0;
}

/* Authenticate user with email and password */
int auth_authenticate_user(const char *email, const char *password) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, password_hash, is_active FROM users WHERE email = ?";
    int user_id;
    char password_hash[256];
    int is_active;

    if (db_prepare(sql, &stmt) != 0) {
        LOG_ERROR("auth", "Failed to prepare authentication query");
        return -1;
    }

    db_bind_text(stmt, 1, email);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        db_finalize(stmt);
        LOG_INFO("auth", "User not found: %s", email ? email : "(null)");
        return -1;
    }

    user_id = db_column_int(stmt, 0);
    safe_strncpy(password_hash, db_column_text(stmt, 1), sizeof(password_hash));
    is_active = db_column_int(stmt, 2);
    db_finalize(stmt);

    if (!is_active) {
        LOG_WARN("auth", "Inactive user attempted login: %s", email);
        return -1;
    }

    if (!auth_verify_password(password, password_hash)) {
        LOG_WARN("auth", "Invalid password for user: %s", email);
        return -1;
    }

    return user_id;
}

/* Create user account */
int auth_create_user(int account_id, const char *email, const char *password, const char *role) {
    char password_hash[256];
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO users "
                      "(account_id, email, password_hash, role, is_active, created_at) "
                      "VALUES (?, ?, ?, ?, 1, ?)";
    time_t now = time(NULL);

    if (auth_hash_password(password, password_hash, sizeof(password_hash)) != 0) {
        return -1;
    }

    if (db_prepare(sql, &stmt) != 0) {
        LOG_ERROR("auth", "Failed to prepare user creation query");
        return -1;
    }

    db_bind_int(stmt, 1, account_id);
    db_bind_text(stmt, 2, email);
    db_bind_text(stmt, 3, password_hash);
    db_bind_text(stmt, 4, role ? role : "user");
    db_bind_int64(stmt, 5, now);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERROR("auth", "Failed to create user: %s", db_error_message());
        db_finalize(stmt);
        return -1;
    }

    db_finalize(stmt);
    return (int)db_last_insert_rowid();
}

/* Update user last login timestamp */
int auth_update_last_login(int user_id) {
    sqlite3_stmt *stmt;
    const char *sql = "UPDATE users SET last_login_at = ? WHERE id = ?";
    time_t now = time(NULL);

    if (db_prepare(sql, &stmt) != 0) {
        LOG_ERROR("auth", "Failed to prepare login update query");
        return -1;
    }

    db_bind_int64(stmt, 1, now);
    db_bind_int(stmt, 2, user_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERROR("auth", "Failed to update last login");
        db_finalize(stmt);
        return -1;
    }

    db_finalize(stmt);
    return 0;
}

/* Route handler: Login page */
http_response_t *handle_login_page(http_request_t *req) {
    http_response_t *resp = response_new();
    char *html;

    if (!resp) {
        return NULL;
    }

    if (req->is_authenticated) {
        response_redirect(resp, "/dashboard", 0);
        return resp;
    }

    html = auth_render_login_page(req, NULL);
    if (!html) {
        auth_set_html_response(resp, HTTP_500_INTERNAL_SERVER_ERROR,
                               "<h1>Error</h1><p>Failed to render login page</p>");
        return resp;
    }

    auth_set_html_response(resp, HTTP_200_OK, html);
    free(html);
    return resp;
}

/* Route handler: Login form submission */
http_response_t *handle_login_submit(http_request_t *req) {
    http_response_t *resp = response_new();
    char *email = request_get_post_param(req, "email");
    char *password = request_get_post_param(req, "password");
    int user_id;

    if (!resp) {
        free(email);
        free(password);
        return NULL;
    }

    if (!email || !password) {
        char *html = auth_render_login_page(req, "Email and password are required.");
        auth_set_html_response(resp, HTTP_400_BAD_REQUEST,
                               html ? html : "<h1>Bad Request</h1>");
        free(html);
        free(email);
        free(password);
        return resp;
    }

    user_id = auth_authenticate_user(email, password);
    auth_secure_zero(password, strlen(password));
    free(password);

    if (user_id < 0) {
        char *html = auth_render_login_page(req, "Invalid email or password.");
        auth_set_html_response(resp, HTTP_401_UNAUTHORIZED,
                               html ? html : "<h1>Unauthorized</h1>");
        free(html);
        free(email);
        return resp;
    }

    auth_update_last_login(user_id);

    {
        char session_token[65];
        if (session_create(user_id, req->client_ip,
                           request_get_header(req, "User-Agent"),
                           session_token, sizeof(session_token)) != 0) {
            free(email);
            auth_set_html_response(resp, HTTP_500_INTERNAL_SERVER_ERROR,
                                   "<h1>Error</h1><p>Failed to create session.</p>");
            return resp;
        }

        response_set_cookie(resp, "session_token", session_token,
                            86400 * 30, 1, 0, "Strict");
    }

    free(email);
    response_redirect(resp, "/dashboard", 0);
    return resp;
}

/* Route handler: Logout */
http_response_t *handle_logout(http_request_t *req) {
    http_response_t *resp = response_new();
    const char *token;

    if (!resp) {
        return NULL;
    }

    token = request_get_cookie(req, "session_token");
    if (token) {
        session_delete(token);
    }

    response_delete_cookie(resp, "session_token");
    response_redirect(resp, "/", 0);
    return resp;
}
