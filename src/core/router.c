/*
 * HTTP routing system
 * Routes HTTP requests to appropriate handlers
 */

#include "router.h"
#include "../auth/auth.h"
#include "../billing/admin.h"
#include "../billing/subscription.h"
#include "../reports/reports.h"
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

#define MAX_ROUTES 100

typedef struct {
    char *values[32];
    int count;
} owned_strings_t;

typedef struct {
    char account_name[256];
    char account_status[32];
    char subscription_plan[32];
    char subscription_status[32];
    time_t valid_from;
    time_t valid_until;
    time_t grace_until;
} account_snapshot_t;

static route_t routes[MAX_ROUTES];
static int route_count = 0;

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

static void format_date_only(time_t timestamp, char *buffer, size_t buffer_size) {
    char iso[32];

    if (timestamp <= 0) {
        safe_strncpy(buffer, "N/A", buffer_size);
        return;
    }

    format_timestamp_iso8601(timestamp, iso, sizeof(iso));
    iso[10] = '\0';
    safe_strncpy(buffer, iso, buffer_size);
}

static int load_account_snapshot(int account_id, account_snapshot_t *snapshot) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT a.name, a.status, COALESCE(s.plan, 'free'), "
                      "COALESCE(s.status, 'active'), COALESCE(s.valid_from, 0), "
                      "COALESCE(s.valid_until, 0), COALESCE(s.grace_until, 0) "
                      "FROM accounts a LEFT JOIN subscriptions s ON s.account_id = a.id "
                      "WHERE a.id = ?";

    if (!snapshot) {
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

    memset(snapshot, 0, sizeof(*snapshot));
    safe_strncpy(snapshot->account_name, db_column_text(stmt, 0), sizeof(snapshot->account_name));
    safe_strncpy(snapshot->account_status, db_column_text(stmt, 1), sizeof(snapshot->account_status));
    safe_strncpy(snapshot->subscription_plan, db_column_text(stmt, 2), sizeof(snapshot->subscription_plan));
    safe_strncpy(snapshot->subscription_status, db_column_text(stmt, 3), sizeof(snapshot->subscription_status));
    snapshot->valid_from = (time_t)db_column_int64(stmt, 4);
    snapshot->valid_until = (time_t)db_column_int64(stmt, 5);
    snapshot->grace_until = (time_t)db_column_int64(stmt, 6);

    db_finalize(stmt);
    return 0;
}

static http_response_t *render_error_response(http_request_t *req,
                                              int status_code,
                                              const char *message) {
    http_response_t *resp = response_new();
    template_ctx_t *ctx = template_ctx_new();
    char *page;
    owned_strings_t owned = {{0}, 0};
    char status_code_str[16];
    char debug_html[1] = "";

    if (!resp || !ctx) {
        response_free(resp);
        template_ctx_free(ctx);
        return NULL;
    }

    snprintf(status_code_str, sizeof(status_code_str), "%d", status_code);
    template_set(ctx, "error_code", status_code_str);
    template_set(ctx, "error_message", track_escape(&owned, message));
    template_set(ctx, "debug_html", debug_html);

    page = template_render_page("Error", "error.html", ctx,
                                req ? req->is_authenticated : 0,
                                req ? req->user_email : "",
                                req && strcmp(req->user_role, "admin") == 0);

    response_set_status(resp, status_code);
    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, page ? page : "<h1>Error</h1>");

    free(page);
    free_owned(&owned);
    template_ctx_free(ctx);
    return resp;
}

static char *build_billing_rows(int account_id) {
    billing_event_t *events = NULL;
    int event_count = 0;
    int result;
    char *html = NULL;
    size_t length = 0;
    size_t capacity = 0;
    owned_strings_t owned = {{0}, 0};

    result = billing_get_events_for_account(account_id, &events, &event_count);
    if (result != 0) {
        return strdup(
            "<tr><td colspan=\"5\" style=\"text-align: center; color: #c0392b;\">"
            "Billing history is temporarily unavailable</td></tr>");
    }

    if (event_count == 0) {
        return strdup("<tr><td colspan=\"5\" style=\"text-align: center; color: #7f8c8d;\">No billing events yet</td></tr>");
    }

    for (int i = 0; i < event_count; i++) {
        char occurred_at[32];
        char amount[32];
        const char *plan = events[i].new_plan[0] ? events[i].new_plan :
                           (events[i].previous_plan[0] ? events[i].previous_plan : "-");

        format_date_only(events[i].occurred_at, occurred_at, sizeof(occurred_at));
        if (events[i].amount_cents > 0) {
            snprintf(amount, sizeof(amount), "$%.2f", events[i].amount_cents / 100.0);
        } else {
            safe_strncpy(amount, "-", sizeof(amount));
        }

        append_format(&html, &length, &capacity,
                      "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
                      occurred_at,
                      track_escape(&owned, events[i].event_type),
                      track_escape(&owned, plan),
                      amount,
                      track_escape(&owned, events[i].external_reference[0] ? events[i].external_reference : "-"));
    }

    free(events);
    free_owned(&owned);
    return html ? html : strdup("");
}

/* Forward declarations for route handlers */
http_response_t *handle_home_page(http_request_t *req);
http_response_t *handle_dashboard(http_request_t *req);
http_response_t *handle_billing_page(http_request_t *req);

/* Initialize routing system */
void router_init(void) {
    route_count = 0;
    LOG_INFO("router", "Router initialized");
}

/* Register a route */
void router_add_route(http_method_t method, const char *path, route_handler_t handler,
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

/* Find and execute route handler */
http_response_t *router_handle_request(http_request_t *req) {
    for (int i = 0; i < route_count; i++) {
        if (routes[i].method == req->method && strcmp(routes[i].path, req->path) == 0) {
            if (routes[i].requires_auth && !req->is_authenticated) {
                http_response_t *resp = response_new();
                if (!resp) {
                    return NULL;
                }
                response_redirect(resp, "/login", 0);
                return resp;
            }

            if (routes[i].requires_admin && strcmp(req->user_role, "admin") != 0) {
                return render_error_response(req, HTTP_403_FORBIDDEN,
                                             "Admin access is required for this page.");
            }

            return routes[i].handler(req);
        }
    }

    return render_error_response(req, HTTP_404_NOT_FOUND,
                                 "The requested page does not exist.");
}

http_response_t *handle_home_page(http_request_t *req) {
    http_response_t *resp = response_new();
    char *page;
    owned_strings_t owned = {{0}, 0};
    char content[2048];

    if (!resp) {
        return NULL;
    }

    if (req->is_authenticated) {
        snprintf(content, sizeof(content),
                 "<h2>Welcome back</h2>"
                 "<div class=\"alert alert-info\">Logged in as %s.</div>"
                 "<p>This system keeps authentication, billing, and reporting in one simple binary.</p>"
                 "<p style=\"margin-top: 1rem;\"><a class=\"button\" href=\"/dashboard\">Open Dashboard</a></p>",
                 track_escape(&owned, req->user_email));
    } else {
        snprintf(content, sizeof(content),
                 "<h2>Welcome to Cero</h2>"
                 "<p>This is a durable, boring, self-contained SaaS platform designed for longevity.</p>"
                 "<p style=\"margin-top: 1rem;\"><a class=\"button\" href=\"/login\">Login</a></p>");
    }

    page = template_render_layout("Home", content, req->is_authenticated,
                                  req->user_email, strcmp(req->user_role, "admin") == 0);
    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, page ? page : "<h1>Home</h1>");

    free(page);
    free_owned(&owned);
    return resp;
}

http_response_t *handle_dashboard(http_request_t *req) {
    http_response_t *resp = response_new();
    template_ctx_t *ctx = template_ctx_new();
    account_snapshot_t snapshot;
    char valid_until[32];
    char *page;
    char *grace_period_html = NULL;
    char *upgrade_html = NULL;
    owned_strings_t owned = {{0}, 0};

    if (!resp || !ctx) {
        response_free(resp);
        template_ctx_free(ctx);
        return NULL;
    }

    if (load_account_snapshot(req->account_id, &snapshot) != 0) {
        response_free(resp);
        template_ctx_free(ctx);
        return render_error_response(req, HTTP_500_INTERNAL_SERVER_ERROR,
                                     "Failed to load dashboard data.");
    }

    format_date_only(snapshot.valid_until, valid_until, sizeof(valid_until));

    if (strcmp(snapshot.subscription_status, "grace_period") == 0) {
        grace_period_html = strdup(
            "<div class=\"alert alert-error\">Your subscription is in a grace period. "
            "Please contact billing to renew.</div>");
    } else {
        grace_period_html = strdup("");
    }

    if (strcmp(snapshot.subscription_plan, "free") == 0) {
        upgrade_html = strdup(
            "<div class=\"alert alert-info\">"
            "<p><strong>Upgrade to Pro for advanced features:</strong></p>"
            "<ul style=\"margin-left: 2rem; margin-top: 0.5rem;\">"
            "<li>Unlimited report date ranges</li><li>CSV export</li>"
            "<li>Report grouping</li><li>Priority support</li></ul>"
            "<p style=\"margin-top: 1rem;\"><a href=\"/billing\" class=\"button\">View Upgrade Options</a></p>"
            "</div>");
    } else {
        upgrade_html = strdup("");
    }

    template_set(ctx, "user_email", track_escape(&owned, req->user_email));
    template_set(ctx, "account_name", track_escape(&owned, snapshot.account_name));
    template_set(ctx, "account_status", track_escape(&owned, snapshot.account_status));
    template_set(ctx, "subscription_plan", track_escape(&owned, snapshot.subscription_plan));
    template_set(ctx, "subscription_status", track_escape(&owned, snapshot.subscription_status));
    template_set(ctx, "subscription_valid_until", valid_until);
    template_set(ctx, "grace_period_html", grace_period_html ? grace_period_html : "");
    template_set(ctx, "upgrade_html", upgrade_html ? upgrade_html : "");

    page = template_render_page("Dashboard", "dashboard.html", ctx,
                                req->is_authenticated, req->user_email,
                                strcmp(req->user_role, "admin") == 0);
    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, page ? page : "<h1>Dashboard</h1>");

    free(page);
    free(grace_period_html);
    free(upgrade_html);
    free_owned(&owned);
    template_ctx_free(ctx);
    return resp;
}

http_response_t *handle_billing_page(http_request_t *req) {
    http_response_t *resp = response_new();
    template_ctx_t *ctx = template_ctx_new();
    account_snapshot_t snapshot;
    char valid_from[32];
    char valid_until[32];
    char grace_until[32];
    char *subscription_grace_row = NULL;
    char *plan_specific_html = NULL;
    char *billing_events_rows = NULL;
    char *page;
    owned_strings_t owned = {{0}, 0};

    if (!resp || !ctx) {
        response_free(resp);
        template_ctx_free(ctx);
        return NULL;
    }

    if (load_account_snapshot(req->account_id, &snapshot) != 0) {
        response_free(resp);
        template_ctx_free(ctx);
        return render_error_response(req, HTTP_500_INTERNAL_SERVER_ERROR,
                                     "Failed to load billing data.");
    }

    format_date_only(snapshot.valid_from, valid_from, sizeof(valid_from));
    format_date_only(snapshot.valid_until, valid_until, sizeof(valid_until));
    format_date_only(snapshot.grace_until, grace_until, sizeof(grace_until));

    if (snapshot.grace_until > 0) {
        char row[256];
        snprintf(row, sizeof(row),
                 "<tr><th>Grace Period Until</th><td>%s</td></tr>", grace_until);
        subscription_grace_row = strdup(row);
    } else {
        subscription_grace_row = strdup("");
    }

    if (strcmp(snapshot.subscription_plan, "free") == 0) {
        char upgrade_block[2048];
        snprintf(upgrade_block, sizeof(upgrade_block),
                 "<div style=\"margin-top: 2rem; padding: 2rem; background: #f8f9fa; border-radius: 4px;\">"
                 "<h3>Upgrade to Pro</h3><p><strong>$49/month or $490/year</strong></p>"
                 "<ul style=\"margin-left: 2rem; margin-top: 0.5rem;\">"
                 "<li>Unlimited report date ranges</li><li>CSV export functionality</li>"
                 "<li>Report grouping</li><li>Priority support</li></ul>"
                 "<h4 style=\"margin-top: 1.5rem;\">How to Upgrade</h4>"
                 "<ol style=\"margin-left: 2rem; margin-top: 0.5rem;\">"
                 "<li>Choose your payment method</li><li>Complete payment externally</li>"
                 "<li>Email the receipt to billing@example.com</li>"
                 "<li>Admin activates your subscription manually</li></ol>"
                 "<p style=\"margin-top: 1rem;\"><a href=\"https://pay.example.com/invoice/%d\">"
                 "Payment Link for Account %d</a></p></div>",
                 req->account_id, req->account_id);
        plan_specific_html = strdup(upgrade_block);
    } else {
        plan_specific_html = strdup("<div class=\"alert alert-success\">Thank you for your paid subscription!</div>");
    }

    billing_events_rows = build_billing_rows(req->account_id);

    template_set(ctx, "subscription_plan", track_escape(&owned, snapshot.subscription_plan));
    template_set(ctx, "subscription_status", track_escape(&owned, snapshot.subscription_status));
    template_set(ctx, "subscription_valid_from", valid_from);
    template_set(ctx, "subscription_valid_until", valid_until);
    template_set(ctx, "subscription_grace_row", subscription_grace_row ? subscription_grace_row : "");
    template_set(ctx, "plan_specific_html", plan_specific_html ? plan_specific_html : "");
    template_set(ctx, "billing_events_rows", billing_events_rows ? billing_events_rows : "");

    page = template_render_page("Billing", "billing.html", ctx,
                                req->is_authenticated, req->user_email,
                                strcmp(req->user_role, "admin") == 0);
    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, page ? page : "<h1>Billing</h1>");

    free(page);
    free(subscription_grace_row);
    free(plan_specific_html);
    free(billing_events_rows);
    free_owned(&owned);
    template_ctx_free(ctx);
    return resp;
}

/* Register all application routes */
void routes_register_all(void) {
    router_add_route(HTTP_GET, "/", handle_home_page, 0, 0);
    router_add_route(HTTP_GET, "/login", handle_login_page, 0, 0);
    router_add_route(HTTP_POST, "/login", handle_login_submit, 0, 0);
    router_add_route(HTTP_GET, "/logout", handle_logout, 0, 0);

    router_add_route(HTTP_GET, "/dashboard", handle_dashboard, 1, 0);
    router_add_route(HTTP_GET, "/billing", handle_billing_page, 1, 0);
    router_add_route(HTTP_GET, "/reports", handle_reports_page, 1, 0);
    router_add_route(HTTP_POST, "/reports/generate", handle_reports_generate, 1, 0);
    router_add_route(HTTP_GET, "/reports/export", handle_reports_export_csv, 1, 0);

    router_add_route(HTTP_GET, "/admin/billing", handle_admin_billing_page, 1, 1);
    router_add_route(HTTP_POST, "/admin/billing/mark-paid", handle_admin_mark_paid, 1, 1);
    router_add_route(HTTP_POST, "/admin/search", handle_admin_search_accounts, 1, 1);
}
