// WorldContent.cpp - the client's side of WorldDatabase.h: SceneView and Avatar come from RasaEdit, where they also draw a World
// database's server content. The client has no World database (its server content will arrive from the server), so the lookups
// find nothing; the plain accessors are RasaEdit's.
#include "WorldDatabase.h"

const SpawnSlot& spawnSlot(const SpawnPool& pool, int index) {
    switch (index) {
        case 0: return pool.slot1;
        case 1: return pool.slot2;
        case 2: return pool.slot3;
        case 3: return pool.slot4;
        case 4: return pool.slot5;
        default: return pool.slot6;
    }
}

SpawnSlot& spawnSlot(SpawnPool& pool, int index) { return const_cast<SpawnSlot&>(spawnSlot(static_cast<const SpawnPool&>(pool), index)); }

const Creature* WorldDatabase::creature(int32_t) const { return nullptr; }

const std::vector<AppearanceSlot>& WorldDatabase::appearance(int32_t) const {
    static const std::vector<AppearanceSlot> none;
    return none;
}
