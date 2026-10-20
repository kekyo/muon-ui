#!/usr/bin/env bash
set -euo pipefail
coproc NOTIFICATIONS { "$1"; }
server_pid=$NOTIFICATIONS_PID
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
read -r ready <&"${NOTIFICATIONS[0]}"
[[ "$ready" == *'"ready"'* ]]
"$2"
