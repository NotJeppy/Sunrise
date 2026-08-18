#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../spawn/spawn_keybind_store.h"

namespace sunrise::client::hooks::spawn {

enum class Origin : std::uint8_t {
    player,
    crosshair,
};

struct Settings {
    float lift{1.0F};
    float rayDistance{100.0F};
    float scale{1.0F};
    std::array<float, 3> offset{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    bool useCameraRotation{};
    bool overrideRotation{};
};

[[nodiscard]] bool install() noexcept;
void uninstall() noexcept;

[[nodiscard]] bool ready() noexcept;
[[nodiscard]] bool busy() noexcept;
[[nodiscard]] bool is_tag_resident(std::uint32_t tag) noexcept;
[[nodiscard]] bool object_type(std::uint32_t tag, std::uint8_t& type) noexcept;

[[nodiscard]] bool request(std::uint32_t tag,
                           Origin origin,
                           std::uint32_t amount,
                           const Settings& settings) noexcept;

[[nodiscard]] bool request_line(std::span<const std::uint32_t> tags,
                                Origin origin,
                                std::uint32_t itemsPerRow,
                                float spacing,
                                const Settings& settings) noexcept;

void configure_shortcut(client::spawn::Action action,
                        std::uint32_t tag,
                        std::uint32_t amount,
                        const Settings& settings) noexcept;

void cancel() noexcept;

/**
 * World-population settings. The populator keeps a ring of live entities around the player and
 * replaces them as they die, so a destination stays inhabited while it is explored.
 */
struct PopulationSettings {
    bool enabled{};
    /** Live placements tracked around the player. Reached count stops further placement. */
    std::uint32_t target{12};
    /** Closest and furthest placement distance from the player. */
    float minimumRadius{18.0F};
    float maximumRadius{55.0F};
    /**
     * Distance past which a live placement stops being tracked, so new ground populates.
     * The entity stays in the world: the game offers no removal call this module can make.
     */
    float forgetRadius{140.0F};
    /** Milliseconds between placement attempts. One placement runs per attempt. */
    std::uint32_t intervalMs{600};
    /** Height added to the ground hit before the entity is placed. */
    float lift{0.5F};
    float scale{1.0F};
    /**
     * Places from the recorded map instead of a roaming ring around the player.
     * With no map published this places nothing, which is the point: a map is authoritative.
     */
    bool useMap{};
    /** Milliseconds a recorded point waits after its entity dies before it is placed again. */
    std::uint32_t respawnDelayMs{45000};
    /**
     * Drops a recorded point onto the surface under it before placing.
     * An authored height is only as good as the frame it was authored in, and a point that lands
     * inside terrain puts an entity somewhere the player cannot see or reach.
     */
    bool snapToGround{true};
    /**
     * Loads the arriving destination's saved map and starts placing from it without being asked.
     * Off by default: a world that fills itself is a change the player opts into, not one that
     * happens the first time they load in.
     */
    bool autoOnLoad{};
};

/** One recorded placement the populator can fill. */
struct PopulationPoint {
    std::uint32_t tag{};
    std::array<float, 3> position{};
};

/** Replaces the population settings. Clearing `enabled` stops placement but keeps tracking. */
void configure_population(const PopulationSettings& settings) noexcept;

/** @return The current population settings. */
[[nodiscard]] PopulationSettings population() noexcept;

/**
 * Replaces the entity tags the populator draws from.
 * @param tags Resident entity tags. An empty span leaves the populator with nothing to place.
 */
void set_population_tags(std::span<const std::uint32_t> tags) noexcept;

/** @return Placements currently tracked as live. */
[[nodiscard]] std::size_t population_live() noexcept;

/** @return Entity tags the populator draws from. */
[[nodiscard]] std::size_t population_source_count() noexcept;

/**
 * Replaces the recorded points the populator fills while it is in map mode.
 * @param points Recorded placements. An empty span leaves map mode with nothing to place.
 */
void set_population_points(std::span<const PopulationPoint> points) noexcept;

/** @return Recorded points the populator holds. */
[[nodiscard]] std::size_t population_point_count() noexcept;

/** Why the last populator step did or did not place anything. */
enum class PlacementOutcome : std::uint8_t {
    idle,
    placed,
    disabled,
    noPlayer,
    noPoints,
    atTarget,
    noneInRange,
    notResident,
    noGround,
    spawnFailed,
};

/** What the populator is doing, so a world that stays empty can say why. */
struct PopulationStatus {
    std::size_t points{};
    std::size_t live{};
    /** Distance to the closest unfilled point, or a negative value when there is none. */
    float nearest{-1.0F};
    PlacementOutcome last{PlacementOutcome::idle};
    /** Where the player was on the last step, which is the frame a recorded point must share. */
    std::array<float, 3> player{};
    /** Where the last placement actually went, after any ground snap. */
    std::array<float, 3> placed{};
    /** Height the ground probe moved the last placement by, or zero when it did not run. */
    float snapped{};
};

/** @return The populator's current status. */
[[nodiscard]] PopulationStatus population_status() noexcept;

/** Drops tracking of every placement. Entities already in the world are left alone. */
void clear_population_tracking() noexcept;


} // namespace sunrise::client::hooks::spawn
