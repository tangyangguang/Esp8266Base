#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

mkdir -p "$TEST_DIR/bin"
firmware="$TEST_DIR/firmware.bin"
printf '\351\002\002\100\200\364\020\100\000\360\020\100\140\015\000\000' > "$firmware"

cat > "$TEST_DIR/bin/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  if [[ "${FAKE_CURL_FAIL_WITH_BODY:-0}" == "1" ]]; then
    echo "     --fail-with-body  Fail on HTTP errors but save the body"
  fi
  exit 0
fi

output_file=""
has_fail_with_body=0
has_fail=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      output_file="$2"
      shift 2
      ;;
    --fail-with-body)
      has_fail_with_body=1
      shift
      ;;
    --fail)
      has_fail=1
      shift
      ;;
    --user|--form|--write-out)
      shift 2
      ;;
    --silent|--show-error)
      shift
      ;;
    *)
      shift
      ;;
  esac
done

printf '%s' "${FAKE_CURL_BODY:-}" > "$output_file"
printf '%s' "${FAKE_CURL_HTTP_CODE:-200}"
printf '%s,%s' "$has_fail" "$has_fail_with_body" > "$FAKE_CURL_ARGS_FILE"
exit "${FAKE_CURL_STATUS:-0}"
EOF
chmod +x "$TEST_DIR/bin/curl"

run_case() {
  local name="$1"
  local expected_status="$2"
  local expected_flags="$3"
  local expected_text="$4"
  shift 4

  local output_file="$TEST_DIR/${name}.output"
  local args_file="$TEST_DIR/${name}.args"
  set +e
  env PATH="$TEST_DIR/bin:$PATH" \
      OTA_PASSWORD=test-password \
      FAKE_CURL_ARGS_FILE="$args_file" \
      "$@" \
      "$ROOT_DIR/tools/ota_upload.sh" http://device.invalid "$firmware" admin \
      > "$output_file" 2>&1
  local status=$?
  set -e

  [[ "$status" -eq "$expected_status" ]] ||
    fail "$name exit: expected $expected_status, got $status"
  [[ "$(cat "$args_file")" == "$expected_flags" ]] ||
    fail "$name flags: expected $expected_flags, got $(cat "$args_file")"
  grep -F -- "$expected_text" "$output_file" >/dev/null ||
    fail "$name output missing: $expected_text"
}

run_case modern_http_error 22 "0,1" "denied" \
  FAKE_CURL_FAIL_WITH_BODY=1 FAKE_CURL_HTTP_CODE=403 FAKE_CURL_BODY=denied FAKE_CURL_STATUS=22
run_case legacy_http_error 22 "0,0" "conflict" \
  FAKE_CURL_FAIL_WITH_BODY=0 FAKE_CURL_HTTP_CODE=409 FAKE_CURL_BODY=conflict FAKE_CURL_STATUS=0
run_case legacy_success 0 "0,0" "OTA upload accepted." \
  FAKE_CURL_FAIL_WITH_BODY=0 FAKE_CURL_HTTP_CODE=200 FAKE_CURL_BODY=ok FAKE_CURL_STATUS=0
run_case transport_error 7 "0,1" "OTA upload failed (curl exit 7)." \
  FAKE_CURL_FAIL_WITH_BODY=1 FAKE_CURL_HTTP_CODE=000 FAKE_CURL_BODY= FAKE_CURL_STATUS=7

echo "[ota-upload] ok"
