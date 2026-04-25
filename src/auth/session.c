/*
 * Session management
 * Handles session creation, validation, and cleanup
 */

#include "session.h"
#include "../utils/config.h"
#include "../utils/db.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static time_t session_expiry_seconds(void) {
    return g_config.session_expiry_seconds > 0
        ? (time_t)g_config.session_expiry_seconds
        : (time_t)(86400 * 30);
}

/* Create new session for user */
int session_create(int user_id, const char *ip_address, const char *user_agent,
                   char *token_out, size_t token_size) {
    char *token = generate_random_hex(64);
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO sessions "
                      "(user_id, token, created_at, expires_at, last_activity_at, ip_address, user_agent) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?)";
    time_t now = time(NULL);
    time_t expires_at = now + session_expiry_seconds();

    if (!token) {
        LOG_ERROR("session", "Failed to generate session token");
        return -1;
    }

    if (db_prepare(sql, &stmt) != 0) {
        free(token);
        return -1;
    }

    db_bind_int(stmt, 1, user_id);
    db_bind_text(stmt, 2, token);
    db_bind_int64(stmt, 3, now);
    db_bind_int64(stmt, 4, expires_at);
    db_bind_int64(stmt, 5, now);
    db_bind_text(stmt, 6, ip_address ? ip_address : "");
    db_bind_text(stmt, 7, user_agent ? user_agent : "");

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERROR("session", "Failed to create session: %s", db_error_message());
        db_finalize(stmt);
        free(token);
        return -1;
    }

    db_finalize(stmt);
    safe_strncpy(token_out, token, token_size);
    free(token);
    return 0;
}

/* Validate session token and load user context */
int session_validate(const char *token, http_request_t *req) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT s.user_id, s.expires_at, u.account_id, u.email, u.role "
                      "FROM sessions s "
                      "JOIN users u ON s.user_id = u.id "
                      "WHERE s.token = ? AND u.is_active = 1";
    time_t now = time(NULL);
    time_t expires_at;

    if (!token || !req) {
        return 0;
    }

    if (db_prepare(sql, &stmt) != 0) {
        return 0;
    }

    db_bind_text(stmt, 1, token);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        db_finalize(stmt);
        return 0;
    }

    expires_at = (time_t)db_column_int64(stmt, 1);
    if (now > expires_at) {
        db_finalize(stmt);
        session_delete(token);
        return 0;
    }

    req->user_id = db_column_int(stmt, 0);
    req->account_id = db_column_int(stmt, 2);
    safe_strncpy(req->user_email, db_column_text(stmt, 3), sizeof(req->user_email));
    safe_strncpy(req->user_role, db_column_text(stmt, 4), sizeof(req->user_role));
    req->is_authenticated = 1;
    db_finalize(stmt);

    session_update_activity(token);
    return 1;
}

/* Update session last activity timestamp */
int session_update_activity(const char *token) {
    sqlite3_stmt *stmt;
    const char *sql = "UPDATE sessions SET last_activity_at = ?, expires_at = ? WHERE token = ?";
    time_t now = time(NULL);
    time_t expires_at = now + session_expiry_seconds();

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int64(stmt, 1, now);
    db_bind_int64(stmt, 2, expires_at);
    db_bind_text(stmt, 3, token);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        db_finalize(stmt);
        return -1;
    }

    db_finalize(stmt);
    return 0;
}

/* Delete session (logout) */
int session_delete(const char *token) {
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM sessions WHERE token = ?";

    if (!token) {
        return -1;
    }

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_text(stmt, 1, token);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        db_finalize(stmt);
        return -1;
    }

    db_finalize(stmt);
    return 0;
}

/* Delete expired sessions (cleanup) */
int session_cleanup_expired(void) {
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM sessions WHERE expires_at < ?";
    time_t now = time(NULL);
    int deleted;

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int64(stmt, 1, now);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        db_finalize(stmt);
        return -1;
    }

    deleted = sqlite3_changes(g_db);
    db_finalize(stmt);
    return deleted;
}

/* Get session by token */
int session_get_by_token(const char *token, session_t *session) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, user_id, token, created_at, expires_at, "
                      "last_activity_at, ip_address, user_agent "
                      "FROM sessions WHERE token = ?";

    if (!token || !session) {
        return -1;
    }

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_text(stmt, 1, token);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        db_finalize(stmt);
        return -1;
    }

    session->id = db_column_int(stmt, 0);
    session->user_id = db_column_int(stmt, 1);
    safe_strncpy(session->token, db_column_text(stmt, 2), sizeof(session->token));
    session->created_at = (time_t)db_column_int64(stmt, 3);
    session->expires_at = (time_t)db_column_int64(stmt, 4);
    session->last_activity_at = (time_t)db_column_int64(stmt, 5);
    safe_strncpy(session->ip_address, db_column_text(stmt, 6), sizeof(session->ip_address));
    safe_strncpy(session->user_agent, db_column_text(stmt, 7), sizeof(session->user_agent));

    db_finalize(stmt);
    return 0;
}
