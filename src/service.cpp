#include "service.hpp"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>

namespace {

std::string get_self_executable_path() {
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "/usr/local/bin/legion-power";
}

std::string get_user_home() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : "";
}

bool run_cmd(const std::string& cmd) {
    int ret = std::system(cmd.c_str());
    return (WIFEXITED(ret) && WEXITSTATUS(ret) == 0);
}

} // namespace

namespace ServiceManager {

bool install_user_service(std::string& msg) {
    // Disable legacy conflicting services if present
    run_cmd("systemctl is-active --quiet legion-fnq-override.service 2>/dev/null && "
            "pkexec sh -c 'systemctl disable --now legion-fnq-override.service; systemctl mask legion-fnq-override.service; pkill -f legion-fnq-override'");

    std::string home = get_user_home();
    if (home.empty()) {
        msg = "Could not determine HOME directory.";
        return false;
    }

    std::string service_dir = home + "/.config/systemd/user";
    std::string mkdir_cmd = "mkdir -p " + service_dir;
    std::system(mkdir_cmd.c_str());

    std::string service_file = service_dir + "/legion-power-watcher.service";
    std::string exe_path = get_self_executable_path();

    std::ofstream out(service_file);
    if (!out.is_open()) {
        msg = "Failed to create " + service_file + ": " + std::strerror(errno);
        return false;
    }

    out << "[Unit]\n"
        << "Description=Lenovo Legion Power Mode Watcher & Auto-Dismiss Notifier\n"
        << "After=graphical-session.target\n"
        << "PartOf=graphical-session.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "ExecStart=" << exe_path << " watch --timeout 1500\n"
        << "Restart=always\n"
        << "RestartSec=2\n"
        << "Slice=background.slice\n\n"
        << "[Install]\n"
        << "WantedBy=graphical-session.target default.target\n";
    out.close();

    // Reload systemd and enable
    run_cmd("systemctl --user daemon-reload");
    bool ok = run_cmd("systemctl --user restart legion-power-watcher.service 2>/dev/null || systemctl --user enable --now legion-power-watcher.service");

    // Also configure XDG autostart entry for DEs without systemd user sessions (XFCE, Sway, Hyprland, etc.)
    std::string xdg_msg;
    install_xdg_autostart(xdg_msg);

    if (ok) {
        msg = "Installed and active: " + service_file + " (and XDG autostart configured)";
        return true;
    } else {
        msg = "Service file written to " + service_file;
        return true;
    }
}

bool uninstall_user_service(std::string& msg) {
    std::string home = get_user_home();
    if (home.empty()) {
        msg = "Could not determine HOME directory.";
        return false;
    }

    std::string service_file = home + "/.config/systemd/user/legion-power-watcher.service";
    run_cmd("systemctl --user stop legion-power-watcher.service");
    run_cmd("systemctl --user disable legion-power-watcher.service");
    unlink(service_file.c_str());
    run_cmd("systemctl --user daemon-reload");

    std::string xdg_msg;
    uninstall_xdg_autostart(xdg_msg);

    msg = "Stopped and uninstalled legion-power-watcher.service and XDG autostart.";
    return true;
}

bool install_xdg_autostart(std::string& msg) {
    std::string home = get_user_home();
    if (home.empty()) {
        msg = "Could not determine HOME directory.";
        return false;
    }

    std::string autostart_dir = home + "/.config/autostart";
    std::string mkdir_cmd = "mkdir -p " + autostart_dir;
    std::system(mkdir_cmd.c_str());

    std::string desktop_file = autostart_dir + "/legion-power-watcher.desktop";
    std::string exe_path = get_self_executable_path();

    std::ofstream out(desktop_file);
    if (!out.is_open()) {
        msg = "Failed to create " + desktop_file + ": " + std::strerror(errno);
        return false;
    }

    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Lenovo Legion Power Watcher\n"
        << "Comment=Monitors Fn+Q and manages Lenovo Legion power profiles and auto-dismiss notifications\n"
        << "Exec=" << exe_path << " watch --timeout 1500\n"
        << "Icon=preferences-system-power\n"
        << "Terminal=false\n"
        << "Categories=Utility;System;\n"
        << "X-GNOME-Autostart-enabled=true\n"
        << "X-KDE-autostart-after=panel\n";
    out.close();

    msg = "Installed XDG autostart entry: " + desktop_file;
    return true;
}

bool uninstall_xdg_autostart(std::string& msg) {
    std::string home = get_user_home();
    if (home.empty()) {
        msg = "Could not determine HOME directory.";
        return false;
    }

    std::string desktop_file = home + "/.config/autostart/legion-power-watcher.desktop";
    unlink(desktop_file.c_str());
    msg = "Removed XDG autostart entry.";
    return true;
}

bool install_udev_rule(std::string& msg) {
    const char* rule_content = 
        "# Allow unprivileged users to change platform_profile\n"
        "ACTION==\"add|change\", SUBSYSTEM==\"acpi\", KERNEL==\"platform_profile\", RUN+=\"/usr/bin/chmod 0666 /sys/firmware/acpi/platform_profile\"\n"
        "SUBSYSTEM==\"acpi\", ATTR{platform_profile}!=\"\", MODE=\"0666\"\n\n"
        "ACTION==\"add|change\", SUBSYSTEM==\"platform-profile\", RUN+=\"/usr/bin/chmod 0666 /sys/class/platform-profile/%k/profile\"\n"
        "SUBSYSTEM==\"platform-profile\", ATTR{profile}!=\"\", MODE=\"0666\"\n";

    std::string temp_file = "/tmp/99-legion-power.rules";
    std::ofstream out(temp_file);
    if (!out.is_open()) {
        msg = "Failed to create temp file.";
        return false;
    }
    out << rule_content;
    out.close();

    const char* tmpfiles_content =
        "# Grant non-root write access to platform_profile and fan targets\n"
        "z /sys/firmware/acpi/platform_profile 0666 root root -\n"
        "z /sys/class/platform-profile/platform-profile*/profile 0666 root root -\n"
        "z /sys/class/hwmon/hwmon*/fan*_target 0666 root root -\n";

    std::string temp_tmpfiles = "/tmp/99-legion-power.conf";
    std::ofstream out_tmp(temp_tmpfiles);
    if (out_tmp.is_open()) {
        out_tmp << tmpfiles_content;
        out_tmp.close();
    }

    std::string cmd = "pkexec sh -c 'cp " + temp_file + " /etc/udev/rules.d/99-legion-power.rules && "
                      "mkdir -p /etc/tmpfiles.d && cp " + temp_tmpfiles + " /etc/tmpfiles.d/99-legion-power.conf && "
                      "systemd-tmpfiles --create /etc/tmpfiles.d/99-legion-power.conf 2>/dev/null || true && "
                      "udevadm control --reload-rules 2>/dev/null || true && udevadm trigger 2>/dev/null || true && "
                      "chmod 0666 /sys/firmware/acpi/platform_profile 2>/dev/null || true && "
                      "chmod 0666 /sys/class/platform-profile/platform-profile*/profile 2>/dev/null || true'";
    int ret = std::system(cmd.c_str());
    unlink(temp_file.c_str());
    unlink(temp_tmpfiles.c_str());

    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
        msg = "Successfully installed /etc/udev/rules.d/99-legion-power.rules and /etc/tmpfiles.d/99-legion-power.conf. Platform profile and fan controls are now writable!";
        return true;
    } else {
        msg = "Failed to install udev and tmpfiles rules via pkexec.";
        return false;
    }
}

} // namespace ServiceManager
