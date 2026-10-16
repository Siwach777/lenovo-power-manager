#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>
#include <sys/file.h>
#include <unistd.h>
#include "common.hpp"
#include "hardware.hpp"
#include "notifier.hpp"
#include "service.hpp"

namespace {

std::atomic<bool> g_keep_running(true);

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_keep_running.store(false);
    }
}

void print_help(const char* prog_name) {
    std::cout << "\033[1;36m=== Lenovo Legion Power Manager ===\033[0m\n\n"
              << "Usage: " << prog_name << " [COMMAND|OPTIONS]\n\n"
              << "\033[1mCommands:\033[0m\n"
              << "  status                 Display full hardware dashboard & active mode (default)\n"
              << "  current, get, -r       Print current raw mode string (e.g. max-power)\n"
              << "  short, -s              Print concise mode name and virtual LED color\n"
              << "  json, -j               Output status in JSON format\n"
              << "  list                   List all supported power profiles\n"
              << "  set <mode>             Switch power profile (e.g. performance, balanced)\n"
              << "  next                   Cycle to next power profile (Quiet -> Balanced -> Performance -> Max-Power)\n"
              << "  fan [status]           Display current fan speeds, min/max limits, and target\n"
              << "  fan set <rpm>          Safely set fan speed (1700 - 5300 RPM)\n"
              << "  fan auto               Restore automatic EC fan curve management\n"
              << "  fan max                Set fans to maximum safe speed (5300 RPM)\n"
              << "  watch, daemon          Run background watcher with auto-dismissing notifications\n"
              << "                         Options: [--timeout <ms>] [--no-extend-fnq]\n"
              << "  test-notify            Send an immediate test auto-dismissing notification & OSD\n"
              << "  install-service        Install and enable systemd user daemon (auto-start on boot)\n"
              << "  uninstall-service      Disable and remove systemd user daemon\n"
              << "  install-autostart      Install XDG autostart entry (~/.config/autostart)\n"
              << "  uninstall-autostart    Remove XDG autostart entry\n"
              << "  install-udev           Install udev rule for passwordless mode switching\n"
              << "  help, -h, --help       Show this help message\n\n"
              << "\033[1mPower Modes & Virtual LED Indicators:\033[0m\n"
              << "  \033[1;34m🔵 low-power\033[0m    - Quiet Mode        (Reduced TDP, silent acoustic profile)\n"
              << "  \033[1;37m⚪ balanced\033[0m     - Balanced Mode     (Intelligent dynamic fan & thermal curve)\n"
              << "  \033[1;31m🔴 performance\033[0m  - Performance Mode  (High clocks, aggressive cooling, high TDP)\n"
              << "  \033[1;35m🟣 max-power\033[0m    - Extreme Mode      (Peak wattage, 125W+ PL2 boost, maximum fans)\n"
              << "  \033[1;36m🟠 custom\033[0m       - Custom Profile    (User-defined limits & custom curves)\n";
}

void print_status_dashboard() {
    std::string current_id = read_current_mode();
    ModeInfo mode = get_mode_info(current_id);
    HardwareDetails hw = read_hardware_details();

    std::cout << "\033[1;36m┌────────────────────────────────────────────────────────┐\033[0m\n"
              << "\033[1;36m│          Lenovo Legion Pro 5 - Power Manager           │\033[0m\n"
              << "\033[1;36m└────────────────────────────────────────────────────────┘\033[0m\n\n";

    std::cout << "  \033[1mCurrent Mode:\033[0m    " << mode.terminal_color << mode.led_emoji << " " << mode.name 
              << " (" << mode.id << ")\033[0m\n";
    std::cout << "  \033[1mVirtual LED:\033[0m     " << mode.terminal_color << mode.led_color << " LED indicator\033[0m\n";
    std::cout << "  \033[1mDescription:\033[0m     " << mode.description << "\n";
    std::cout << "  \033[1mStatus:\033[0m          " << mode.summary << "\n\n";

    std::cout << "\033[1;33m  --- Hardware Telemetry ---\033[0m\n";
    
    // Fans
    std::cout << "  \033[1mFans:\033[0m            ";
    if (hw.fan1_rpm >= 0 && hw.fan2_rpm >= 0) {
        std::cout << "CPU: " << hw.fan1_rpm << " RPM  |  GPU: " << hw.fan2_rpm << " RPM\n";
    } else {
        std::cout << "Auto / Sensor idle\n";
    }

    // Power limits
    if (hw.ppt_pl1_spl > 0 || hw.ppt_pl2_sppt > 0 || hw.gpu_nv_ctgp > 0) {
        std::cout << "  \033[1mPower Limits:\033[0m    ";
        if (hw.ppt_pl1_spl > 0) std::cout << "PL1 (Sustained): " << hw.ppt_pl1_spl << "W  ";
        if (hw.ppt_pl2_sppt > 0) std::cout << "PL2 (Burst): " << hw.ppt_pl2_sppt << "W  ";
        if (hw.gpu_nv_ctgp > 0) std::cout << "GPU CTGP: " << hw.gpu_nv_ctgp << "W";
        if (hw.gpu_nv_ppab > 0) std::cout << " (+" << hw.gpu_nv_ppab << "W Boost)";
        std::cout << "\n";
    }

    // Conservation mode
    if (hw.conservation_mode >= 0) {
        std::cout << "  \033[1mConservation:\033[0m    " 
                  << (hw.conservation_mode == 1 ? "\033[1;32mEnabled (Preserving battery lifespan ~80%)\033[0m" : "\033[1;30mDisabled\033[0m") 
                  << "\n";
    }

    std::cout << "\n\033[90mTip: Fn+Q cycles Quiet -> Balanced -> Performance -> Extreme (max-power).\033[0m\n";
}

void print_fan_status() {
    FanDetails fan = read_fan_details();

    std::cout << "\033[1;36m┌────────────────────────────────────────────────────────┐\033[0m\n"
              << "\033[1;36m│          Lenovo Legion Pro 5 - Fan Controller          │\033[0m\n"
              << "\033[1;36m└────────────────────────────────────────────────────────┘\033[0m\n\n";

    std::cout << "  \033[1mCPU Fan:\033[0m          " << fan.fan1_rpm << " RPM "
              << "\033[90m(Floor: " << fan.fan1_min << " RPM, Ceiling: " << fan.fan1_max << " RPM)\033[0m\n";
    std::cout << "  \033[1mGPU Fan:\033[0m          " << fan.fan2_rpm << " RPM "
              << "\033[90m(Floor: " << fan.fan2_min << " RPM, Ceiling: " << fan.fan2_max << " RPM)\033[0m\n";

    std::cout << "  \033[1mControl Mode:\033[0m     ";
    if (fan.fan1_target == 0 && fan.fan2_target == 0) {
        std::cout << "\033[1;32mAutomatic (EC Managed Curve)\033[0m\n";
    } else {
        std::cout << "\033[1;33mManual Target (" << fan.fan1_target << " RPM)\033[0m\n";
    }

    std::cout << "\n\033[90mCommands:\033[0m\n"
              << "  legion-power fan set <rpm>  - Set manual fan speed safely\n"
              << "  legion-power fan auto       - Restore automatic EC management\n"
              << "  legion-power fan max        - Maximum safe cooling (5300 RPM)\n";
}

void print_json() {
    std::string current_id = read_current_mode();
    ModeInfo mode = get_mode_info(current_id);
    HardwareDetails hw = read_hardware_details();
    FanDetails fan = read_fan_details();

    std::cout << "{\n"
              << "  \"mode\": \"" << mode.id << "\",\n"
              << "  \"name\": \"" << mode.name << "\",\n"
              << "  \"led_color\": \"" << mode.led_color << "\",\n"
              << "  \"led_emoji\": \"" << mode.led_emoji << "\",\n"
              << "  \"summary\": \"" << mode.summary << "\",\n"
              << "  \"fan1_rpm\": " << hw.fan1_rpm << ",\n"
              << "  \"fan2_rpm\": " << hw.fan2_rpm << ",\n"
              << "  \"fan1_target\": " << fan.fan1_target << ",\n"
              << "  \"fan2_target\": " << fan.fan2_target << ",\n"
              << "  \"pl1_spl_watts\": " << hw.ppt_pl1_spl << ",\n"
              << "  \"pl2_sppt_watts\": " << hw.ppt_pl2_sppt << ",\n"
              << "  \"gpu_ctgp_watts\": " << hw.gpu_nv_ctgp << ",\n"
              << "  \"gpu_boost_watts\": " << hw.gpu_nv_ppab << ",\n"
              << "  \"conservation_mode\": " << (hw.conservation_mode == 1 ? "true" : "false") << "\n"
              << "}\n";
}

void print_list() {
    std::string current_id = read_current_mode();
    auto available = read_available_modes();

    std::cout << "\033[1mSupported Power Profiles:\033[0m\n";
    for (const auto& id : available) {
        ModeInfo info = get_mode_info(id);
        bool is_active = (id == current_id);
        std::cout << "  " << (is_active ? "\033[1;32m[● Active]\033[0m " : "  [      ]  ")
                  << info.terminal_color << info.led_emoji << " " << std::left << std::setw(15) << info.id << "\033[0m"
                  << " -> " << info.name << " (" << info.led_color << " LED) - " << info.summary << "\n";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string cmd = (argc > 1) ? argv[1] : "status";

    if (cmd == "status") {
        print_status_dashboard();
        return 0;
    } else if (cmd == "current" || cmd == "get" || cmd == "-r" || cmd == "--raw") {
        std::cout << read_current_mode() << "\n";
        return 0;
    } else if (cmd == "short" || cmd == "-s" || cmd == "--short") {
        ModeInfo mode = get_mode_info(read_current_mode());
        std::cout << mode.led_emoji << " " << mode.name << " [" << mode.led_color << "]\n";
        return 0;
    } else if (cmd == "json" || cmd == "-j" || cmd == "--json") {
        print_json();
        return 0;
    } else if (cmd == "list") {
        print_list();
        return 0;
    } else if (cmd == "fan") {
        std::string sub = (argc > 2) ? argv[2] : "status";
        if (sub == "status") {
            print_fan_status();
            return 0;
        } else if (sub == "auto" || sub == "0") {
            std::string msg;
            if (set_fan_auto(msg)) {
                std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
                PowerNotifier notifier(1500);
                ModeInfo info = {"custom", "Fan: Auto", "Cyan", "", "preferences-system-power", "Automatic EC fan control restored", "", ""};
                notifier.send_mode_notification(info);
                return 0;
            } else {
                std::cerr << "\033[1;31m[ERROR] " << msg << "\033[0m\n";
                return 1;
            }
        } else if (sub == "max") {
            std::string msg;
            if (set_fan_speed(5300, msg)) {
                std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
                PowerNotifier notifier(1500);
                ModeInfo info = {"custom", "Fan: Max (5300 RPM)", "Cyan", "", "preferences-system-power-performance", "Maximum safe fan cooling active", "", ""};
                notifier.send_mode_notification(info);
                return 0;
            } else {
                std::cerr << "\033[1;31m[ERROR] " << msg << "\033[0m\n";
                return 1;
            }
        } else {
            // Check if sub is a number or "set <rpm>"
            std::string rpm_str = sub;
            if (sub == "set" && argc > 3) {
                rpm_str = argv[3];
            }
            try {
                int target = std::stoi(rpm_str);
                std::string msg;
                if (set_fan_speed(target, msg)) {
                    std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
                    PowerNotifier notifier(1500);
                    ModeInfo info = {"custom", "Fan: " + std::to_string(target) + " RPM", "Cyan", "", "preferences-system-power", "Manual fan speed applied", "", ""};
                    notifier.send_mode_notification(info);
                    return 0;
                } else {
                    std::cerr << "\033[1;31m[ERROR] " << msg << "\033[0m\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Invalid fan command or RPM: " << sub << "\n";
                print_fan_status();
                return 1;
            }
        }
    } else if (cmd == "set") {
        if (argc < 3) {
            std::cerr << "Error: Missing mode argument. Usage: legion-power set <mode>\n";
            return 1;
        }
        std::string target = argv[2];
        std::string err;
        if (write_mode(target, err)) {
            ModeInfo new_info = get_mode_info(target);
            std::cout << "\033[1;32m[OK] Switched to " << new_info.led_emoji << " " << new_info.name 
                      << " (" << new_info.led_color << " LED)\033[0m\n";
            PowerNotifier notifier(1500);
            notifier.send_mode_notification(new_info);
            return 0;
        } else {
            std::cerr << "\033[1;31m[ERROR] " << err << "\033[0m\n";
            return 1;
        }
    } else if (cmd == "next") {
        std::string next_id = get_next_mode();
        std::string err;
        if (write_mode(next_id, err)) {
            ModeInfo new_info = get_mode_info(next_id);
            std::cout << "\033[1;32m[OK] Cycled to " << new_info.led_emoji << " " << new_info.name 
                      << " (" << new_info.led_color << " LED)\033[0m\n";
            PowerNotifier notifier(1500);
            notifier.send_mode_notification(new_info);
            return 0;
        } else {
            std::cerr << "\033[1;31m[ERROR] " << err << "\033[0m\n";
            return 1;
        }
    } else if (cmd == "watch" || cmd == "daemon") {
        // Single-instance lock to prevent duplicate watchers across systemd, XDG autostart, and manual runs
        std::string lock_path = "/tmp/legion-power-watcher-" + std::to_string(getuid()) + ".lock";
        int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd >= 0) {
            if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
                std::cout << "\033[1;33m[Legion Power Watcher]\033[0m Another watcher daemon is already active (PID lock held). Exiting cleanly.\n";
                close(lock_fd);
                return 0;
            }
        }

        bool extend_fnq = true;
        int timeout_ms = 1500; // 1.5 seconds default

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--no-extend-fnq") {
                extend_fnq = false;
            } else if (arg == "--timeout" && i + 1 < argc) {
                try {
                    timeout_ms = std::stoi(argv[++i]);
                } catch (...) {}
            }
        }

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::cout << "\033[1;32m[Legion Power Watcher]\033[0m Background watcher active.\n"
                  << "Fn+Q extended cycle: " << (extend_fnq ? "\033[1;32mEnabled (includes max-power)\033[0m" : "Disabled") << "\n"
                  << "Notification timeout: " << timeout_ms << "ms (auto-dismiss)\n"
                  << "Press Ctrl+C to stop.\n";

        PowerNotifier notifier(timeout_ms);
        std::string initial_mode = read_current_mode();
        ModeInfo init_info = get_mode_info(initial_mode);
        std::cout << "Active Mode: " << init_info.terminal_color << init_info.led_emoji << " " << init_info.name 
                  << " (" << init_info.led_color << ")\033[0m\n";

        watch_profile_changes(g_keep_running, extend_fnq, [&notifier](const std::string& new_mode, const std::string& old_mode) {
            ModeInfo info = get_mode_info(new_mode);
            std::cout << "\033[1;36m[Event]\033[0m " << old_mode << " -> " 
                      << info.terminal_color << info.led_emoji << " " << info.name 
                      << " (" << info.led_color << ")\033[0m\n";
            notifier.send_mode_notification(info, old_mode);
        });

        std::cout << "\033[1;33m[Legion Power Watcher]\033[0m Watcher stopped.\n";
        if (lock_fd >= 0) {
            close(lock_fd);
            unlink(lock_path.c_str());
        }
        return 0;
    } else if (cmd == "test-notify") {
        int timeout_ms = 1500;
        if (argc > 2) {
            try { timeout_ms = std::stoi(argv[2]); } catch (...) {}
        }
        std::cout << "Sending auto-dismissing notification (timeout: " << timeout_ms << "ms)...\n";
        PowerNotifier notifier(timeout_ms);
        std::string curr = read_current_mode();
        notifier.send_mode_notification(get_mode_info(curr));
        std::cout << "\033[1;32m[OK] Notification dispatched.\033[0m Notice that it automatically disappears!\n";
        return 0;
    } else if (cmd == "install-service") {
        std::string msg;
        if (ServiceManager::install_user_service(msg)) {
            std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
            return 0;
        } else {
            std::cerr << "\033[1;31m[ERROR] " << msg << "\033[0m\n";
            return 1;
        }
    } else if (cmd == "uninstall-service") {
        std::string msg;
        ServiceManager::uninstall_user_service(msg);
        std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
        return 0;
    } else if (cmd == "install-autostart") {
        std::string msg;
        if (ServiceManager::install_xdg_autostart(msg)) {
            std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
            return 0;
        } else {
            std::cerr << "\033[1;31m[ERROR] " << msg << "\033[0m\n";
            return 1;
        }
    } else if (cmd == "uninstall-autostart") {
        std::string msg;
        ServiceManager::uninstall_xdg_autostart(msg);
        std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
        return 0;
    } else if (cmd == "install-udev") {
        std::string msg;
        if (ServiceManager::install_udev_rule(msg)) {
            std::cout << "\033[1;32m[OK] " << msg << "\033[0m\n";
            return 0;
        } else {
            std::cerr << "\033[1;31m[ERROR] " << msg << "\033[0m\n";
            return 1;
        }
    } else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << cmd << "\n\n";
        print_help(argv[0]);
        return 1;
    }
}
