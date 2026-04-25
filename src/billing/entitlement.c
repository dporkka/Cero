/*
 * Entitlement checking
 * Determines feature access based on subscription plan
 */

#include "entitlement.h"
#include "subscription.h"
#include "../utils/db.h"
#include "../utils/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Get feature name as string */
const char *entitlement_feature_name(feature_t feature) {
    switch (feature) {
        case FEATURE_BASIC_REPORTS: return "Basic Reports";
        case FEATURE_ADVANCED_REPORTS: return "Advanced Reports";
        case FEATURE_EXTENDED_DATE_RANGE: return "Extended Date Range";
        case FEATURE_CSV_EXPORT: return "CSV Export";
        case FEATURE_REPORT_GROUPING: return "Report Grouping";
        case FEATURE_API_ACCESS: return "API Access";
        case FEATURE_PRIORITY_SUPPORT: return "Priority Support";
        default: return "Unknown";
    }
}

/* Check if account has access to feature */
int entitlement_has_feature(int account_id, feature_t feature) {
    subscription_t sub;

    /* Get subscription */
    if (subscription_get_by_account(account_id, &sub) != 0) {
        LOG_WARN("entitlement", "No subscription found for account %d", account_id);
        return 0;
    }

    /* Check if subscription is valid */
    if (!subscription_is_valid(&sub)) {
        LOG_INFO("entitlement", "Invalid subscription for account %d", account_id);
        return 0;
    }

    /* Check feature entitlements by plan */
    switch (sub.plan) {
        case PLAN_FREE:
            /* Free plan: only basic reports */
            return (feature == FEATURE_BASIC_REPORTS);

        case PLAN_PRO:
            /* Pro plan: all features except priority support */
            return (feature != FEATURE_PRIORITY_SUPPORT);

        case PLAN_ENTERPRISE:
            /* Enterprise plan: all features */
            return 1;

        default:
            LOG_WARN("entitlement", "Unknown plan for account %d", account_id);
            return 0;
    }
}

/* Get maximum date range (in days) for reports */
int entitlement_get_max_report_days(int account_id) {
    subscription_t sub;

    /* Get subscription */
    if (subscription_get_by_account(account_id, &sub) != 0) {
        return 7; /* Default to 7 days */
    }

    /* Check if subscription is valid */
    if (!subscription_is_valid(&sub)) {
        return 7; /* Default to 7 days */
    }

    /* Return max days based on plan */
    switch (sub.plan) {
        case PLAN_FREE:
            return 7; /* 1 week */

        case PLAN_PRO:
            return 0; /* Unlimited */

        case PLAN_ENTERPRISE:
            return 0; /* Unlimited */

        default:
            return 7;
    }
}

/* Check if account can export CSV */
int entitlement_can_export_csv(int account_id) {
    return entitlement_has_feature(account_id, FEATURE_CSV_EXPORT);
}

/* Check if account can use report grouping */
int entitlement_can_use_grouping(int account_id) {
    return entitlement_has_feature(account_id, FEATURE_REPORT_GROUPING);
}
