/*
 * String utilities
 * Safe string operations and encoding/decoding
 */

#include "string_utils.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Safe string copy - always null terminates */
void safe_strncpy(char *dest, const char *src, size_t dest_size) {
    if (dest_size == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/* URL decode in place */
void url_decode(char *str) {
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int value;
            if (sscanf(src + 1, "%2x", &value) == 1) {
                *dst++ = (char)value;
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* URL encode - caller must free result */
char *url_encode(const char *str) {
    if (!str) return NULL;

    /* Worst case: every character needs %XX encoding (3x size) */
    size_t max_len = strlen(str) * 3 + 1;
    char *encoded = malloc(max_len);
    if (!encoded) return NULL;

    char *p = encoded;
    for (const char *s = str; *s; s++) {
        if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            *p++ = *s;
        } else if (*s == ' ') {
            *p++ = '+';
        } else {
            sprintf(p, "%%%02X", (unsigned char)*s);
            p += 3;
        }
    }
    *p = '\0';

    return encoded;
}

/* HTML entity escape - caller must free result */
char *html_escape(const char *str) {
    if (!str) return NULL;

    /* Allocate buffer (worst case: every char needs escaping) */
    size_t len = strlen(str);
    char *escaped = malloc(len * 6 + 1); /* &quot; = 6 chars */
    if (!escaped) return NULL;

    char *p = escaped;
    for (const char *s = str; *s; s++) {
        switch (*s) {
            case '<': strcpy(p, "&lt;"); p += 4; break;
            case '>': strcpy(p, "&gt;"); p += 4; break;
            case '&': strcpy(p, "&amp;"); p += 5; break;
            case '"': strcpy(p, "&quot;"); p += 6; break;
            case '\'': strcpy(p, "&#39;"); p += 5; break;
            default: *p++ = *s; break;
        }
    }
    *p = '\0';

    return escaped;
}

/* Case-insensitive string comparison */
int strcasecmp_portable(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/* Trim whitespace from both ends - modifies string in place */
void str_trim(char *str) {
    if (!str || !*str) return;

    /* Trim leading whitespace */
    char *start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    /* Trim trailing whitespace */
    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    char *end = str + strlen(str) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }

    /* Shift string to start */
    size_t len = end - start + 1;
    if (start != str) {
        memmove(str, start, len);
    }
    str[len] = '\0';
}

/* Split string by delimiter - returns number of parts */
int str_split(char *str, char delimiter, char **parts, int max_parts) {
    int count = 0;
    char *start = str;

    for (char *p = str; *p && count < max_parts; p++) {
        if (*p == delimiter) {
            *p = '\0';
            parts[count++] = start;
            start = p + 1;
        }
    }

    /* Add final part if room */
    if (count < max_parts && *start) {
        parts[count++] = start;
    }

    return count;
}

/* Check if string starts with prefix */
int str_starts_with(const char *str, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    size_t str_len = strlen(str);

    if (prefix_len > str_len) return 0;

    return strncmp(str, prefix, prefix_len) == 0;
}

/* Check if string ends with suffix */
int str_ends_with(const char *str, const char *suffix) {
    size_t suffix_len = strlen(suffix);
    size_t str_len = strlen(str);

    if (suffix_len > str_len) return 0;

    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/* Generate random hex string - caller must free result */
char *generate_random_hex(size_t length) {
    static const char hex_chars[] = "0123456789abcdef";
    char *hex = malloc(length + 1);
    if (!hex) return NULL;

    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        free(hex);
        return NULL;
    }

    size_t byte_count = (length + 1) / 2;

    for (size_t i = 0; i < byte_count; i++) {
        size_t pos = i * 2;
        unsigned char byte;
        if (fread(&byte, 1, 1, urandom) != 1) {
            free(hex);
            fclose(urandom);
            return NULL;
        }
        hex[pos] = hex_chars[(byte >> 4) & 0x0F];
        if (pos + 1 < length) {
            hex[pos + 1] = hex_chars[byte & 0x0F];
        }
    }
    hex[length] = '\0';
    fclose(urandom);

    return hex;
}
