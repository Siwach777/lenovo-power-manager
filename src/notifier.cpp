#include "notifier.hpp"
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <sstream>

PowerNotifier::PowerNotifier(int timeout_ms) : timeout_ms_(timeout_ms) {
    ensure_bus();
}

PowerNotifier::~PowerNotifier() {
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
}

void PowerNotifier::ensure_bus() {
    if (!bus_) {
        int r = sd_bus_open_user(&bus_);
        if (r < 0) {
            bus_ = nullptr;
        }
    }
}

bool PowerNotifier::send_dbus_notification(const ModeInfo& mode, const std::string& previous_mode) {
    ensure_bus();
    if (!bus_) return false;

    sd_bus_message *m = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_message_new_method_call(bus_, &m,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify");
    if (r < 0) return false;

    std::string app_name = "Lenovo Legion Power";
    std::string summary = mode.led_emoji + " " + mode.name + " (" + mode.led_color + ")";

    std::ostringstream body_ss;
    body_ss << "<b>Virtual LED:</b> " << mode.led_color << " (" << mode.led_emoji << ")\n"
            << "<b>Profile:</b> " << mode.id << "\n"
            << "<b>Performance:</b> " << mode.summary;
    if (!previous_mode.empty()) {
        body_ss << "\n<i>(Switched from " << previous_mode << ")</i>";
    }
    std::string body = body_ss.str();

    // 1. app_name (s)
    r = sd_bus_message_append(m, "s", app_name.c_str());
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // 2. replaces_id (u) - updating in-place prevents notification popup flood when cycling
    r = sd_bus_message_append(m, "u", last_notification_id_);
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // 3. app_icon (s)
    r = sd_bus_message_append(m, "s", mode.icon.c_str());
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // 4. summary (s)
    r = sd_bus_message_append(m, "s", summary.c_str());
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // 5. body (s)
    r = sd_bus_message_append(m, "s", body.c_str());
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // 6. actions (as)
    r = sd_bus_message_open_container(m, 'a', "s");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_close_container(m);
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // 7. hints (a{sv})
    r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (r < 0) { sd_bus_message_unref(m); return false; }

    // Urgency = 1 (Normal): Allows the notification daemon to auto-expire/auto-dismiss cleanly
    r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "urgency");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_open_container(m, 'v', "y");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    uint8_t urgency = 1; // Normal urgency -> auto-expires on its own
    r = sd_bus_message_append(m, "y", urgency);
    if (r < 0) { sd_bus_message_unref(m); return false; }
    sd_bus_message_close_container(m); // close v
    sd_bus_message_close_container(m); // close e

    // Category = "device"
    r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "category");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_open_container(m, 'v', "s");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "device");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    sd_bus_message_close_container(m); // close v
    sd_bus_message_close_container(m); // close e

    // Transient = true: Auto-dismisses without persisting in tray history
    r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "transient");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_open_container(m, 'v', "b");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    int transient_val = 1;
    r = sd_bus_message_append(m, "b", transient_val);
    if (r < 0) { sd_bus_message_unref(m); return false; }
    sd_bus_message_close_container(m); // close v
    sd_bus_message_close_container(m); // close e

    // desktop-entry = "legion-power"
    r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "desktop-entry");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_open_container(m, 'v', "s");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "legion-power");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    sd_bus_message_close_container(m); // close v
    sd_bus_message_close_container(m); // close e

    // x-canonical-private-synchronous = "legion-power-mode" (prevents notification queue backlog on GNOME, Dunst, etc.)
    r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "x-canonical-private-synchronous");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_open_container(m, 'v', "s");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    r = sd_bus_message_append(m, "s", "legion-power-mode");
    if (r < 0) { sd_bus_message_unref(m); return false; }
    sd_bus_message_close_container(m); // close v
    sd_bus_message_close_container(m); // close e

    sd_bus_message_close_container(m); // close hints dict

    // 8. expire_timeout (i) - e.g. 1500ms
    r = sd_bus_message_append(m, "i", timeout_ms_);
    if (r < 0) { sd_bus_message_unref(m); return false; }

    sd_bus_message *reply = nullptr;
    r = sd_bus_call(bus_, m, 0, &error, &reply);
    if (r >= 0 && reply) {
        uint32_t new_id = 0;
        if (sd_bus_message_read(reply, "u", &new_id) >= 0) {
            last_notification_id_ = new_id;
        }
        sd_bus_message_unref(reply);
    }

    sd_bus_message_unref(m);
    sd_bus_error_free(&error);

    return (r >= 0);
}

void PowerNotifier::send_kde_osd(const ModeInfo& mode) {
    ensure_bus();
    if (!bus_) return;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    std::string text = mode.led_emoji + " " + mode.name + " (" + mode.led_color + ")";

    // Call KDE Plasma OSD service if available
    sd_bus_call_method(bus_,
        "org.kde.plasmashell",
        "/org/kde/osdService",
        "org.kde.osdService",
        "showText",
        &error,
        nullptr,
        "ss",
        mode.icon.c_str(),
        text.c_str());

    sd_bus_error_free(&error);
}

void PowerNotifier::send_fallback_notification(const ModeInfo& mode, const std::string& previous_mode) {
    pid_t pid = fork();
    if (pid == 0) {
        std::string summary = mode.led_emoji + " " + mode.name + " (" + mode.led_color + ")";
        std::ostringstream body_ss;
        body_ss << "Virtual LED: " << mode.led_color << " (" << mode.led_emoji << ")\n"
                << "Profile: " << mode.id << "\n"
                << mode.summary;
        if (!previous_mode.empty()) {
            body_ss << "\n(Switched from " << previous_mode << ")";
        }
        std::string body = body_ss.str();
        std::string timeout_str = std::to_string(timeout_ms_);

        char* const args[] = {
            const_cast<char*>("notify-send"),
            const_cast<char*>("-u"), const_cast<char*>("normal"),
            const_cast<char*>("-a"), const_cast<char*>("Lenovo Legion Power"),
            const_cast<char*>("-i"), const_cast<char*>(mode.icon.c_str()),
            const_cast<char*>("-c"), const_cast<char*>("device"),
            const_cast<char*>("-h"), const_cast<char*>("int:transient:1"),
            const_cast<char*>("-h"), const_cast<char*>("string:x-canonical-private-synchronous:legion-power-mode"),
            const_cast<char*>("-t"), const_cast<char*>(timeout_str.c_str()),
            const_cast<char*>(summary.c_str()),
            const_cast<char*>(body.c_str()),
            nullptr
        };
        execvp("notify-send", args);
        _exit(1);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

void PowerNotifier::send_mode_notification(const ModeInfo& mode, const std::string& previous_mode) {
    const char* desktop_env = std::getenv("XDG_CURRENT_DESKTOP");
    std::string de = desktop_env ? desktop_env : "";
    bool is_kde = (de.find("KDE") != std::string::npos || de.find("Plasma") != std::string::npos);

    // 1. On KDE Plasma, dispatch sleek native OSD overlay
    if (is_kde) {
        send_kde_osd(mode);
    }

    // 2. Send auto-expiring desktop notification via universal Freedesktop D-Bus specification
    // Fully compatible across KDE, GNOME, XFCE, Cinnamon, MATE, LXQt, Hyprland, Sway, Dunst, Mako, SwayNC
    bool ok = send_dbus_notification(mode, previous_mode);

    // 3. Fallback to notify-send process if direct D-Bus connection was unavailable
    if (!ok) {
        send_fallback_notification(mode, previous_mode);
    }
}

void PowerNotifier::send_test_notification() {
    ModeInfo test_mode = {
        "max-power",
        "Extreme / Max Power",
        "Purple",
        "🟣",
        "preferences-system-power-performance",
        "Auto-Dismiss Notification Test",
        "Lenovo Legion Power Manager verification alert.",
        "\033[1;35m"
    };
    send_mode_notification(test_mode, "balanced");
}
