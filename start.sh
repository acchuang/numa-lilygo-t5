#!/usr/bin/env zsh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

mkdir -p "$DIR/data"

if ! pgrep -f "numa.*numa.toml" >/dev/null 2>&1; then
  echo "Starting Numa DNS & API server (:5553 DNS, :5380 Web UI)..."
  /opt/homebrew/bin/numa "$DIR/numa.toml" > "$DIR/data/numa.log" 2>&1 &
  sleep 1
fi

echo "Starting LilyGO T5 4.7\" USB-C Serial Bridge & Companion UI (:5381)..."
exec uv run --with pyserial python3 "$DIR/bridge.py"
