#!/usr/bin/env bash
# Runs a JavaScript file with node, or with the node built into VS Code when node itself is not installed.
set -euo pipefail
if command -v node >/dev/null 2>&1; then
    exec node "$@"
fi
for electron in /usr/share/code/code /opt/visual-studio-code/code /usr/lib/code/code /usr/share/codium/codium \
                "$(dirname "$(dirname "$(readlink -f "$(command -v code 2>/dev/null || echo /nonexistent)")")")/code"; do
    if [ -x "$electron" ] && [ -f "$electron" ]; then
        ELECTRON_RUN_AS_NODE=1 exec "$electron" "$@"
    fi
done
echo "skipped $1: no node (install nodejs, or VS Code)"
