#pragma once
#include <string>

namespace ServiceManager {

// Installs systemd user unit for background watcher
bool install_user_service(std::string& msg);

// Uninstalls systemd user unit
bool uninstall_user_service(std::string& msg);

// Installs XDG autostart desktop entry (~/.config/autostart)
bool install_xdg_autostart(std::string& msg);

// Uninstalls XDG autostart desktop entry
bool uninstall_xdg_autostart(std::string& msg);

// Installs udev rule allowing passwordless mode switching
bool install_udev_rule(std::string& msg);

} // namespace ServiceManager
