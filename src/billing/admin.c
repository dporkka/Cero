/*
 * Admin billing operations
 * Manual billing workflows and administrative functions
 */

#include "admin.h"
#include "subscription.h"
#include "../templates/template.h"
#include "../utils/db.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include "../utils/time_utils.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *values[32];
    int count;
} owned_strings_t;

typedef struct {
    int account_id;
    char account_name[256];
    char plan[32];
    char status[32];
    time_t valid_until;
} account_search_result_t;

static const char *track_string(owned_strings_t *owned, char *value) {
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

static const char *track_escape(owned_strings_t *owned, const char *value) {
    return track_string(owned, html_escape(value ? value : ""));
}

static void free_owned(owned_strings_t *owned) {
    for (int i = 0; i < owned->count; i++) {
        free(owned->values[i]);
    }
}

static int append_format(char **buffer, size_t *length, size_t *capacity,
                         const char *fmt, ...) {
    va_list args;
    int needed;

    if (!*buffer) {
        *capacity = 1024;
        *buffer = calloc(1, *capacity);
        if (!*buffer) {
            return -1;
        }
    }

    while (1) {
        va_start(args, fmt);
        needed = vsnprintf(*buffer + *length, *capacity - *length, fmt, args);
        va_end(args);

        if (needed < 0) {
            return -1;
        }

        if (*length + (size_t)needed < *capacity) {
            *length += (size_t)needed;
            return 0;
        }

        *capacity *= 2;
        {
            char *new_buffer = realloc(*buffer, *capacity);
            if (!new_buffer) {
                return -1;
            }
            *buffer = new_buffer;
        }
    }
}

static void format_display_date(time_t timestamp, char *buffer, size_t buffer_size) {
    char iso[32];

    if (timestamp <= 0) {
        safe_strncpy(buffer, "N/A", buffer_size);
        return;
    }

    format_timestamp_iso8601(timestamp, iso, sizeof(iso));
    iso[10] = '\0';
    safe_strncpy(buffer, iso, buffer_size);
}

static int admin_search_accounts_query(const char *search_term,
                                       account_search_result_t **results_out,
                                       int *count_out) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT DISTINCT a.id, a.name, COALESCE(s.plan, 'free'), "
                      "COALESCE(s.status, 'active'), COALESCE(s.valid_until, 0) "
                      "FROM accounts a "
                      "LEFT JOIN subscriptions s ON s.account_id = a.id "
                      "LEFT JOIN users u ON u.account_id = a.id "
                      "WHERE (? = '' OR a.name LIKE ? ESCAPE '\\' OR u.email LIKE ? ESCAPE '\\') "
                      "ORDER BY a.name";
    char *like_pattern = NULL;
    account_search_result_t *results = NULL;
    int count = 0;
    int capacity = 0;

    *results_out = NULL;
    *count_out = 0;

    if (!search_term) {
        search_term = "";
    }

    like_pattern = malloc(strlen(search_term) + 3);
    if (!like_pattern) {
        return -1;
    }
    snprintf(like_pattern, strlen(search_term) + 3, "%%%s%%", search_term);

    if (db_prepare(sql, &stmt) != 0) {
        free(like_pattern);
        return -1;
    }

    db_bind_text(stmt, 1, search_term);
    db_bind_text(stmt, 2, like_pattern);
    db_bind_text(stmt, 3, like_pattern);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count == capacity) {
            int new_capacity = capacity == 0 ? 8 : capacity * 2;
            account_search_result_t *new_results =
                realloc(results, sizeof(account_search_result_t) * new_capacity);
            if (!new_results) {
                db_finalize(stmt);
                free(like_pattern);
                free(results);
                return -1;
            }
            results = new_results;
            capacity = new_capacity;
        }

        memset(&results[count], 0, sizeof(results[count]));
        results[count].account_id = db_column_int(stmt, 0);
        safe_strncpy(results[count].account_name, db_column_text(stmt, 1),
                     sizeof(results[count].account_name));
        safe_strncpy(results[count].plan, db_column_text(stmt, 2),
                     sizeof(results[count].plan));
        safe_strncpy(results[count].status, db_column_text(stmt, 3),
                     sizeof(results[count].status));
        results[count].valid_until = (time_t)db_column_int64(stmt, 4);
        count++;
    }

    db_finalize(stmt);
    free(like_pattern);

    *results_out = results;
    *count_out = count;
    return 0;
}

static char *admin_render_search_results(const char *search_term) {
    account_search_result_t *results = NULL;
    int count = 0;
    char *html = NULL;
    size_t length = 0;
    size_t capacity = 0;
    owned_strings_t owned = {{0}, 0};

    if (!search_term || search_term[0] == '\0') {
        return strdup("");
    }

    if (admin_search_accounts_query(search_term, &results, &count) != 0) {
        return strdup("<div class=\"alert alert-error\">Failed to search accounts.</div>");
    }

    if (append_format(&html, &length, &capacity,
                      "<h3 style=\"margin-top: 2rem;\">Search Results</h3>"
                      "<p style=\"color: #7f8c8d;\">Query: %s</p>"
                      "<table><thead><tr>"
                      "<th>Account ID</th><th>Account Name</th><th>Plan</th>"
                      "<th>Status</th><th>Valid Until</th><th>Actions</th>"
                      "</tr></thead><tbody>",
                      track_escape(&owned, search_term)) != 0) {
        free(results);
        free_owned(&owned);
        free(html);
        return NULL;
    }

    if (count == 0) {
        append_format(&html, &length, &capacity,
                      "<tr><td colspan=\"6\" style=\"text-align: center; color: #7f8c8d;\">"
                      "No matching accounts found</td></tr>");
    }

    for (int i = 0; i < count; i++) {
        char valid_until[32];
        format_display_date(results[i].valid_until, valid_until, sizeof(valid_until));
        append_format(&html, &length, &capacity,
                      "<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
                      "<td><a class=\"button button-secondary\" href=\"/admin/billing?account_id=%d\">Manage</a></td></tr>",
                      results[i].account_id,
                      track_escape(&owned, results[i].account_name),
                      track_escape(&owned, results[i].plan),
                      track_escape(&owned, results[i].status),
                      valid_until,
                      results[i].account_id);
    }

    append_format(&html, &length, &capacity, "</tbody></table>");

    free(results);
    free_owned(&owned);
    return html ? html : strdup("");
}

static char *admin_render_account_management(int account_id) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT a.name, COALESCE(s.plan, 'free'), COALESCE(s.status, 'active'), "
                      "COALESCE(s.valid_until, 0) "
                      "FROM accounts a LEFT JOIN subscriptions s ON s.account_id = a.id "
                      "WHERE a.id = ?";
    char *html = NULL;
    size_t length = 0;
    size_t capacity = 0;
    char account_name[256] = "";
    char current_plan[32] = "free";
    char current_status[32] = "active";
    char valid_until[32];
    billing_event_t *events = NULL;
    int event_count = 0;
    owned_strings_t owned = {{0}, 0};

    if (account_id <= 0) {
        return strdup("");
    }

    if (db_prepare(sql, &stmt) != 0) {
        return strdup("<div class=\"alert alert-error\">Failed to load account details.</div>");
    }

    db_bind_int(stmt, 1, account_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        db_finalize(stmt);
        return strdup("<div class=\"alert alert-error\">Account not found.</div>");
    }

    safe_strncpy(account_name, db_column_text(stmt, 0), sizeof(account_name));
    safe_strncpy(current_plan, db_column_text(stmt, 1), sizeof(current_plan));
    safe_strncpy(current_status, db_column_text(stmt, 2), sizeof(current_status));
    format_display_date((time_t)db_column_int64(stmt, 3), valid_until, sizeof(valid_until));
    db_finalize(stmt);

    append_format(&html, &length, &capacity,
                  "<h3 style=\"margin-top: 2rem;\">Manage Subscription: %s</h3>"
                  "<h4>Current Subscription</h4>"
                  "<table><tr><th>Plan</th><td>%s</td></tr>"
                  "<tr><th>Status</th><td>%s</td></tr>"
                  "<tr><th>Valid Until</th><td>%s</td></tr></table>"
                  "<h4 style=\"margin-top: 1.5rem;\">Mark Payment as Received</h4>"
                  "<form method=\"POST\" action=\"/admin/billing/mark-paid\">"
                  "<input type=\"hidden\" name=\"account_id\" value=\"%d\">"
                  "<div class=\"form-group\"><label for=\"plan\">New Plan</label>"
                  "<select id=\"plan\" name=\"plan\" required>"
                  "<option value=\"free\" %s>Free</option>"
                  "<option value=\"pro\" %s>Pro</option>"
                  "<option value=\"enterprise\" %s>Enterprise</option>"
                  "</select></div>"
                  "<div class=\"form-group\"><label for=\"duration_days\">Duration (days)</label>"
                  "<input type=\"number\" id=\"duration_days\" name=\"duration_days\" value=\"30\" min=\"1\" required></div>"
                  "<div class=\"form-group\"><label for=\"amount_cents\">Amount (cents)</label>"
                  "<input type=\"number\" id=\"amount_cents\" name=\"amount_cents\" value=\"4900\" min=\"0\" required></div>"
                  "<div class=\"form-group\"><label for=\"payment_method\">Payment Method</label>"
                  "<select id=\"payment_method\" name=\"payment_method\" required>"
                  "<option value=\"manual\">Manual</option><option value=\"bank_transfer\">Bank Transfer</option>"
                  "<option value=\"check\">Check</option><option value=\"payment_link\">Payment Link</option>"
                  "<option value=\"other\">Other</option></select></div>"
                  "<div class=\"form-group\"><label for=\"external_reference\">External Reference</label>"
                  "<input type=\"text\" id=\"external_reference\" name=\"external_reference\"></div>"
                  "<div class=\"form-group\"><label for=\"notes\">Admin Notes</label>"
                  "<textarea id=\"notes\" name=\"notes\" rows=\"3\"></textarea></div>"
                  "<div class=\"form-group\"><button type=\"submit\" class=\"button\">Mark as Paid &amp; Update Subscription</button>"
                  "<a href=\"/admin/billing\" class=\"button button-secondary\">Cancel</a></div>"
                  "</form><h4 style=\"margin-top: 2rem;\">Billing Event History</h4>"
                  "<table><thead><tr><th>Date</th><th>Event</th><th>From → To</th><th>Amount</th>"
                  "<th>Reference</th><th>Admin</th><th>Notes</th></tr></thead><tbody>",
                  track_escape(&owned, account_name),
                  track_escape(&owned, current_plan),
                  track_escape(&owned, current_status),
                  valid_until,
                  account_id,
                  strcmp(current_plan, "free") == 0 ? "selected" : "",
                  strcmp(current_plan, "pro") == 0 ? "selected" : "",
                  strcmp(current_plan, "enterprise") == 0 ? "selected" : "");

    if (billing_get_events_for_account(account_id, &events, &event_count) != 0 || event_count == 0) {
        append_format(&html, &length, &capacity,
                      "<tr><td colspan=\"7\" style=\"text-align: center; color: #7f8c8d;\">"
                      "No billing events recorded</td></tr>");
    } else {
        for (int i = 0; i < event_count; i++) {
            char occurred_at[32];
            char amount[32];

            format_display_date(events[i].occurred_at, occurred_at, sizeof(occurred_at));
            if (events[i].amount_cents > 0) {
                snprintf(amount, sizeof(amount), "$%.2f", events[i].amount_cents / 100.0);
            } else {
                safe_strncpy(amount, "-", sizeof(amount));
            }

            append_format(&html, &length, &capacity,
                          "<tr><td>%s</td><td>%s</td><td>%s → %s</td><td>%s</td><td>%s</td><td>%d</td><td>%s</td></tr>",
                          occurred_at,
                          track_escape(&owned, events[i].event_type),
                          track_escape(&owned, events[i].previous_plan[0] ? events[i].previous_plan : "-"),
                          track_escape(&owned, events[i].new_plan[0] ? events[i].new_plan : "-"),
                          amount,
                          track_escape(&owned, events[i].external_reference[0] ? events[i].external_reference : "-"),
                          events[i].admin_user_id,
                          track_escape(&owned, events[i].notes[0] ? events[i].notes : "-"));
        }
    }

    append_format(&html, &length, &capacity, "</tbody></table>");

    free(events);
    free_owned(&owned);
    return html ? html : strdup("");
}

static http_response_t *admin_render_page(http_request_t *req,
                                          const char *search_term,
                                          int selected_account_id) {
    http_response_t *resp = response_new();
    template_ctx_t *ctx = template_ctx_new();
    char *search_results_html = admin_render_search_results(search_term);
    char *account_management_html = admin_render_account_management(selected_account_id);
    char *page = NULL;

    if (!resp || !ctx || !search_results_html || !account_management_html) {
        response_free(resp);
        template_ctx_free(ctx);
        free(search_results_html);
        free(account_management_html);
        return NULL;
    }

    template_set(ctx, "search_results_html", search_results_html);
    template_set(ctx, "account_management_html", account_management_html);
    page = template_render_page("Admin Billing", "admin_billing.html", ctx,
                                req->is_authenticated, req->user_email,
                                strcmp(req->user_role, "admin") == 0);

    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, page ? page : "<h1>Error</h1>");

    free(page);
    free(search_results_html);
    free(account_management_html);
    template_ctx_free(ctx);
    return resp;
}

/* Log billing event (append-only) */
int billing_log_event(int account_id, const char *event_type,
                     const char *previous_plan, const char *new_plan,
                     const char *previous_status, const char *new_status,
                     int amount_cents, const char *currency,
                     const char *payment_method, const char *external_reference,
                     int admin_user_id, const char *notes) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO billing_events "
                      "(account_id, event_type, previous_plan, new_plan, previous_status, new_status, "
                      "amount_cents, currency, payment_method, external_reference, admin_user_id, notes, occurred_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    time_t now = time(NULL);

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int(stmt, 1, account_id);
    db_bind_text(stmt, 2, event_type ? event_type : "");
    db_bind_text(stmt, 3, previous_plan ? previous_plan : "");
    db_bind_text(stmt, 4, new_plan ? new_plan : "");
    db_bind_text(stmt, 5, previous_status ? previous_status : "");
    db_bind_text(stmt, 6, new_status ? new_status : "");
    db_bind_int(stmt, 7, amount_cents);
    db_bind_text(stmt, 8, currency ? currency : "USD");
    db_bind_text(stmt, 9, payment_method ? payment_method : "");
    db_bind_text(stmt, 10, external_reference ? external_reference : "");
    db_bind_int(stmt, 11, admin_user_id);
    db_bind_text(stmt, 12, notes ? notes : "");
    db_bind_int64(stmt, 13, now);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        db_finalize(stmt);
        return -1;
    }

    db_finalize(stmt);
    return 0;
}

/* Mark subscription as paid (admin action) */
int billing_mark_as_paid(int account_id, plan_t plan, int duration_days,
                        int amount_cents, const char *payment_method,
                        const char *external_reference, int admin_user_id,
                        const char *notes) {
    subscription_t current;
    int has_current = subscription_get_by_account(account_id, &current) == 0;
    time_t now = time(NULL);
    time_t base = now;
    time_t valid_until;

    if (duration_days <= 0) {
        return -1;
    }

    if (has_current && current.valid_until > now) {
        base = current.valid_until;
    }

    valid_until = base + ((time_t)duration_days * 86400);

    if (subscription_update(account_id, plan, STATUS_ACTIVE, valid_until,
                            admin_user_id, notes) != 0) {
        return -1;
    }

    return billing_log_event(account_id, "payment_received",
                             has_current ? subscription_plan_to_string(current.plan) : "",
                             subscription_plan_to_string(plan),
                             has_current ? subscription_status_to_string(current.status) : "",
                             subscription_status_to_string(STATUS_ACTIVE),
                             amount_cents, "USD", payment_method,
                             external_reference, admin_user_id, notes);
}

/* Get billing events for account */
int billing_get_events_for_account(int account_id, billing_event_t **events, int *count) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, account_id, event_type, COALESCE(previous_plan, ''), COALESCE(new_plan, ''), "
                      "COALESCE(previous_status, ''), COALESCE(new_status, ''), COALESCE(amount_cents, 0), "
                      "COALESCE(currency, 'USD'), COALESCE(payment_method, ''), "
                      "COALESCE(external_reference, ''), COALESCE(admin_user_id, 0), "
                      "COALESCE(notes, ''), occurred_at "
                      "FROM billing_events WHERE account_id = ? ORDER BY occurred_at DESC";
    billing_event_t *result = NULL;
    int result_count = 0;
    int capacity = 0;

    *events = NULL;
    *count = 0;

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int(stmt, 1, account_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (result_count == capacity) {
            int new_capacity = capacity == 0 ? 8 : capacity * 2;
            billing_event_t *new_result =
                realloc(result, sizeof(billing_event_t) * new_capacity);
            if (!new_result) {
                db_finalize(stmt);
                free(result);
                return -1;
            }
            result = new_result;
            capacity = new_capacity;
        }

        memset(&result[result_count], 0, sizeof(result[result_count]));
        result[result_count].id = db_column_int(stmt, 0);
        result[result_count].account_id = db_column_int(stmt, 1);
        safe_strncpy(result[result_count].event_type, db_column_text(stmt, 2),
                     sizeof(result[result_count].event_type));
        safe_strncpy(result[result_count].previous_plan, db_column_text(stmt, 3),
                     sizeof(result[result_count].previous_plan));
        safe_strncpy(result[result_count].new_plan, db_column_text(stmt, 4),
                     sizeof(result[result_count].new_plan));
        safe_strncpy(result[result_count].previous_status, db_column_text(stmt, 5),
                     sizeof(result[result_count].previous_status));
        safe_strncpy(result[result_count].new_status, db_column_text(stmt, 6),
                     sizeof(result[result_count].new_status));
        result[result_count].amount_cents = db_column_int(stmt, 7);
        safe_strncpy(result[result_count].currency, db_column_text(stmt, 8),
                     sizeof(result[result_count].currency));
        safe_strncpy(result[result_count].payment_method, db_column_text(stmt, 9),
                     sizeof(result[result_count].payment_method));
        safe_strncpy(result[result_count].external_reference, db_column_text(stmt, 10),
                     sizeof(result[result_count].external_reference));
        result[result_count].admin_user_id = db_column_int(stmt, 11);
        safe_strncpy(result[result_count].notes, db_column_text(stmt, 12),
                     sizeof(result[result_count].notes));
        result[result_count].occurred_at = (time_t)db_column_int64(stmt, 13);
        result_count++;
    }

    db_finalize(stmt);
    *events = result;
    *count = result_count;
    return 0;
}

/* Route handler: Admin billing page */
http_response_t *handle_admin_billing_page(http_request_t *req) {
    char *search_term = request_get_query_param(req, "q");
    char *account_id_str = request_get_query_param(req, "account_id");
    int account_id = account_id_str ? atoi(account_id_str) : 0;
    http_response_t *resp = admin_render_page(req, search_term ? search_term : "", account_id);

    free(search_term);
    free(account_id_str);
    return resp;
}

/* Route handler: Mark account as paid */
http_response_t *handle_admin_mark_paid(http_request_t *req) {
    http_response_t *resp = response_new();
    char *account_id_str = request_get_post_param(req, "account_id");
    char *plan_str = request_get_post_param(req, "plan");
    char *duration_str = request_get_post_param(req, "duration_days");
    char *amount_cents_str = request_get_post_param(req, "amount_cents");
    char *payment_method = request_get_post_param(req, "payment_method");
    char *external_reference = request_get_post_param(req, "external_reference");
    char *notes = request_get_post_param(req, "notes");
    int account_id = account_id_str ? atoi(account_id_str) : 0;
    int duration_days = duration_str ? atoi(duration_str) : 0;
    int amount_cents = amount_cents_str ? atoi(amount_cents_str) : 0;

    if (!resp) {
        goto cleanup;
    }

    if (account_id <= 0 || !plan_str || duration_days <= 0 || amount_cents < 0) {
        response_set_status(resp, HTTP_400_BAD_REQUEST);
        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, "<h1>Bad Request</h1><p>Missing or invalid billing fields.</p>");
        goto cleanup;
    }

    if (billing_mark_as_paid(account_id, subscription_string_to_plan(plan_str), duration_days,
                             amount_cents, payment_method ? payment_method : "manual",
                             external_reference ? external_reference : "",
                             req->user_id, notes ? notes : "") != 0) {
        response_set_status(resp, HTTP_500_INTERNAL_SERVER_ERROR);
        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, "<h1>Error</h1><p>Failed to update the subscription.</p>");
        goto cleanup;
    }

    {
        char redirect_url[128];
        snprintf(redirect_url, sizeof(redirect_url), "/admin/billing?account_id=%d", account_id);
        response_redirect(resp, redirect_url, 0);
    }

cleanup:
    free(account_id_str);
    free(plan_str);
    free(duration_str);
    free(amount_cents_str);
    free(payment_method);
    free(external_reference);
    free(notes);
    return resp;
}

/* Route handler: Search accounts (admin) */
http_response_t *handle_admin_search_accounts(http_request_t *req) {
    char *search_term = request_get_post_param(req, "q");
    http_response_t *resp = admin_render_page(req, search_term ? search_term : "", 0);
    free(search_term);
    return resp;
}
