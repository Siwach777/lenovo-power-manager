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

bool write_sysfs_profile(const std::string& mode, std::string& err_msg) {
    g_last_self_write_ms.store(get_current_time_ms());

    int fd = open(SYSFS_PLATFORM_PROFILE, O_WRONLY);
    if (fd >= 0) {
        std::string payload = mode + "\n";
        ssize_t written = write(fd, payload.c_str(), payload.size());
        close(fd);
        if (written == static_cast<ssize_t>(payload.size())) {
            return true;
        }
    }

    if (errno == EACCES || errno == EPERM || fd < 0) {
        const char* helper = (isatty(STDIN_FILENO) && std::getenv("SUDO_USER") == nullptr) ? "pkexec" : "sudo";
        std::string cmd = std::string(helper) + " sh -c 'echo \"" + mode + "\" > " + SYSFS_PLATFORM_PROFILE + "'";
        int ret = std::system(cmd.c_str());
        if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
            return true;
        }
        err_msg = "Permission denied writing to " + std::string(SYSFS_PLATFORM_PROFILE) + 
                  ". Run with sudo/pkexec or run ./install.sh to apply udev permissions.";
        return false;
    }

    err_msg = "Failed to write to " + std::string(SYSFS_PLATFORM_PROFILE) + ": " + std::strerror(errno);
    return false;
}

} // namespace

void record_manual_set() {
    g_last_self_write_ms.store(get_current_time_ms());
    std::string path = "/tmp/legion-power-manual-set-" + std::to_string(getuid());
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
}

bool is_recent_manual_set() {
    std::string path = "/tmp/legion-power-manual-set-" + std::to_string(getuid());
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        int64_t file_ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + (st.st_mtim.tv_nsec / 1000000);
        int64_t diff = now_ms - file_ms;
        if (diff >= 0 && diff < 600) {
            return true;
        }
    }
    return false;
}

std::string read_current_mode() {
    int fd = open(SYSFS_PLATFORM_PROFILE, O_RDONLY);
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
    int fd = open(SYSFS_PLATFORM_PROFILE_CHOICES, O_RDONLY);
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
    int fd = open(SYSFS_PLATFORM_PROFILE, O_RDONLY);
    if (fd < 0) {
        std::cerr << "[Error] Cannot open " << SYSFS_PLATFORM_PROFILE << " for monitoring: " << std::strerror(errno) << "\n";
        return;
    }

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    std::string current_mode = (n > 0) ? trim(std::string(buf, n)) : "balanced";

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLPRI | POLLERR;

    int64_t last_event_ms = 0;

    while (keep_running.load()) {
        int ret = poll(&pfd, 1, 200);

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Warning] poll() error: " << std::strerror(errno) << "\n";
            break;
        }

        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            close(fd);
            fd = open(SYSFS_PLATFORM_PROFILE, O_RDONLY);
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

            // 2. Ignore sysfs events triggered by our own writes within 150ms
            if ((now_ms - g_last_self_write_ms.load()) < 150) {
                continue;
            }

            // 3. Debounce hardware keypress bursts (< 150ms)
            if ((now_ms - last_event_ms) < 150) {
                continue;
            }
            last_event_ms = now_ms;

            std::string old_mode = current_mode;
            std::string target_mode = detected;

            bool manual_set = is_recent_manual_set();

            if (extend_fnq && !manual_set) {
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
                }

                // Synchronize hardware sysfs profile if it differs from the deterministic target
                if (detected != target_mode) {
                    std::string err;
                    write_sysfs_profile(target_mode, err);
                }
            }

            current_mode = target_mode;
            on_change(target_mode, old_mode);
        }
    }

    if (fd >= 0) close(fd);
}
