// Assets.cpp - see Assets.h.
#include "Assets.h"

#include <memory>

namespace engine {

tr::AssetLibrary& assets() {
    static std::unique_ptr<tr::AssetLibrary> library;
    if (!library) library = std::make_unique<tr::AssetLibrary>(".");
    return *library;
}

}  // namespace engine
