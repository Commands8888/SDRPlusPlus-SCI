#include <config.h>
#include <utils/flog.h>
#include <fstream>

#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#endif

ConfigManager::ConfigManager() {
}

ConfigManager::~ConfigManager() {
    disableAutoSave();
}

void ConfigManager::setPath(std::string file) {
    path = std::filesystem::absolute(file).string();
}

void ConfigManager::load(json def, bool lock) {
    std::unique_lock<std::mutex> guard(mtx, std::defer_lock);
    if (lock) { guard.lock(); }
    conf = def;
    if (path == "") {
        flog::error("Config manager tried to load file with no path specified");
        return;
    }
    if (!std::filesystem::exists(path)) {
        flog::warn("Config file '{0}' does not exist, creating it", path);
        conf = def;
        save(false);
    }
    if (!std::filesystem::is_regular_file(path)) {
        flog::error("Config file '{0}' isn't a file", path);
        return;
    }

    try {
        std::ifstream file(path.c_str());
        json loaded;
        file >> loaded;
        if (def.is_object() && !loaded.is_object()) {
            throw std::runtime_error("expected a JSON object");
        }
        conf = std::move(loaded);
        file.close();
    }
    catch (const std::exception& e) {
        flog::error("Config file '{}' is corrupted, resetting it: {}", path, e.what());
        conf = def;
        save(false);
    }
}

void ConfigManager::save(bool lock) {
    std::unique_lock<std::mutex> guard(mtx, std::defer_lock);
    if (lock) { guard.lock(); }
    try {
        const std::string data = conf.dump(4);
#ifndef _WIN32
        // Write a private sibling file and atomically replace the old config.
        // A force-close during autosave must not leave a truncated JSON file.
        auto target = std::filesystem::path(path);
        if (std::filesystem::is_symlink(target)) {
            auto link = std::filesystem::read_symlink(target);
            target = std::filesystem::weakly_canonical(link.is_absolute() ? link : target.parent_path() / link);
        }
        std::string pattern = target.string() + ".tmp.XXXXXX";
        std::vector<char> temp(pattern.begin(),pattern.end());
        temp.push_back('\0');
        const int fd = mkstemp(temp.data());
        if (fd < 0) {
            flog::error("Could not save config '{}': {}", path, std::strerror(errno));
            return;
        }
        struct stat previous;
        if (stat(target.c_str(), &previous) == 0) { fchmod(fd, previous.st_mode & 0777); }
        FILE* file = fdopen(fd, "w");
        if (!file) {
            close(fd);
            std::remove(temp.data());
            flog::error("Could not open temporary config for '{}'", path);
            return;
        }
        bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
        if (std::fclose(file) != 0) { written = false; }
        if (!written || std::rename(temp.data(), target.c_str()) != 0) {
            flog::error("Could not replace config '{}': {}", path, std::strerror(errno));
            std::remove(temp.data());
        }
#else
        std::ofstream file(path.c_str());
        file << data;
        if (!file) { flog::error("Could not save config '{}'", path); }
#endif
    }
    catch (const std::exception& e) {
        flog::error("Could not save config '{}': {}", path, e.what());
    }
}

void ConfigManager::enableAutoSave() {
    if (autoSaveEnabled) { return; }
    autoSaveEnabled = true;
    {
        std::lock_guard<std::mutex> lock(termMtx);
        termFlag = false;
    }
    autoSaveThread = std::thread(&ConfigManager::autoSaveWorker, this);
}

void ConfigManager::disableAutoSave() {
    if (!autoSaveEnabled) { return; }
    {
        std::lock_guard<std::mutex> lock(termMtx);
        autoSaveEnabled = false;
        termFlag = true;
    }
    termCond.notify_one();
    if (autoSaveThread.joinable()) { autoSaveThread.join(); }
}

void ConfigManager::acquire() {
    mtx.lock();
}

void ConfigManager::release(bool modified) {
    changed |= modified;
    mtx.unlock();
}

void ConfigManager::autoSaveWorker() {
    while (autoSaveEnabled) {
        if (mtx.try_lock()) {
            if (changed) {
                changed = false;
                save(false);
            }
            mtx.unlock();
        }

        // Also interruptible when the config is busy: shutdown must not wait
        // through a fixed one-second polling sleep for each module.
        {
            std::unique_lock<std::mutex> lock(termMtx);
            termCond.wait_for(lock, std::chrono::milliseconds(1000), [this]() { return termFlag; });
        }
    }
}
