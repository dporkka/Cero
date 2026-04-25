/*
 * Rate limiting system
 * Token bucket rate limiting persisted in SQLite.
 */

#include "ratelimit.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE_LIMIT_WINDOW_SECONDS 60
#define RATE_LIMIT_RETENTION_SECONDS (86400 * 7)

static ratelimit_result_t ratelimit_check_identifier_internal(const char *identifier) {
    sqlite3_stmt *stmt;
    const char *select_sql = "SELECT tokens, last_refill_at, window_start_at "
                             "FROM rate_limits WHERE identifier = ?";
    const char *upsert_sql = "INSERT INTO rate_limits "
                             "(identifier, tokens, last_refill_at, window_start_at) "
                             "VALUES (?, ?, ?, ?) "
                             "ON CONFLICT(identifier) DO UPDATE SET "
                             "tokens = excluded.tokens, "
                             "last_refill_at = excluded.last_refill_at, "
                             "window_start_at = excluded.window_start_at";
    time_t now;
    double capacity;
    double refill_rate;
    double tokens;
    time_t last_refill_at;
    time_t window_start_at;
    int step_result;

    if (!identifier || identifier[0] == '\0') {
        return RATELIMIT_ERROR;
    }

    now = time(NULL);
    capacity = g_config.rate_limit_requests_per_minute > 0
        ? (double)g_config.rate_limit_requests_per_minute
        : 60.0;
    refill_rate = capacity / RATE_LIMIT_WINDOW_SECONDS;
    tokens = capacity;
    last_refill_at = now;
    window_start_at = now;

    ratelimit_cleanup();

    if (db_prepare(select_sql, &stmt) != 0) {
        LOG_ERROR("ratelimit", "Failed to prepare rate limit select");
        return RATELIMIT_ERROR;
    }

    db_bind_text(stmt, 1, identifier);
    step_result = sqlite3_step(stmt);
    if (step_result == SQLITE_ROW) {
        double stored_tokens = sqlite3_column_double(stmt, 0);
        time_t stored_last_refill = (time_t)db_column_int64(stmt, 1);
        time_t stored_window_start = (time_t)db_column_int64(stmt, 2);
        double elapsed = difftime(now, stored_last_refill);

        if (elapsed < 0) {
            elapsed = 0;
        }

        tokens = stored_tokens + (elapsed * refill_rate);
        if (tokens > capacity) {
            tokens = capacity;
        }

        last_refill_at = now;
        window_start_at = (tokens >= capacity - 0.0001) ? now : stored_window_start;
    } else if (step_result != SQLITE_DONE) {
        db_finalize(stmt);
        LOG_ERROR("ratelimit", "Failed to step rate limit select");
        return RATELIMIT_ERROR;
    }
    db_finalize(stmt);

    if (tokens < 1.0) {
        if (db_prepare(upsert_sql, &stmt) != 0) {
            LOG_ERROR("ratelimit", "Failed to prepare rate limit update");
            return RATELIMIT_ERROR;
        }

        db_bind_text(stmt, 1, identifier);
        sqlite3_bind_double(stmt, 2, tokens);
        db_bind_int64(stmt, 3, last_refill_at);
        db_bind_int64(stmt, 4, window_start_at);
        step_result = sqlite3_step(stmt);
        db_finalize(stmt);

        if (step_result != SQLITE_DONE) {
            LOG_ERROR("ratelimit", "Failed to persist exceeded rate limit");
            return RATELIMIT_ERROR;
        }

        LOG_WARN("ratelimit", "Rate limit exceeded for %s", identifier);
        return RATELIMIT_EXCEEDED;
    }

    tokens -= 1.0;

    if (db_prepare(upsert_sql, &stmt) != 0) {
        LOG_ERROR("ratelimit", "Failed to prepare rate limit upsert");
        return RATELIMIT_ERROR;
    }

    db_bind_text(stmt, 1, identifier);
    sqlite3_bind_double(stmt, 2, tokens);
    db_bind_int64(stmt, 3, now);
    db_bind_int64(stmt, 4, window_start_at);
    step_result = sqlite3_step(stmt);
    db_finalize(stmt);

    if (step_result != SQLITE_DONE) {
        LOG_ERROR("ratelimit", "Failed to persist rate limit state");
        return RATELIMIT_ERROR;
    }

    return RATELIMIT_OK;
}

/* Check rate limit for IP address */
ratelimit_result_t ratelimit_check_ip(const char *ip_address) {
    return ratelimit_check_identifier_internal(ip_address);
}

/* Check rate limit for user */
ratelimit_result_t ratelimit_check_user(int user_id) {
    char identifier[64];
    snprintf(identifier, sizeof(identifier), "user:%d", user_id);
    return ratelimit_check_identifier_internal(identifier);
}

/* Clean up old rate limit entries (call periodically) */
int ratelimit_cleanup(void) {
    time_t now = time(NULL);
    time_t cutoff = now - RATE_LIMIT_RETENTION_SECONDS;

    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM rate_limits WHERE last_refill_at < ?";

    if (db_prepare(sql, &stmt) != 0) {
        LOG_ERROR("ratelimit", "Failed to prepare cleanup query");
        return -1;
    }

    db_bind_int64(stmt, 1, cutoff);

    int result = sqlite3_step(stmt);
    db_finalize(stmt);

    if (result != SQLITE_DONE) {
        LOG_ERROR("ratelimit", "Failed to cleanup rate limits");
        return -1;
    }

    int deleted = sqlite3_changes(g_db);
    if (deleted > 0) {
        LOG_DEBUG("ratelimit", "Cleaned up %d old rate limit entries", deleted);
    }

    return deleted;
}

/* Reset rate limit for identifier (admin tool) */
int ratelimit_reset(const char *identifier) {
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM rate_limits WHERE identifier = ?";

    if (db_prepare(sql, &stmt) != 0) {
        LOG_ERROR("ratelimit", "Failed to prepare reset query");
        return -1;
    }

    db_bind_text(stmt, 1, identifier);

    int result = sqlite3_step(stmt);
    db_finalize(stmt);

    if (result != SQLITE_DONE) {
        LOG_ERROR("ratelimit", "Failed to reset rate limit");
        return -1;
    }

    LOG_INFO("ratelimit", "Reset rate limit for: %s", identifier);
    return 0;
}
