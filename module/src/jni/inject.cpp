#include "inject.h"

#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "config.h"
#include "log.h"
#include "child_gating.h"
#include "xdl.h"
#include "remapper.h"

static bool is_target_uid(uid_t target_uid) {
    return getuid() == target_uid;
}

static uid_t get_app_uid(const std::string &app_name) {
    std::ifstream status_file("/proc/self/status");
    std::string line;
    while (std::getline(status_file, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            std::istringstream iss(line.substr(4));
            uid_t uid;
            iss >> uid;
            return uid;
        }
    }
    return (uid_t)-1;
}
static void wait_for_init(const std::string &app_name, uid_t target_uid) {
    LOGI("Wait for process to complete init");

    while (getuid() != target_uid) {
        LOGI("Still in zygote phase, current UID: %d, waiting for target UID: %d", getuid(), target_uid);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 安全緩衝
    // additional tolerance for the init to complete after process rename
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    LOGI("Process init completed for app: %s (UID matched: %d)", app_name.c_str(), target_uid);
}

static void delay_start_up(uint64_t start_up_delay_ms) {
    if (start_up_delay_ms <= 0) {
        return;
    }

    LOGI("Waiting for configured start up delay %" PRIu64"ms", start_up_delay_ms);

    int countdown = 0;
    uint64_t delay = start_up_delay_ms;

    for (int i = 0; i < 10 && delay > 1000; i++) {
        delay -= 1000;
        countdown++;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(delay));

    for (int i = countdown; i > 0; i--) {
        LOGI("Injecting libs in %d seconds", i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void inject_lib(std::string const &lib_path, std::string const &logContext) {
    auto *handle = xdl_open(lib_path.c_str(), XDL_TRY_FORCE_LOAD);
    if (handle) {
        LOGI("%sInjected %s with handle %p", logContext.c_str(), lib_path.c_str(), handle);
        remap_lib(lib_path);
        return;
    }

    auto xdl_err = dlerror();

    handle = dlopen(lib_path.c_str(), RTLD_NOW);
    if (handle) {
        LOGI("%sInjected %s with handle %p (dlopen)", logContext.c_str(), lib_path.c_str(), handle);
        remap_lib(lib_path);
        return;
    }

    auto dl_err = dlerror();

    LOGE("%sFailed to inject %s (xdl_open): %s", logContext.c_str(), lib_path.c_str(), xdl_err);
    LOGE("%sFailed to inject %s (dlopen): %s", logContext.c_str(), lib_path.c_str(), dl_err);
}

static void inject_libs(target_config const &cfg) {
    // We need to wait for process initialization to complete.
    // Loading the gadget before that will freeze the process
    // before the init has completed. This make the process
    // undiscoverable or otherwise cause issue attaching.
    wait_for_init(cfg.app_name,getuid());

    if (cfg.child_gating.enabled) {
        enable_child_gating(cfg.child_gating);
    }

    delay_start_up(cfg.start_up_delay_ms);

    for (auto &lib_path : cfg.injected_libraries) {
        LOGI("Injecting %s", lib_path.c_str());
        inject_lib(lib_path, "");
    }
}

bool check_and_inject(std::string const &app_name) {
    std::string module_dir = std::string("/data/local/tmp/re.zyg.fri");

    std::optional<target_config> cfg = load_config(module_dir, app_name);
    if (!cfg.has_value()) {
        return false;
    }

    LOGI("App detected: %s", app_name.c_str());
    LOGD("Config app_name: %s", cfg->app_name.c_str())

    uid_t app_uid  = get_app_uid(cfg->app_name);
    LOGI("App UID detected: %d", app_uid);

    LOGI("PID: %d", getpid());


    auto target_config = cfg.value();
    if (!target_config.enabled) {
        LOGI("Injection disabled for %s", app_name.c_str());
        return false;
    }

//    std::thread inject_thread(inject_libs, target_config);
//    inject_thread.detach();
    std::thread inject_thread([target_config, app_uid]() {
        wait_for_init(target_config.app_name, app_uid);
        inject_libs(target_config);
    });
    inject_thread.detach();

    return true;
}
