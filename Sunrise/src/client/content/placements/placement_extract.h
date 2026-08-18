#pragma once

#include <cstddef>
#include <string_view>

namespace sunrise::client::content::placements {

/** What one extraction pass found. */
struct ExtractResult {
    /** Placement records read out of the authored chain. */
    std::size_t placements{};
    /** Records written into the spawn map. */
    std::size_t kept{};
    /** Records whose entity the game types as something other than a combatant. */
    std::size_t notCombatant{};
    /** Records whose entity is not streamed in for this destination. */
    std::size_t notResident{};
    /** Records refused because the map filled. */
    std::size_t overflowed{};
    /** Records skipped because their bubble is not one free roam opens. */
    std::size_t notPublic{};
    /** True when a walk budget stopped the pass before the chain ran out. */
    bool budgetHit{};
};

/**
 * Reads one destination's authored placements into the spawn map.
 *
 * A destination's scenario reaches its placed objects through its slice-set registries, and each
 * placed handle wraps a blob of placement records holding an entity tag and a transform. Those
 * transforms are the positions the destination ships with, so nothing here has to guess where
 * anything belongs. The held map is replaced, not appended to.
 *
 * @param destination Destination name, as the activity snapshot reports it.
 * @param combatantsOnly Keeps only entities the game types as combatants. A record whose entity is
 * not streamed in has no type to check and is kept either way.
 * @param publicOnly Skips bubbles free roam does not open, which the missions and strikes sharing
 * the map use.
 * @param result Receives the pass totals whether or not it succeeds.
 * @return True when the chain was walked.
 */
[[nodiscard]] bool extract(std::string_view destination,
                           bool combatantsOnly,
                           bool publicOnly,
                           ExtractResult& result) noexcept;

} // namespace sunrise::client::content::placements
