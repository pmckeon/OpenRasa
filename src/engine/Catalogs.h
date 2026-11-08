// Catalogs.h - gameclient's asset preloads (tabula_rasa.exe, traced 2026-10-26): PreloadAssets(catalog, callable, args) (FUN_004d86a0),
// ReleaseAssets(catalog) (FUN_004d87e0), PreloadAssetsForClassId(classId) / ReleaseAssetsForClassId (0x004d88b0 / 0x004d88f0:
// "class%d.cat", FUN_004d8500), PreloadAssetsForAbility(abilityId, level) / ReleaseAssetsForAbility (0x004d8930 / 0x004d8970:
// "ability%d+%d.cat", FUN_004d85d0). A catalog (catalogs.glm, tr::readCatalog) lists the assets an entity class, ability, action or the
// start-up needs.
//
// The catalog manager (app + 0x80; request FUN_005a3400, release FUN_005a3590): a request for a catalog not held makes an entry
// (refcount 1), keeps the callback and requests the catalog's assets (FUN_006ae120); when nothing is left pending after that (all loaded
// before, or loaded at once with the loader's async flag off) the callback runs at once, else when the last asset completes. A request
// for a held catalog adds a reference and runs the callback at once when it has completed, else keeps it. The request always succeeds
// (Python gets True). A release drops a reference and frees the catalog's assets at none; an unknown catalog: False. The callback is
// callable(*args) (a callable or None, else TypeError; actor.py starts an ability's LocalWindup / Windup this way when its action has
// info.preload).
//
// OpenRasa keeps no asset cache of its own (each scene loads its models), so loading an asset here means reading its file in the
// background (on a worker; at once while the loader's async flag is off): that gives the scripts the game's order of callbacks, references
// and pending loads (NumLoadsPending counts them), not a cache.
#pragma once

#include <Python.h>

namespace engine::catalogs {

void install(PyObject* gameClient);
void frame();          // the completions (the loader's, FUN_005bc000): callbacks of catalogs whose assets are all in
long pending();        // assets requested and not yet read
void finishAll();      // the loader made idle (PushAsyncEnable(False)): every pending asset read and every completion run
void shutdown();       // the reader stopped

}  // namespace engine::catalogs
