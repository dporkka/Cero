/*
 * Subscription management
 * Handles subscription lifecycle and plan management
 */

#include "subscription.h"
#include "../utils/db.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_GRACE_DAYS 7
/* Assumes a modern 64-bit time_t runtime, which matches the deployment target. */
#define FREE_PLAN_VALID_YEARS 100

static int subscription_log_change(int account_id,
                                   const subscription_t *previous,
                                   plan_t new_plan,
                                   subscription_status_t new_status,
                                   int admin_user_id,
                                   const char *notes,
                                   time_t occurred_at) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO billing_events "
                      "(account_id, event_type, previous_plan, new_plan, previous_status, new_status, admin_user_id, notes, occurred_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
    const char *event_type = previous ? "subscription_updated" : "subscription_created";

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int(stmt, 1, account_id);
    db_bind_text(stmt, 2, event_type);
    db_bind_text(stmt, 3, previous ? subscription_plan_to_string(previous->plan) : "");
    db_bind_text(stmt, 4, subscription_plan_to_string(new_plan));
    db_bind_text(stmt, 5, previous ? subscription_status_to_string(previous->status) : "");
    db_bind_text(stmt, 6, subscription_status_to_string(new_status));
    db_bind_int(stmt, 7, admin_user_id);
    db_bind_text(stmt, 8, notes ? notes : "");
    db_bind_int64(stmt, 9, occurred_at);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        db_finalize(stmt);
        return -1;
    }

    db_finalize(stmt);
    return 0;
}

/* Convert plan enum to string */
const char *subscription_plan_to_string(plan_t plan) {
    switch (plan) {
        case PLAN_FREE: return "free";
        case PLAN_PRO: return "pro";
        case PLAN_ENTERPRISE: return "enterprise";
        default: return "free";
    }
}

/* Convert status enum to string */
const char *subscription_status_to_string(subscription_status_t status) {
    switch (status) {
        case STATUS_ACTIVE: return "active";
        case STATUS_GRACE_PERIOD: return "grace_period";
        case STATUS_EXPIRED: return "expired";
        case STATUS_CANCELLED: return "cancelled";
        default: return "expired";
    }
}

/* Convert string to plan enum */
plan_t subscription_string_to_plan(const char *str) {
    if (str && strcmp(str, "pro") == 0) return PLAN_PRO;
    if (str && strcmp(str, "enterprise") == 0) return PLAN_ENTERPRISE;
    return PLAN_FREE;
}

/* Convert string to status enum */
subscription_status_t subscription_string_to_status(const char *str) {
    if (str && strcmp(str, "active") == 0) return STATUS_ACTIVE;
    if (str && strcmp(str, "grace_period") == 0) return STATUS_GRACE_PERIOD;
    if (str && strcmp(str, "cancelled") == 0) return STATUS_CANCELLED;
    return STATUS_EXPIRED;
}

/* Get subscription for account */
int subscription_get_by_account(int account_id, subscription_t *sub) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, account_id, plan, status, valid_from, valid_until, "
                      "COALESCE(grace_until, 0), COALESCE(provider, 'manual'), "
                      "COALESCE(external_id, ''), COALESCE(notes, ''), created_at, updated_at "
                      "FROM subscriptions WHERE account_id = ?";

    if (!sub) {
        return -1;
    }

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int(stmt, 1, account_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        db_finalize(stmt);
        return -1;
    }

    memset(sub, 0, sizeof(*sub));
    sub->id = db_column_int(stmt, 0);
    sub->account_id = db_column_int(stmt, 1);
    sub->plan = subscription_string_to_plan(db_column_text(stmt, 2));
    sub->status = subscription_string_to_status(db_column_text(stmt, 3));
    sub->valid_from = (time_t)db_column_int64(stmt, 4);
    sub->valid_until = (time_t)db_column_int64(stmt, 5);
    sub->grace_until = (time_t)db_column_int64(stmt, 6);
    safe_strncpy(sub->provider, db_column_text(stmt, 7), sizeof(sub->provider));
    safe_strncpy(sub->external_id, db_column_text(stmt, 8), sizeof(sub->external_id));
    safe_strncpy(sub->notes, db_column_text(stmt, 9), sizeof(sub->notes));
    sub->created_at = (time_t)db_column_int64(stmt, 10);
    sub->updated_at = (time_t)db_column_int64(stmt, 11);

    db_finalize(stmt);
    return 0;
}

/* Check if subscription is currently valid (including grace period) */
int subscription_is_valid(const subscription_t *sub) {
    time_t now;

    if (!sub) {
        return 0;
    }

    now = time(NULL);

    if (sub->status == STATUS_ACTIVE) {
        return now >= sub->valid_from && now <= sub->valid_until;
    }

    if (sub->status == STATUS_GRACE_PERIOD) {
        return sub->grace_until > 0 && now <= sub->grace_until;
    }

    if ((sub->status == STATUS_CANCELLED || sub->status == STATUS_EXPIRED) &&
        sub->grace_until > 0 && now <= sub->grace_until) {
        return 1;
    }

    return 0;
}

/* Update subscription (logs event to billing_events) */
int subscription_update(int account_id, plan_t new_plan,
                       subscription_status_t new_status,
                       time_t valid_until, int admin_user_id,
                       const char *notes) {
    subscription_t current_sub;
    int has_current = subscription_get_by_account(account_id, &current_sub) == 0;
    sqlite3_stmt *stmt;
    time_t now = time(NULL);
    time_t valid_from = has_current ? current_sub.valid_from : now;
    time_t grace_until = 0;

    if (valid_until <= 0) {
        valid_until = now;
    }

    if (new_status == STATUS_ACTIVE) {
        valid_from = now;
    }

    if (new_status == STATUS_GRACE_PERIOD) {
        grace_until = valid_until + (DEFAULT_GRACE_DAYS * 86400);
    } else if (has_current && current_sub.grace_until > now &&
               (new_status == STATUS_CANCELLED || new_status == STATUS_EXPIRED)) {
        grace_until = current_sub.grace_until;
    }

    if (db_begin_transaction() != SQLITE_OK) {
        return -1;
    }

    if (has_current) {
        const char *sql = "UPDATE subscriptions "
                          "SET plan = ?, status = ?, valid_from = ?, valid_until = ?, grace_until = ?, "
                          "provider = 'manual', notes = ?, updated_at = ? "
                          "WHERE account_id = ?";
        if (db_prepare(sql, &stmt) != 0) {
            db_rollback_transaction();
            return -1;
        }

        db_bind_text(stmt, 1, subscription_plan_to_string(new_plan));
        db_bind_text(stmt, 2, subscription_status_to_string(new_status));
        db_bind_int64(stmt, 3, valid_from);
        db_bind_int64(stmt, 4, valid_until);
        if (grace_until > 0) {
            db_bind_int64(stmt, 5, grace_until);
        } else {
            sqlite3_bind_null(stmt, 5);
        }
        db_bind_text(stmt, 6, notes ? notes : "");
        db_bind_int64(stmt, 7, now);
        db_bind_int(stmt, 8, account_id);
    } else {
        const char *sql = "INSERT INTO subscriptions "
                          "(account_id, plan, status, valid_from, valid_until, grace_until, provider, external_id, notes, created_at, updated_at) "
                          "VALUES (?, ?, ?, ?, ?, ?, 'manual', '', ?, ?, ?)";
        if (db_prepare(sql, &stmt) != 0) {
            db_rollback_transaction();
            return -1;
        }

        db_bind_int(stmt, 1, account_id);
        db_bind_text(stmt, 2, subscription_plan_to_string(new_plan));
        db_bind_text(stmt, 3, subscription_status_to_string(new_status));
        db_bind_int64(stmt, 4, valid_from);
        db_bind_int64(stmt, 5, valid_until);
        if (grace_until > 0) {
            db_bind_int64(stmt, 6, grace_until);
        } else {
            sqlite3_bind_null(stmt, 6);
        }
        db_bind_text(stmt, 7, notes ? notes : "");
        db_bind_int64(stmt, 8, now);
        db_bind_int64(stmt, 9, now);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        db_finalize(stmt);
        db_rollback_transaction();
        return -1;
    }
    db_finalize(stmt);

    if (subscription_log_change(account_id,
                                has_current ? &current_sub : NULL,
                                new_plan, new_status,
                                admin_user_id, notes, now) != 0) {
        db_rollback_transaction();
        return -1;
    }

    if (db_commit_transaction() != SQLITE_OK) {
        db_rollback_transaction();
        return -1;
    }

    return 0;
}

/* Create initial subscription for new account */
int subscription_create(int account_id, plan_t plan) {
    time_t now = time(NULL);
    time_t valid_until;

    if (plan == PLAN_FREE) {
        valid_until = now + ((time_t)FREE_PLAN_VALID_YEARS * 365 * 86400);
    } else {
        valid_until = now + (30 * 86400);
    }

    return subscription_update(account_id, plan, STATUS_ACTIVE, valid_until, 0,
                               "Initial subscription");
}
