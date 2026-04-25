#ifndef REPORTS_H
#define REPORTS_H

#include "../core/request.h"
#include "../core/response.h"
#include <time.h>

/* Report grouping options */
typedef enum {
    GROUP_NONE,
    GROUP_BY_DAY,
    GROUP_BY_WEEK,
    GROUP_BY_MONTH
} report_grouping_t;

/* Report parameters */
typedef struct {
    time_t start_date;
    time_t end_date;
    report_grouping_t grouping;
    int export_csv;
} report_params_t;

/* Report row structure (example - customize based on actual data) */
typedef struct {
    char date[48]; /* fits range labels like 2024-01-01..2024-01-31 */
    int user_count;
    int session_count;
    int account_count;
} report_row_t;

/* Generate report data */
int report_generate(int account_id, const report_params_t *params,
                   report_row_t **rows, int *row_count);

/* Validate report parameters against entitlements */
int report_validate_params(int account_id, const report_params_t *params,
                          char *error_message, size_t error_size);

/* Route handlers */
http_response_t *handle_reports_page(http_request_t *req);
http_response_t *handle_reports_generate(http_request_t *req);
http_response_t *handle_reports_export_csv(http_request_t *req);

/* Free report rows */
void report_free_rows(report_row_t *rows, int row_count);

#endif /* REPORTS_H */
