#!/bin/sh
# runs a command with CIPHON_TEST_URL pointing at a fresh local server.
fifo=$(mktemp -u)
mkfifo "$fifo"
python3 "$(dirname "$0")/server.py" > "$fifo" &
server=$!
read -r port < "$fifo"
rm -f "$fifo"
CIPHON_TEST_URL="http://127.0.0.1:$port" "$@"
status=$?
kill "$server"
exit $status
