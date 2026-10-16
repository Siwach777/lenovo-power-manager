#pragma once
#include <string>
#include <cstdint>
#include "common.hpp"

// Forward declare sd_bus to keep header lightweight
struct sd_bus;

class PowerNotifier {
public:
    explicit PowerNotifier(int timeout_ms = 1500);
    ~PowerNotifier();

    // Prevent copies
    PowerNotifier(const PowerNotifier&) = delete;
    PowerNotifier& operator=(const PowerNotifier&) = delete;

    // Dispatches an auto-dismissing notification and KDE Plasma OSD
    void send_mode_notification(const ModeInfo& mode, const std::string& previous_mode = "");

    // Direct test notification
    void send_test_notification();

    void set_timeout_ms(int ms) { timeout_ms_ = ms; }

private:
    sd_bus* bus_ = nullptr;
    uint32_t last_notification_id_ = 0;
    int timeout_ms_ = 1500;

    void ensure_bus();
    bool send_dbus_notification(const ModeInfo& mode, const std::string& previous_mode);
    void send_kde_osd(const ModeInfo& mode);
    void send_fallback_notification(const ModeInfo& mode, const std::string& previous_mode);
};
