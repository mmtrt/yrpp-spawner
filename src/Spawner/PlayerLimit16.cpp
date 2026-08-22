/*
 * PlayerLimit16.cpp – yrpp-spawner port of working 16player.dll
 *
 * Phase B2-batch+restore:
 *  - House array expand to 16
 *  - AI create loop: after first 8-slot pass, refill AISlots for missing AI
 *  - Save/restore originals across second AssignHouses
 *  - Waypoint table rewrite (cell values → house indices)
 *  - Score screen: 16-row custom buffers (entries + ptr table) with
 *    ESI bias so stock [esi+0xA8D1xx] stores land in g_ScoreEntries
 *
 * Drop standalone 16player.dll from Syringe inject list after integrating.
 */

#include "PlayerLimit16.h"

#include <Utilities/Macro.h>
#include <Helpers/Macro.h>

#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <ScenarioClass.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Addresses (YR 1.001 / gamemd). Prefer YRpp globals when available.
// ---------------------------------------------------------------------------

// File-scope aliases (MSVC needs these in free functions; class statics alone are not enough)
static constexpr int MaxPlayers = PlayerLimit16::MaxPlayers;
static constexpr int EngineAISlots = PlayerLimit16::EngineAISlots;

static constexpr DWORD ADDR_HOUSE_PTR   = 0x00A8022C; // HouseClass::Array items ptr
static constexpr DWORD ADDR_HOUSE_CAP   = 0x00A80230; // capacity
static constexpr DWORD ADDR_HOUSE_COUNT = 0x00A80238; // count
static constexpr DWORD ADDR_HOUSE_ALLOC = 0x00A80234; // IsAllocated (bool)
static constexpr DWORD ADDR_CUR_HOUSE   = 0x00A8B23C;
static constexpr DWORD ADDR_MAP         = 0x00A8B230; // ScenarioClass*
static constexpr DWORD ADDR_GMO         = 0x00A8B250; // GameModeOptionsClass*

static constexpr DWORD OFF_GMO_AIPLAYERS = 0x24;

// GameModeOptions AISlots parallel arrays (8 ints each, engine layout)
static constexpr DWORD ADDR_AIS_DIFF    = 0x00A8B27C;
static constexpr DWORD ADDR_AIS_COUNTRY = 0x00A8B29C;
static constexpr DWORD ADDR_AIS_COLOR   = 0x00A8B2BC;
static constexpr DWORD ADDR_AIS_START   = 0x00A8B2DC;
static constexpr DWORD ADDR_AIS_TEAM    = 0x00A8B2FC;
static constexpr DWORD ADDR_AIS_END     = 0x00A8B2BC; // end of Country[] scan

static constexpr DWORD ADDR_HTYPE_PTR   = 0x00A83C9C;
static constexpr DWORD ADDR_HTYPE_CNT   = 0x00A83CA8;

// Map / Scenario waypoint house-index table offset
static constexpr DWORD OFF_WP_HOUSE_TABLE = 0x1180;
/* ScenarioClass layout (YR): HouseIndices @ +0x1180 → StartingPoints just before */
static constexpr DWORD OFF_NUM_START_POINTS = 0x113C; /* int NumberStartingPoints */
static constexpr DWORD OFF_STARTING_POINTS  = 0x1140; /* Point2D StartingPoints[8] */
static constexpr DWORD OFF_HOUSE_HOME_CELLS = 0x11C0; /* CellStruct HouseHomeCells[8] */
/* Waypoints[702] CellStruct – each 4 bytes; YRpp places them before map header.
 * GetWaypointCoords @ 0x68BCC0. Waypoint cell packed: often low 16 = X, high = Y. */
static constexpr DWORD OFF_WAYPOINTS = 0x5D4; /* approximate; validated at runtime */
static constexpr int   STOCK_START_SLOTS = 8;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static HouseClass* g_HouseArray16[PlayerLimit16::MaxPlayers] {};
static bool g_HouseExpanded = false;
static int  g_LastCount = -1;
static bool g_ForceEnabled = false;
static bool g_BatchDone = false;
static bool g_HaveOrig = false;
static bool g_LoggedSlots = false;
static bool g_WpTableDumped = false;
static bool g_UsingEngineBuffer = false;

static int g_TplCountry[8], g_TplColor[8], g_TplDiff[8], g_TplStart[8], g_TplTeam[8];
static int g_TplCount = 0;

static int g_OrigCountry[8], g_OrigColor[8], g_OrigDiff[8], g_OrigStart[8], g_OrigTeam[8];

static FILE* g_Log = nullptr;

// ---------------------------------------------------------------------------
// Logging (optional – remove or wire to Debug::Log)
// ---------------------------------------------------------------------------

static void LogInit()
{
	if (g_Log)
		return;
	g_Log = fopen("playerlimit16.log", "a");
	if (g_Log)
	{
		fprintf(g_Log, "\n========== PlayerLimit16 loaded ==========\n");
		fflush(g_Log);
	}
}

static void Log(const char* fmt, ...)
{
	if (!g_Log)
		LogInit();
	if (!g_Log)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_Log, fmt, args);
	va_end(args);
	fflush(g_Log);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool IsPlausiblePtr(DWORD p)
{
	return p >= 0x01000000 && p < 0x7FFF0000;
}

static bool IsPlausibleVTable(DWORD vt)
{
	return vt >= 0x00400000 && vt < 0x00A00000;
}

static bool IsValidHouse(DWORD house)
{
	if (!IsPlausiblePtr(house))
		return false;
	return IsPlausibleVTable(*reinterpret_cast<DWORD*>(house));
}

static int GetAIPlayers()
{
	int n = *reinterpret_cast<int*>(ADDR_GMO + OFF_GMO_AIPLAYERS);
	if (n < 0)
		n = 0;
	if (n > 15)
		n = 15;
	return n;
}

bool PlayerLimit16::IsActive()
{
	if (g_ForceEnabled)
		return true;
	return GetAIPlayers() > 7;
}

void PlayerLimit16::SetForceEnabled(bool enabled)
{
	g_ForceEnabled = enabled;
}

// ---------------------------------------------------------------------------
// House array expand
// ---------------------------------------------------------------------------

void PlayerLimit16::ExpandHouseArray(const char* caller)
{
	if (!caller)
		caller = "?";

	DWORD* cur = *reinterpret_cast<DWORD**>(ADDR_HOUSE_PTR);
	int count = *reinterpret_cast<int*>(ADDR_HOUSE_COUNT);
	if (count < 0)
		count = 0;
	if (count > MaxPlayers)
		count = MaxPlayers;

	if (!g_HouseExpanded)
	{
		if (!cur)
		{
			Log("[Expand] %s: pointer still NULL – waiting\n", caller);
			return;
		}
		for (int i = 0; i < count; i++)
			g_HouseArray16[i] = reinterpret_cast<HouseClass*>(cur[i]);
		for (int i = count; i < MaxPlayers; i++)
			g_HouseArray16[i] = nullptr;

		*reinterpret_cast<DWORD**>(ADDR_HOUSE_PTR) = reinterpret_cast<DWORD*>(g_HouseArray16);
		*reinterpret_cast<DWORD*>(ADDR_HOUSE_CAP) = MaxPlayers;
		*reinterpret_cast<BYTE*>(ADDR_HOUSE_ALLOC) = 0; // do not free static buffer
		g_HouseExpanded = true;
		g_LastCount = count;
		Log("[Expand] %s: FIRST – Count=%d Cap=%d ptr=%p\n",
			caller, count, MaxPlayers, static_cast<void*>(g_HouseArray16));
		return;
	}

	if (g_UsingEngineBuffer)
	{
		/* Engine owns a Cap>=16 buffer – do not touch Count or Items */
		return;
	}

	if (cur != reinterpret_cast<DWORD*>(g_HouseArray16))
	{
		DWORD engCap = *reinterpret_cast<DWORD*>(ADDR_HOUSE_CAP);
		Log("[Expand] %s: pointer now %p engCap=%u count=%d\n",
			caller, static_cast<void*>(cur), engCap, count);

		if (cur && engCap >= static_cast<DWORD>(MaxPlayers))
		{
			/* One-time adopt: mirror for our redirects, then leave engine alone */
			for (int i = 0; i < MaxPlayers; i++)
			{
				DWORD h = (i < count) ? cur[i] : 0;
				g_HouseArray16[i] = reinterpret_cast<HouseClass*>(h);
			}
			g_UsingEngineBuffer = true;
			g_LastCount = count;
			Log("[Expand] adopted engine buffer Cap=%u Count=%d – no further reclaim\n",
				engCap, count);
			return;
		}

		/* Engine buffer too small – keep static */
		if (cur)
		{
			for (int i = 0; i < count && i < MaxPlayers; i++)
				g_HouseArray16[i] = reinterpret_cast<HouseClass*>(cur[i]);
			for (int i = count; i < MaxPlayers; i++)
				g_HouseArray16[i] = nullptr;
		}
		*reinterpret_cast<DWORD**>(ADDR_HOUSE_PTR) = reinterpret_cast<DWORD*>(g_HouseArray16);
		*reinterpret_cast<DWORD*>(ADDR_HOUSE_CAP) = MaxPlayers;
		*reinterpret_cast<BYTE*>(ADDR_HOUSE_ALLOC) = 0;
	}
	else
	{
		*reinterpret_cast<DWORD*>(ADDR_HOUSE_CAP) = MaxPlayers;
		*reinterpret_cast<BYTE*>(ADDR_HOUSE_ALLOC) = 0;
	}

	if (count != g_LastCount)
	{
		Log("[Expand] %s: Count %d → %d\n", caller, g_LastCount, count);
		if (count < g_LastCount)
		{
			g_BatchDone = false;
			g_TplCount = 0;
			g_WpTableDumped = false;
			g_UsingEngineBuffer = false;
			Log("[Expand] Count dropped – batch state reset\n");
		}
		g_LastCount = count;
	}
}

// ---------------------------------------------------------------------------
// AISlots save / restore / batch refill
// ---------------------------------------------------------------------------

static void SaveOriginals()
{
	if (g_HaveOrig)
		return;
	int* eCountry = reinterpret_cast<int*>(ADDR_AIS_COUNTRY);
	int* eColor = reinterpret_cast<int*>(ADDR_AIS_COLOR);
	int* eDiff = reinterpret_cast<int*>(ADDR_AIS_DIFF);
	int* eStart = reinterpret_cast<int*>(ADDR_AIS_START);
	int* eTeam = reinterpret_cast<int*>(ADDR_AIS_TEAM);
	for (int i = 0; i < EngineAISlots; i++)
	{
		g_OrigCountry[i] = eCountry[i];
		g_OrigColor[i] = eColor[i];
		g_OrigDiff[i] = eDiff[i];
		g_OrigStart[i] = eStart[i];
		g_OrigTeam[i] = eTeam[i];
	}
	g_HaveOrig = true;
	Log("[Batch] saved original AISlots\n");
}

static void RestoreOriginals()
{
	if (!g_HaveOrig)
		return;
	int* eCountry = reinterpret_cast<int*>(ADDR_AIS_COUNTRY);
	int* eColor = reinterpret_cast<int*>(ADDR_AIS_COLOR);
	int* eDiff = reinterpret_cast<int*>(ADDR_AIS_DIFF);
	int* eStart = reinterpret_cast<int*>(ADDR_AIS_START);
	int* eTeam = reinterpret_cast<int*>(ADDR_AIS_TEAM);
	for (int i = 0; i < EngineAISlots; i++)
	{
		eCountry[i] = g_OrigCountry[i];
		eColor[i] = g_OrigColor[i];
		eDiff[i] = g_OrigDiff[i];
		eStart[i] = g_OrigStart[i];
		eTeam[i] = g_OrigTeam[i];
	}
	Log("[Batch] restored original AISlots\n");
}

static void CaptureTemplates()
{
	if (g_TplCount > 0)
		return;

	int htCount = *reinterpret_cast<int*>(ADDR_HTYPE_CNT);
	DWORD* htArr = *reinterpret_cast<DWORD**>(ADDR_HTYPE_PTR);

	const int* srcCountry = g_HaveOrig ? g_OrigCountry : reinterpret_cast<int*>(ADDR_AIS_COUNTRY);
	const int* srcColor = g_HaveOrig ? g_OrigColor : reinterpret_cast<int*>(ADDR_AIS_COLOR);
	const int* srcDiff = g_HaveOrig ? g_OrigDiff : reinterpret_cast<int*>(ADDR_AIS_DIFF);
	const int* srcStart = g_HaveOrig ? g_OrigStart : reinterpret_cast<int*>(ADDR_AIS_START);
	const int* srcTeam = g_HaveOrig ? g_OrigTeam : reinterpret_cast<int*>(ADDR_AIS_TEAM);

	for (int i = 0; i < EngineAISlots; i++)
	{
		int c = srcCountry[i];
		if (c < 0 || c >= htCount)
			continue;
		if (!htArr || !IsPlausiblePtr(htArr[c]))
			continue;
		g_TplCountry[g_TplCount] = c;
		g_TplColor[g_TplCount] = srcColor[i];
		g_TplDiff[g_TplCount] = srcDiff[i];
		g_TplStart[g_TplCount] = srcStart[i];
		g_TplTeam[g_TplCount] = srcTeam[i];
		g_TplCount++;
	}
	Log("[Batch] captured %d AI templates\n", g_TplCount);
}

static void RefillEngineSlots(int need)
{
	int* eCountry = reinterpret_cast<int*>(ADDR_AIS_COUNTRY);
	int* eColor = reinterpret_cast<int*>(ADDR_AIS_COLOR);
	int* eDiff = reinterpret_cast<int*>(ADDR_AIS_DIFF);
	int* eStart = reinterpret_cast<int*>(ADDR_AIS_START);
	int* eTeam = reinterpret_cast<int*>(ADDR_AIS_TEAM);

	if (g_TplCount <= 0)
	{
		Log("[Batch] no templates – cannot refill\n");
		return;
	}
	if (need > EngineAISlots)
		need = EngineAISlots;
	if (need < 0)
		need = 0;

	for (int i = 0; i < need; i++)
	{
		int t = i % g_TplCount;
		eCountry[i] = g_TplCountry[t];
		eColor[i] = (g_TplColor[t] + 8 + i) & 15;
		eDiff[i] = g_TplDiff[t];
		// Keep starts in 0..7 so parallel assign path never OOB
		eStart[i] = (g_TplStart[t] >= 0) ? ((g_TplStart[t] + i) % 8) : (i % 8);
		eTeam[i] = g_TplTeam[t];
		Log("[Batch] refill slot %d country=%d color=%d start=%d\n",
			i, eCountry[i], eColor[i], eStart[i]);
	}
	for (int i = need; i < EngineAISlots; i++)
	{
		eCountry[i] = -1;
		eColor[i] = -1;
		eStart[i] = -1;
		eTeam[i] = -1;
	}
}

static void LogAISlotsAndTypes()
{
	if (g_LoggedSlots)
		return;
	g_LoggedSlots = true;

	int aiPlayers = GetAIPlayers();
	int aiDiff = *reinterpret_cast<int*>(ADDR_GMO + 0x28);
	int htCount = *reinterpret_cast<int*>(ADDR_HTYPE_CNT);
	DWORD* htPtr = *reinterpret_cast<DWORD**>(ADDR_HTYPE_PTR);

	Log("[GMO] AIPlayers=%d AIDifficulty=%d HouseTypeCount=%d\n",
		aiPlayers, aiDiff, htCount);

	int* eCountry = reinterpret_cast<int*>(ADDR_AIS_COUNTRY);
	int* eColor = reinterpret_cast<int*>(ADDR_AIS_COLOR);
	int* eDiffA = reinterpret_cast<int*>(ADDR_AIS_DIFF);
	int* eStart = reinterpret_cast<int*>(ADDR_AIS_START);

	for (int i = 0; i < EngineAISlots; i++)
	{
		int c = eCountry[i];
		bool ok = (c >= 0 && c < htCount && htPtr && IsPlausiblePtr(htPtr[c]));
		Log("[AISlots] %d country=%d color=%d diff=%d start=%d htype_ok=%d\n",
			i, c, eColor[i], eDiffA[i], eStart[i], static_cast<int>(ok));
	}
}

static void DumpWaypointTable()
{
	if (g_WpTableDumped)
		return;
	g_WpTableDumped = true;
	DWORD map = *reinterpret_cast<DWORD*>(ADDR_MAP);
	if (!IsPlausiblePtr(map))
	{
		Log("[Waypoint] Map pointer invalid (%08X)\n", map);
		return;
	}
	int* table = reinterpret_cast<int*>(map + OFF_WP_HOUSE_TABLE);
	Log("[Waypoint] house-index table @ %p:", static_cast<void*>(table));
	for (int i = 0; i < MaxPlayers; i++)
		Log(" [%d]=%d", i, table[i]);
	Log("\n");
}

/*
 * Loading-screen minimap uses ScenarioClass::StartingPoints[8] + NumberStartingPoints
 * and house colors. With 16-player maps the spawner often leaves these empty → no
 * colored indicators. Fill stock 8 slots from waypoint cells / house start spots.
 */
static bool g_StartPtsFixed = false;

static void EnsureStartingPoints(const char* why)
{
	DWORD scen = *reinterpret_cast<DWORD*>(ADDR_MAP);
	if (!IsPlausiblePtr(scen))
		return;

	int* pNum = reinterpret_cast<int*>(scen + OFF_NUM_START_POINTS);
	int* pts  = reinterpret_cast<int*>(scen + OFF_STARTING_POINTS); /* Point2D = 2 ints */
	int* houseIdx = reinterpret_cast<int*>(scen + OFF_WP_HOUSE_TABLE);

	int num = *pNum;
	int houseCount = *reinterpret_cast<int*>(ADDR_HOUSE_COUNT);
	if (houseCount < 0)
		houseCount = 0;
	if (houseCount > MaxPlayers)
		houseCount = MaxPlayers;

	Log("[StartPts] %s: NumberStartingPoints=%d HouseCount=%d\n", why, num, houseCount);

	/* Already has valid starts — only log once */
	bool anyNonZero = false;
	for (int i = 0; i < STOCK_START_SLOTS; i++)
	{
		if (pts[i * 2] != 0 || pts[i * 2 + 1] != 0)
			anyNonZero = true;
		Log("[StartPts]   slot %d = (%d,%d) houseIdx=%d\n",
			i, pts[i * 2], pts[i * 2 + 1], houseIdx[i]);
	}
	if (anyNonZero && num > 0)
	{
		if (!g_StartPtsFixed)
			Log("[StartPts] stock slots already populated – leave alone\n");
		g_StartPtsFixed = true;
		return;
	}

	/*
	 * Rebuild from Scenario waypoints 0..7 via engine GetWaypointCoords (0x68BCC0).
	 * CellStruct { short X, Y }; Point2D stores same as ints for StartingPoints.
	 */
	using GetWpFn = void* (__thiscall*)(void* scen, void* dest, int idx);
	auto getWp = reinterpret_cast<GetWpFn>(0x68BCC0);

	int filled = 0;
	for (int i = 0; i < STOCK_START_SLOTS; i++)
	{
		short cell[2] = { 0, 0 };
		getWp(reinterpret_cast<void*>(scen), cell, i);
		/* Undefined waypoint is often (-1,-1) or (0,0) */
		if (cell[0] < 0 || cell[1] < 0)
			continue;
		if (cell[0] == 0 && cell[1] == 0 && i > 0)
			continue;

		pts[i * 2]     = static_cast<int>(cell[0]);
		pts[i * 2 + 1] = static_cast<int>(cell[1]);

		/* Keep HouseIndices aligned: start slot i → house i when plausible */
		if (houseIdx[i] < 0 || houseIdx[i] >= houseCount)
			houseIdx[i] = (i < houseCount) ? i : -1;

		filled++;
		Log("[StartPts] filled slot %d from WP → (%d,%d) house=%d\n",
			i, pts[i * 2], pts[i * 2 + 1], houseIdx[i]);
	}

	if (filled > 0)
	{
		*pNum = filled;
		g_StartPtsFixed = true;
		Log("[StartPts] NumberStartingPoints set to %d\n", filled);
	}
	else
	{
		Log("[StartPts] WARNING: could not fill any start points from waypoints\n");
	}
}



// ---------------------------------------------------------------------------
// Hooks – house array
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x5EEA19, PlayerLimit16_HouseArray_Loop, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("Loop");
	if (g_UsingEngineBuffer)
		return 0; /* engine ptr already correct */
	R->EDX(reinterpret_cast<DWORD>(g_HouseArray16));
	return 0x5EEA1F;
}

DEFINE_HOOK(0x640F46, PlayerLimit16_HouseArray_640F, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("640F");

	/* EDX = house index from map; OOB caused AV at house+0x16054 with 16 players */
	DWORD idx = R->EDX();
	DWORD* arr = *reinterpret_cast<DWORD**>(ADDR_HOUSE_PTR);
	int count = *reinterpret_cast<int*>(ADDR_HOUSE_COUNT);
	if (!arr)
		arr = reinterpret_cast<DWORD*>(g_HouseArray16);
	if (count < 0)
		count = 0;
	if (count > MaxPlayers)
		count = MaxPlayers;

	if (idx >= static_cast<DWORD>(count) || !arr[idx])
	{
		Log("[640F] skip bad idx=%u count=%d\n", idx, count);
		return 0x641055; /* original je not-found path */
	}

	R->EBP(reinterpret_cast<DWORD>(arr));
	return 0x640F4C;
}

DEFINE_HOOK(0x4F61E6, PlayerLimit16_HouseArray_Create, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("Create");
	LogAISlotsAndTypes();
	if (g_UsingEngineBuffer)
		return 0;
	R->ECX(reinterpret_cast<DWORD>(g_HouseArray16));
	return 0x4F61EC;
}

DEFINE_HOOK(0x687572, PlayerLimit16_NullCheck_CurrentHouse, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("NullCheck");
	DWORD housePtr = *reinterpret_cast<DWORD*>(ADDR_CUR_HOUSE);
	if (!IsValidHouse(housePtr))
	{
		Log("[NullCheck] reject house %08X – skip\n", housePtr);
		return 0x687581;
	}
	R->ECX(housePtr);
	return 0;
}

DEFINE_HOOK(0x4F6032, PlayerLimit16_CreatePath_Redirect, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("CreatePath");
	if (g_UsingEngineBuffer)
		return 0;
	R->EAX(reinterpret_cast<DWORD>(g_HouseArray16));
	return 0x4F6037;
}

DEFINE_HOOK(0x650B5A, PlayerLimit16_Hot_650B_Redirect, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("Hot650B");
	if (g_UsingEngineBuffer)
		return 0;
	R->EAX(reinterpret_cast<DWORD>(g_HouseArray16));
	return 0x650B5F;
}

DEFINE_HOOK(0x686A2E, PlayerLimit16_Hot_686A_Redirect, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	PlayerLimit16::ExpandHouseArray("Hot686A");
	if (g_UsingEngineBuffer)
		return 0;
	R->EAX(reinterpret_cast<DWORD>(g_HouseArray16));
	return 0x686A33;
}

// ---------------------------------------------------------------------------
// Hooks – waypoint
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x5D6CBF, PlayerLimit16_Waypoint_HouseLoad, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;

	PlayerLimit16::ExpandHouseArray("Waypoint");
	DWORD* live = *reinterpret_cast<DWORD**>(ADDR_HOUSE_PTR);
	if (!live)
		live = reinterpret_cast<DWORD*>(g_HouseArray16);
	R->EDX(reinterpret_cast<DWORD>(live));

	DWORD idx = R->ESI();
	if (idx >= MaxPlayers) {
		Log("[Waypoint] idx %u out of range – skip\n", idx);
		return 0x5D6D3F;
	}

	// Once per pass: if table holds cells instead of house indices, rewrite
	if (idx == 0)
	{
		DWORD map = *reinterpret_cast<DWORD*>(ADDR_MAP);
		if (IsPlausiblePtr(map))
		{
			DWORD* table = reinterpret_cast<DWORD*>(map + OFF_WP_HOUSE_TABLE);
			int count = *reinterpret_cast<int*>(ADDR_HOUSE_COUNT);
			if (count > MaxPlayers)
				count = MaxPlayers;
			if (count < 0)
				count = 0;

			if (table[0] > 16)
			{
				Log("[Waypoint] rewriting cell-table → house indices (count=%d)\n", count);
				for (int i = 0; i < count; i++)
					table[i] = static_cast<DWORD>(i);
				for (int i = count; i < MaxPlayers; i++)
					table[i] = 0xFFFFFFFFu;
			}
		}
		g_WpTableDumped = false;
		DumpWaypointTable();
		EnsureStartingPoints("Waypoint");
	}

	DWORD house = (idx < MaxPlayers) ? live[idx] : 0;
	/* Null only – strict vtable checks rejected valid Ares/Phobos houses */
	if (!house)
	{
		Log("[Waypoint] idx %u null house – skip body\n", idx);
		return 0x5D6D3F;
	}
	return 0x5D6CC5;
}

DEFINE_HOOK(0x5D6D02, PlayerLimit16_Waypoint_NotFoundSkip, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	Log("[Waypoint] idx %u – not-found, skip\n", R->ESI());
	return 0x5D6D3F;
}


// ---------------------------------------------------------------------------
// Loading-screen minimap: ensure StartingPoints + colors before progress draw
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x552D60, PlayerLimit16_LoadProgress_Draw, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	EnsureStartingPoints("LoadProgress::Draw");
	return 0;
}

/* Also run once after houses exist (waypoint pass) so data is ready early */

// ---------------------------------------------------------------------------
// Hooks – AI create batch
// ---------------------------------------------------------------------------


/*
 * HouseClass::Array clear loop destroys Items[0] repeatedly.
 * Guard: skip entries with null/invalid vtable to avoid AV at [edx+0x20].
 */
/* HouseClear_Guard removed – size-5 on mov eax,[imm] was optional and risky */


DEFINE_HOOK(0x688158, PlayerLimit16_AICreate_LoopStart, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;

	if (g_HaveOrig)
	{
		RestoreOriginals();
		g_BatchDone = false;
		g_TplCount = 0;
	}
	else
	{
		SaveOriginals();
	}
	R->EBX(ADDR_AIS_COUNTRY);
	return 0x68815D;
}

DEFINE_HOOK(0x6882C5, PlayerLimit16_AICreate_EndBound, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;

	DWORD ebx = R->EBX();
	int created = static_cast<int>(R->EAX());
	int aiPlayers = GetAIPlayers();

	if (ebx < ADDR_AIS_END)
		return 0x68815D;

	CaptureTemplates();

	int need = aiPlayers - created;
	if (need > 0 && !g_BatchDone && g_TplCount > 0)
	{
		g_BatchDone = true;
		if (need > EngineAISlots)
			need = EngineAISlots;
		Log("[Batch] first pass done created=%d AIPlayers=%d need=%d – refill & restart\n",
			created, aiPlayers, need);
		RefillEngineSlots(need);
		R->EBX(ADDR_AIS_COUNTRY);
		return 0x68815D;
	}

	Log("[Batch] loop complete created=%d AIPlayers=%d Count=%d\n",
		created, aiPlayers, *reinterpret_cast<int*>(ADDR_HOUSE_COUNT));
	return 0x6882D1;
}

// ---------------------------------------------------------------------------
// Hooks – end-game score screen expansion (16 rows)
//
// Stock layout (cannot grow in place — next global is at 0xA8D57C):
//   0xA8D1FC  score entries  8 × 0x70
//   0xA8D580  filled slot count
//   0xABF958  pointer table   8 × DWORD  (rebuilt + qsort'd)
//
// Strategy: custom 16-entry buffer + 16 ptrs. After the game computes
//   esi = slot * 0x70
// we bias ESI so  esi + 0xA8D1FC  lands inside g_ScoreEntries. All later
// stores of the form  [esi+0xA8D2xx]  then hit our buffer. Rebuild/draw
// sites that use immediates 0xA8D1FC / 0xABF958 are redirected the same way.
//
// Note: AI rows showing difficulty text ("Easy") under Kills is stock YR
// behavior (entry+0x28 / string tables for non-human houses), not the 8-cap.
// ---------------------------------------------------------------------------

static constexpr DWORD ADDR_SCORE_SLOT_COUNT = 0x00A8D580;
static constexpr DWORD ADDR_SCORE_ENTRIES    = 0x00A8D1FC;
static constexpr DWORD ADDR_SCORE_PTRS       = 0x00ABF958;
static constexpr DWORD SCORE_ENTRY_STRIDE    = 0x70;
static constexpr int   SCORE_SLOT_MAX        = 16; // expanded

alignas(16) static uint8_t  g_ScoreEntries[SCORE_SLOT_MAX * SCORE_ENTRY_STRIDE] {};
static uint32_t             g_ScorePtrs[SCORE_SLOT_MAX] {};
static bool                 g_ScoreBufLogged = false;

static DWORD ScoreEntryDelta()
{
	return reinterpret_cast<DWORD>(g_ScoreEntries) - ADDR_SCORE_ENTRIES;
}

/* Bias ESI so stock [esi+0xA8D1FC]-style addressing hits g_ScoreEntries. */
static void ScoreBiasEsiEbx(REGISTERS* R)
{
	DWORD esi = R->ESI() + ScoreEntryDelta();
	R->ESI(esi);
	R->EBX(esi + ADDR_SCORE_ENTRIES);
	if (!g_ScoreBufLogged)
	{
		g_ScoreBufLogged = true;
		Log("[Score] entries@%p ptrs@%p delta=0x%08X\n",
			static_cast<void*>(g_ScoreEntries),
			static_cast<void*>(g_ScorePtrs),
			ScoreEntryDelta());
	}
}

/*
 * 0x5C9911: lea ebx, [esi+0xA8D1FC]  (6 bytes)
 * ESI already = slot*0x70. Bias and set EBX; skip original lea.
 */
DEFINE_HOOK(0x5C9911, PlayerLimit16_Score_EntryBase, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	ScoreBiasEsiEbx(R);
	return 0x5C9917;
}

/*
 * Campaign/alt fill at 0x46DAE4: lea ebx, [esi+0xA8D1FC]  (6 bytes)
 */
DEFINE_HOOK(0x46DAE4, PlayerLimit16_Score_EntryBase_Campaign, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	ScoreBiasEsiEbx(R);
	return 0x46DAEA;
}

/*
 * Safety: never write more than 16 score rows (house count can include neutrals).
 * 0x5C98F1: mov eax, [0xA8D580]  (5 bytes)
 */
DEFINE_HOOK(0x5C98F1, PlayerLimit16_ScoreSlotCap, 5)
{
	if (!PlayerLimit16::IsActive())
		return 0;

	DWORD slots = *reinterpret_cast<DWORD*>(ADDR_SCORE_SLOT_COUNT);
	if (slots >= static_cast<DWORD>(SCORE_SLOT_MAX))
	{
		Log("[Score] slot full (%u) – skip further score rows\n", slots);
		return 0x5C9A7E;
	}
	return 0;
}

/*
 * Replace stock pointer rebuild entirely (avoids fragile push-imm stack hooks
 * that caused qsort to receive base=4 → AV at 0x5C9AEC reading [4+0x60]).
 *
 * Stock 0x5C9AA0:
 *   build ptrs[i] = entries + i*0x70 for i in [0, count)
 *   if count > 1: qsort(ptrs, count, 4, compar@0x5C9AE0)
 */

static int __cdecl ScoreEntryCompar(const void* a, const void* b)
{
	const DWORD ea = *static_cast<const DWORD*>(a);
	const DWORD eb = *static_cast<const DWORD*>(b);
	if (!ea || !eb)
		return 0;
	const int va = *reinterpret_cast<const int*>(ea + 0x60);
	const int vb = *reinterpret_cast<const int*>(eb + 0x60);
	if (va == vb)
		return 0;
	/* Descending: highest score first (leaderboard). Stock was ascending. */
	return (va > vb) ? -1 : 1;
}

static void ScoreRebuildPtrTable()
{
	DWORD count = *reinterpret_cast<DWORD*>(ADDR_SCORE_SLOT_COUNT);
	if (count > static_cast<DWORD>(SCORE_SLOT_MAX))
		count = static_cast<DWORD>(SCORE_SLOT_MAX);

	for (DWORD i = 0; i < count; i++)
		g_ScorePtrs[i] = reinterpret_cast<DWORD>(&g_ScoreEntries[i * SCORE_ENTRY_STRIDE]);
	for (DWORD i = count; i < static_cast<DWORD>(SCORE_SLOT_MAX); i++)
		g_ScorePtrs[i] = 0;

	if (count > 1)
		qsort(g_ScorePtrs, count, sizeof(DWORD), ScoreEntryCompar);

	/*
	 * Stock score panel only fits 8 rows. Keep all 16 entries sorted in
	 * g_ScorePtrs, but clamp the engine count so the draw loop stops at 8
	 * (avoids short DEFINE_HOOKs that Syringe overwrites past instruction
	 * boundaries → AV at 0x5CA04B).
	 */
	if (count > 8)
	{
		*reinterpret_cast<DWORD*>(ADDR_SCORE_SLOT_COUNT) = 8;
		Log("[Score] rebuild ptrs full=%u display=8 first=%08X\n",
			count, g_ScorePtrs[0]);
	}
	else
	{
		Log("[Score] rebuild ptrs count=%u first=%08X\n",
			count, count ? g_ScorePtrs[0] : 0);
	}
}

/* 0x5C9AA0: entire rebuild function → ret at 0x5C9ADD */
/* length 1 = push esi only; skip whole function to ret (no pop needed) */
DEFINE_HOOK(0x5C9AA0, PlayerLimit16_Score_PtrRebuild1, 1)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	ScoreRebuildPtrTable();
	return 0x5C9ADD;
}

/*
 * Inlined second rebuild at 0x5C9D47 (mov ebp,[count]) through qsort.
 * Skip to 0x5C9D82 where original continues with stack locals.
 */
DEFINE_HOOK(0x5C9D47, PlayerLimit16_Score_PtrRebuild2, 6)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	ScoreRebuildPtrTable();
	return 0x5C9D82;
}


/* Map draw row → ptr table; stock panel only has room for 8 rows. */
static DWORD ScoreDrawIndex(DWORD row)
{
	const DWORD count = *reinterpret_cast<DWORD*>(ADDR_SCORE_SLOT_COUNT);
	if (row >= count || row >= static_cast<DWORD>(SCORE_SLOT_MAX))
		return 0;
	return row;
}

/* ---- draw path: mov reg, [index*4 + 0xABF958] (7 bytes) ---- */

DEFINE_HOOK(0x5C9DF4, PlayerLimit16_Score_DrawPtr_EDX, 7)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	R->EDX(g_ScorePtrs[ScoreDrawIndex(R->ECX())]);
	return 0x5C9DFB;
}

DEFINE_HOOK(0x5C9E8A, PlayerLimit16_Score_DrawPtr_ECX_a, 7)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	R->ECX(g_ScorePtrs[ScoreDrawIndex(R->EDX())]);
	return 0x5C9E91;
}

DEFINE_HOOK(0x5C9EC9, PlayerLimit16_Score_DrawPtr_ECX_b, 7)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	R->ECX(g_ScorePtrs[ScoreDrawIndex(R->EAX())]);
	return 0x5C9ED0;
}

DEFINE_HOOK(0x5C9F25, PlayerLimit16_Score_DrawPtr_ECX_c, 7)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	R->ECX(g_ScorePtrs[ScoreDrawIndex(R->EAX())]);
	return 0x5C9F2C;
}

DEFINE_HOOK(0x5C9F81, PlayerLimit16_Score_DrawPtr_ECX_d, 7)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	R->ECX(g_ScorePtrs[ScoreDrawIndex(R->EAX())]);
	return 0x5C9F88;
}

DEFINE_HOOK(0x5C9FDD, PlayerLimit16_Score_DrawPtr_ECX_e, 7)
{
	if (!PlayerLimit16::IsActive())
		return 0;
	R->ECX(g_ScorePtrs[ScoreDrawIndex(R->EAX())]);
	return 0x5C9FE4;
}
