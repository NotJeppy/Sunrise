#pragma once

#include <string_view>

#include <span>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::spawn {

inline constexpr std::uint32_t kNoKey = 0;

enum class Action : std::uint8_t {
    mainPlayer,
    mainCrosshair,
    projectilePlayer,
    projectileCrosshair,
    lootPlayer,
    lootCrosshair,
    count,
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::count);

struct Keybinds {
    std::array<std::uint32_t, kActionCount> virtualKeys{};
};

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Keybinds get() noexcept;
bool publish(const Keybinds& keybinds) noexcept;

/**
 * One recorded placement of a spawn map.
 * A map is authored in game by standing where an entity belongs and recording the point, so the
 * positions are the game's own, not anything derived outside it.
 */
struct MapPoint {
    std::uint32_t tag{};
    std::array<float, 3> position{};
};

/** Recorded points one destination's map holds. */
inline constexpr std::size_t kMapCapacity = 2048;

/**
 * Replaces the held map with the one recorded for a destination.
 * @param destination Destination name the map is filed under.
 * @return True when a map file was read. A missing file clears the map and reports false.
 */
[[nodiscard]] bool load_map(std::string_view destination) noexcept;

/**
 * Writes the held map for a destination.
 * @param destination Destination name the map is filed under.
 * @return True when the whole file was written.
 */
[[nodiscard]] bool save_map(std::string_view destination) noexcept;

/** Drops every held point without touching any file. */
void clear_map() noexcept;

/** @param point Placement to append. @return True when it fit. */
[[nodiscard]] bool add_map_point(const MapPoint& point) noexcept;

/** Drops the last recorded point. @return True when one was dropped. */
[[nodiscard]] bool remove_last_map_point() noexcept;

/** @return Points the held map carries. */
[[nodiscard]] std::size_t map_size() noexcept;

/**
 * Copies the held map out.
 * @param output Receives as many points as it holds.
 * @return Points written.
 */
[[nodiscard]] std::size_t copy_map(std::span<MapPoint> output) noexcept;


} // namespace sunrise::client::spawn
