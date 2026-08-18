#pragma once

#include "../hooks/spawn/spawn_runtime.h"

namespace sunrise::client::spawn {

/**
 * Loads the saved population settings and hands them to the populator.
 *
 * The settings file the game ships is written once and then only rewritten by a version upgrade,
 * so it is no place for state the interface changes. This keeps the population panel's own state
 * beside it, the way the movement and player panels keep theirs.
 *
 * @param module Loaded DLL module, which names the owned folder.
 */
void initialize_population(void* module) noexcept;

/** Drops the resolved path and held settings so the module can unload. */
void shutdown_population() noexcept;

/**
 * Writes the settings and publishes them to the populator.
 * @param settings Settings to hold.
 * @return True when they were published, whether or not the file was written.
 */
bool publish_population(const hooks::spawn::PopulationSettings& settings) noexcept;

} // namespace sunrise::client::spawn
