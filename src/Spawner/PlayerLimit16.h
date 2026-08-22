#pragma once

/*
 * PlayerLimit16 – engine-side 16-player support for yrpp-spawner
 *
 * Port of the working 16player.dll (Phase B2-batch+restore):
 *  - Expand HouseClass DynamicVector to 16 slots
 *  - Batch-refill GameModeOptions AISlots so AIPlayers > 7 create
 *  - Rewrite waypoint house-index table when it holds cell values
 *  - Expand end-game score buffers to 16 rows (custom entries + ptr table)
 *  - Ensure Scenario StartingPoints for loading-screen minimap colors
 *
 * Enable when SpawnerConfig::AIPlayers > 7 or a dedicated flag is set.
 * Add PlayerLimit16.cpp to Spawner.vcxproj; Syringe picks up DEFINE_HOOKs.
 */

class PlayerLimit16
{
public:
	static constexpr int MaxPlayers = 16;
	static constexpr int EngineAISlots = 8;

	/** True when expansion hooks should run (AIPlayers > 7 or flag). */
	static bool IsActive();

	/** Expand HouseClass::Array pointer/capacity to 16. Safe to call often. */
	static void ExpandHouseArray(const char* caller = nullptr);

	/** Optional: call from Spawner init to force-enable regardless of AIPlayers. */
	static void SetForceEnabled(bool enabled);

private:
	PlayerLimit16() = delete;
};
