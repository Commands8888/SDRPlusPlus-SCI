#include <gui/colormaps.h>
#include <filesystem>
#include <utils/flog.h>
#include <fstream>
#include <json.hpp>
#include <cctype>

using nlohmann::json;

namespace colormaps {
    std::map<std::string, Map> maps;
    // Own the RGB arrays; Map keeps its existing public pointer interface.
    static std::map<std::string, std::vector<float>> storage;

    void loadMap(std::string path) {
        if (!std::filesystem::is_regular_file(path)) {
            flog::error("Could not load {0}, file doesn't exist", path);
            return;
        }

        Map map;
        std::vector<std::string> mapTxt;
        try {
            std::ifstream file(path.c_str());
            json data;
            file >> data;
            map.name = data.at("name").get<std::string>();
            map.author = data.at("author").get<std::string>();
            mapTxt = data.at("map").get<std::vector<std::string>>();
            if (map.name.empty() || mapTxt.size() < 2) { throw std::runtime_error("expected a named map with at least two colors"); }
            std::vector<float> values;
            values.reserve(mapTxt.size() * 3);
            for (const auto& col : mapTxt) {
                if (col.size() != 7 || col[0] != '#' ||
                    !std::all_of(col.begin() + 1, col.end(), [](unsigned char c) { return std::isxdigit(c); })) {
                    throw std::runtime_error("expected #RRGGBB colors");
                }
                for (int i = 1; i < 7; i += 2) values.push_back(std::stoi(col.substr(i, 2), nullptr, 16));
            }
            auto& owned = storage[map.name];
            owned = std::move(values);
            map.entryCount = mapTxt.size();
            map.map = owned.data();
            maps[map.name] = map;
        }
        catch (const std::exception& e) {
            flog::error("Could not load color map '{}': {}", path, e.what());
        }
    }

    void ensureDefault() {
        if (maps.count("Turbo")) return;
        // Only used when the default resource is absent/corrupt. Installed
        // palettes, including the user's custom maps, are never replaced.
        auto& colors = storage["Turbo"];
        colors = {0, 0, 0, 0, 0, 160, 0, 180, 255, 255, 220, 0, 255, 255, 255};
        maps["Turbo"] = {"Turbo", "Built-in fallback", colors.data(), 5};
        flog::warn("Default color map unavailable, using the built-in fallback");
    }
}
