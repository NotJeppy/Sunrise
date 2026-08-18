#include "placement_extract.h"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../state/build_data/runtime.h"
#include "../../hooks/spawn/spawn_runtime.h"
#include "../../spawn/spawn_keybind_store.h"
#include "../items/packages/internal.h"

namespace sunrise::client::content::placements {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace layouts = state::build_data::scenarios;

/**
 * Class of the authored placement blob.
 * Its blobs sit one hop under a placed handle, and nearly every one of them carries a transform at
 * the same offset, which is what a fixed field looks like rather than a coincidence.
 */
constexpr std::uint32_t kPlacementClass = 0x808099D6U;
/** Class an authored placement's entity tag resolves to. */
constexpr std::uint32_t kEntityClass = 0x80809C0FU;
/** First placement record of a placement blob. */
constexpr std::size_t kRecordBase = 0x30;
/** Stride between placement records, taken from the spacing of the transform offsets. */
constexpr std::size_t kRecordStride = 0x90;
/** Entity tag offset inside one record. */
constexpr std::size_t kRecordTagOffset = 0x00;
/** Position offset inside one record: three world lanes then a scale. */
constexpr std::size_t kRecordPositionOffset = 0x20;
/** Object type the game gives a combatant. */
constexpr std::uint8_t kCombatantType = 12;
/** Largest world coordinate treated as plausible. */
constexpr float kCoordinateLimit = 20000.0F;
/** Placed handles followed in one pass. */
constexpr std::size_t kHandleBudget = 16384;
/** Tag reads one pass performs in total. */
constexpr std::size_t kReadBudget = 120000;
/**
 * Hops followed below a placed handle. A handle wraps the placement blob, so the records sit two
 * hops down at most.
 */
constexpr std::size_t kMaximumDepth = 2;
/** Tags remembered so a shared child is walked once and a cycle cannot loop. */
constexpr std::size_t kVisitedCapacity = 1U << 17;

/** Everything one extraction pass carries. */
struct Pass {
    ExtractResult result{};
    bool combatantsOnly{};
    bool publicOnly{};
    std::size_t handles{};
    std::size_t reads{};
    std::vector<std::uint32_t> visited{};
};

/** @return True when this tag had not been walked before, marking it walked. */
[[nodiscard]] bool mark_visited(Pass& pass, std::uint32_t tag) noexcept {
    if (pass.visited.size() != kVisitedCapacity) {
        pass.visited.assign(kVisitedCapacity, 0U);
    }
    std::size_t slot = (tag * 2654435761U) % kVisitedCapacity;
    for (std::size_t probe = 0; probe < kVisitedCapacity; ++probe) {
        std::uint32_t& held = pass.visited[slot];
        if (held == 0U) {
            held = tag;
            return true;
        }
        if (held == tag) {
            return false;
        }
        slot = slot + 1 == kVisitedCapacity ? 0 : slot + 1;
    }
    return false;
}

/**
 * @return True when a word looks like a tag rather than a class id or a scalar.
 * Tags in this chain carry 0x80 in the top byte, and the 0x8080 prefix is the class range, so that
 * prefix is excluded rather than read back as a tag.
 */
[[nodiscard]] bool tag_shaped(std::uint32_t value) noexcept {
    return (value >> 24U) == 0x80U && (value >> 16U) != 0x8080U;
}

/** Reads every record of one placement blob into the spawn map. */
void read_placements(Pass& pass, std::span<const std::byte> blob) noexcept {
    for (std::size_t offset = kRecordBase; offset + kRecordStride <= blob.size();
         offset += kRecordStride) {
        std::uint32_t tag = 0;
        std::memcpy(&tag, blob.data() + offset + kRecordTagOffset, sizeof tag);
        std::array<float, 4> position{};
        std::memcpy(position.data(), blob.data() + offset + kRecordPositionOffset, sizeof position);
        // The scale lane is the record's own shape check, so a run of padding cannot read as one.
        if (!tag_shaped(tag) || position[3] != 1.0F) {
            continue;
        }
        bool finite = true;
        for (std::size_t lane = 0; lane < 3; ++lane) {
            finite = finite && std::isfinite(position[lane])
                     && std::fabs(position[lane]) <= kCoordinateLimit;
        }
        if (!finite) {
            continue;
        }
        ++pass.result.placements;

        // Residency is a property of what is streamed in right now, not of the destination, so a
        // record is kept either way. The populator checks it again when it places the point.
        const bool resident = hooks::spawn::is_tag_resident(tag);
        std::uint8_t type = 0;
        const bool typed = resident && hooks::spawn::object_type(tag, type);
        if (!resident) {
            ++pass.result.notResident;
        }
        if (pass.combatantsOnly && typed && type != kCombatantType) {
            ++pass.result.notCombatant;
            continue;
        }
        spawn::MapPoint point{};
        point.tag = tag;
        point.position = {position[0], position[1], position[2]};
        if (!spawn::add_map_point(point)) {
            ++pass.result.overflowed;
            return;
        }
        ++pass.result.kept;
    }
}

/** Follows one tag, reading placements out of it and descending far enough to reach them. */
void extract_tag(Pass& pass,
                 const reader::Source& source,
                 reader::Scratch& scratch,
                 std::uint32_t tag,
                 std::size_t depth) noexcept {
    if (depth > kMaximumDepth || pass.reads >= kReadBudget) {
        pass.result.budgetHit = pass.result.budgetHit || pass.reads >= kReadBudget;
        return;
    }
    if (!mark_visited(pass, tag)) {
        return;
    }
    std::vector<std::byte> blob{};
    std::uint32_t classId = 0;
    if (!reader::read_tag(source, scratch, tag, blob, classId)) {
        return;
    }
    ++pass.reads;
    if (classId == kPlacementClass) {
        read_placements(pass, blob);
        return;
    }
    if (classId == kEntityClass) {
        // The entity is the leaf; its own tags lead away from the placement chain.
        return;
    }
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= blob.size(); offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, blob.data() + offset, sizeof word);
        if (tag_shaped(word)) {
            extract_tag(pass, source, scratch, word, depth + 1);
        }
    }
}

/** Walks one placed object and extracts every placement its handles reach. */
void extract_object(Pass& pass,
                    const reader::Source& source,
                    reader::Scratch& scratch,
                    std::uint32_t objectTag) noexcept {
    std::vector<std::byte> object{};
    std::uint32_t objectClass = 0;
    if (!reader::read_tag(source, scratch, objectTag, object, objectClass)) {
        return;
    }
    tables::Array bubbles{};
    if (!tables::object_bubbles(object, bubbles)) {
        return;
    }
    for (std::uint64_t index = 0; index < bubbles.count && pass.handles < kHandleBudget; ++index) {
        tables::ObjectBubble sub{};
        if (!tables::object_bubble_at(object, bubbles, index, sub)) {
            continue;
        }
        for (std::uint64_t slot = 0; slot < sub.handleCount && pass.handles < kHandleBudget;
             ++slot) {
            std::uint32_t handle = 0;
            if (tables::object_placed_handle_at(object, sub, slot, handle) && handle != 0) {
                ++pass.handles;
                extract_tag(pass, source, scratch, handle, 0);
            }
        }
    }
}

/** Walks every registry one slice-set entry names. */
void extract_registry(Pass& pass,
                      const reader::Source& source,
                      reader::Scratch& scratch,
                      std::uint32_t registryTag) noexcept {
    std::vector<std::byte> registry{};
    std::uint32_t registryClass = 0;
    if (!reader::read_tag(source, scratch, registryTag, registry, registryClass)) {
        return;
    }
    constexpr std::array<std::size_t, 3> descriptors{
        tables::kRegistryFirstDescriptor,
        tables::kRegistrySecondDescriptor,
        tables::kRegistryThirdDescriptor,
    };
    for (const std::size_t descriptor : descriptors) {
        tables::Array objects{};
        if (!tables::registry_objects(registry, descriptor, objects)) {
            continue;
        }
        for (std::uint64_t index = 0; index < objects.count && pass.handles < kHandleBudget;
             ++index) {
            std::uint32_t objectTag = 0;
            if (tables::registry_object_at(registry, objects, index, objectTag) && objectTag != 0) {
                extract_object(pass, source, scratch, objectTag);
            }
        }
    }
}

/** Reports one finished pass. */
void report(const ExtractResult& result) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=placement_extract placements=%zu kept=%zu absent=%zu "
                                      "private=%zu",
                                      result.placements,
                                      result.kept,
                                      result.notResident,
                                      result.notPublic);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

bool extract(std::string_view destination,
             bool combatantsOnly,
             bool publicOnly,
             ExtractResult& result) noexcept {
    result = {};
    layouts::Definition layout{};
    if (!state::build_data::find_scenario_layout(destination, layout)) {
        return false;
    }
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!items::packages::collect_keys(keys) || !items::packages::package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        return false;
    }
    // Held across calls: the visited table alone is half a megabyte, and a batch walks every
    // destination in turn.
    static Pass pass{};
    static reader::Scratch scratch{};
    pass = {};
    pass.combatantsOnly = combatantsOnly;
    pass.publicOnly = publicOnly;
    // The map is replaced: a pass reports one destination's placements, not a running sum.
    spawn::clear_map();

    const reader::Source source{directory.chars.data(), &keys};
    std::vector<std::byte> scenario{};
    if (reader::read_tag(source, scratch, layout.tag, scenario)) {
        tables::Array bubbles{};
        if (tables::scenario_bubbles(scenario, bubbles)) {
            for (std::uint64_t index = 0; index < bubbles.count && pass.handles < kHandleBudget;
                 ++index) {
                tables::Bubble bubble{};
                if (!tables::bubble_at(scenario, bubbles, index, bubble)) {
                    continue;
                }
                for (std::uint64_t state = 0;
                     state < bubble.stateCount && pass.handles < kHandleBudget;
                     ++state) {
                    tables::SliceState sliceState{};
                    if (!tables::slice_state_at(scenario, bubble, state, sliceState)
                        || sliceState.entryTag == 0) {
                        continue;
                    }
                    // A destination's map is shared with the missions and strikes that run on it,
                    // and their bubbles are private. Free roam only opens the public ones, so a
                    // placement outside them sits where the player is turned back.
                    if (pass.publicOnly && !sliceState.isPublic) {
                        ++pass.result.notPublic;
                        continue;
                    }
                    std::vector<std::byte> entry{};
                    tables::SliceEntry parsed{};
                    if (!reader::read_tag(source, scratch, sliceState.entryTag, entry)
                        || !tables::slice_entry(entry, parsed) || parsed.registryTag == 0) {
                        continue;
                    }
                    extract_registry(pass, source, scratch, parsed.registryTag);
                }
            }
        }
    }
    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(scratch);
    result = pass.result;
    report(result);
    return true;
}

} // namespace sunrise::client::content::placements
