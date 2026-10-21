#include "hardware.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>

namespace {

const char* SYSFS_PLATFORM_PROFILE = "/sys/firmware/acpi/platform_profile";
const char* SYSFS_PLATFORM_PROFILE_CHOICES = "/sys/firmware/acpi/platform_profile_choices";
const char* SYSFS_CONSERVATION_MODE = "/sys/devices/pci0000:00/0000:00:1f.0/PNP0C09:00/VPC2004:00/conservation_mode";
const char* SYSFS_FAN_MODE = "/sys/devices/pci0000:00/0000:00:1f.0/PNP0C09:00/VPC2004:00/fan_mode";
const char* SYSFS_FW_ATTR_DIR = "/sys/class/firmware-attributes/lenovo-wmi-other-0/attributes/";

static std::atomic<int64_t> g_last_self_write_ms{0};
static std::atomic<int64_t> g_last_fnq_event_ms{0};

int64_t get_current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

std::string trim(const std::string& str) {
    auto first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

int read_int_from_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    try {
        return std::stoi(trim(buf));
    } catch (...) {
        return -1;
    }
}

std::string find_lenovo_hwmon_path() {
    static std::string cached_path;
    if (!cached_path.empty()) return cached_path;

    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return "";

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string base = std::string("/sys/class/hwmon/") + entry->d_name;
        std::string name_file = base + "/name";
        int fd = open(name_file.c_str(), O_RDONLY);
        if (fd >= 0) {
            char buf[64];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                if (trim(buf) == "lenovo_wmi_other") {
                    cached_path = base;
                    closedir(dir);
                    return cached_path;
                }
            }
        }
    }
    closedir(dir);
    if (access("/sys/class/hwmon/hwmon3/fan1_input", R_OK) == 0) {
        cached_path = "/sys/class/hwmon/hwmon3";
    }
    return cached_path;
}

std::string find_platform_profile_path() {
    static std::string cached_path;
    if (!cached_path.empty()) return cached_path;

    // Check modern per-device class sysfs: /sys/class/platform-profile/platform-profile-*/profile
    DIR* dir = opendir("/sys/class/platform-profile");
    if (dir) {
        struct dirent* entry;
        std::string fallback_profile;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string base = std::string("/sys/class/platform-profile/") + entry->d_name;
            std::string name_file = base + "/name";
            std::string prof_file = base + "/profile";
            if (access(prof_file.c_str(), F_OK) != 0) continue;

            int fd = open(name_file.c_str(), O_RDONLY);
            if (fd >= 0) {
                char buf[64];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string dev_name = trim(buf);
                    if (dev_name == "lenovo-wmi-gamezone" || dev_name.find("lenovo") != std::string::npos) {
                        cached_path = prof_file;
                        closedir(dir);
                        return cached_path;
                    }
                }
            }
            if (fallback_profile.empty()) {
                fallback_profile = prof_file;
            }
        }
        closedir(dir);
        if (!fallback_profile.empty()) {
            cached_path = fallback_profile;
            return cached_path;
        }
    }

    // Fall back to legacy aggregate ACPI path
    cached_path = SYSFS_PLATFORM_PROFILE;
    return cached_path;
}

std::string find_platform_profile_choices_path() {
    static std::string cached_path;
    if (!cached_path.empty()) return cached_path;

    std::string prof = find_platform_profile_path();
    if (prof.size() > 8 && prof.substr(prof.size() - 8) == "/profile") {
        std::string choices_file = prof.substr(0, prof.size() - 8) + "/choices";
        if (access(choices_file.c_str(), R_OK) == 0) {
            cached_path = choices_file;
            return cached_path;
        }
    }

    if (access(SYSFS_PLATFORM_PROFILE_CHOICES, R_OK) == 0) {
        cached_path = SYSFS_PLATFORM_PROFILE_CHOICES;
        return cached_path;
    }

    cached_path = prof;
    return cached_path;
}

bool write_sysfs_profile(const std::string& mode, std::string& err_msg) {
    std::string primary_path = find_platform_profile_path();
    const char* legacy_path = SYSFS_PLATFORM_PROFILE;

    // 1. Direct write attempt to the primary target file (e.g. /sys/class/platform-profile/platform-profile-0/profile)
    int fd = open(primary_path.c_str(), O_WRONLY);
    if (fd >= 0) {
        std::string payload = mode + "\n";
        ssize_t written = write(fd, payload.c_str(), payload.size());
        close(fd);
        if (written == static_cast<ssize_t>(payload.size())) {
            g_last_self_write_ms.store(get_current_time_ms());
            return true;
        }
    }

    // 2. If primary target file was not directly writable without root, and mode != "custom",
    // check if legacy aggregate path is directly writable (since it may already have 0666 permissions).
    // NOTE: Writing "custom" to legacy aggregate /sys/firmware/acpi/platform_profile returns -EINVAL
    // in Linux kernel 6.13+ because aggregate sysfs explicitly rejects PLATFORM_PROFILE_CUSTOM.
    if (mode != "custom" && primary_path != legacy_path) {
        int fd_leg = open(legacy_path, O_WRONLY);
        if (fd_leg >= 0) {
            std::string payload = mode + "\n";
            ssize_t written = write(fd_leg, payload.c_str(), payload.size());
            close(fd_leg);
            if (written == static_cast<ssize_t>(payload.size())) {
                g_last_self_write_ms.store(get_current_time_ms());
                return true;
            }
        }
    }

    // 3. Fallback to privilege escalation (pkexec or sudo) to write to primary_path
    if (errno == EACCES || errno == EPERM || fd < 0) {
        const char* helper = (isatty(STDIN_FILENO) && std::getenv("SUDO_USER") == nullptr) ? "pkexec" : "sudo";
        std::string cmd = std::string(helper) + " sh -c 'echo \"" + mode + "\" > " + primary_path + "'";
        int ret = std::system(cmd.c_str());
        if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
            g_last_self_write_ms.store(get_current_time_ms());
            return true;
        }
        err_msg = "Permission denied writing to " + primary_path + 
                  ". Run with sudo/pkexec or run ./install.sh to apply udev and tmpfiles permissions.";
        return false;
    }

    err_msg = "Failed to write to " + primary_path + ": " + std::strerror(errno);
    return false;
}

} // namespace

void record_manual_set() {
    g_last_self_write_ms.store(get_current_time_ms());
    std::string uid_path = "/tmp/legion-power-manual-set-" + std::to_string(getuid());
    int fd1 = open(uid_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd1 >= 0) {
        fchmod(fd1, 0666);
        close(fd1);
    }
    int fd2 = open("/tmp/legion-power-manual-set", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd2 >= 0) {
        fchmod(fd2, 0666);
        close(fd2);
    }
}

bool is_recent_manual_set() {
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    auto check_file = [&](const std::string& path) -> bool {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            int64_t file_ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + (st.st_mtim.tv_nsec / 1000000);
            int64_t diff = now_ms - file_ms;
            if (diff >= 0 && diff < 1500) {
                return true;
            }
        }
        return false;
    };

    if (check_file("/tmp/legion-power-manual-set")) return true;
    if (check_file("/tmp/legion-power-manual-set-" + std::to_string(getuid()))) return true;
    if (check_file("/tmp/legion-power-manual-set-0")) return true;

    return false;
}

std::string read_current_mode() {
    std::string path = find_platform_profile_path();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0 && path != SYSFS_PLATFORM_PROFILE) {
        fd = open(SYSFS_PLATFORM_PROFILE, O_RDONLY);
    }
    if (fd < 0) return "unknown";
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "unknown";
    buf[n] = '\0';
    return trim(buf);
}

std::vector<std::string> read_available_modes() {
    std::vector<std::string> modes;
    std::string path = find_platform_profile_choices_path();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0 && path != SYSFS_PLATFORM_PROFILE_CHOICES) {
        fd = open(SYSFS_PLATFORM_PROFILE_CHOICES, O_RDONLY);
    }
    if (fd < 0) {
        return {"low-power", "balanced", "performance", "max-power", "custom"};
    }
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return modes;
    buf[n] = '\0';

    std::stringstream ss(buf);
    std::string item;
    while (ss >> item) {
        modes.push_back(trim(item));
    }
    return modes;
}

bool write_mode(const std::string& mode, std::string& err_msg) {
    record_manual_set();
    return write_sysfs_profile(mode, err_msg);
}

std::string get_next_mode() {
    std::string curr = read_current_mode();
    if (curr == "low-power") return "balanced";
    if (curr == "balanced") return "performance";
    if (curr == "performance") return "max-power";
    if (curr == "max-power") return "low-power";
    if (curr == "custom") return "low-power";
    return "balanced";
}

HardwareDetails read_hardware_details() {
    HardwareDetails details;
    std::string hwmon = find_lenovo_hwmon_path();
    if (!hwmon.empty()) {
        details.fan1_rpm = read_int_from_file(hwmon + "/fan1_input");
        details.fan2_rpm = read_int_from_file(hwmon + "/fan2_input");
    }

    std::string fw = SYSFS_FW_ATTR_DIR;
    details.ppt_pl1_spl = read_int_from_file(fw + "ppt_pl1_spl/current_value");
    details.ppt_pl2_sppt = read_int_from_file(fw + "ppt_pl2_sppt/current_value");
    details.gpu_nv_ctgp = read_int_from_file(fw + "gpu_nv_ctgp/current_value");
    details.gpu_nv_ppab = read_int_from_file(fw + "gpu_nv_ppab/current_value");

    details.conservation_mode = read_int_from_file(SYSFS_CONSERVATION_MODE);
    details.fan_mode = read_int_from_file(SYSFS_FAN_MODE);
    return details;
}

FanDetails read_fan_details() {
    FanDetails f;
    std::string hwmon = find_lenovo_hwmon_path();
    if (!hwmon.empty()) {
        f.fan1_rpm = read_int_from_file(hwmon + "/fan1_input");
        f.fan2_rpm = read_int_from_file(hwmon + "/fan2_input");
        int min1 = read_int_from_file(hwmon + "/fan1_min");
        int max1 = read_int_from_file(hwmon + "/fan1_max");
        int min2 = read_int_from_file(hwmon + "/fan2_min");
        int max2 = read_int_from_file(hwmon + "/fan2_max");
        if (min1 > 0) f.fan1_min = min1;
        if (max1 > 0) f.fan1_max = max1;
        if (min2 > 0) f.fan2_min = min2;
        if (max2 > 0) f.fan2_max = max2;
        f.fan1_target = read_int_from_file(hwmon + "/fan1_target");
        f.fan2_target = read_int_from_file(hwmon + "/fan2_target");
    }
    return f;
}

bool set_fan_auto(std::string& msg) {
    std::string hwmon = find_lenovo_hwmon_path();
    if (hwmon.empty()) {
        msg = "Lenovo fan controller (hwmon) not found.";
        return false;
    }
    std::string f1 = hwmon + "/fan1_target";
    std::string f2 = hwmon + "/fan2_target";

    int fd1 = open(f1.c_str(), O_WRONLY);
    int fd2 = open(f2.c_str(), O_WRONLY);
    if (fd1 >= 0 && fd2 >= 0) {
        write(fd1, "0\n", 2);
        write(fd2, "0\n", 2);
        close(fd1);
        close(fd2);
        msg = "Automatic fan curve restored (EC managed).";
        return true;
    }
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);

    const char* helper = (isatty(STDIN_FILENO) && std::getenv("SUDO_USER") == nullptr) ? "pkexec" : "sudo";
    std::string cmd = std::string(helper) + " sh -c 'echo 0 > " + f1 + " && echo 0 > " + f2 + "'";
    int ret = std::system(cmd.c_str());
    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
        msg = "Automatic fan curve restored (EC managed).";
        return true;
    }
    msg = "Permission denied restoring automatic fan curve.";
    return false;
}

bool set_fan_speed(int target_rpm, std::string& msg) {
    FanDetails fan = read_fan_details();
    std::string hwmon = find_lenovo_hwmon_path();
    if (hwmon.empty()) {
        msg = "Lenovo fan controller (hwmon) not found on this system.";
        return false;
    }

    if (target_rpm == 0) {
        return set_fan_auto(msg);
    }

    int min_limit = std::max(1700, std::max(fan.fan1_min, fan.fan2_min));
    int max_limit = std::min(5300, std::min(fan.fan1_max, fan.fan2_max));

    if (target_rpm < min_limit) {
        msg = "Target RPM (" + std::to_string(target_rpm) + ") is below hardware safe minimum (" + 
              std::to_string(min_limit) + " RPM). Fans cannot be stopped for hardware protection.";
        return false;
    }

    if (target_rpm > max_limit) {
        target_rpm = max_limit;
    }

    std::string f1 = hwmon + "/fan1_target";
    std::string f2 = hwmon + "/fan2_target";

    std::string val = std::to_string(target_rpm) + "\n";
    int fd1 = open(f1.c_str(), O_WRONLY);
    int fd2 = open(f2.c_str(), O_WRONLY);

    if (fd1 >= 0 && fd2 >= 0) {
        write(fd1, val.c_str(), val.size());
        write(fd2, val.c_str(), val.size());
        close(fd1);
        close(fd2);
        msg = "Fan speed target safely set to " + std::to_string(target_rpm) + " RPM.";
        return true;
    }
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);

    const char* helper = (isatty(STDIN_FILENO) && std::getenv("SUDO_USER") == nullptr) ? "pkexec" : "sudo";
    std::string cmd = std::string(helper) + " sh -c 'echo " + std::to_string(target_rpm) + " > " + f1 + 
                      " && echo " + std::to_string(target_rpm) + " > " + f2 + "'";
    int ret = std::system(cmd.c_str());
    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
        msg = "Fan speed target safely set to " + std::to_string(target_rpm) + " RPM.";
        return true;
    }

    msg = "Permission denied setting fan speed.";
    return false;
}

void watch_profile_changes(
    const std::atomic<bool>& keep_running,
    bool extend_fnq,
    const std::function<void(const std::string& new_mode, const std::string& old_mode)>& on_change
) {
    std::string watch_path = find_platform_profile_path();
    int fd = open(watch_path.c_str(), O_RDONLY);
    if (fd < 0 && watch_path != SYSFS_PLATFORM_PROFILE) {
        watch_path = SYSFS_PLATFORM_PROFILE;
        fd = open(watch_path.c_str(), O_RDONLY);
    }
    if (fd < 0) {
        std::cerr << "[Error] Cannot open " << watch_path << " for monitoring: " << std::strerror(errno) << "\n";
        return;
    }

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    std::string current_mode = (n > 0) ? trim(std::string(buf, n)) : "balanced";

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLPRI | POLLERR;

    int64_t last_event_ms = 0;
    int64_t last_max_power_entry_ms = 0;

    while (keep_running.load()) {
        int ret = poll(&pfd, 1, 200);

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Warning] poll() error: " << std::strerror(errno) << "\n";
            break;
        }

        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            close(fd);
            fd = open(watch_path.c_str(), O_RDONLY);
            if (fd < 0) break;
            pfd.fd = fd;
        }

        n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::string detected = trim(std::string(buf, n));
            if (detected.empty()) continue;

            int64_t now_ms = get_current_time_ms();

            // 1. If sysfs state hasn't changed, ignore (or self-write echo)
            if (detected == current_mode) {
                continue;
            }

            // 2. Ignore sysfs events triggered by our own writes within 200ms
            if ((now_ms - g_last_self_write_ms.load()) < 200) {
                continue;
            }

            // 3. Hardware EC bounce guard:
            // When entering max-power, Lenovo Legion EC microcode natively defaults to low-power (Quiet mode)
            // upon the physical Fn+Q keystroke. When overridden to max-power, the EC's internal hardware
            // routine finishes 150-500ms later and may emit a trailing bounce back to low-power.
            // If this trailing bounce event arrives while in max-power within 600ms, suppress the bounce,
            // re-assert max-power to sysfs, and prevent the state machine from dropping to low-power.
            if (extend_fnq && current_mode == "max-power" && detected == "low-power" && (now_ms - last_max_power_entry_ms) < 600) {
                std::string err;
                write_sysfs_profile("max-power", err);
                continue;
            }

            // 4. Debounce hardware keypress bursts (< 200ms)
            if ((now_ms - last_event_ms) < 200) {
                continue;
            }
            last_event_ms = now_ms;

            std::string old_mode = current_mode;
            std::string target_mode = detected;

            bool manual_set = is_recent_manual_set();

            // 5. Custom mode handling vs Hardware Fn+Q cycle:
            // The physical Fn+Q hardware keystroke on Lenovo Legion firmware NEVER triggers "custom".
            // Custom mode can ONLY be activated via software/manual set.
            // Therefore, if detected is "custom", it is ALWAYS an intentional external set and
            // must NEVER be overridden by the Fn+Q cycle state machine.
            if (detected == "custom") {
                target_mode = "custom";
            } else if (extend_fnq && !manual_set) {
                // Deterministic 4-stage hardware cycle:
                // Quiet (low-power) -> Balanced (balanced) -> Performance (performance) -> Extreme (max-power) -> Quiet (low-power)
                if (old_mode == "low-power") {
                    target_mode = "balanced";
                } else if (old_mode == "balanced") {
                    target_mode = "performance";
                } else if (old_mode == "performance") {
                    target_mode = "max-power";
                } else if (old_mode == "max-power") {
                    target_mode = "low-power";
                } else if (old_mode == "custom") {
                    // When in Custom mode and user presses physical Fn+Q, cycle to Quiet (low-power)
                    target_mode = "low-power";
                }

                // Synchronize hardware sysfs profile if it differs from the deterministic target
                if (detected != target_mode) {
                    std::string err;
                    if (!write_sysfs_profile(target_mode, err)) {
                        std::cerr << "[Watcher Error] Failed to write " << target_mode 
                                  << " to " << watch_path << ": " << err << "\n";
                        current_mode = detected;
                        continue;
                    }
                }
            }

            if (target_mode == "max-power") {
                last_max_power_entry_ms = now_ms;
            }

            current_mode = target_mode;
            on_change(target_mode, old_mode);
        }
    }

    if (fd >= 0) close(fd);
}
