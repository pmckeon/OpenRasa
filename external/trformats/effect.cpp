#include "effect.h"

#include <cstring>

namespace tr {

bool EffectInfo::hasTechnique(int32_t renderMode, int32_t transformMode, int32_t techniqueMode) const {
    for (const EffectTechnique& t : techniques)
        if (t.renderMode == renderMode && t.transformMode == transformMode && t.techniqueMode == techniqueMode) return true;
    return false;
}

namespace {

struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool ok = true;
    uint32_t u32(size_t offset) {
        if (offset + 4 > size) {
            ok = false;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, data + offset, 4);
        return v;
    }
    // A string object: u32 length, the characters (zero-terminated within the length).
    std::string string(size_t offset) {
        const uint32_t n = u32(offset);
        if (!ok || offset + 4 + n > size) {
            ok = false;
            return {};
        }
        const char* s = reinterpret_cast<const char*>(data + offset + 4);
        return std::string(s, strnlen(s, n));
    }
};

} // namespace

EffectInfo readEffectInfo(const std::vector<uint8_t>& bytes) {
    EffectInfo info;
    static const uint8_t magic[4] = {0x01, 0x09, 0xFF, 0xFE};   // 0xFEFF0901
    size_t start = std::string::npos;
    for (size_t i = 0; i + 8 <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, magic, 4) == 0) {
            start = i;
            break;
        }
    if (start == std::string::npos) return info;
    Reader top{bytes.data(), bytes.size()};
    const uint32_t first = top.u32(start + 4);
    // Offsets inside the effect are relative to the data after the 8-byte header.
    Reader r{bytes.data() + start + 8, bytes.size() - start - 8};
    size_t p = first;
    const uint32_t parameterCount = r.u32(p), techniqueCount = r.u32(p + 4);
    p += 16;
    // Parameters: typedef offset, value offset, flags, annotation count, then (typedef, value) per annotation. A string parameter's
    // value is an object id, resolved from the object table after the techniques.
    uint32_t phaseObject = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < parameterCount && r.ok; ++i) {
        const uint32_t typedefOffset = r.u32(p), valueOffset = r.u32(p + 4), annotations = r.u32(p + 12);
        const uint32_t type = r.u32(typedefOffset), nameAt = r.u32(typedefOffset + 8);
        if (type == 4 && nameAt && r.string(nameAt) == "Phase") phaseObject = r.u32(valueOffset);
        p += 16 + 8 * size_t(annotations);
    }
    for (uint32_t t = 0; t < techniqueCount && r.ok; ++t) {
        EffectTechnique technique;
        const uint32_t nameOffset = r.u32(p), annotations = r.u32(p + 4), passes = r.u32(p + 8);
        if (nameOffset) technique.name = r.string(nameOffset);
        p += 12;
        for (uint32_t a = 0; a < annotations && r.ok; ++a) {
            const uint32_t typedefOffset = r.u32(p + 8 * size_t(a)), valueOffset = r.u32(p + 8 * size_t(a) + 4);
            // typedef: type, class, name offset, semantic offset, elements, ...
            const uint32_t type = r.u32(typedefOffset), nameAt = r.u32(typedefOffset + 8);
            const std::string name = nameAt ? r.string(nameAt) : std::string();
            const uint32_t raw = r.u32(valueOffset);
            float f;
            std::memcpy(&f, &raw, 4);
            const int32_t value = type == 3 ? int32_t(f) : int32_t(raw);   // 2 int, 3 float
            if (name == "RenderMode") technique.renderMode = value;
            else if (name == "TransformMode") technique.transformMode = value;
            else if (name == "TechniqueMode") technique.techniqueMode = value;
            else if (name == "ComplexityFactor") technique.complexity = type == 3 ? f : float(raw);
        }
        p += 8 * size_t(annotations);
        for (uint32_t s = 0; s < passes && r.ok; ++s) {
            const uint32_t passAnnotations = r.u32(p + 4), states = r.u32(p + 8);
            p += 12 + 8 * size_t(passAnnotations) + 16 * size_t(states);
        }
        if (r.ok) info.techniques.push_back(std::move(technique));
    }
    // Objects with initial data (strings, shaders): u32 string count, u32 resource count, then (id, size, data padded to 4).
    const uint32_t stringCount = r.u32(p);
    p += 8;
    for (uint32_t i = 0; i < stringCount && r.ok; ++i) {
        const uint32_t id = r.u32(p), length = r.u32(p + 4);
        if (id == phaseObject && p + 8 + length <= r.size) {
            const char* s = reinterpret_cast<const char*>(r.data + p + 8);
            info.defaultPhase.assign(s, strnlen(s, length));
        }
        p += 8 + ((size_t(length) + 3) & ~size_t(3));
    }
    info.valid = r.ok && top.ok;
    return info;
}

std::string compiledEffectName(const std::string& effect, const std::function<bool(const std::string&)>& exists) {
    std::string base = effect;
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    for (const char* suffix : {"SP_3", "SP_2b", "SP_2a", "SP_2", "SP_1"}) {
        const std::string name = base + suffix + ".fxc";
        if (exists(name)) return name;
    }
    return {};
}

} // namespace tr
