#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <config.h>

static void checkUnlocked(ConfigManager& config) {
    std::atomic<bool> unlocked{false};
    std::thread probe([&] { config.acquire(); unlocked=true; config.release(); });
    for (int i = 0; i < 100 && !unlocked.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!unlocked.load()) {
        std::fputs("FAIL: config load left its mutex locked on an error path\n",stderr);
        std::_Exit(2);
    }
    probe.join();
}

int main() {
    namespace fs = std::filesystem;
    char pattern[] = "/tmp/sdrpp-config-test-XXXXXX";
    const fs::path root = mkdtemp(pattern);
    assert(!root.empty());
    const json defaults = {{"sequence",0},{"mode","DSD"}};
    ConfigManager config;
    config.load(defaults);
    checkUnlocked(config);
    config.setPath(root.string());
    config.load(defaults);
    checkUnlocked(config);
    const auto path = root / "config.json";
    config.setPath(path.string());
    config.load(defaults);
    assert(config.conf == defaults);
    for (const char* broken : {"{broken", "[]", "null"}) {
        { std::ofstream file(path); file << broken; }
        config.load(defaults);
        assert(config.conf == defaults);
        checkUnlocked(config);
    }
    // Readers must see a whole old or new JSON object during a save.
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) {
            std::ifstream file(path);
            json snapshot;
            file >> snapshot;
            assert(snapshot.at("sequence").is_number_integer());
        }
    });
    for (int i = 1; i <= 150; ++i) {
        config.acquire();
        config.conf["sequence"] = i;
        config.release(true);
        config.save();
    }
    done = true;
    reader.join();
    const auto link = root / "linked.json";
    fs::create_symlink("config.json", link);
    config.setPath(link.string());
    config.conf["sequence"] = 151;
    config.save();
    assert(fs::is_symlink(link));
    { std::ifstream file(path); json saved; file >> saved; assert(saved["sequence"] == 151); }
    // Serialization failure must preserve the last readable config.
    config.conf["mode"] = std::string(1, static_cast<char>(0xFF));
    config.save();
    { std::ifstream file(path); json saved; file >> saved; assert(saved["mode"] == "DSD"); }
    config.conf["mode"] = "DSD";
    config.acquire();
    config.enableAutoSave();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto stopStart = std::chrono::steady_clock::now();
    config.disableAutoSave();
    const auto stopTime = std::chrono::steady_clock::now()-stopStart;
    config.release();
    assert(stopTime < std::chrono::milliseconds(250));
    for (int cycle = 0; cycle < 4; ++cycle) {
        config.enableAutoSave();
        for (int i = 0; i < 500; ++i) {
            config.acquire();
            config.conf["sequence"] = i;
            config.release(true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        config.disableAutoSave();
        checkUnlocked(config);
    }
    for (const auto& item : fs::directory_iterator(root))
        assert(item.path().filename().string().find(".tmp.") == std::string::npos);
    fs::remove_all(root);
    std::puts("OK: config errors unlock, invalid files recover, atomic saves preserve JSON/symlinks, autosave joins promptly even when busy");
}
