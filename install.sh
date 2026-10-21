#!/usr/bin/env bash
set -e

# ==============================================================================
# Lenovo Legion Power Manager - Automated Installation Script
# ==============================================================================

BOLD="\033[1m"
GREEN="\033[1;32m"
BLUE="\033[1;34m"
YELLOW="\033[1;33m"
RESET="\033[0m"

echo -e "${BLUE}${BOLD}==> Installing Lenovo Legion Power Manager...${RESET}"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

# 1. Stop active watcher instances before rebuilding to prevent 'Text file busy'
echo -e "${BLUE}--> Stopping any active watcher instances...${RESET}"
systemctl --user stop legion-power-watcher.service 2>/dev/null || true
pkill -x legion-power 2>/dev/null || true

# 2. Disable and remove conflicting legacy scripts (e.g. legion-fnq-override)
if [ -f "/etc/systemd/system/legion-fnq-override.service" ] || systemctl is-active --quiet legion-fnq-override.service 2>/dev/null; then
    echo -e "${YELLOW}--> Disabling and removing conflicting legacy legion-fnq-override.service...${RESET}"
    if [ "$EUID" -eq 0 ]; then
        systemctl disable --now legion-fnq-override.service 2>/dev/null || true
        systemctl mask legion-fnq-override.service 2>/dev/null || true
        pkill -9 -f legion-fnq-override 2>/dev/null || true
        rm -f /etc/systemd/system/legion-fnq-override.service /usr/local/sbin/legion-fnq-override.sh 2>/dev/null || true
        systemctl daemon-reload 2>/dev/null || true
    elif command -v sudo >/dev/null 2>&1; then
        sudo systemctl disable --now legion-fnq-override.service 2>/dev/null || true
        sudo systemctl mask legion-fnq-override.service 2>/dev/null || true
        sudo pkill -9 -f legion-fnq-override 2>/dev/null || true
        sudo rm -f /etc/systemd/system/legion-fnq-override.service /usr/local/sbin/legion-fnq-override.sh 2>/dev/null || true
        sudo systemctl daemon-reload 2>/dev/null || true
    fi
fi

# 3. Build optimized binary
echo -e "${BLUE}--> Building optimized C++ binary...${RESET}"
make clean
make -j"$(nproc)"

# 4. Determine installation target and safely install using install -m 755 (avoids Text file busy)
TARGET_BIN="/usr/local/bin/legion-power"
TARGET_LINK="/usr/local/bin/power-manager"

if [ "$EUID" -eq 0 ]; then
    install -d /usr/local/bin
    install -m 755 legion-power "$TARGET_BIN"
    ln -sf "$TARGET_BIN" "$TARGET_LINK"
else
    if command -v sudo >/dev/null 2>&1; then
        echo -e "${BLUE}--> Installing binary to /usr/local/bin via sudo...${RESET}"
        sudo install -d /usr/local/bin
        sudo install -m 755 legion-power "$TARGET_BIN"
        sudo ln -sf "$TARGET_BIN" "$TARGET_LINK"
    else
        echo -e "${YELLOW}--> Installing to ~/.local/bin (user prefix)...${RESET}"
        install -d "$HOME/.local/bin"
        install -m 755 legion-power "$HOME/.local/bin/legion-power"
        ln -sf "$HOME/.local/bin/legion-power" "$HOME/.local/bin/power-manager"
        TARGET_BIN="$HOME/.local/bin/legion-power"
    fi
fi

# 5. Install udev rule and systemd-tmpfiles configuration for passwordless switching
echo -e "${BLUE}--> Configuring permissions for /sys/firmware/acpi/platform_profile...${RESET}"
if [ "$EUID" -eq 0 ]; then
    [ -f "udev/99-legion-power.rules" ] && cp udev/99-legion-power.rules /etc/udev/rules.d/
    if [ -f "systemd/99-legion-power.conf" ]; then
        install -d /etc/tmpfiles.d
        cp systemd/99-legion-power.conf /etc/tmpfiles.d/
        systemd-tmpfiles --create /etc/tmpfiles.d/99-legion-power.conf 2>/dev/null || true
    fi
    udevadm control --reload-rules 2>/dev/null || true
    udevadm trigger 2>/dev/null || true
    chmod 0666 /sys/firmware/acpi/platform_profile 2>/dev/null || true
    chmod 0666 /sys/class/platform-profile/platform-profile*/profile 2>/dev/null || true
elif command -v sudo >/dev/null 2>&1; then
    [ -f "udev/99-legion-power.rules" ] && sudo cp udev/99-legion-power.rules /etc/udev/rules.d/
    if [ -f "systemd/99-legion-power.conf" ]; then
        sudo install -d /etc/tmpfiles.d
        sudo cp systemd/99-legion-power.conf /etc/tmpfiles.d/
        sudo systemd-tmpfiles --create /etc/tmpfiles.d/99-legion-power.conf 2>/dev/null || true
    fi
    sudo udevadm control --reload-rules 2>/dev/null || true
    sudo udevadm trigger 2>/dev/null || true
    sudo chmod 0666 /sys/firmware/acpi/platform_profile 2>/dev/null || true
    sudo chmod 0666 /sys/class/platform-profile/platform-profile*/profile 2>/dev/null || true
fi

# 6. Install and restart systemd user service for background watcher (auto-start on boot)
echo -e "${BLUE}--> Configuring systemd background watcher service...${RESET}"
USER_SERVICE_DIR="$HOME/.config/systemd/user"
mkdir -p "$USER_SERVICE_DIR"

cat << EOF > "$USER_SERVICE_DIR/legion-power-watcher.service"
[Unit]
Description=Lenovo Legion Power Mode Watcher & Auto-Dismiss Notifier
After=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStart=$TARGET_BIN watch --timeout 1500
Restart=always
RestartSec=2
Slice=background.slice

[Install]
WantedBy=graphical-session.target default.target
EOF

systemctl --user daemon-reload || true
systemctl --user restart legion-power-watcher.service 2>/dev/null || systemctl --user enable --now legion-power-watcher.service || true

# 7. Install XDG autostart entry for DEs / window managers without systemd user sessions (XFCE, Sway, Hyprland, etc.)
echo -e "${BLUE}--> Configuring XDG autostart entry (~/.config/autostart)...${RESET}"
AUTOSTART_DIR="$HOME/.config/autostart"
mkdir -p "$AUTOSTART_DIR"
cat << EOF > "$AUTOSTART_DIR/legion-power-watcher.desktop"
[Desktop Entry]
Type=Application
Name=Lenovo Legion Power Watcher
Comment=Monitors Fn+Q and manages Lenovo Legion power profiles and auto-dismiss notifications
Exec=$TARGET_BIN watch --timeout 1500
Icon=preferences-system-power
Terminal=false
Categories=Utility;System;
X-GNOME-Autostart-enabled=true
X-KDE-autostart-after=panel
EOF

# 8. Verification test notification
echo -e "${BLUE}--> Sending test notification (disappears automatically in 1.5s)...${RESET}"
"$TARGET_BIN" test-notify 1500 || true

echo -e "\n${GREEN}${BOLD}[OK] Installation Successful!${RESET}"
echo -e "  • Binary:       ${TARGET_BIN}"
echo -e "  • Alias:        power-manager"
echo -e "  • Daemon:       legion-power-watcher.service (active & enabled on boot)"
echo -e "  • Fn+Q Cycle:   Quiet (Blue) -> Balanced (White) -> Performance (Red) -> Extreme (Purple)"
echo -e "  • Fan Control:  legion-power fan [status|set <rpm>|auto|max]"
echo -e "  • Notifications: Auto-dismissing in 1.5s"
echo -e "\nRun '${BOLD}legion-power status${RESET}' anytime to inspect active performance metrics."
