#!/usr/bin/env bash
# ─────────────────────────────────────────────────
#  Claude Token Meter — Companion setup (Linux/macOS)
# ─────────────────────────────────────────────────
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║  Claude Token Meter — Companion Setup   ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# ── Check Python ─────────────────────────────────
if ! command -v python3 &>/dev/null; then
  echo "❌  Python 3 is required but not found."
  echo "    Install from https://python.org"
  exit 1
fi

PYVER=$(python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
echo "✅  Python $PYVER found"

# ── Create venv ───────────────────────────────────
VENV="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV" ]; then
  echo "📦  Creating virtual environment..."
  python3 -m venv "$VENV"
fi

# ── Install deps ──────────────────────────────────
echo "📦  Installing dependencies..."
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$SCRIPT_DIR/requirements.txt"
echo "✅  Dependencies installed"

# ── Create launcher script ────────────────────────
LAUNCHER="$SCRIPT_DIR/run_monitor.sh"
cat > "$LAUNCHER" << 'EOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$DIR/.venv/bin/python" "$DIR/claude_monitor.py" "$@"
EOF
chmod +x "$LAUNCHER"
echo "✅  Launcher created:  companion/run_monitor.sh"

echo ""
echo "────────────────────────────────────────────"
echo "  Ready! Run the monitor:"
echo ""
echo "  # Auto-discover device:"
echo "  ./run_monitor.sh --discover"
echo ""
echo "  # Specify IP directly:"
echo "  ./run_monitor.sh --ip 192.168.1.42"
echo ""
echo "  # Test with simulated data:"
echo "  ./run_monitor.sh --ip claude-meter.local --simulate"
echo "────────────────────────────────────────────"
echo ""
