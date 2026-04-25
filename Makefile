# Core SaaS Platform - Makefile
# Simple, portable build system

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lsqlite3 -lpthread -lm -lcrypt
TARGET = cero

# Directories
SRC_DIR = src
OBJ_DIR = obj

# Source files (to be implemented)
CORE_SOURCES = \
	$(SRC_DIR)/core/server.c \
	$(SRC_DIR)/core/request.c \
	$(SRC_DIR)/core/response.c \
	$(SRC_DIR)/core/router.c

UTILS_SOURCES = \
	$(SRC_DIR)/utils/log.c \
	$(SRC_DIR)/utils/time_utils.c \
	$(SRC_DIR)/utils/config.c \
	$(SRC_DIR)/utils/db.c \
	$(SRC_DIR)/utils/string_utils.c \
	$(SRC_DIR)/utils/ratelimit.c

AUTH_SOURCES = \
	$(SRC_DIR)/auth/auth.c \
	$(SRC_DIR)/auth/session.c

BILLING_SOURCES = \
	$(SRC_DIR)/billing/subscription.c \
	$(SRC_DIR)/billing/entitlement.c \
	$(SRC_DIR)/billing/admin.c

REPORTS_SOURCES = \
	$(SRC_DIR)/reports/reports.c \
	$(SRC_DIR)/reports/csv.c

TEMPLATE_SOURCES = \
	$(SRC_DIR)/templates/template.c

# Main source
MAIN_SOURCE = $(SRC_DIR)/main.c

# All sources (add more as implemented)
SOURCES = $(MAIN_SOURCE) $(CORE_SOURCES) $(UTILS_SOURCES) $(AUTH_SOURCES) $(BILLING_SOURCES) $(REPORTS_SOURCES) $(TEMPLATE_SOURCES)

# Object files
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Default target
all: directories $(TARGET)

# Create necessary directories
directories:
	@mkdir -p $(OBJ_DIR)/core
	@mkdir -p $(OBJ_DIR)/auth
	@mkdir -p $(OBJ_DIR)/billing
	@mkdir -p $(OBJ_DIR)/reports
	@mkdir -p $(OBJ_DIR)/templates
	@mkdir -p $(OBJ_DIR)/utils
	@mkdir -p logs
	@mkdir -p data
	@mkdir -p backups

# Link target
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	@$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<..."
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(OBJ_DIR)
	@rm -f $(TARGET)
	@echo "Clean complete"

# Clean everything including runtime data
distclean: clean
	@echo "Removing runtime data..."
	@rm -rf data/*.db data/*.db-wal data/*.db-shm
	@rm -rf logs/*.log
	@echo "Distclean complete"

# Install (requires root)
install: $(TARGET)
	@echo "Installing to /usr/local/bin/..."
	@install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installation complete"

# Uninstall
uninstall:
	@echo "Removing from /usr/local/bin/..."
	@rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstall complete"

# Initialize database
init-db:
	@echo "Initializing database..."
	@sqlite3 data/app.db < config/schema.sql
	@echo "Database initialized"

# Backup database
backup:
	@echo "Backing up database..."
	@mkdir -p backups
	@cp data/app.db backups/app.db.$$(date +%Y-%m-%d-%H%M%S)
	@echo "Backup complete"

# Development mode (with debug symbols)
debug: CFLAGS += -g -DDEBUG -O0
debug: clean all

# Check dependencies
check-deps:
	@echo "Checking dependencies..."
	@command -v gcc >/dev/null 2>&1 || { echo "gcc not found"; exit 1; }
	@command -v sqlite3 >/dev/null 2>&1 || { echo "sqlite3 not found"; exit 1; }
	@pkg-config --exists sqlite3 || { echo "libsqlite3-dev not found"; exit 1; }
	@echo "All dependencies satisfied"

# Help target
help:
	@echo "Core SaaS Platform - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the application (default)"
	@echo "  clean      - Remove build artifacts"
	@echo "  distclean  - Remove build artifacts and runtime data"
	@echo "  install    - Install binary to /usr/local/bin (requires root)"
	@echo "  uninstall  - Remove binary from /usr/local/bin (requires root)"
	@echo "  init-db    - Initialize database with schema"
	@echo "  backup     - Backup database file"
	@echo "  debug      - Build with debug symbols"
	@echo "  test       - Run end-to-end smoke test"
	@echo "  check-deps - Check for required dependencies"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Build configuration:"
	@echo "  CC      = $(CC)"
	@echo "  CFLAGS  = $(CFLAGS)"
	@echo "  LDFLAGS = $(LDFLAGS)"
	@echo "  TARGET  = $(TARGET)"

test: all
	@echo "Running smoke test..."
	@./tests/smoke.sh

.PHONY: all clean distclean install uninstall init-db backup debug test check-deps help directories
