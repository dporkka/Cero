/*
 * Template rendering engine
 * Simple Mustache-style template rendering with {{variable}} syntax
 */

#include "template.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *template_strdup(const char *value) {
    const char *safe_value = value ? value : "";
    size_t length = strlen(safe_value) + 1;
    char *copy = malloc(length);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, safe_value, length);
    return copy;
}

static char *template_build_navigation(int is_authenticated,
                                       const char *user_email,
                                       int is_admin) {
    char *escaped_email = html_escape(user_email ? user_email : "");
    if (!escaped_email) {
        escaped_email = template_strdup("");
    }

    char buffer[2048];
    if (is_authenticated) {
        snprintf(buffer, sizeof(buffer),
                 "<li><a href=\"/dashboard\">Dashboard</a></li>"
                 "<li><a href=\"/reports\">Reports</a></li>"
                 "<li><a href=\"/billing\">Billing</a></li>"
                 "%s"
                 "<li><a href=\"/logout\">Logout (%s)</a></li>",
                 is_admin ? "<li><a href=\"/admin/billing\">Admin</a></li>" : "",
                 escaped_email ? escaped_email : "");
    } else {
        snprintf(buffer, sizeof(buffer),
                 "<li><a href=\"/login\">Login</a></li>");
    }

    free(escaped_email);
    return template_strdup(buffer);
}

/* Create new template context */
template_ctx_t *template_ctx_new(void) {
    template_ctx_t *ctx = calloc(1, sizeof(template_ctx_t));
    if (!ctx) {
        LOG_ERROR("template", "Failed to allocate template context");
    }
    return ctx;
}

/* Set template variable */
void template_set(template_ctx_t *ctx, const char *key, const char *value) {
    if (ctx->var_count >= MAX_TEMPLATE_VARS) {
        LOG_WARN("template", "Too many template variables");
        return;
    }

    ctx->vars[ctx->var_count].key = key;
    ctx->vars[ctx->var_count].value = value;
    ctx->var_count++;
}

/* Set template variable (integer) */
void template_set_int(template_ctx_t *ctx, const char *key, int value) {
    /* Convert integer to string and store */
    static char int_buffers[MAX_TEMPLATE_VARS][32];
    static int buffer_index = 0;

    if (ctx->var_count >= MAX_TEMPLATE_VARS) {
        LOG_WARN("template", "Too many template variables");
        return;
    }

    /* Use circular buffer for integer string storage */
    snprintf(int_buffers[buffer_index], 32, "%d", value);
    ctx->vars[ctx->var_count].key = key;
    ctx->vars[ctx->var_count].value = int_buffers[buffer_index];
    ctx->var_count++;

    buffer_index = (buffer_index + 1) % MAX_TEMPLATE_VARS;
}

/* Load template file */
char *template_load(const char *template_name) {
    char path[512];
    snprintf(path, sizeof(path), "templates/%s", template_name);

    FILE *file = fopen(path, "r");
    if (!file) {
        LOG_ERROR("template", "Failed to open template file: %s", path);
        return NULL;
    }

    /* Get file size */
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    /* Read file content */
    char *content = malloc(size + 1);
    if (!content) {
        LOG_ERROR("template", "Failed to allocate memory for template");
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(content, 1, size, file);
    content[bytes_read] = '\0';

    fclose(file);

    LOG_DEBUG("template", "Loaded template: %s (%ld bytes)", template_name, size);
    return content;
}

/* Render template with context */
char *template_render(const char *template_content, template_ctx_t *ctx) {
    /* Estimate result size (2x template size should be enough) */
    size_t template_len = strlen(template_content);
    size_t result_size = template_len * 2 + 1024;
    char *result = malloc(result_size);
    if (!result) {
        LOG_ERROR("template", "Failed to allocate render buffer");
        return NULL;
    }

    const char *p = template_content;
    char *out = result;
    size_t out_len = 0;

    while (*p) {
        /* Check for variable marker */
        if (*p == '{' && *(p + 1) == '{') {
            /* Find closing }} */
            const char *end = strstr(p + 2, "}}");
            if (!end) {
                /* No closing marker, treat as literal */
                *out++ = *p++;
                out_len++;
                continue;
            }

            /* Extract variable name */
            size_t key_len = end - (p + 2);
            char key[256];
            if (key_len >= sizeof(key)) {
                LOG_WARN("template", "Variable name too long");
                p = end + 2;
                continue;
            }

            strncpy(key, p + 2, key_len);
            key[key_len] = '\0';

            /* Trim whitespace from key */
            char *key_start = key;
            while (*key_start && isspace((unsigned char)*key_start)) {
                key_start++;
            }

            char *key_end = key_start + strlen(key_start) - 1;
            while (key_end > key_start && isspace((unsigned char)*key_end)) {
                *key_end = '\0';
                key_end--;
            }

            /* Find value in context */
            const char *value = NULL;
            for (int i = 0; i < ctx->var_count; i++) {
                if (strcmp(ctx->vars[i].key, key_start) == 0) {
                    value = ctx->vars[i].value;
                    break;
                }
            }

            /* Insert value (or empty string if not found) */
            if (value) {
                size_t value_len = strlen(value);

                /* Check if we need to expand buffer */
                if (out_len + value_len + 1 > result_size) {
                    result_size = (result_size + value_len) * 2;
                    char *new_result = realloc(result, result_size);
                    if (!new_result) {
                        LOG_ERROR("template", "Failed to expand render buffer");
                        free(result);
                        return NULL;
                    }
                    result = new_result;
                    out = result + out_len;
                }

                strcpy(out, value);
                out += value_len;
                out_len += value_len;
            }

            p = end + 2;
        } else {
            /* Regular character */
            *out++ = *p++;
            out_len++;

            /* Check if we need to expand buffer */
            if (out_len + 1 > result_size) {
                result_size *= 2;
                char *new_result = realloc(result, result_size);
                if (!new_result) {
                    LOG_ERROR("template", "Failed to expand render buffer");
                    free(result);
                    return NULL;
                }
                result = new_result;
                out = result + out_len;
            }
        }
    }

    *out = '\0';

    LOG_DEBUG("template", "Rendered template: %zu bytes -> %zu bytes",
              template_len, out_len);

    return result;
}

/* Render template file by name */
char *template_render_file(const char *template_name, template_ctx_t *ctx) {
    char *content = template_load(template_name);
    if (!content) return NULL;

    char *result = template_render(content, ctx);
    free(content);

    return result;
}

char *template_render_layout(const char *page_title, const char *content_html,
                            int is_authenticated, const char *user_email,
                            int is_admin) {
    template_ctx_t *layout_ctx = template_ctx_new();
    char *navigation_links;
    char *rendered;

    if (!layout_ctx) {
        return NULL;
    }

    navigation_links = template_build_navigation(is_authenticated, user_email, is_admin);
    if (!navigation_links) {
        template_ctx_free(layout_ctx);
        return NULL;
    }

    template_set(layout_ctx, "page_title", page_title ? page_title : "");
    template_set(layout_ctx, "content", content_html ? content_html : "");
    template_set(layout_ctx, "navigation_links", navigation_links);

    rendered = template_render_file("layout.html", layout_ctx);

    free(navigation_links);
    template_ctx_free(layout_ctx);
    return rendered;
}

char *template_render_page(const char *page_title, const char *template_name,
                          template_ctx_t *ctx, int is_authenticated,
                          const char *user_email, int is_admin) {
    char *content;
    char *page;

    content = template_render_file(template_name, ctx);
    if (!content) {
        return NULL;
    }

    page = template_render_layout(page_title, content, is_authenticated,
                                  user_email, is_admin);
    free(content);
    return page;
}

/* Free template context */
void template_ctx_free(template_ctx_t *ctx) {
    free(ctx);
}
