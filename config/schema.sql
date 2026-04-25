-- ============================================================================
-- Core SaaS Platform - SQLite Schema
-- Designed for 50+ year longevity with minimal maintenance
--
-- Design Principles:
-- 1. Foreign keys enforced for referential integrity
-- 2. Append-only tables for audit and billing (never UPDATE or DELETE)
-- 3. All timestamps are UTC epoch integers (seconds since 1970-01-01)
-- 4. No AUTOINCREMENT (simple INTEGER PRIMARY KEY is faster)
-- 5. Explicit NOT NULL where appropriate
-- 6. Indexes for common query patterns
-- 7. Simple, flat schema (no over-normalization)
-- ============================================================================

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA temp_store = MEMORY;
PRAGMA mmap_size = 30000000000;
PRAGMA page_size = 4096;

-- ============================================================================
-- SCHEMA VERSION TRACKING
-- ============================================================================
-- Track schema version for migrations
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL,  -- UTC epoch
    description TEXT NOT NULL
);

INSERT OR IGNORE INTO schema_version (version, applied_at, description)
VALUES (1, strftime('%s', 'now'), 'Initial schema');

-- ============================================================================
-- ACCOUNTS
-- ============================================================================
-- Multi-tenant account structure
-- One account can have multiple users
CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,              -- Organization/account name
    created_at INTEGER NOT NULL,     -- UTC epoch
    status TEXT NOT NULL DEFAULT 'active',  -- active, suspended, cancelled
    CONSTRAINT valid_status CHECK (status IN ('active', 'suspended', 'cancelled'))
);

CREATE INDEX idx_accounts_status ON accounts(status);
CREATE INDEX idx_accounts_created_at ON accounts(created_at);

-- ============================================================================
-- USERS
-- ============================================================================
-- Individual users belong to accounts
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY,
    account_id INTEGER NOT NULL,
    email TEXT NOT NULL UNIQUE,      -- Email is unique across system
    password_hash TEXT NOT NULL,     -- bcrypt hash
    role TEXT NOT NULL DEFAULT 'user',  -- user, admin
    is_active INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,     -- UTC epoch
    last_login_at INTEGER,           -- UTC epoch, NULL if never logged in
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    CONSTRAINT valid_role CHECK (role IN ('user', 'admin')),
    CONSTRAINT valid_is_active CHECK (is_active IN (0, 1))
);

CREATE UNIQUE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_account_id ON users(account_id);
CREATE INDEX idx_users_role ON users(role);

-- ============================================================================
-- SESSIONS
-- ============================================================================
-- Server-side session storage
CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL,
    token TEXT NOT NULL UNIQUE,      -- Random session token (set in cookie)
    created_at INTEGER NOT NULL,     -- UTC epoch
    expires_at INTEGER NOT NULL,     -- UTC epoch
    last_activity_at INTEGER NOT NULL,  -- UTC epoch, updated on each request
    ip_address TEXT,                 -- Last known IP
    user_agent TEXT,                 -- Last known user agent
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX idx_sessions_token ON sessions(token);
CREATE INDEX idx_sessions_user_id ON sessions(user_id);
CREATE INDEX idx_sessions_expires_at ON sessions(expires_at);

-- ============================================================================
-- SUBSCRIPTIONS
-- ============================================================================
-- Authoritative subscription state
-- This table is the SINGLE SOURCE OF TRUTH for entitlements
-- Only this application updates this table (not webhooks, not external APIs)
CREATE TABLE IF NOT EXISTS subscriptions (
    id INTEGER PRIMARY KEY,
    account_id INTEGER NOT NULL UNIQUE,  -- One subscription per account
    plan TEXT NOT NULL,               -- free, pro, enterprise
    status TEXT NOT NULL,             -- active, grace_period, expired, cancelled
    valid_from INTEGER NOT NULL,      -- UTC epoch, when current period started
    valid_until INTEGER NOT NULL,     -- UTC epoch, when current period ends
    grace_until INTEGER,              -- UTC epoch, grace period end (NULL if no grace)
    provider TEXT NOT NULL DEFAULT 'manual',  -- manual, stripe, bank_transfer
    external_id TEXT,                 -- External reference (invoice #, receipt #)
    notes TEXT,                       -- Admin notes
    created_at INTEGER NOT NULL,      -- UTC epoch
    updated_at INTEGER NOT NULL,      -- UTC epoch, last modification
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    CONSTRAINT valid_plan CHECK (plan IN ('free', 'pro', 'enterprise')),
    CONSTRAINT valid_status CHECK (status IN ('active', 'grace_period', 'expired', 'cancelled'))
);

CREATE UNIQUE INDEX idx_subscriptions_account_id ON subscriptions(account_id);
CREATE INDEX idx_subscriptions_plan ON subscriptions(plan);
CREATE INDEX idx_subscriptions_status ON subscriptions(status);
CREATE INDEX idx_subscriptions_valid_until ON subscriptions(valid_until);

-- ============================================================================
-- BILLING EVENTS (APPEND-ONLY)
-- ============================================================================
-- Immutable audit log of all billing-related events
-- NEVER UPDATE OR DELETE from this table
CREATE TABLE IF NOT EXISTS billing_events (
    id INTEGER PRIMARY KEY,
    account_id INTEGER NOT NULL,
    event_type TEXT NOT NULL,        -- subscription_created, payment_received,
                                     -- plan_changed, subscription_cancelled,
                                     -- grace_period_started, subscription_expired
    previous_plan TEXT,              -- Plan before this event (NULL if new)
    new_plan TEXT,                   -- Plan after this event (NULL if cancelled)
    previous_status TEXT,            -- Status before event
    new_status TEXT,                 -- Status after event
    amount_cents INTEGER,            -- Payment amount in cents (NULL if no payment)
    currency TEXT DEFAULT 'USD',     -- Currency code
    payment_method TEXT,             -- manual, bank_transfer, stripe, etc.
    external_reference TEXT,         -- Invoice number, receipt ID, etc.
    admin_user_id INTEGER,           -- User who performed action (NULL if system)
    notes TEXT,                      -- Admin notes
    occurred_at INTEGER NOT NULL,    -- UTC epoch, when event occurred
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    FOREIGN KEY (admin_user_id) REFERENCES users(id) ON DELETE SET NULL
);

CREATE INDEX idx_billing_events_account_id ON billing_events(account_id);
CREATE INDEX idx_billing_events_event_type ON billing_events(event_type);
CREATE INDEX idx_billing_events_occurred_at ON billing_events(occurred_at);
CREATE INDEX idx_billing_events_admin_user_id ON billing_events(admin_user_id);

-- ============================================================================
-- AUDIT LOG (APPEND-ONLY)
-- ============================================================================
-- Immutable audit trail of all significant actions
-- NEVER UPDATE OR DELETE from this table (except for retention policy)
CREATE TABLE IF NOT EXISTS audit_log (
    id INTEGER PRIMARY KEY,
    user_id INTEGER,                 -- NULL for system actions
    account_id INTEGER,              -- NULL for system-wide actions
    action TEXT NOT NULL,            -- login, logout, create_account, update_subscription, etc.
    resource_type TEXT,              -- account, user, subscription, report
    resource_id INTEGER,             -- ID of affected resource
    ip_address TEXT,                 -- IP address of requester
    user_agent TEXT,                 -- User agent string
    details TEXT,                    -- JSON or plain text details
    occurred_at INTEGER NOT NULL,    -- UTC epoch
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE SET NULL
);

CREATE INDEX idx_audit_log_user_id ON audit_log(user_id);
CREATE INDEX idx_audit_log_account_id ON audit_log(account_id);
CREATE INDEX idx_audit_log_action ON audit_log(action);
CREATE INDEX idx_audit_log_occurred_at ON audit_log(occurred_at);

-- ============================================================================
-- RATE LIMITS
-- ============================================================================
-- Token bucket rate limiting stored in SQLite (no Redis)
-- Periodically cleaned up by application
CREATE TABLE IF NOT EXISTS rate_limits (
    id INTEGER PRIMARY KEY,
    identifier TEXT NOT NULL,        -- IP address or user_id (prefixed)
    tokens REAL NOT NULL,            -- Current token count
    last_refill_at INTEGER NOT NULL, -- UTC epoch, last time tokens were refilled
    window_start_at INTEGER NOT NULL, -- UTC epoch, start of current window
    UNIQUE(identifier)
);

CREATE UNIQUE INDEX idx_rate_limits_identifier ON rate_limits(identifier);
CREATE INDEX idx_rate_limits_last_refill_at ON rate_limits(last_refill_at);

-- ============================================================================
-- REPORT CACHE (OPTIONAL)
-- ============================================================================
-- Optional: Cache report results to avoid recomputation
-- Can be cleared at any time without data loss
-- If longevity is priority, consider omitting this entirely
CREATE TABLE IF NOT EXISTS report_cache (
    id INTEGER PRIMARY KEY,
    account_id INTEGER NOT NULL,
    report_type TEXT NOT NULL,
    parameters TEXT NOT NULL,        -- JSON or serialized parameters
    result_hash TEXT NOT NULL,       -- Hash of result for validation
    generated_at INTEGER NOT NULL,   -- UTC epoch
    expires_at INTEGER NOT NULL,     -- UTC epoch
    result_data TEXT,                -- Cached result (JSON or CSV)
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE INDEX idx_report_cache_account_id ON report_cache(account_id);
CREATE INDEX idx_report_cache_expires_at ON report_cache(expires_at);
CREATE INDEX idx_report_cache_report_type ON report_cache(report_type);

-- ============================================================================
-- SAMPLE DATA (FOR DEVELOPMENT)
-- ============================================================================
-- Example data structure
-- NOTE: This section can be omitted in production deployment

-- Example account
-- INSERT INTO accounts (id, name, created_at, status)
-- VALUES (1, 'Example Corp', strftime('%s', 'now'), 'active');

-- Example admin user (password: 'admin123' - CHANGE IN PRODUCTION)
-- Bcrypt hash for 'admin123': $2b$12$LQv3c1yqBWVHxkd0LHAkCOYz6TtxMQJqhN8/LewY5oeWP1oTXe3kO
-- INSERT INTO users (id, account_id, email, password_hash, role, created_at)
-- VALUES (1, 1, 'admin@example.com',
--         '$2b$12$LQv3c1yqBWVHxkd0LHAkCOYz6TtxMQJqhN8/LewY5oeWP1oTXe3kO',
--         'admin', strftime('%s', 'now'));

-- Example subscription (free plan)
-- INSERT INTO subscriptions (account_id, plan, status, valid_from, valid_until, created_at, updated_at)
-- VALUES (1, 'free', 'active',
--         strftime('%s', 'now'),
--         strftime('%s', 'now', '+100 years'),
--         strftime('%s', 'now'),
--         strftime('%s', 'now'));

-- ============================================================================
-- VIEWS (FOR CONVENIENCE)
-- ============================================================================

-- Active subscriptions with account details
CREATE VIEW IF NOT EXISTS v_active_subscriptions AS
SELECT
    s.id,
    s.account_id,
    a.name AS account_name,
    s.plan,
    s.status,
    s.valid_from,
    s.valid_until,
    s.grace_until,
    s.provider,
    s.external_id,
    CASE
        WHEN s.status = 'active' AND s.valid_until > strftime('%s', 'now') THEN 1
        WHEN s.status = 'grace_period' AND s.grace_until > strftime('%s', 'now') THEN 1
        ELSE 0
    END AS is_valid
FROM subscriptions s
JOIN accounts a ON s.account_id = a.id;

-- User details with account and subscription info
CREATE VIEW IF NOT EXISTS v_users_with_accounts AS
SELECT
    u.id AS user_id,
    u.email,
    u.role,
    u.created_at AS user_created_at,
    u.last_login_at,
    a.id AS account_id,
    a.name AS account_name,
    a.status AS account_status,
    s.plan AS subscription_plan,
    s.status AS subscription_status,
    s.valid_until AS subscription_valid_until
FROM users u
JOIN accounts a ON u.account_id = a.id
LEFT JOIN subscriptions s ON a.id = s.account_id;

-- Recent billing events with account names
CREATE VIEW IF NOT EXISTS v_recent_billing_events AS
SELECT
    be.id,
    be.account_id,
    a.name AS account_name,
    be.event_type,
    be.previous_plan,
    be.new_plan,
    be.previous_status,
    be.new_status,
    be.amount_cents,
    be.currency,
    be.payment_method,
    be.external_reference,
    be.admin_user_id,
    u.email AS admin_email,
    be.notes,
    be.occurred_at
FROM billing_events be
JOIN accounts a ON be.account_id = a.id
LEFT JOIN users u ON be.admin_user_id = u.id
ORDER BY be.occurred_at DESC;

-- ============================================================================
-- SCHEMA DOCUMENTATION
-- ============================================================================

-- Table relationships:
--
-- accounts (1) <---> (many) users
-- accounts (1) <---> (1) subscriptions
-- accounts (1) <---> (many) billing_events
-- accounts (1) <---> (many) audit_log
--
-- users (1) <---> (many) sessions
-- users (1) <---> (many) audit_log
-- users (1) <---> (many) billing_events (as admin_user_id)
--
-- Append-only tables:
-- - billing_events: Never UPDATE or DELETE
-- - audit_log: Never UPDATE or DELETE
--
-- Regular tables:
-- - accounts: Can UPDATE status
-- - users: Can UPDATE last_login_at
-- - sessions: Can UPDATE last_activity_at, DELETE on expiration
-- - subscriptions: Can UPDATE (but log changes to billing_events)
-- - rate_limits: Can UPDATE tokens, DELETE old entries
-- - report_cache: Can DELETE expired entries

-- ============================================================================
-- INDEXES SUMMARY
-- ============================================================================
-- All foreign keys have indexes
-- All lookup columns (email, token) have unique indexes
-- Time-based queries (created_at, occurred_at, expires_at) have indexes
-- Enumerated types (status, plan, role) have indexes where frequently queried

-- ============================================================================
-- MAINTENANCE QUERIES
-- ============================================================================

-- Clean up expired sessions (run periodically):
-- DELETE FROM sessions WHERE expires_at < strftime('%s', 'now');

-- Clean up old rate limit entries (run daily):
-- DELETE FROM rate_limits WHERE last_refill_at < strftime('%s', 'now', '-1 day');

-- Clean up expired report cache (run daily):
-- DELETE FROM report_cache WHERE expires_at < strftime('%s', 'now');

-- Vacuum database (run monthly, during low traffic):
-- VACUUM;

-- ============================================================================
-- BACKUP STRATEGY
-- ============================================================================
-- 1. Daily: Copy entire .db file (while app is running, WAL mode is safe)
-- 2. Weekly: Full backup with .dump
-- 3. Monthly: Offsite backup
-- 4. Test restoration quarterly

-- Backup command (while app running):
-- cp data/app.db backups/app.db.$(date +%Y-%m-%d)

-- Restore command (app must be stopped):
-- cp backups/app.db.2026-01-21 data/app.db

-- ============================================================================
-- SCHEMA MIGRATION STRATEGY
-- ============================================================================
-- When schema changes are needed:
-- 1. Add new schema_version entry
-- 2. Use ALTER TABLE (backward compatible only)
-- 3. Never drop columns (mark as deprecated in comments)
-- 4. Add columns with DEFAULT values for backward compatibility
-- 5. Test migration on backup copy first

-- Example migration:
-- BEGIN TRANSACTION;
-- ALTER TABLE accounts ADD COLUMN industry TEXT DEFAULT 'unspecified';
-- INSERT INTO schema_version (version, applied_at, description)
-- VALUES (2, strftime('%s', 'now'), 'Added industry to accounts');
-- COMMIT;

-- ============================================================================
-- END OF SCHEMA
-- ============================================================================
