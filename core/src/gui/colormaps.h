#pragma once
#include <string>
#include <vector>
#include <module.h>
#include <map>

namespace colormaps {
    struct Map {
        std::string name;
        std::string author;
        float* map = nullptr;
        int entryCount = 0;
    };

    void loadMap(std::string path);
    void ensureDefault();

    SDRPP_EXPORT std::map<std::string, Map> maps;
}
