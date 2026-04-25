#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <stddef.h>

#define MAX_TEMPLATE_VARS 64

/* Template variable */
typedef struct {
    const char *key;
    const char *value;
} template_var_t;

/* Template context */
typedef struct {
    template_var_t vars[MAX_TEMPLATE_VARS];
    int var_count;
} template_ctx_t;

/* Create new template context */
template_ctx_t *template_ctx_new(void);

/* Set template variable */
void template_set(template_ctx_t *ctx, const char *key, const char *value);

/* Set template variable (integer) */
void template_set_int(template_ctx_t *ctx, const char *key, int value);

/* Load template file */
char *template_load(const char *template_name);

/* Render template with context */
/* Returns rendered HTML - caller must free */
char *template_render(const char *template_content, template_ctx_t *ctx);

/* Render template file by name */
/* Returns rendered HTML - caller must free */
char *template_render_file(const char *template_name, template_ctx_t *ctx);

/* Render raw content inside the shared layout template */
/* Returns rendered HTML - caller must free */
char *template_render_layout(const char *page_title, const char *content_html,
                            int is_authenticated, const char *user_email,
                            int is_admin);

/* Render template file and wrap it in the shared layout template */
/* Returns rendered HTML - caller must free */
char *template_render_page(const char *page_title, const char *template_name,
                          template_ctx_t *ctx, int is_authenticated,
                          const char *user_email, int is_admin);

/* Free template context */
void template_ctx_free(template_ctx_t *ctx);

#endif /* TEMPLATE_H */
