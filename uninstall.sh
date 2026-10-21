#!/usr/bin/env bash
set -e

# ==============================================================================
# Lenovo Legion Power Manager - Uninstallation Script
# ==============================================================================

echo "==> Uninstalling Lenovo Legion Power Manager..."

# 1. Stop and disable systemd service and remove autostart entries
systemctl --user stop legion-power-watcher.service 2>/dev/null || true
systemctl --user disable legion-power-watcher.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/legion-power-watcher.service"
rm -f "$HOME/.config/autostart/legion-power-watcher.desktop"
systemctl --user daemon-reload 2>/dev/null || true

# 2. Remove binaries and symlinks
if [ "$EUID" -eq 0 ]; then
    rm -f /usr/local/bin/legion-power /usr/local/bin/power-manager
    rm -f /etc/udev/rules.d/99-legion-power.rules
    rm -f /etc/tmpfiles.d/99-legion-power.conf
    udevadm control --reload-rules 2>/dev/null || true
else
    if command -v sudo >/dev/null 2>&1; then
        sudo rm -f /usr/local/bin/legion-power /usr/local/bin/power-manager
        sudo rm -f /etc/udev/rules.d/99-legion-power.rules
        sudo rm -f /etc/tmpfiles.d/99-legion-power.conf
        sudo udevadm control --reload-rules 2>/dev/null || true
    fi
    rm -f "$HOME/.local/bin/legion-power" "$HOME/.local/bin/power-manager"
fi

echo "[OK] Uninstallation complete."
