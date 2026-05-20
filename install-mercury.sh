#!/bin/bash
#
# Author  : Sylvain Deguire (VA2OPS)
# Date    : May 2026
# Purpose : Install Mercury modem support for QtTermTCP.
#           - Deploys updated QtTermTCP binary (Mercury dialog support)
#           - Deploys updated mode_engine.py (Mercury INI/config actions)
#           - Patches running user's QtTermTCP.ini with Mercury keys
#           - Restarts et-supervisor
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ET_LIB="/opt/emcomm-tools/lib/et_supervisor"
ET_BIN="/opt/qttermtcp/bin"
QTTERMTCP_INI="$HOME/.config/QtTermTCP.ini"

# 1. Install QtTermTCP binary
echo "Installing QtTermTCP..."
sudo install -m 755 "$SCRIPT_DIR/QtTermTCP" "$ET_BIN/QtTermTCP"

# 2. Install updated mode_engine.py
echo "Installing mode_engine.py..."
sudo install -m 644 "$SCRIPT_DIR/mode_engine.py" "$ET_LIB/mode_engine.py"

# 3. Patch running user's QtTermTCP.ini with Mercury keys if missing
if [ -f "$QTTERMTCP_INI" ]; then
    echo "Patching QtTermTCP.ini..."
    grep -q "^MERCURYEnable=" "$QTTERMTCP_INI"  || echo "MERCURYEnable=0"    >> "$QTTERMTCP_INI"
    grep -q "^MERCURYTermCall=" "$QTTERMTCP_INI" || echo "MERCURYTermCall="   >> "$QTTERMTCP_INI"
    grep -q "^MERCURY500=" "$QTTERMTCP_INI"      || echo "MERCURY500=1"       >> "$QTTERMTCP_INI"
    grep -q "^MERCURY2300=" "$QTTERMTCP_INI"     || echo "MERCURY2300=0"      >> "$QTTERMTCP_INI"
    grep -q "^MERCURY2750=" "$QTTERMTCP_INI"     || echo "MERCURY2750=0"      >> "$QTTERMTCP_INI"
    # Remove legacy typo key if present
    sed -i '/^MERCRUYTermCall=/d' "$QTTERMTCP_INI"
else
    echo "Warning: $QTTERMTCP_INI not found — Mercury keys will be added on first QtTermTCP run."
fi

# 4. Restart et-supervisor to pick up new mode_engine.py
echo "Restarting et-supervisor..."
sudo systemctl restart et-supervisor || true

echo ""
echo "Done. Mercury modem support installed."
echo "Start a BBS mode with Mercury modem to activate."
