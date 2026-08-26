#!/bin/bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 <device-base-url> <firmware.bin> [username]" >&2
  exit 2
fi

device_url="${1%/}"
firmware="$2"
ota_user="${3:-admin}"

if [[ ! -f "$firmware" ]]; then
  echo "Firmware not found: $firmware" >&2
  exit 2
fi

if [[ -z "${OTA_PASSWORD:-}" ]]; then
  read -r -s -p "Web password: " OTA_PASSWORD
  echo
fi

response_file="$(mktemp)"
trap 'rm -f "$response_file"' EXIT
firmware_size="$(wc -c < "$firmware" | tr -d ' ')"

echo "Firmware: $firmware"
echo "Firmware size: ${firmware_size} bytes"
echo "Target: ${device_url}/ota"

set +e
http_code="$(curl --fail --fail-with-body --silent --show-error \
  --user "${ota_user}:${OTA_PASSWORD}" \
  --form "firmware=@${firmware};type=application/octet-stream" \
  --output "$response_file" \
  --write-out '%{http_code}' \
  "${device_url}/ota")"
curl_status=$?
set -e

echo "HTTP result: ${http_code:-000}"
echo "Device response:"
if [[ -s "$response_file" ]]; then
  sed -n '1,20p' "$response_file"
else
  echo "(empty)"
fi

if [[ $curl_status -ne 0 ]]; then
  echo "OTA upload failed (curl exit ${curl_status})." >&2
  exit "$curl_status"
fi

echo "OTA upload accepted."
