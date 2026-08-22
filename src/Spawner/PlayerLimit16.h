#pragma once

/*
 * PlayerLimit16 – engine-side 16-player support for yrpp-spawner
 *
 *  - Expand HouseClass DynamicVector to 16 slots
 *  - Batch-refill GameModeOptions AISlots so AIPlayers > 7 create
 *  - Rewrite waypoint house-index table when it holds cell values
 *  - Cap Scenario StartingPoints to stock 8; repair corrupt HouseIndices
 *  - Expand end-game score buffers / related safety hooks
 *
 * Loading-screen minimap multi-color marks come from the map Preview
 * (client); this module only keeps engine start tables from corrupting.
 *
 * Active when AIPlayers > 7, NumberStartingPoints > 8, or SetForceEnabled.
 * Add PlayerLimit16.cpp to Spawner.vcxproj; Syringe picks up DEFINE_HOOKs.
 */

class PlayerLimit16
{
public:
	static constexpr int MaxPlayers = 16;
	static constexpr int EngineAISlots = 8;

	/** True when expansion hooks should run. */
	static bool IsActive();

	/** Expand HouseClass::Array pointer/capacity to 16. Safe to call often. */
	static void ExpandHouseArray(const char* caller = nullptr);

	/** Optional: force-enable regardless of AIPlayers / start count. */
	static void SetForceEnabled(bool enabled);

private:
	PlayerLimit16() = delete;
};
