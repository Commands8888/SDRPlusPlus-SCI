#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <gui/colormaps.h>
#include <gui/theme_manager.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    namespace fs = std::filesystem;
    ThemeManager themes;
    size_t loaded = 0;
    for (const auto& item : fs::directory_iterator(fs::path(argv[1]) / "colormaps")) {
        if (item.path().extension() != ".json") continue;
        const auto before = colormaps::maps.size();
        colormaps::loadMap(item.path());
        assert(colormaps::maps.size() == before+1);
        ++loaded;
    }
    assert(loaded > 0);
    for (const auto& item : fs::directory_iterator(fs::path(argv[1]) / "themes"))
        if (item.path().extension() == ".json") assert(themes.loadTheme(item.path()));
    const size_t themeCount = themes.getThemeNames().size();
    assert(themeCount > 0);
    char pattern[] = "/tmp/sdrpp-resource-test-XXXXXX";
    const fs::path root = mkdtemp(pattern);
    assert(!root.empty());
    const auto path = root / "bad.json";
    for (const char* bad : {"{", "[]", "null", "{\"name\":5}",
        "{\"name\":\"bad\",\"author\":\"test\",\"map\":[]}",
        "{\"name\":\"bad\",\"author\":\"test\",\"map\":[\"#GGFFFF\",\"#FFFFFF\"]}",
        "{\"name\":\"bad\",\"Text\":\"\"}",
        "{\"name\":\"bad\",\"Text\":42}"}) {
        { std::ofstream file(path); file << bad; }
        colormaps::loadMap(path);
        assert(colormaps::maps.size() == loaded);
        assert(!themes.loadTheme(path));
        assert(themes.getThemeNames().size() == themeCount);
    }
    colormaps::maps.erase("Turbo");
    colormaps::ensureDefault();
    assert(colormaps::maps.at("Turbo").entryCount >= 2);
    assert(colormaps::maps.at("Turbo").map != nullptr);
    fs::remove_all(root);
    std::printf("OK: %zu shipped colormaps and %zu themes; bad/missing resources recover safely\n", loaded, themeCount);
}
