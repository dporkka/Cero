#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT=18080
COOKIE_JAR="$TMPDIR/cookies.txt"
SERVER_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID"
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

cat > "$TMPDIR/config.txt" <<EOF
HOST=127.0.0.1
PORT=$PORT
DB_PATH=$TMPDIR/app.db
LOG_PATH=$TMPDIR/app.log
LOG_LEVEL=0
SESSION_EXPIRY_SECONDS=2592000
RATE_LIMIT_REQUESTS_PER_MINUTE=20
EOF

cat > "$TMPDIR/secrets.txt" <<EOF
SESSION_SECRET=test-session-secret
CSRF_SECRET=test-csrf-secret
ADMIN_PASSWORD_HASH=
EOF

sqlite3 "$TMPDIR/app.db" < "$REPO_DIR/config/schema.sql" > /dev/null

HASH="$(python -W ignore::DeprecationWarning - <<'PY'
import crypt
print(crypt.crypt('secret123', '$2b$12$abcdefghijklmnopqrstuu'))
PY
)"

sqlite3 "$TMPDIR/app.db" <<SQL
INSERT INTO accounts (id, name, created_at, status)
VALUES (1, 'Example Corp', strftime('%s','now'), 'active');
INSERT INTO users (id, account_id, email, password_hash, role, is_active, created_at)
VALUES (1, 1, 'admin@example.com', '$HASH', 'admin', 1, strftime('%s','now'));
INSERT INTO subscriptions
    (account_id, plan, status, valid_from, valid_until, provider, external_id, notes, created_at, updated_at)
VALUES
    (1, 'free', 'active', strftime('%s','now','-1 day'), strftime('%s','now','+100 years'),
     'manual', '', 'Initial subscription', strftime('%s','now'), strftime('%s','now'));
SQL

cd "$REPO_DIR"
./cero "$TMPDIR/config.txt" "$TMPDIR/secrets.txt" "$REPO_DIR/config/schema.sql" > "$TMPDIR/server.out" 2>&1 &
SERVER_PID="$!"

for _ in $(seq 1 20); do
    if curl -fsS "http://127.0.0.1:$PORT/login" > /dev/null 2>&1; then
        break
    fi
    sleep 0.25
done

curl -fsS "http://127.0.0.1:$PORT/login" | grep -q "<h2>Login</h2>"
curl -sS -o /dev/null -D "$TMPDIR/anon_headers.txt" "http://127.0.0.1:$PORT/dashboard"
grep -q "Location: /login" "$TMPDIR/anon_headers.txt"

curl -sS -o /dev/null -D "$TMPDIR/login_headers.txt" \
    -c "$COOKIE_JAR" -b "$COOKIE_JAR" \
    -X POST "http://127.0.0.1:$PORT/login" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "email=admin%40example.com&password=secret123"
grep -q "HTTP/1.1 302 Found" "$TMPDIR/login_headers.txt"
grep -q "Set-Cookie: session_token=" "$TMPDIR/login_headers.txt"

curl -fsS -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/dashboard" | grep -q "Example Corp"
curl -fsS -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/billing" | grep -q "Upgrade to Pro"

TODAY="$(date -u +%F)"
curl -fsS -b "$COOKIE_JAR" \
    -X POST "http://127.0.0.1:$PORT/reports/generate" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "start_date=$TODAY&end_date=$TODAY" | grep -q "Report Results"

curl -sS -o "$TMPDIR/report_denied.txt" -D "$TMPDIR/report_denied_headers.txt" \
    -b "$COOKIE_JAR" \
    -X POST "http://127.0.0.1:$PORT/reports/generate" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "start_date=$TODAY&end_date=$TODAY&export_csv=1"
grep -q "HTTP/1.1 403 Forbidden" "$TMPDIR/report_denied_headers.txt"
grep -q "CSV export is not available" "$TMPDIR/report_denied.txt"

curl -sS -o /dev/null -D "$TMPDIR/paid_headers.txt" \
    -b "$COOKIE_JAR" \
    -X POST "http://127.0.0.1:$PORT/admin/billing/mark-paid" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "account_id=1&plan=pro&duration_days=30&amount_cents=4900&payment_method=manual&external_reference=INV-001&notes=Smoke+test"
grep -q "Location: /admin/billing?account_id=1" "$TMPDIR/paid_headers.txt"

curl -fsS -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/admin/billing?account_id=1" | grep -q "payment_received"
curl -fsS -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/admin/billing?account_id=1" | grep -q "INV-001"
curl -fsS -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/billing" | grep -q "Thank you for your paid subscription"

curl -sS -o "$TMPDIR/report.csv" -D "$TMPDIR/report_csv_headers.txt" \
    -b "$COOKIE_JAR" \
    -X POST "http://127.0.0.1:$PORT/reports/generate" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    --data "start_date=$TODAY&end_date=$TODAY&export_csv=1"
grep -q "Content-Type: text/csv; charset=utf-8" "$TMPDIR/report_csv_headers.txt"
grep -q "Date,Active Users,Sessions,Accounts" "$TMPDIR/report.csv"

curl -sS -o /dev/null -D "$TMPDIR/logout_headers.txt" -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/logout"
grep -q "Location: /" "$TMPDIR/logout_headers.txt"

python - <<PY
import subprocess
codes = []
for _ in range(25):
    result = subprocess.run(
        ['curl', '-s', '-o', '/dev/null', '-w', '%{http_code}', 'http://127.0.0.1:$PORT/'],
        check=True,
        capture_output=True,
        text=True,
    )
    codes.append(result.stdout.strip())
assert '429' in codes, f'expected a 429 response, saw: {codes}'
PY

echo "Smoke test passed"
