/*
 * SQLite database wrapper
 * Provides simple interface to SQLite operations
 */

#include "db.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global database connection */
sqlite3 *g_db = NULL;

/* Execute SQL file */
static int execute_sql_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        LOG_WARN("db", "Schema file not found: %s (skipping)", filename);
        return 0;  /* Not an error - database may already exist */
    }

    /* Read entire file */
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *sql = malloc(file_size + 1);
    if (sql == NULL) {
        fclose(file);
        return -1;
    }

    size_t bytes_read = fread(sql, 1, file_size, file);
    sql[bytes_read] = '\0';
    fclose(file);

    /* Execute SQL */
    char *err_msg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err_msg);

    free(sql);

    if (rc != SQLITE_OK) {
        if (err_msg != NULL &&
            (strstr(err_msg, "already exists") != NULL ||
             strstr(err_msg, "UNIQUE constraint failed: schema_version.version") != NULL)) {
            LOG_DEBUG("db", "Schema already applied: %s", err_msg);
            sqlite3_free(err_msg);
            return 0;
        }
        LOG_ERROR("db", "Failed to execute schema: %s", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

/* Initialize database */
int db_init(const char *db_path, const char *schema_path) {
    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "Failed to open database: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    /* Enable foreign keys */
    rc = sqlite3_exec(g_db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "Failed to enable foreign keys");
        return -1;
    }

    /* Set WAL mode */
    rc = sqlite3_exec(g_db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("db", "Failed to set WAL mode");
    }

    /* Execute schema (if file exists) */
    if (schema_path != NULL) {
        execute_sql_file(schema_path);
    }

    LOG_INFO("db", "Database initialized successfully");
    return 0;
}

/* Close database */
void db_close(void) {
    if (g_db != NULL) {
        sqlite3_close(g_db);
        g_db = NULL;
        LOG_INFO("db", "Database closed");
    }
}

/* Begin transaction */
int db_begin_transaction(void) {
    return sqlite3_exec(g_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
}

/* Commit transaction */
int db_commit_transaction(void) {
    return sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
}

/* Rollback transaction */
int db_rollback_transaction(void) {
    return sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
}

/* Execute SQL statement */
int db_exec(const char *sql) {
    char *err_msg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err_msg);

    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "SQL error: %s", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

/* Prepare SQL statement */
int db_prepare(const char *sql, sqlite3_stmt **stmt) {
    int rc = sqlite3_prepare_v2(g_db, sql, -1, stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "Failed to prepare statement: %s", sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

/* Finalize statement */
void db_finalize(sqlite3_stmt *stmt) {
    if (stmt != NULL) {
        sqlite3_finalize(stmt);
    }
}

/* Bind text parameter */
int db_bind_text(sqlite3_stmt *stmt, int index, const char *value) {
    return sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
}

/* Bind integer parameter */
int db_bind_int(sqlite3_stmt *stmt, int index, int value) {
    return sqlite3_bind_int(stmt, index, value);
}

/* Bind int64 parameter */
int db_bind_int64(sqlite3_stmt *stmt, int index, sqlite3_int64 value) {
    return sqlite3_bind_int64(stmt, index, value);
}

/* Get column as text */
const char *db_column_text(sqlite3_stmt *stmt, int index) {
    return (const char *)sqlite3_column_text(stmt, index);
}

/* Get column as integer */
int db_column_int(sqlite3_stmt *stmt, int index) {
    return sqlite3_column_int(stmt, index);
}

/* Get column as int64 */
sqlite3_int64 db_column_int64(sqlite3_stmt *stmt, int index) {
    return sqlite3_column_int64(stmt, index);
}

/* Get last insert rowid */
sqlite3_int64 db_last_insert_rowid(void) {
    return sqlite3_last_insert_rowid(g_db);
}

/* Get error message */
const char *db_error_message(void) {
    return sqlite3_errmsg(g_db);
}

/* Backup database */
int db_backup(const char *backup_path) {
    sqlite3 *backup_db = NULL;
    int rc = sqlite3_open(backup_path, &backup_db);

    if (rc != SQLITE_OK) {
        LOG_ERROR("db", "Failed to open backup database: %s",
                 sqlite3_errmsg(backup_db));
        return -1;
    }

    sqlite3_backup *backup = sqlite3_backup_init(backup_db, "main", g_db, "main");
    if (backup == NULL) {
        LOG_ERROR("db", "Failed to initialize backup: %s",
                 sqlite3_errmsg(backup_db));
        sqlite3_close(backup_db);
        return -1;
    }

    rc = sqlite3_backup_step(backup, -1);
    sqlite3_backup_finish(backup);
    sqlite3_close(backup_db);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("db", "Backup failed: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    LOG_INFO("db", "Database backed up to %s", backup_path);
    return 0;
}
