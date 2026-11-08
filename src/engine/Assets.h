// Assets.h - the game's files (the archives and loose folders tabula_rasa.cfg mounts), shared by the engine's parts.
#pragma once

#include "assetlibrary.h"

namespace engine {

// Opened on first use with the working directory (the game folder) as the root.
tr::AssetLibrary& assets();

}  // namespace engine
