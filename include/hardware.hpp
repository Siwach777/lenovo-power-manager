#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include "common.hpp"

struct HardwareDetails {
    int fan1_rpm = -1;
    int fan2_rpm = -1;
    int ppt_pl1_spl = -1;       // PL1 sustained power limit (Watts)
    int ppt_pl2_sppt = -1;      // PL2 short power limit (Watts)
    int gpu_nv_ctgp = -1;       // GPU Configurable TGP (Watts)
    int gpu_nv_ppab = -1;       // GPU Dynamic Boost / PPAB (Watts)
    int conservation_mode = -1; // 1 = Battery conservation enabled (stops charging at 80%)
    int fan_mode = -1;
};

struct FanDetails {
    int fan1_rpm = -1;          // Current CPU fan RPM
    int fan2_rpm = -1;          // Current GPU fan RPM
    int fan1_min = 1700;        // Hardware floor (1700 RPM)
    int fan1_max = 5300;        // Hardware ceiling (5300 RPM)
    int fan2_min = 1700;        // Hardware floor (1700 RPM)
    int fan2_max = 5300;        // Hardware ceiling (5300 RPM)
    int fan1_target = 0;        // 0 = Auto / EC managed, >0 = Manual RPM target
    int fan2_target = 0;        // 0 = Auto / EC managed, >0 = Manual RPM target
};

// Fast sysfs operations
std::string read_current_mode();
std::vector<std::string> read_available_modes();
bool write_mode(const std::string& mode, std::string& err_msg);
std::string get_next_mode();
HardwareDetails read_hardware_details();

// Fan controls (focusing safely on fan speed)
FanDetails read_fan_details();
bool set_fan_speed(int target_rpm, std::string& msg);
bool set_fan_auto(std::string& msg);

// Mark that a manual set occurred so the watcher doesn't treat it as a hardware Fn+Q loop wrap
void record_manual_set();

// Zero-CPU event-driven watcher using poll() on sysfs with 400ms fallback heartbeat
// If extend_fnq is true, hardware Fn+Q wraps from performance -> max-power -> low-power
void watch_profile_changes(
    const std::atomic<bool>& keep_running,
    bool extend_fnq,
    const std::function<void(const std::string& new_mode, const std::string& old_mode)>& on_change
);
