/*
 * Reports generation
 * Generates usage and activity reports
 */

#define _GNU_SOURCE
#include "reports.h"
#include "csv.h"
#include "../billing/entitlement.h"
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

    format_timestamp_iso8601(timestamp, iso, sizeof(iso));
    iso[10] = '\0';
    safe_strncpy(buffer, iso, buffer_size);
}

static int report_count_value(const char *sql, int account_id,
                              time_t start_date, time_t end_date) {
    sqlite3_stmt *stmt;
    int value = 0;

    if (db_prepare(sql, &stmt) != 0) {
        return -1;
    }

    db_bind_int(stmt, 1, account_id);
    db_bind_int64(stmt, 2, start_date);
    db_bind_int64(stmt, 3, end_date);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = db_column_int(stmt, 0);
    }

    db_finalize(stmt);
    return value;
}

static time_t report_next_bucket_end(time_t start, report_grouping_t grouping, time_t overall_end) {
    struct tm tm_info;
    time_t bucket_end;

    if (grouping == GROUP_NONE) {
        return overall_end;
    }

    if (grouping == GROUP_BY_DAY) {
        bucket_end = end_of_day(start);
    } else if (grouping == GROUP_BY_WEEK) {
        bucket_end = end_of_day(add_days(start, 6));
    } else {
        tm_info = *gmtime(&start);
        tm_info.tm_mday = 1;
        tm_info.tm_hour = 0;
        tm_info.tm_min = 0;
        tm_info.tm_sec = 0;
        tm_info.tm_mon += 1;
        bucket_end = timegm(&tm_info) - 1;
    }

    if (bucket_end > overall_end) {
        bucket_end = overall_end;
    }

    return bucket_end;
}

static int report_parse_grouping(const char *grouping_str, report_grouping_t *grouping) {
    if (!grouping_str || strcmp(grouping_str, "none") == 0 || grouping_str[0] == '\0') {
        *grouping = GROUP_NONE;
        return 0;
    }
    if (strcmp(grouping_str, "day") == 0) {
        *grouping = GROUP_BY_DAY;
        return 0;
    }
    if (strcmp(grouping_str, "week") == 0) {
        *grouping = GROUP_BY_WEEK;
        return 0;
    }
    if (strcmp(grouping_str, "month") == 0) {
        *grouping = GROUP_BY_MONTH;
        return 0;
    }
    return -1;
}

static int report_load_params_from_values(const char *start_date_str,
                                          const char *end_date_str,
                                          const char *grouping_str,
                                          int export_csv,
                                          report_params_t *params) {
    if (!start_date_str || !end_date_str) {
        return -1;
    }

    params->start_date = start_of_day(parse_iso8601(start_date_str));
    params->end_date = end_of_day(parse_iso8601(end_date_str));
    params->export_csv = export_csv;

    if (params->start_date == 0 || params->end_date == 0) {
        return -1;
    }

    if (report_parse_grouping(grouping_str, &params->grouping) != 0) {
        return -1;
    }

    return 0;
}

static char *report_build_results_html(const report_params_t *params,
                                       report_row_t *rows, int row_count) {
    char *html = NULL;
    size_t length = 0;
    size_t capacity = 0;
    owned_strings_t owned = {{0}, 0};
    char start_date[16];
    char end_date[16];

    format_date_only(params->start_date, start_date, sizeof(start_date));
    format_date_only(params->end_date, end_date, sizeof(end_date));

    append_format(&html, &length, &capacity,
                  "<h3 style=\"margin-top: 2rem;\">Report Results</h3>"
                  "<p style=\"color: #7f8c8d;\">Date Range: %s to %s%s%s</p>"
                  "<table style=\"margin-top: 1rem;\"><thead><tr><th>Date</th><th>Active Users</th>"
                  "<th>Sessions</th><th>Accounts</th></tr></thead><tbody>",
                  start_date, end_date,
                  params->grouping != GROUP_NONE ? " | Grouped by: " : "",
                  params->grouping == GROUP_BY_DAY ? "day" :
                  params->grouping == GROUP_BY_WEEK ? "week" :
                  params->grouping == GROUP_BY_MONTH ? "month" : "");

    if (row_count == 0) {
        append_format(&html, &length, &capacity,
                      "<tr><td colspan=\"4\" style=\"text-align: center; color: #7f8c8d;\">"
                      "No data available for this date range</td></tr>");
    }

    for (int i = 0; i < row_count; i++) {
        append_format(&html, &length, &capacity,
                      "<tr><td>%s</td><td>%d</td><td>%d</td><td>%d</td></tr>",
                      track_escape(&owned, rows[i].date),
                      rows[i].user_count, rows[i].session_count, rows[i].account_count);
    }

    append_format(&html, &length, &capacity,
                  "</tbody></table>"
                  "<p style=\"margin-top: 1rem; color: #7f8c8d;\">"
                  "<em>Report generated on-demand. No caching or background processing.</em></p>");

    free_owned(&owned);
    return html;
}

static char *report_render_page(http_request_t *req,
                                const char *start_date,
                                const char *end_date,
                                const char *grouping,
                                const char *results_html) {
    http_response_t *unused = NULL;
    template_ctx_t *ctx = template_ctx_new();
    char *page;
    char *plan_notice_html = NULL;
    char *grouping_controls_html = NULL;
    char *export_controls_html = NULL;

    (void)unused;

    if (!ctx) {
        return NULL;
    }

    if (entitlement_get_max_report_days(req->account_id) > 0) {
        plan_notice_html = strdup(
            "<div class=\"alert alert-info\">"
            "You are on the Free plan. Reports are limited to 7-day date ranges with no CSV export. "
            "<a href=\"/billing\" style=\"color: white; text-decoration: underline;\">Upgrade to Pro</a> "
            "for unlimited date ranges, grouping, and CSV export.</div>");
    } else {
        plan_notice_html = strdup("");
    }

    if (entitlement_can_use_grouping(req->account_id)) {
        char grouping_controls[512];
        snprintf(grouping_controls, sizeof(grouping_controls),
                 "<div class=\"form-group\"><label for=\"grouping\">Group By</label>"
                 "<select id=\"grouping\" name=\"grouping\">"
                 "<option value=\"none\" %s>No Grouping</option>"
                 "<option value=\"day\" %s>Daily</option>"
                 "<option value=\"week\" %s>Weekly</option>"
                 "<option value=\"month\" %s>Monthly</option>"
                 "</select></div>",
                 strcmp(grouping ? grouping : "none", "none") == 0 ? "selected" : "",
                 strcmp(grouping ? grouping : "", "day") == 0 ? "selected" : "",
                 strcmp(grouping ? grouping : "", "week") == 0 ? "selected" : "",
                 strcmp(grouping ? grouping : "", "month") == 0 ? "selected" : "");
        grouping_controls_html = strdup(grouping_controls);
    } else {
        grouping_controls_html = strdup("");
    }

    if (entitlement_can_export_csv(req->account_id)) {
        export_controls_html = strdup(
            "<button type=\"submit\" name=\"export_csv\" value=\"1\" class=\"button button-secondary\">"
            "Export as CSV</button>");
    } else {
        export_controls_html = strdup("");
    }

    template_set(ctx, "plan_notice_html", plan_notice_html ? plan_notice_html : "");
    template_set(ctx, "grouping_controls_html", grouping_controls_html ? grouping_controls_html : "");
    template_set(ctx, "export_controls_html", export_controls_html ? export_controls_html : "");
    template_set(ctx, "start_date", start_date ? start_date : "");
    template_set(ctx, "end_date", end_date ? end_date : "");
    template_set(ctx, "report_results_html", results_html ? results_html : "");

    page = template_render_page("Reports", "reports.html", ctx,
                                req->is_authenticated, req->user_email,
                                strcmp(req->user_role, "admin") == 0);

    free(plan_notice_html);
    free(grouping_controls_html);
    free(export_controls_html);
    template_ctx_free(ctx);
    return page;
}

static http_response_t *report_csv_response(int account_id, const report_params_t *params) {
    http_response_t *resp = response_new();
    report_row_t *rows = NULL;
    int row_count = 0;
    csv_writer_t *writer = NULL;
    char *csv_copy = NULL;
    const char *columns[] = {"Date", "Active Users", "Sessions", "Accounts"};

    if (!resp) {
        return NULL;
    }

    if (report_generate(account_id, params, &rows, &row_count) != 0) {
        response_set_status(resp, HTTP_500_INTERNAL_SERVER_ERROR);
        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, "<h1>Error</h1><p>Failed to generate report.</p>");
        return resp;
    }

    writer = csv_writer_new();
    if (!writer) {
        report_free_rows(rows, row_count);
        response_set_status(resp, HTTP_500_INTERNAL_SERVER_ERROR);
        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, "<h1>Error</h1><p>Failed to generate CSV.</p>");
        return resp;
    }

    csv_add_header(writer, columns, 4);
    for (int i = 0; i < row_count; i++) {
        char user_count[32];
        char session_count[32];
        char account_count[32];
        const char *values[4];

        snprintf(user_count, sizeof(user_count), "%d", rows[i].user_count);
        snprintf(session_count, sizeof(session_count), "%d", rows[i].session_count);
        snprintf(account_count, sizeof(account_count), "%d", rows[i].account_count);
        values[0] = rows[i].date;
        values[1] = user_count;
        values[2] = session_count;
        values[3] = account_count;
        csv_add_row(writer, values, 4);
    }

    csv_copy = strdup(csv_get_content(writer));
    response_set_content_type(resp, "text/csv; charset=utf-8");
    response_add_header(resp, "Content-Disposition", "attachment; filename=\"report.csv\"");
    response_set_body(resp, csv_copy ? csv_copy : "");

    free(csv_copy);
    csv_writer_free(writer);
    report_free_rows(rows, row_count);
    return resp;
}

/* Generate report data */
int report_generate(int account_id, const report_params_t *params,
                   report_row_t **rows, int *row_count) {
    const char *user_sql = "SELECT COUNT(*) FROM users WHERE account_id = ? AND created_at BETWEEN ? AND ?";
    const char *session_sql = "SELECT COUNT(*) FROM sessions s "
                              "JOIN users u ON u.id = s.user_id "
                              "WHERE u.account_id = ? AND s.created_at BETWEEN ? AND ?";
    const char *account_sql = "SELECT COUNT(*) FROM accounts WHERE id = ? AND created_at <= ?";
    report_row_t *result = NULL;
    int count = 0;
    int capacity = 0;
    time_t cursor;
    time_t end;

    if (!rows || !row_count || !params || params->end_date < params->start_date) {
        return -1;
    }

    cursor = start_of_day(params->start_date);
    end = end_of_day(params->end_date);

    while (cursor <= end) {
        report_row_t row;
        time_t bucket_end = report_next_bucket_end(cursor, params->grouping, end);
        int account_count_value;

        if (count == capacity) {
            int new_capacity = capacity == 0 ? 8 : capacity * 2;
            report_row_t *new_rows = realloc(result, sizeof(report_row_t) * new_capacity);
            if (!new_rows) {
                free(result);
                return -1;
            }
            result = new_rows;
            capacity = new_capacity;
        }

        memset(&row, 0, sizeof(row));
        if (params->grouping == GROUP_NONE || cursor == bucket_end) {
            format_date_only(cursor, row.date, sizeof(row.date));
        } else {
            char start_label[16];
            char end_label[16];
            format_date_only(cursor, start_label, sizeof(start_label));
            format_date_only(bucket_end, end_label, sizeof(end_label));
            snprintf(row.date, sizeof(row.date), "%s..%s", start_label, end_label);
        }

        row.user_count = report_count_value(user_sql, account_id, cursor, bucket_end);
        row.session_count = report_count_value(session_sql, account_id, cursor, bucket_end);

        account_count_value = 0;
        {
            sqlite3_stmt *stmt;
            if (db_prepare(account_sql, &stmt) == 0) {
                db_bind_int(stmt, 1, account_id);
                db_bind_int64(stmt, 2, bucket_end);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    account_count_value = db_column_int(stmt, 0);
                }
                db_finalize(stmt);
            }
        }
        row.account_count = account_count_value;

        result[count++] = row;

        if (params->grouping == GROUP_NONE) {
            break;
        }
        cursor = bucket_end + 1;
    }

    *rows = result;
    *row_count = count;
    return 0;
}

/* Validate report parameters against entitlements */
int report_validate_params(int account_id, const report_params_t *params,
                          char *error_message, size_t error_size) {
    int max_days;
    int day_count;

    if (!params || params->end_date < params->start_date) {
        snprintf(error_message, error_size, "End date must be on or after start date.");
        return -1;
    }

    day_count = (int)(((params->end_date - params->start_date) / 86400) + 1);
    max_days = entitlement_get_max_report_days(account_id);
    if (max_days > 0 && day_count > max_days) {
        snprintf(error_message, error_size,
                 "Date range exceeds the %d-day limit for your plan.", max_days);
        return -1;
    }

    if (params->export_csv && !entitlement_can_export_csv(account_id)) {
        snprintf(error_message, error_size, "CSV export is not available on your plan.");
        return -1;
    }

    if (params->grouping != GROUP_NONE && !entitlement_can_use_grouping(account_id)) {
        snprintf(error_message, error_size, "Report grouping is not available on your plan.");
        return -1;
    }

    return 0;
}

/* Route handler: Reports page */
http_response_t *handle_reports_page(http_request_t *req) {
    http_response_t *resp = response_new();
    char start_date[16];
    char end_date[16];
    char *page;
    time_t now = time(NULL);

    if (!resp) {
        return NULL;
    }

    format_date_only(add_days(now, -6), start_date, sizeof(start_date));
    format_date_only(now, end_date, sizeof(end_date));

    page = report_render_page(req, start_date, end_date, "none", "");
    response_set_content_type(resp, "text/html; charset=utf-8");
    response_set_body(resp, page ? page : "<h1>Error</h1>");
    free(page);
    return resp;
}

/* Route handler: Generate report */
http_response_t *handle_reports_generate(http_request_t *req) {
    http_response_t *resp = response_new();
    char *start_date_str = request_get_post_param(req, "start_date");
    char *end_date_str = request_get_post_param(req, "end_date");
    char *grouping_str = request_get_post_param(req, "grouping");
    char *export_csv_str = request_get_post_param(req, "export_csv");
    report_params_t params;
    char validation_error[256];

    if (!resp) {
        goto cleanup;
    }

    if (report_load_params_from_values(start_date_str, end_date_str, grouping_str,
                                       export_csv_str && strcmp(export_csv_str, "1") == 0,
                                       &params) != 0) {
        response_set_status(resp, HTTP_400_BAD_REQUEST);
        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, "<h1>Bad Request</h1><p>Invalid report parameters.</p>");
        goto cleanup;
    }

    if (report_validate_params(req->account_id, &params,
                               validation_error, sizeof(validation_error)) != 0) {
        char *escaped_error = html_escape(validation_error);
        char body[512];
        snprintf(body, sizeof(body),
                 "<h1>Access Denied</h1><p>%s</p><p><a href=\"/reports\">Back to Reports</a></p>",
                 escaped_error ? escaped_error : validation_error);
        free(escaped_error);
        response_set_status(resp, HTTP_403_FORBIDDEN);
        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, body);
        goto cleanup;
    }

    if (params.export_csv) {
        response_free(resp);
        resp = report_csv_response(req->account_id, &params);
        goto cleanup;
    }

    {
        report_row_t *rows = NULL;
        int row_count = 0;
        char *results_html = NULL;
        char *page = NULL;
        const char *grouping_value = grouping_str && grouping_str[0] ? grouping_str : "none";

        if (report_generate(req->account_id, &params, &rows, &row_count) != 0) {
            response_set_status(resp, HTTP_500_INTERNAL_SERVER_ERROR);
            response_set_content_type(resp, "text/html; charset=utf-8");
            response_set_body(resp, "<h1>Error</h1><p>Failed to generate report.</p>");
            goto cleanup;
        }

        results_html = report_build_results_html(&params, rows, row_count);
        page = report_render_page(req, start_date_str, end_date_str, grouping_value, results_html);

        response_set_content_type(resp, "text/html; charset=utf-8");
        response_set_body(resp, page ? page : "<h1>Error</h1>");

        free(page);
        free(results_html);
        report_free_rows(rows, row_count);
    }

cleanup:
    free(start_date_str);
    free(end_date_str);
    free(grouping_str);
    free(export_csv_str);
    return resp;
}

/* Route handler: Export report as CSV */
http_response_t *handle_reports_export_csv(http_request_t *req) {
    char *start_date_str = request_get_query_param(req, "start_date");
    char *end_date_str = request_get_query_param(req, "end_date");
    char *grouping_str = request_get_query_param(req, "grouping");
    report_params_t params;
    http_response_t *resp;
    char validation_error[256];

    if (report_load_params_from_values(start_date_str, end_date_str, grouping_str, 1, &params) != 0) {
        resp = response_new();
        if (resp) {
            response_set_status(resp, HTTP_400_BAD_REQUEST);
            response_set_content_type(resp, "text/html; charset=utf-8");
            response_set_body(resp, "<h1>Bad Request</h1><p>Invalid report parameters.</p>");
        }
        free(start_date_str);
        free(end_date_str);
        free(grouping_str);
        return resp;
    }

    if (report_validate_params(req->account_id, &params, validation_error, sizeof(validation_error)) != 0) {
        resp = response_new();
        if (resp) {
            response_set_status(resp, HTTP_403_FORBIDDEN);
            response_set_content_type(resp, "text/html; charset=utf-8");
            response_set_body(resp, "<h1>Access Denied</h1><p>CSV export not available.</p>");
        }
        free(start_date_str);
        free(end_date_str);
        free(grouping_str);
        return resp;
    }

    resp = report_csv_response(req->account_id, &params);
    free(start_date_str);
    free(end_date_str);
    free(grouping_str);
    return resp;
}

/* Free report rows */
void report_free_rows(report_row_t *rows, int row_count) {
    (void)row_count;
    free(rows);
}
