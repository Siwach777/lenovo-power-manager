#pragma once
#include <string>
#include <vector>
#include <unordered_map>

struct ModeInfo {
    std::string id;             // sysfs id: "low-power", "balanced", "performance", "max-power", "custom"
    std::string name;           // Human readable: "Quiet Mode", "Balanced Mode", etc.
    std::string led_color;      // Original power button LED: "Blue", "White", "Red", "Purple", "Cyan"
    std::string led_emoji;      // Visual indicator emoji: "🔵", "⚪", "🔴", "🟣", "🟠"
    std::string icon;           // Freedesktop / KDE icon name
    std::string summary;        // Short status summary
    std::string description;    // Detailed description
    std::string terminal_color; // ANSI color escape code
};

inline const std::vector<ModeInfo>& get_known_modes() {
    static const std::vector<ModeInfo> modes = {
        {
            "low-power",
            "Quiet Mode",
            "Blue",
            "🔵",
            "battery-profile-powersave",
            "Low Noise & Max Battery",
            "Reduced power limits, quiet fan curve, optimal for battery and low noise.",
            "\033[1;34m" // Bright Blue
        },
        {
            "balanced",
            "Balanced Mode",
            "White",
            "⚪",
            "battery-profile-balanced",
            "Dynamic & Balanced",
            "Automatic thermal management, balanced fan curves for daily tasks.",
            "\033[1;37m" // Bright White
        },
        {
            "performance",
            "Performance Mode",
            "Red",
            "🔴",
            "battery-profile-performance",
            "High Clocks & Gaming",
            "High power limits, aggressive fan curve, optimal for gaming.",
            "\033[1;31m" // Bright Red
        },
        {
            "max-power",
            "Extreme / Max Power",
            "Purple",
            "🟣",
            "preferences-system-power-performance",
            "Max Wattage & Turbo Boost",
            "Maximum sustained power envelope (PL2 & GPU Boost unlocked), max fans.",
            "\033[1;35m" // Bright Magenta / Purple
        },
        {
            "custom",
            "Custom Profile",
            "Cyan",
            "🟠",
            "preferences-system-power",
            "User Custom Limits",
            "Custom fan curves, power limits, and thermal offsets.",
            "\033[1;36m" // Bright Cyan
        }
    };
    return modes;
}

inline ModeInfo get_mode_info(const std::string& id) {
    for (const auto& m : get_known_modes()) {
        if (m.id == id) {
            return m;
        }
    }
    // Fallback for unknown mode
    return {
        id,
        "Mode: " + id,
        "Unknown",
        "",
        "preferences-system-power",
        "Active Mode",
        "Custom or unrecognized platform profile.",
        "\033[1;33m" // Yellow
    };
}
