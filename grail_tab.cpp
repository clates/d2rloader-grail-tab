// Grail Tab: a block of shared stash tabs with a home slot for every unique and
// set item. Empty home slots show the item's name; Ctrl+click a unique or set
// item in your inventory and it moves to its slot. A chalice button on the stash
// frame jumps to the grail and back.
//
// The tabs are ordinary shared stash tabs. Setup (the chalice, the first time)
// adds them after your existing ones by raising D2RCore's add_shared_tabs, with
// a backup of the config, and the plugin only ever fills empty home slots inside
// its block. The layout comes from gen_layout.py and is frozen by layout_v1.tsv.
//
// What the stash doesn't tell a plugin, it works out:
//   - which shared tab is showing: the stash's own Left/Right arrow messages,
//     anchored by clicked items (they report their tab) and by jumps the plugin
//     makes itself (Left until the first tab, which clamps, then Right);
//   - which top tab is showing: SelectTab has no payload, so the mouse position
//     on the tab bar when it fires;
//   - where the grid is on screen: the widget's layout rect, mapped with the HD
//     profile's left-panel anchor (the loader has no screen-rect call here).
//
// It never creates or edits items; filing moves the existing item.
// Offline single player only.

#include <D2RLPlugin/api.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "grail_layout.h"

static constexpr D2RL::PluginInfo Info {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "grail-tab",
	.name        = "Grail Tab",
	.version     = "1.0.0",
	.author      = "Lates",
	.description = "Shared stash tabs with a home slot for every unique and set item.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr int   VanillaSharedTabs = 5;      // the stash paginator: 105 tabs with add_shared_tabs = 100
static constexpr float ReferenceHeight   = 2160;   // HD layout units per screen height
static constexpr float PanelAnchorX = 0.0f, PanelAnchorY = 0.397f;   // _profilehd.json $LeftPanelAnchor
static constexpr int   TabBarX = 82, TabBarY = 146, TabWidth = 195, TabCount = 5, SharedTabIndex = 1;
static constexpr ULONGLONG RefreshMs = 250, ToastMs = 3000;

static constexpr char DefaultConfig[] =
	"# Grail Tab settings. Setup fills in first_tab; edit the rest freely.\n"
	"\n"
	"# Shared stash tab (1 = first) where the grail block starts. 0 = not set up.\n"
	"first_tab = 0\n"
	"# Include the 7 set item tabs after the 24 unique tabs.\n"
	"sets = true\n"
	"# Ctrl+click a unique or set item in your inventory to file it.\n"
	"ctrl_click = true\n"
	"# Where Ctrl+click files from: \"grail\" (only while a grail tab shows), \"shared\"\n"
	"# (any Shared tab page) or \"stash\" (any stash tab, personal too). Away from the\n"
	"# grail tabs, only items still missing from the grail are filed; the rest move as usual.\n"
	"ctrl_click_from = \"shared\"\n"
	"# On a grail tab, block Ctrl+click for items that have no grail slot.\n"
	"block_non_grail = true\n"
	"# Show the names of missing items in their empty slots.\n"
	"ghost_labels = true\n"
	"# Outline items in the grail tabs that aren't in their home slot.\n"
	"mark_misplaced = true\n"
	"# Give items sitting in their home slot a thin gold border.\n"
	"mark_filed = true\n"
	"# The chalice button on the stash frame: jump to the grail and back. Off means\n# only a key bound under Controls > Grail Tab does that.\n"
	"button = true\n";

static const D2RL::PluginContext*    g_ctx;
static const D2RL::WidgetService*    g_widgets;
static const D2RL::OverlayService*   g_overlay;
static const D2RL::ThreadService*    g_threads;
static const D2RL::ItemService*      g_items;
static const D2RL::InventoryService* g_inventory;

// ---------------------------------------------------------------- settings

enum class CtrlFrom { Grail, Shared, Stash };

struct Settings {
	int  firstTab = 0;   // 1-based, 0 = not set up
	CtrlFrom ctrlFrom = CtrlFrom::Shared;
	bool sets = true, ctrlClick = true, blockNonGrail = true, ghostLabels = true, markMisplaced = true, markFiled = true, button = true;
};

static Settings g_cfg;
static int      g_liveAddedTabs = -1;   // add_shared_tabs as the game launched with it (D2RCore applies changes on relaunch)
static int      g_fileAddedTabs = -1;   // add_shared_tabs in the file now, re-read when the stash opens
static wchar_t  g_loaderToml[MAX_PATH];

static auto BlockPages() noexcept -> int {
	return g_cfg.sets ? PageCount : UniquePageCount;
}

static auto TotalTabs() noexcept -> int {
	return g_liveAddedTabs < 0 ? -1 : VanillaSharedTabs + g_liveAddedTabs;
}

static auto FirstIndex() noexcept -> int {   // 0-based tab index of the block
	return g_cfg.firstTab - 1;
}

// TabsMissing: the configured tab count no longer reaches the grail block (the
// Options slider or an old backup lowered it), so the grail tabs are gone.
enum class Setup { NotSetUp, NeedsRestart, Active, TabsMissing };

static auto SetupState() noexcept -> Setup {
	if (g_cfg.firstTab <= 0) {
		return Setup::NotSetUp;
	}
	const int need = FirstIndex() + BlockPages();
	const int file = g_fileAddedTabs < 0 ? g_liveAddedTabs : g_fileAddedTabs;
	if (file >= 0 && VanillaSharedTabs + file < need) {
		return Setup::TabsMissing;
	}
	return TotalTabs() >= need ? Setup::Active : Setup::NeedsRestart;
}

static auto ParseBool(const char* value, bool fallback) noexcept -> bool {
	return std::strncmp(value, "true", 4) == 0 ? true : std::strncmp(value, "false", 5) == 0 ? false : fallback;
}

static void LoadSettings() noexcept {
	(void)g_ctx->EnsureConfig(DefaultConfig);
	char text[4096] = {};
	if (!g_ctx->ReadConfig(text, sizeof text)) {
		return;
	}
	for (char* line = std::strtok(text, "\r\n"); line != nullptr; line = std::strtok(nullptr, "\r\n")) {
		while (*line == ' ' || *line == '\t') {
			++line;
		}
		char* eq = std::strchr(line, '=');
		if (*line == '#' || eq == nullptr) {
			continue;
		}
		char* end = eq;
		while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
			--end;
		}
		*end = 0;
		const char* value = eq + 1;
		while (*value == ' ' || *value == '\t') {
			++value;
		}
		if (std::strcmp(line, "first_tab") == 0) g_cfg.firstTab = std::atoi(value);
		else if (std::strcmp(line, "sets") == 0) g_cfg.sets = ParseBool(value, true);
		else if (std::strcmp(line, "ctrl_click") == 0) g_cfg.ctrlClick = ParseBool(value, true);
		else if (std::strcmp(line, "ctrl_click_from") == 0) {
			if (*value == '"') {
				++value;
			}
			g_cfg.ctrlFrom = std::strncmp(value, "grail", 5) == 0 ? CtrlFrom::Grail : std::strncmp(value, "stash", 5) == 0 ? CtrlFrom::Stash : CtrlFrom::Shared;
		}
		else if (std::strcmp(line, "block_non_grail") == 0) g_cfg.blockNonGrail = ParseBool(value, true);
		else if (std::strcmp(line, "ghost_labels") == 0) g_cfg.ghostLabels = ParseBool(value, true);
		else if (std::strcmp(line, "mark_misplaced") == 0) g_cfg.markMisplaced = ParseBool(value, true);
		else if (std::strcmp(line, "mark_filed") == 0) g_cfg.markFiled = ParseBool(value, true);
		else if (std::strcmp(line, "button") == 0) g_cfg.button = ParseBool(value, true);
	}
}

static auto SaveFirstTab(int firstTab) noexcept -> bool {
	char text[4096] = {};
	if (!g_ctx->ReadConfig(text, sizeof text)) {
		return false;
	}
	char out[4200];
	char* line = std::strncmp(text, "first_tab", 9) == 0 ? text : std::strstr(text, "\nfirst_tab");
	if (line == nullptr) {   // someone deleted it: add it back at the end
		const size_t n = std::strlen(text);
		std::snprintf(out, sizeof out, "%s%sfirst_tab = %d\n", text, n && text[n - 1] != '\n' ? "\n" : "", firstTab);
		return g_ctx->WriteConfig(out);
	}
	line += *line == '\n';
	const char* rest = std::strchr(line, '\n');
	std::snprintf(out, sizeof out, "%.*sfirst_tab = %d%s", static_cast<int>(line - text), text, firstTab, rest ? rest : "\n");
	return g_ctx->WriteConfig(out);
}

// ---------------------------------------------------------------- d2rloader.toml (D2RCore's add_shared_tabs)

static auto ReadFile(const wchar_t* path, char* out, size_t size) noexcept -> size_t {
	FILE* f = _wfopen(path, L"rb");
	if (f == nullptr) {
		return 0;
	}
	const size_t n = std::fread(out, 1, size - 1, f);
	std::fclose(f);
	out[n] = 0;
	return n;
}

// Finds the add_shared_tabs line under [d2rcore.stash]; returns its value or -1.
static auto FindAddedTabs(char* text, char** lineStart, char** lineEnd) noexcept -> int {
	char* section = std::strstr(text, "[d2rcore.stash]");
	for (char* p = section; p != nullptr && *p; p = std::strchr(p, '\n') ? std::strchr(p, '\n') + 1 : nullptr) {
		if (p != section && *p == '[') {
			break;
		}
		if (std::strncmp(p, "add_shared_tabs", 15) == 0) {
			const char* eq = std::strchr(p, '=');
			char* end = std::strchr(p, '\n');
			if (lineStart) *lineStart = p;
			if (lineEnd) *lineEnd = end ? end : p + std::strlen(p);
			return eq ? std::atoi(eq + 1) : -1;
		}
	}
	return -1;
}

static auto ReadAddedTabs() noexcept -> int {
	static char text[256 * 1024];
	return ReadFile(g_loaderToml, text, sizeof text) ? FindAddedTabs(text, nullptr, nullptr) : -1;
}

// Raises add_shared_tabs by `extra`, keeping the rest of the file byte for byte.
static auto RaiseAddedTabs(int extra, int* before) noexcept -> bool {
	static char text[256 * 1024], out[256 * 1024 + 64];
	const size_t n = ReadFile(g_loaderToml, text, sizeof text);
	char *start = nullptr, *end = nullptr;
	*before = n ? FindAddedTabs(text, &start, &end) : -1;
	if (*before < 0) {
		return false;
	}
	// Named for what it is: restoring it after setup takes the grail tabs away.
	wchar_t backup[MAX_PATH + 48];
	std::swprintf(backup, MAX_PATH + 48, L"%ls.before-grail-tab.bak", g_loaderToml);
	if (!CopyFileW(g_loaderToml, backup, FALSE)) {
		return false;
	}
	const bool crlf = end > start && end[-1] == '\r';
	const int  len  = std::snprintf(out, sizeof out, "%.*sadd_shared_tabs = %d%s%s", static_cast<int>(start - text), text,
		*before + extra, crlf ? "\r" : "", end);
	wchar_t temp[MAX_PATH + 32];
	std::swprintf(temp, MAX_PATH + 32, L"%ls.grail-tab.tmp", g_loaderToml);
	FILE* f = _wfopen(temp, L"wb");
	if (f == nullptr) {
		return false;
	}
	const bool written = std::fwrite(out, 1, len, f) == static_cast<size_t>(len);
	std::fclose(f);
	return written && MoveFileExW(temp, g_loaderToml, MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------- shared state (UI thread writes, overlay reads)

enum class TopTab { Unknown, Shared, Other };

struct Misplaced {
	uint8_t page, x, y, w, h;
};

struct View {
	bool    stashOpen;
	TopTab  top;
	int     tab;                       // 0-based shared tab showing, -1 unknown
	bool    filled[sizeof Slots / sizeof Slots[0]];
	int     filledCount;
	Misplaced misplaced[256];
	int     misplacedCount;
	D2RL::Widgets::Rect panel, grid;   // layout rects
	char    toast[160];
	ULONGLONG toastUntil;
	bool    banner;                    // setup/restart banner on the stash
};

static View    g_view { .top = TopTab::Other, .tab = -1, .panel = { 236, -651, 1162, 1507 }, .grid = { 89, 219, 980, 980 } };
static SRWLOCK g_lock = SRWLOCK_INIT;
static float   g_screenW = 2560, g_screenH = 1440;   // latest overlay frame
static int     g_selfSends;                          // our own arrow messages still to come back
static ULONGLONG g_selfSendAt;
static uint32_t g_run;
static bool    g_debug;

struct Locked {
	Locked() noexcept { AcquireSRWLockExclusive(&g_lock); }
	~Locked() noexcept { ReleaseSRWLockExclusive(&g_lock); }
};

static void Say(const char* text) noexcept {
	g_ctx->WriteConsoleMessage(text);
	g_ctx->LogInfo(text);
}

static void Debug(const char* text) noexcept {
	if (g_debug) {
		Say(text);
	}
}

static void Toast(const char* text) noexcept {
	Say(text);
	Locked lock;
	std::snprintf(g_view.toast, sizeof g_view.toast, "%s", text);
	g_view.toastUntil = GetTickCount64() + ToastMs;
}

static auto FindSlot(D2RL::Items::Quality quality, int32_t row) noexcept -> int {
	const int kind = quality == D2RL::Items::Quality::Unique ? 0 : quality == D2RL::Items::Quality::Set ? 1 : -1;
	for (int i = 0; i < static_cast<int>(sizeof Slots / sizeof Slots[0]); ++i) {
		if (Slots[i].kind == kind && Slots[i].row == row && Slots[i].page < BlockPages()) {
			return i;
		}
	}
	return -1;
}

static auto BaseSizeOf(uint32_t code, uint8_t* w, uint8_t* h) noexcept -> bool {
	for (const BaseSize& base : BaseSizes) {
		const auto at = [&](int i) { return base.code[i] ? base.code[i] : ' '; };
		if (D2RL::Items::MakeItemCode(at(0), at(1), at(2), base.code[2] ? at(3) : ' ') == code) {
			*w = base.w;
			*h = base.h;
			return true;
		}
	}
	return false;
}

static auto InBlock(int tab) noexcept -> bool {
	return SetupState() == Setup::Active && tab >= FirstIndex() && tab < FirstIndex() + BlockPages();
}

// ---------------------------------------------------------------- stash contents (UI thread)

struct Scan {
	bool      filled[sizeof Slots / sizeof Slots[0]];
	Misplaced misplaced[256];
	int       misplacedCount;
};

static auto __cdecl ScanItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	auto* scan = static_cast<Scan*>(userData);
	const int tab = static_cast<int>(item->sharedStashPage);
	if (item->sharedStashPage == UINT32_MAX || !InBlock(tab)) {
		return D2RL::Inventory::IterationAction::Continue;
	}
	const int page = tab - FirstIndex();
	const int slot = FindSlot(item->quality, item->qualityRecordId);
	if (slot >= 0 && Slots[slot].page == page && Slots[slot].x == item->x && Slots[slot].y == item->y && !scan->filled[slot]) {
		scan->filled[slot] = true;
	} else if (scan->misplacedCount < 256) {
		uint8_t w = 1, h = 1;
		BaseSizeOf(item->code, &w, &h);
		scan->misplaced[scan->misplacedCount++] = { static_cast<uint8_t>(page), static_cast<uint8_t>(item->x), static_cast<uint8_t>(item->y), w, h };
	}
	return D2RL::Inventory::IterationAction::Continue;
}

static void Refresh() noexcept {
	if (SetupState() != Setup::Active) {
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(g_ctx, &player) != D2RL::Inventory::Result::Success) {
		return;
	}
	static Scan scan;
	scan = {};
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::SharedStash),
	};
	if (g_inventory->forEachInventoryItem(g_ctx, player, &filter, ScanItem, &scan) != D2RL::Inventory::Result::Success) {
		return;
	}
	int count = 0;
	for (bool f : scan.filled) {
		count += f;
	}
	Locked lock;
	std::memcpy(g_view.filled, scan.filled, sizeof scan.filled);
	std::memcpy(g_view.misplaced, scan.misplaced, sizeof scan.misplaced);
	g_view.misplacedCount = scan.misplacedCount;
	g_view.filledCount    = count;
}

static void ReadRects() noexcept {
	D2RL::Widgets::WidgetHandle bank = D2RL::Widgets::InvalidHandle, grid = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::Rect panelRect {}, gridRect {};
	if (g_widgets->findPanel(g_ctx, "BankExpansionLayout", &bank) != D2RL::Widgets::Result::Success
		|| g_widgets->getWidgetRect(g_ctx, bank, &panelRect) != D2RL::Widgets::Result::Success
		|| g_widgets->findWidget(g_ctx, bank, "grid", &grid) != D2RL::Widgets::Result::Success
		|| g_widgets->getWidgetRect(g_ctx, grid, &gridRect) != D2RL::Widgets::Result::Success || gridRect.width <= 0) {
		return;
	}
	Locked lock;
	g_view.panel = panelRect;
	g_view.grid  = gridRect;
}

static void JumpTo(int tab) noexcept;
static void SendArrow(const char* command, int times) noexcept;
static void SetTab(TopTab top, int tab) noexcept;
static int  g_showTab = -1;   // after filing: show this tab, and redraw it
static void PollButton() noexcept;

// Arrow presses from one tab to another when each turns `step` pages and stops at
// either end; false if no such path exists.
static bool StepTo(int from, int to, int step) noexcept {
	static constexpr int MaxTabs = 1024;
	const int count = TotalTabs();
	if (from < 0 || to < 0 || from >= count || to >= count || count > MaxTabs) {
		return false;
	}
	static int prev[MaxTabs], queue[MaxTabs], path[MaxTabs];
	for (int i = 0; i < count; ++i) {
		prev[i] = -1;
	}
	prev[from] = from;
	int head = 0, tail = 0;
	queue[tail++] = from;
	while (head < tail && prev[to] < 0) {
		const int at = queue[head++];
		for (const int next : { at - step < 0 ? 0 : at - step, at + step >= count ? count - 1 : at + step }) {
			if (prev[next] < 0) {
				prev[next] = at;
				queue[tail++] = next;
			}
		}
	}
	if (prev[to] < 0) {
		return false;
	}
	int length = 0;
	for (int at = to; at != from; at = prev[at]) {
		path[length++] = at;
	}
	for (int at = from; length > 0; at = path[length]) {
		--length;
		SendArrow(path[length] > at ? "SharedStashRight" : "SharedStashLeft", 1);
	}
	SetTab(TopTab::Shared, to);
	return true;
}

// The arrows read the keyboard: with Ctrl held (it is, right after a Ctrl+click)
// each one turns 10 pages, with Shift 5. Turn there now if those steps reach the
// page; otherwise wait until the keys are up.
static void ShowFiledTab() noexcept {
	if (g_showTab < 0) {
		return;
	}
	const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	const int  step     = ctrlHeld ? 10 : shiftHeld ? 5 : 1;
	const int  target   = g_showTab;
	if (!g_view.stashOpen || g_view.top != TopTab::Shared) {
		g_showTab = -1;
		return;
	}
	if (g_view.tab == target) {
		// The grid doesn't redraw for a move made on the game side; turning the
		// page away and back does.
		const int away = target - step >= 0 ? target - step : target + step < TotalTabs() ? target + step : -1;
		if (away < 0) {
			return;
		}
		StepTo(target, away, step);
		StepTo(away, target, step);
	} else if (step == 1) {
		JumpTo(target);
	} else if (!StepTo(g_view.tab, target, step)) {
		return;
	}
	g_showTab = -1;
}

static void __cdecl Tick(const D2RL::PluginContext*, void* userData) noexcept {
	if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userData)) != g_run || !g_view.stashOpen) {
		return;
	}
	static ULONGLONG next;
	const ULONGLONG now = GetTickCount64();
	if (now >= next) {
		next = now + RefreshMs;
		ReadRects();
		Refresh();
	}
	ShowFiledTab();
	PollButton();
	if (g_selfSends > 0 && now - g_selfSendAt > 1000) {
		g_selfSends = 0;   // some never came back; don't swallow real presses
	}
	g_threads->runOnUiThread(g_ctx, Tick, userData);
}

// ---------------------------------------------------------------- navigation (UI thread)

static void SetTab(TopTab top, int tab) noexcept {
	Locked lock;
	g_view.top = top;
	g_view.tab = tab;
}

static void SendArrow(const char* command, int times) noexcept {
	const D2RL::Widgets::UiAction action {
		.structSize = D2RL::Widgets::UiActionSize,
		.target     = "BankPanelMessage",
		.command    = command,
	};
	for (int i = 0; i < times; ++i) {
		++g_selfSends;
		g_selfSendAt = GetTickCount64();
		g_widgets->dispatchUiAction(g_ctx, &action);
	}
}

// From a known tab, step there. From an unknown one, Left until the first tab
// (the arrow stops there), then Right to the target.
static void JumpTo(int tab) noexcept {
	if (tab == g_view.tab) {
		return;
	}
	if (g_view.tab >= 0) {
		SendArrow(tab > g_view.tab ? "SharedStashRight" : "SharedStashLeft", tab > g_view.tab ? tab - g_view.tab : g_view.tab - tab);
	} else {
		SendArrow("SharedStashLeft", TotalTabs());
		SendArrow("SharedStashRight", tab);
	}
	SetTab(TopTab::Shared, tab);
	char line[96];
	std::snprintf(line, sizeof line, "grail-tab: jumped to tab %d.", tab + 1);
	Debug(line);
}

static void ScreenMap(float* k, float* panelX, float* panelY) noexcept {
	*k      = g_screenH / ReferenceHeight;
	*panelX = PanelAnchorX * (g_screenW / *k) + g_view.panel.x;
	*panelY = PanelAnchorY * ReferenceHeight + g_view.panel.y;
}

// SelectTab carries no payload: tell the tabs apart by where the mouse is.
static auto TopTabUnderMouse() noexcept -> TopTab {
	POINT p {};
	GetCursorPos(&p);
	if (HWND window = GetForegroundWindow()) {
		ScreenToClient(window, &p);
	}
	float k, px, py;
	ScreenMap(&k, &px, &py);
	const float x = p.x / k - px, y = p.y / k - py;
	char line[128];
	std::snprintf(line, sizeof line, "grail-tab: SelectTab at layout %.0f,%.0f", x, y);
	Debug(line);
	if (y < TabBarY - 40 || y > TabBarY + 120 || x < TabBarX || x >= TabBarX + TabCount * TabWidth) {
		return TopTab::Unknown;
	}
	return static_cast<int>((x - TabBarX) / TabWidth) == SharedTabIndex ? TopTab::Shared : TopTab::Other;
}

// ---------------------------------------------------------------- setup and the chalice (UI thread)

static void RunSetup() noexcept {
	const int need   = BlockPages();
	const int before = ReadAddedTabs();
	if (before < 0) {
		Toast("Grail Tab needs D2RCore's extra stash tabs: no add_shared_tabs under [d2rcore.stash] in d2rloader.toml.");
		return;
	}
	// Remember the block first: if raising the tab count then fails, nothing was
	// added, and first_tab goes back to 0.
	const int firstTab = VanillaSharedTabs + before + 1;
	if (!SaveFirstTab(firstTab)) {
		Toast("Grail Tab: couldn't save its settings.");
		return;
	}
	int raisedFrom = -1;
	if (!RaiseAddedTabs(need, &raisedFrom)) {
		(void)SaveFirstTab(0);
		Toast("Grail Tab: couldn't update d2rloader.toml.");
		return;
	}
	g_cfg.firstTab = firstTab;
	g_fileAddedTabs = before + need;
	char line[200];
	std::snprintf(line, sizeof line, "Grail Tab: shared tabs %d -> %d. Quit and relaunch the game to finish; the grail will be tabs %d-%d.",
		VanillaSharedTabs + before, VanillaSharedTabs + before + need, firstTab, firstTab + need - 1);
	Toast(line);
}

static void __cdecl OnGrail(const D2RL::PluginContext*, void*) noexcept {
	switch (SetupState()) {
	case Setup::NotSetUp: return g_view.stashOpen ? RunSetup() : Toast("Open your stash to set up Grail Tab.");
	case Setup::NeedsRestart: return Toast("Grail Tab: quit and relaunch the game to finish setup.");
	case Setup::TabsMissing: return Toast("Grail Tab: add_shared_tabs is too low for the grail tabs; see the banner.");
	case Setup::Active: break;
	}
	if (!g_view.stashOpen) {
		return Toast("Open your stash first.");
	}
	if (g_view.top != TopTab::Shared) {
		return Toast("Switch to the Shared tab first.");
	}
	// From a grail page, back to the first shared page; from anywhere else, to the grail.
	JumpTo(InBlock(g_view.tab) ? 0 : FirstIndex());
}

// ---------------------------------------------------------------- filing

// The click event's handles go stale before the game thread runs (result 9),
// so carry the item's identity and look it up again there, with the player
// handle from getLocalPlayer as the SDK asks.
struct FileRequest {
	D2RL::PlayerHandle  player;
	uint32_t            runtimeId, itemSeed, code;
	int32_t             x, y;   // inventory cell, the same on both threads
	int                 slot;
	D2RL::Items::Result result;
	D2RL::ItemHandle    item;   // found again on the game thread
	D2RL::ItemHandle    byPosition;
	int                 seen;
	char                log[200];
	bool                show;   // turn to its page afterwards (filed from a grail tab)
};

static auto __cdecl FindByIdentity(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	auto* request = static_cast<FileRequest*>(userData);
	++request->seen;
	if (item->code == request->code && item->x == request->x && item->y == request->y) {
		request->byPosition = item->handle;
		if (request->log[0] == 0) {
			std::snprintf(request->log, sizeof request->log, "game-thread copy: id %u seed %u (UI thread: id %u seed %u)", item->runtimeId,
				item->itemSeed, request->runtimeId, request->itemSeed);
		}
	}
	if (item->runtimeId != request->runtimeId || item->itemSeed != request->itemSeed) {
		return D2RL::Inventory::IterationAction::Continue;
	}
	request->item = item->handle;
	return D2RL::Inventory::IterationAction::Stop;
}

static void __cdecl AfterFile(const D2RL::PluginContext*, void* userData) noexcept {
	auto* request = static_cast<FileRequest*>(userData);
	const Slot& slot = Slots[request->slot];
	char line[192];
	if (request->result == D2RL::Items::Result::Success) {
		std::snprintf(line, sizeof line, "Filed %s.", slot.name);
		if (request->show) {
			g_showTab = FirstIndex() + slot.page;   // Tick turns there once Ctrl is up
		} else {
			std::snprintf(line, sizeof line, "Filed %s (grail page %d).", slot.name, slot.page + 1);
		}
	} else if (request->result == D2RL::Items::Result::NotFound) {
		std::snprintf(line, sizeof line, "Couldn't find %s in your inventory anymore.", slot.name);
	} else if (request->result == D2RL::Items::Result::Conflict) {
		std::snprintf(line, sizeof line, "Something is in %s's slot.", slot.name);
	} else {
		std::snprintf(line, sizeof line, "Couldn't file %s (result %u).", slot.name, static_cast<unsigned>(request->result));
	}
	delete request;
	Toast(line);
	Refresh();
}

static void __cdecl FileItem(const D2RL::PluginContext*, void* userData) noexcept {
	auto* request = static_cast<FileRequest*>(userData);
	const Slot& slot = Slots[request->slot];
	const D2RL::Inventory::ItemFilter inventory {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory),
	};
	const auto found = g_inventory->forEachInventoryItem(g_ctx, request->player, &inventory, FindByIdentity, request);
	if (request->item == 0) {
		request->item = request->byPosition;   // same base in the same cell
	}
	if (g_debug) {
		char line[320];
		std::snprintf(line, sizeof line, "grail-tab: lookup result %u, %d items seen, %s; %s", static_cast<unsigned>(found), request->seen,
			request->item ? "found" : "not found", request->log[0] ? request->log : "no item at that cell");
		g_ctx->LogInfo(line);
	}
	if (found != D2RL::Inventory::Result::Success || request->item == 0) {
		request->result = D2RL::Items::Result::NotFound;
		if (g_threads->runOnUiThread(g_ctx, AfterFile, request) != D2RL::Threads::Result::Success) {
			delete request;
		}
		return;
	}
	D2RL::Items::ExistingItemOperation op {
		.structSize = D2RL::Items::ExistingItemOperationSize,
		.kind       = D2RL::Items::ExistingItemOperationKind::Move,
		.item       = request->item,
	};
	op.move.destination = {
		.structSize      = D2RL::Items::ItemDestinationSize,
		.container       = D2RL::Items::ItemContainer::SharedStash,
		.placement       = D2RL::Items::Placement::Exact,
		.x               = slot.x,
		.y               = slot.y,
		.sharedStashPage = static_cast<uint32_t>(FirstIndex() + slot.page),
	};
	const D2RL::Items::ExistingItemTransaction transaction {
		.structSize     = D2RL::Items::ExistingItemTransactionSize,
		.player         = request->player,
		.operationCount = 1,
		.operations     = &op,
	};
	D2RL::Items::ExistingItemTransactionResult result { .structSize = D2RL::Items::ExistingItemTransactionResultSize };
	request->result = g_items->executeExistingItemTransaction(g_ctx, &transaction, &result);
	if (g_threads->runOnUiThread(g_ctx, AfterFile, request) != D2RL::Threads::Result::Success) {
		delete request;
	}
}

static auto __cdecl OnItemClick(const D2RL::PluginContext*, const D2RL::ItemInteractions::ItemInteractionEvent* event, void*) noexcept
	-> D2RL::ItemInteractions::Decision {
	using D2RL::ItemInteractions::Decision;
	if (event == nullptr || event->structSize < D2RL::ItemInteractions::ItemInteractionEventRequiredSize) {
		return Decision::Continue;
	}
	D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
	const bool known = g_items->getItemInfo(g_ctx, event->item, &info) == D2RL::Items::Result::Success;

	// Every stash click tells us where we are.
	if (event->container == D2RL::Items::ItemContainer::SharedStash && known && info.sharedStashPage != UINT32_MAX) {
		SetTab(TopTab::Shared, static_cast<int>(info.sharedStashPage));
	} else if (event->container == D2RL::Items::ItemContainer::PersonalStash) {
		SetTab(TopTab::Other, g_view.tab);
	}

	const bool ctrl = (event->modifiers & D2RL::ItemInteractions::ModifierBit(D2RL::ItemInteractions::Modifier::Control)) != 0;
	if (g_debug) {
		char line[200];
		std::snprintf(line, sizeof line, "grail-tab: click container %u mods 0x%x | stash %d top %d tab %d in-block %d | item q%u row %d tab %d",
			static_cast<unsigned>(event->container), event->modifiers, g_view.stashOpen, static_cast<int>(g_view.top), g_view.tab + 1,
			InBlock(g_view.tab), static_cast<unsigned>(info.quality), info.qualityRecordId, static_cast<int>(info.sharedStashPage) + 1);
		Say(line);
	}
	// Away from the grail tabs, only missing grail items are taken; anything else
	// gets the game's own Ctrl+click.
	const bool onGrailTab = g_view.top == TopTab::Shared && InBlock(g_view.tab);
	const bool inScope    = onGrailTab || g_cfg.ctrlFrom == CtrlFrom::Stash || (g_cfg.ctrlFrom == CtrlFrom::Shared && g_view.top == TopTab::Shared);
	if (!ctrl || !g_cfg.ctrlClick || event->container != D2RL::Items::ItemContainer::Inventory || !g_view.stashOpen || !inScope
		|| SetupState() != Setup::Active) {
		return Decision::Continue;
	}
	if (!onGrailTab && (!known || FindSlot(info.quality, info.qualityRecordId) < 0 || g_view.filled[FindSlot(info.quality, info.qualityRecordId)])) {
		return Decision::Continue;
	}
	if (!known) {
		Toast("Couldn't read that item.");
		return Decision::Consume;
	}
	const int slot = FindSlot(info.quality, info.qualityRecordId);
	if (slot < 0) {
		if (!g_cfg.blockNonGrail) {
			return Decision::Continue;
		}
		Toast(info.quality == D2RL::Items::Quality::Unique || info.quality == D2RL::Items::Quality::Set
			? "That one has no grail slot." : "Only unique and set items go here.");
		return Decision::Consume;
	}
	if (g_view.filled[slot]) {
		char line[128];
		std::snprintf(line, sizeof line, "Already filed: %s.", Slots[slot].name);
		Toast(line);
		return Decision::Consume;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(g_ctx, &player) != D2RL::Inventory::Result::Success) {
		Toast("Couldn't find your character.");
		return Decision::Consume;
	}
	auto* request = new FileRequest { player, info.runtimeId, info.itemSeed, info.code, info.x, info.y, slot, D2RL::Items::Result::Success };
	request->show = onGrailTab;
	if (g_threads->runOnGameThread(g_ctx, FileItem, request) != D2RL::Threads::Result::Success) {
		delete request;
		Toast("Couldn't queue the move.");
	}
	return Decision::Consume;
}

// ---------------------------------------------------------------- UI messages and input

static void StashOpened(bool open) noexcept {
	if (open) {
		g_fileAddedTabs = ReadAddedTabs();
	}
	{
		Locked lock;
		g_view.stashOpen = open;
		g_view.banner    = open && SetupState() != Setup::Active;
	}
	if (open) {
		g_threads->runOnUiThread(g_ctx, Tick, reinterpret_cast<void*>(static_cast<uintptr_t>(++g_run)));
	} else {
		g_selfSends = 0;
		g_showTab   = -1;
	}
}

// Every game opens the stash on the Personal tab, and the shared page it shows
// later isn't the one we knew; forget it until a hover, a click or a jump says.
static void __cdecl OnGameplay(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (event != nullptr && event->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) {
		Locked lock;
		g_view.top = TopTab::Other;
		g_view.tab = -1;
	}
}

static auto __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	if (event == nullptr || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize || event->target == nullptr || event->command == nullptr) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	const char* text = event->text ? event->text : "";
	if (std::strcmp(event->target, "PanelManager") == 0 && std::strcmp(text, "BankExpansionLayout") == 0) {
		if (std::strcmp(event->command, "OpenPanel") == 0) StashOpened(true);
		else if (std::strcmp(event->command, "ClosePanel") == 0) StashOpened(false);
	} else if (std::strcmp(event->target, "BankPanelMessage") == 0) {
		const bool left = std::strcmp(event->command, "SharedStashLeft") == 0, right = std::strcmp(event->command, "SharedStashRight") == 0;
		if ((left || right) && g_selfSends > 0) {
			--g_selfSends;   // one of ours; JumpTo already set the tab
			if (g_selfSends == 0) {
				Debug("grail-tab: our arrow messages all came back.");
			}
		} else if (left || right) {
			// Ctrl+click on an arrow turns 10 pages, Shift+click 5; the arrows stop at either end.
			const bool ctrlHeld  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			const int  step      = (left ? -1 : 1) * (ctrlHeld ? 10 : shiftHeld ? 5 : 1);
			const int  last     = TotalTabs() > 0 ? TotalTabs() - 1 : INT32_MAX;
			const int  tab      = g_view.tab < 0 ? -1 : g_view.tab + step < 0 ? 0 : g_view.tab + step > last ? last : g_view.tab + step;
			SetTab(TopTab::Shared, tab);
			char line[128];
			std::snprintf(line, sizeof line, "grail-tab: arrow %s -> tab %d (shift %d, ctrl %d)", left ? "left" : "right", tab + 1,
				(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
			Debug(line);
		} else if (std::strcmp(event->command, "SelectTab") == 0) {
			SetTab(TopTabUnderMouse(), g_view.tab);   // the shared tab is remembered by the stash
		}
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

// The overlay can only draw after the game UI, so it steps aside for item
// tooltips. If the event repeats while the tooltip stays up, the short window
// covers it; if it fires once per hover, the mouse staying put does.
static ULONGLONG    g_tooltipAt;
static POINT        g_tooltipMouse;
static volatile long g_tooltipEvents;

static void MousePos(POINT* p) noexcept {
	GetCursorPos(p);
	if (HWND window = GetForegroundWindow()) {
		ScreenToClient(window, p);
	}
}

// Also the best tab anchor there is: it fires every frame while you hover an
// item, and a shared-stash item knows its tab.
static void __cdecl OnTooltip(const D2RL::PluginContext*, D2RL::SharedEvents::ItemTooltipEvent* event, void*) noexcept {
	MousePos(&g_tooltipMouse);
	g_tooltipAt = GetTickCount64();
	InterlockedIncrement(&g_tooltipEvents);
	D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
	if (event == nullptr || !g_view.stashOpen || g_items->getItemInfo(g_ctx, event->item, &info) != D2RL::Items::Result::Success
		|| info.container != D2RL::Items::ItemContainer::SharedStash || info.sharedStashPage == UINT32_MAX
		|| (g_view.top == TopTab::Shared && g_view.tab == static_cast<int>(info.sharedStashPage))) {
		return;
	}
	char line[96];
	std::snprintf(line, sizeof line, "grail-tab: hover resync, tab %d -> %u.", g_view.tab + 1, info.sharedStashPage + 1);
	Debug(line);
	SetTab(TopTab::Shared, static_cast<int>(info.sharedStashPage));
}

static auto TooltipShowing() noexcept -> bool {
	const ULONGLONG age = GetTickCount64() - g_tooltipAt;
	if (age < 150) {
		return true;
	}
	POINT now;
	MousePos(&now);
	return age < 30000 && std::abs(now.x - g_tooltipMouse.x) <= 4 && std::abs(now.y - g_tooltipMouse.y) <= 4;
}

// ---------------------------------------------------------------- chalice button

// On the stash frame right of the grid's top row, spanning from the grid to the
// panel's outer edge less a couple of pixels. The overlay can't take input, so
// Tick watches the mouse button; the game sees the click too, but the frame does
// nothing with it.
static constexpr float ButtonDrop = 26, ButtonHeight = 56;
static ULONGLONG g_buttonPressedAt;

static void ButtonRect(const View& view, float* l, float* t, float* r, float* b) noexcept {
	float k, px, py;
	ScreenMap(&k, &px, &py);
	*l = static_cast<float>(static_cast<int>((px + view.grid.x + view.grid.width) * k) + 2);
	*r = static_cast<float>(static_cast<int>((px + view.panel.width) * k) - 2);
	*t = static_cast<float>(static_cast<int>((py + view.grid.y + ButtonDrop) * k));
	*b = *t + static_cast<float>(static_cast<int>(ButtonHeight * k));
}

static auto ButtonShown(const View& view) noexcept -> bool {
	return g_cfg.button && view.stashOpen && view.grid.width > 0 && view.panel.width > view.grid.x + view.grid.width;
}

static auto MouseOverButton(const View& view) noexcept -> bool {
	float l, t, r, b;
	ButtonRect(view, &l, &t, &r, &b);
	POINT p;
	MousePos(&p);
	return p.x >= l && p.x < r && p.y >= t && p.y < b;
}

static void PollButton() noexcept {
	static bool wasDown;
	const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	const bool pressed = down && !wasDown;
	wasDown = down;
	DWORD pid = 0;
	if (HWND window = GetForegroundWindow()) {
		GetWindowThreadProcessId(window, &pid);
	}
	if (pressed && pid == GetCurrentProcessId() && ButtonShown(g_view) && MouseOverButton(g_view)) {
		g_buttonPressedAt = GetTickCount64();
		OnGrail(g_ctx, nullptr);
	}
}

static auto __cdecl OnKey(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
	if (!D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize) || event->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	g_threads->runOnUiThread(g_ctx, OnGrail, nullptr);
	return D2RL::Input::ActionResult::Handled;
}

// ---------------------------------------------------------------- overlay

// On D2RLoader 1.3.1 the overlay's clip rectangle arrives broken (a float2
// mismatch between D2RCore and the game's ImGui), so every draw is clipped
// away while reporting success. Like Floating Damage (Pyre-Flies/D2R-Mods),
// push a full-screen clip on the game's draw list around our drawing. The
// addresses were found on D2R 3.3.93787 and still match 3.3.94995 (D2RLoader
// 1.3.1-beta); the guards are the game's own instruction bytes there, checked
// once at load, and any mismatch leaves this off.
static constexpr uint64_t DrawListRva = 0xCEAED0, PushClipRva = 0xD03A30, PopClipRva = 0xD03760;
static constexpr uint8_t  DrawListGuard[] = { 0x48, 0x8b, 0x0d, 0x79, 0x5f, 0x80, 0x02, 0x48, 0x8b, 0x81, 0x80, 0x1a, 0x00, 0x00 };
static constexpr uint8_t  PushClipGuard[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x57, 0x48, 0x83, 0xec, 0x40 };
static constexpr uint8_t  PopClipGuard[]  = { 0x8b, 0x41, 0x60, 0x83, 0xe8, 0x01, 0x89, 0x41, 0x60 };
static bool g_clipFix;

static auto Packed(float x, float y) noexcept -> uint64_t {
	uint64_t packed;
	const float pair[2] = { x, y };
	std::memcpy(&packed, pair, sizeof packed);
	return packed;
}

static auto PushScreenClip(float width, float height) noexcept -> void* {
	__try {
		void* list = reinterpret_cast<void*(__fastcall*)()>(g_ctx->exeBase + DrawListRva)();
		if (list != nullptr) {
			reinterpret_cast<void(__fastcall*)(void*, uint64_t, uint64_t, bool)>(g_ctx->exeBase + PushClipRva)(list, Packed(0, 0), Packed(width, height), false);
		}
		return list;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_clipFix = false;
		return nullptr;
	}
}

static void PopScreenClip(void* list) noexcept {
	__try {
		reinterpret_cast<void(__fastcall*)(void*)>(g_ctx->exeBase + PopClipRva)(list);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_clipFix = false;
	}
}

static constexpr D2RL::Overlay::Color UniqueGhost { 0.89f, 0.80f, 0.52f, 1.0f }, SetGhost { 0.35f, 0.95f, 0.35f, 1.0f },
	GhostOutline { 0.60f, 0.52f, 0.34f, 0.55f }, GhostBacking { 0.04f, 0.03f, 0.02f, 0.82f }, MisplacedRed { 0.95f, 0.15f, 0.15f, 0.95f }, FiledGold { 0.85f, 0.70f, 0.35f, 1.0f },
	HeaderGold { 0.89f, 0.80f, 0.52f, 1.0f }, ToastWhite { 1, 1, 1, 1 }, Shadow { 0, 0, 0, 1 };

// Logs the first failure of each draw call, from the render thread.
static void DrawResult(int which, D2RL::Overlay::Result result) noexcept {
	static bool logged[4];
	if (result != D2RL::Overlay::Result::Success && !logged[which]) {
		logged[which] = true;
		static constexpr const char* Names[] = { "rectangle", "filled rectangle", "text", "measure" };
		char line[96];
		std::snprintf(line, sizeof line, "grail-tab: overlay %s failed (result %u).", Names[which], static_cast<unsigned>(result));
		g_ctx->LogInfo(line);
	}
}

static void Rect(D2RL::Overlay::CanvasHandle canvas, float l, float t, float r, float b, D2RL::Overlay::Color color, float thickness) noexcept {
	const D2RL::Overlay::RectangleRequest request {
		.structSize = D2RL::Overlay::RectangleRequestSize, .canvas = canvas, .rect = { l, t, r, b }, .color = color, .thickness = thickness };
	DrawResult(0, g_overlay->drawRectangle(g_ctx, &request));
}

static void Fill(D2RL::Overlay::CanvasHandle canvas, float l, float t, float r, float b, D2RL::Overlay::Color color) noexcept {
	const D2RL::Overlay::FilledRectangleRequest request {
		.structSize = D2RL::Overlay::FilledRectangleRequestSize, .canvas = canvas, .rect = { l, t, r, b }, .color = color };
	DrawResult(1, g_overlay->drawFilledRectangle(g_ctx, &request));
}

static auto Measure(D2RL::Overlay::CanvasHandle canvas, const char* text, int length, float size) noexcept -> float {
	const D2RL::Overlay::MeasureTextRequest request {
		.structSize = D2RL::Overlay::MeasureTextRequestSize, .canvas = canvas, .textSize = size, .text = text, .textLength = static_cast<uint32_t>(length) };
	D2RL::Overlay::TextMetrics metrics { .structSize = D2RL::Overlay::TextMetricsSize };
	const auto result = g_overlay->measureText(g_ctx, &request, &metrics);
	DrawResult(3, result);
	return result == D2RL::Overlay::Result::Success ? metrics.width : length * size * 0.5f;
}

// Pixel-aligned text with a one-pixel black outline, like the game's own labels.
static void Text(D2RL::Overlay::CanvasHandle canvas, float x, float y, const char* text, int length, float size, D2RL::Overlay::Color color) noexcept {
	x = static_cast<float>(static_cast<int>(x + 0.5f));
	y = static_cast<float>(static_cast<int>(y + 0.5f));
	D2RL::Overlay::TextRequest request {
		.structSize = D2RL::Overlay::TextRequestSize, .canvas = canvas, .color = Shadow,
		.textSize = size, .text = text, .textLength = static_cast<uint32_t>(length) };
	static constexpr float Offsets[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
	for (const auto& offset : Offsets) {
		request.position = { x + offset[0], y + offset[1] };
		DrawResult(2, g_overlay->drawText(g_ctx, &request));
	}
	request.position = { x, y };
	request.color    = color;
	DrawResult(2, g_overlay->drawText(g_ctx, &request));
}

// Word-wraps a name into the slot, centered, at most four lines.
static void SlotLabel(D2RL::Overlay::CanvasHandle canvas, const char* name, float l, float t, float w, float h, float size, D2RL::Overlay::Color color) noexcept {
	const char* lines[4];
	int lengths[4], count = 0;
	const char* p = name;
	while (*p && count < 4) {
		int fit = 0;
		for (int end = 0;; ++end) {
			if (p[end] == ' ' || p[end] == 0) {
				if (fit == 0 || Measure(canvas, p, end, size) <= w - 6) {
					fit = end;
				} else {
					break;
				}
				if (p[end] == 0) {
					break;
				}
			}
		}
		lines[count] = p;
		lengths[count++] = fit;
		p += fit;
		while (*p == ' ') {
			++p;
		}
	}
	const float lineHeight = size * 1.15f;
	float y = t + (h - count * lineHeight) / 2;
	for (int i = 0; i < count; ++i, y += lineHeight) {
		Text(canvas, l + (w - Measure(canvas, lines[i], lengths[i], size)) / 2, y, lines[i], lengths[i], size, color);
	}
}

static void DrawStash(D2RL::Overlay::CanvasHandle canvas, const View& view, float gx, float gy, float cell, float size) noexcept;

// 16x16 pixel art. K outline, G gold, L light, D shade, R ruby, W glint.
static constexpr const char* Chalice[] = {
	"................",
	"..KKKKKKKKKKKK..",
	"..KLGGGGGGGGDK..",
	"..KLGGGGGGGGDK..",
	"..KLGGGWRGGGDK..",
	"...KLGGRRGGDK...",
	"...KLGGGGGGDK...",
	"....KLGGGGDK....",
	".....KKGGKK.....",
	"......KGDK......",
	"......KGDK......",
	".....KLGGDK.....",
	"....KLGGGGDK....",
	"...KLGGGGGGDK...",
	"...KKKKKKKKKK...",
	"................",
};

static auto ChaliceColor(char c) noexcept -> D2RL::Overlay::Color {
	switch (c) {
	case 'K': return { 0.08f, 0.05f, 0.03f, 1 };
	case 'G': return { 0.78f, 0.63f, 0.32f, 1 };
	case 'L': return { 0.94f, 0.84f, 0.55f, 1 };
	case 'D': return { 0.51f, 0.38f, 0.16f, 1 };
	case 'R': return { 0.67f, 0.12f, 0.12f, 1 };
	case 'W': return { 1.00f, 0.59f, 0.59f, 1 };
	default:  return { 0, 0, 0, 0 };
	}
}

static void DrawButton(D2RL::Overlay::CanvasHandle canvas, const View& view) noexcept {
	if (!ButtonShown(view)) {
		return;
	}
	float l, t, r, b;
	ButtonRect(view, &l, &t, &r, &b);
	const bool hover   = MouseOverButton(view);
	const bool pressed = GetTickCount64() - g_buttonPressedAt < 150;
	// A bevelled bronze plate: dark rim, gold border, lit top edge, shaded bottom edge.
	// Hovered, the bronze warms a little and the border brightens.
	Fill(canvas, l - 1, t - 1, r + 1, b + 1, { 0, 0, 0, 1 });
	Fill(canvas, l, t, r, b, pressed ? D2RL::Overlay::Color { 0.07f, 0.05f, 0.04f, 1 }
		: hover ? D2RL::Overlay::Color { 0.22f, 0.17f, 0.11f, 1 } : D2RL::Overlay::Color { 0.16f, 0.12f, 0.09f, 1 });
	if (hover && !pressed) {
		Fill(canvas, l + 3, t + 3, r - 3, b - 3, { 0.89f, 0.75f, 0.40f, 0.06f });
	}
	Fill(canvas, l, t + (b - t) / 2, r, b, { 0, 0, 0, pressed ? 0.1f : 0.3f });
	Fill(canvas, l + 3, t + 3, r - 3, t + 4, { 1, 0.9f, 0.7f, pressed ? 0.05f : hover ? 0.35f : 0.25f });
	Fill(canvas, l + 3, b - 4, r - 3, b - 3, { 0, 0, 0, 0.5f });
	static constexpr float Fade[] = { 1.0f, 0.75f, 0.5f };   // the filed-item border
	for (int ring = 0; ring < 3; ++ring) {
		D2RL::Overlay::Color color = hover ? HeaderGold : FiledGold;
		color.alpha *= Fade[ring];
		Rect(canvas, l + ring, t + ring, r - ring, b - ring, color, 1.0f);
	}

	const int px = (std::max)(1, static_cast<int>(((std::min)(r - l, b - t) - 8) / 16));
	const float ix = static_cast<float>(static_cast<int>(l + (r - l - 16 * px) / 2) + (pressed ? 1 : 0));
	const float iy = static_cast<float>(static_cast<int>(t + (b - t - 16 * px) / 2) + (pressed ? 1 : 0));
	for (int y = 0; y < 16; ++y) {
		for (int x = 0; x < 16; ++x) {
			const auto color = ChaliceColor(Chalice[y][x]);
			if (color.alpha > 0) {
				Fill(canvas, ix + x * px, iy + y * px, ix + (x + 1) * px, iy + (y + 1) * px, color);
			}
		}
	}
}

static void DrawFrame(const D2RL::Overlay::Frame* frame) noexcept {
	g_screenW = frame->screenWidth;
	g_screenH = frame->screenHeight;
	static bool first = true;
	if (first) {
		first = false;
		char line[128];
		std::snprintf(line, sizeof line, "grail-tab: overlay running, screen %.0fx%.0f, default text %.1f", frame->screenWidth, frame->screenHeight,
			frame->defaultTextSize);
		g_ctx->LogInfo(line);
	}
	static uint64_t frames;
	if (++frames % 1200 == 0 && g_debug) {
		char line[128];
		std::snprintf(line, sizeof line, "grail-tab: overlay frame %llu (game frame %llu), tooltip events %ld", static_cast<unsigned long long>(frames),
			static_cast<unsigned long long>(frame->frameNumber), g_tooltipEvents);
		g_ctx->LogInfo(line);
	}

	static View view;
	{
		AcquireSRWLockShared(&g_lock);
		view = g_view;
		ReleaseSRWLockShared(&g_lock);
	}
	float k, px, py;
	ScreenMap(&k, &px, &py);
	const float gx = (px + view.grid.x) * k, gy = (py + view.grid.y) * k;
	const float cell = view.grid.width / static_cast<float>(GridWidth) * k;
	// The overlay font is a pixel font: whole multiples of its native size stay sharp.
	const float base = frame->defaultTextSize > 0 ? frame->defaultTextSize : 13.0f;
	const float size = base * (cell >= 5 * base ? static_cast<float>(static_cast<int>(cell / (5 * base))) : 1.0f);
	const auto canvas = frame->canvas;

	if (view.stashOpen) {
		DrawStash(canvas, view, gx, gy, cell, size);
	}
	// Last, so the slot tiles don't cover it.
	if (view.toastUntil > GetTickCount64() && view.toast[0]) {   // over the grid, or the screen when the stash is closed
		const int len = static_cast<int>(std::strlen(view.toast));
		const float tw = Measure(canvas, view.toast, len, size);
		const float tx = view.stashOpen ? gx + (GridWidth * cell - tw) / 2 : (g_screenW - tw) / 2;
		const float ty = view.stashOpen ? gy + GridHeight * cell / 2 - size : g_screenH * 0.3f;
		Fill(canvas, tx - 12, ty - 8, tx + tw + 12, ty + size + 10, { 0, 0, 0, 0.85f });
		Text(canvas, tx, ty, view.toast, len, size, ToastWhite);
	}
}

static void DrawStash(D2RL::Overlay::CanvasHandle canvas, const View& view, float gx, float gy, float cell, float size) noexcept {
	const float gridBottom = gy + GridHeight * cell;
	DrawButton(canvas, view);
	if (view.banner) {
		static constexpr char Banner[] = "Grail Tab: click the chalice to add its shared tabs (then quit and relaunch the game).";
		static constexpr char Restart[] = "Grail Tab: quit and relaunch the game to finish setup.";
		const Setup state = SetupState();
		if (state == Setup::TabsMissing) {
			char text[200];
			const int len = std::snprintf(text, sizeof text, "Grail Tab: your grail tabs are gone. Set add_shared_tabs to at least %d in d2rloader.toml and relaunch.",
				FirstIndex() + BlockPages() - VanillaSharedTabs);
			Text(canvas, gx, gridBottom + 6, text, len, size, MisplacedRed);
			return;
		}
		const char* text = state == Setup::NeedsRestart ? Restart : Banner;
		Text(canvas, gx, gridBottom + 6, text, static_cast<int>(std::strlen(text)), size, HeaderGold);
		return;
	}
	if (view.top != TopTab::Shared || !InBlock(view.tab) || TooltipShowing()) {
		return;
	}
	const int page = view.tab - FirstIndex();

	for (int i = 0; i < static_cast<int>(sizeof Slots / sizeof Slots[0]); ++i) {
		const Slot& slot = Slots[i];
		if (slot.page != page) {
			continue;
		}
		if (view.filled[i]) {
			if (g_cfg.markFiled) {
				// Three 1 px rings, fading inward: solid, 75%, 50%.
				const float l = gx + slot.x * cell, t = gy + slot.y * cell, r = l + slot.w * cell, b = t + slot.h * cell;
				static constexpr float Fade[] = { 1.0f, 0.75f, 0.5f };
				for (int ring = 0; ring < 3; ++ring) {
					D2RL::Overlay::Color color = FiledGold;
					color.alpha *= Fade[ring];
					Rect(canvas, l + 1 + ring, t + 1 + ring, r - 1 - ring, b - 1 - ring, color, 1.0f);
				}
			}
			continue;
		}
		if (!g_cfg.ghostLabels) {
			continue;
		}
		const float l = gx + slot.x * cell, t = gy + slot.y * cell, w = slot.w * cell, h = slot.h * cell;
		Fill(canvas, l + 2, t + 2, l + w - 2, t + h - 2, GhostBacking);   // hides the cell lines behind the name
		Rect(canvas, l + 2, t + 2, l + w - 2, t + h - 2, GhostOutline, 1.0f);
		SlotLabel(canvas, slot.name, l, t, w, h, size, slot.kind == 0 ? UniqueGhost : SetGhost);
	}
	if (g_cfg.markMisplaced) {
		for (int i = 0; i < view.misplacedCount; ++i) {
			const Misplaced& m = view.misplaced[i];
			if (m.page == page) {
				Rect(canvas, gx + m.x * cell + 1, gy + m.y * cell + 1, gx + (m.x + m.w) * cell - 1, gy + (m.y + m.h) * cell - 1, MisplacedRed, 3.0f);
			}
		}
	}
	// The grail page on a small plate centred under the stash's paginator, over the
	// seam above the gold box (layout y 1313, 114 below the grid); the counts go
	// to the right of the paginator.
	char line[96];
	int len = std::snprintf(line, sizeof line, "Grail %d/%d", page + 1, BlockPages());
	{
		const float tw = Measure(canvas, line, len, size);
		const float cx = gx + GridWidth * cell / 2, cy = gridBottom + 114 * cell / 98 - 1;
		const float l = static_cast<float>(static_cast<int>(cx - tw / 2 - 8)), r = static_cast<float>(static_cast<int>(cx + tw / 2 + 8));
		const float t = static_cast<float>(static_cast<int>(cy - size / 2 - 4)), b = static_cast<float>(static_cast<int>(cy + size / 2 + 4));
		Fill(canvas, l, t, r, b, { 0.05f, 0.04f, 0.03f, 0.95f });
		D2RL::Overlay::Color ring = FiledGold;
		ring.alpha = 0.75f;
		Rect(canvas, l, t, r, b, ring, 1.0f);
		Text(canvas, static_cast<float>(static_cast<int>(cx - tw / 2)), static_cast<float>(static_cast<int>(cy - size / 2)), line, len, size, HeaderGold);
	}
	// Right of the paginator (layout x 789, 700 past the grid's left edge): the section
	// this page belongs to, in a two-line block (wrapped at commas, centred vertically, right-aligned),
	// its count across all its pages beside it, then the whole grail underneath.
	const char* title = PageTitles[page];
	int sectionFilled = 0, sectionSlots = 0;
	for (int i = 0; i < static_cast<int>(sizeof Slots / sizeof Slots[0]); ++i) {
		if (std::strcmp(PageTitles[Slots[i].page], title) == 0) {
			++sectionSlots;
			sectionFilled += view.filled[i] ? 1 : 0;
		}
	}
	const char* name = std::strstr(title, ": ") ? std::strstr(title, ": ") + 2 : title;
	len = std::snprintf(line, sizeof line, "%d/%d", sectionFilled, sectionSlots);
	const float left = gx + 700 * cell / 98 + size, right = gx + GridWidth * cell;
	// Counts sit right-aligned in a column wide enough for "000/000"; the section name
	// and the word "Grail" both end at its left edge, one space short.
	const float countColumn = Measure(canvas, "000/000", 7, size), labelRight = right - countColumn - Measure(canvas, " ", 1, size);
	const float countWidth = Measure(canvas, line, len, size), room = labelRight - left;
	const float step = size * 1.15f, top = gridBottom + 5, block = size + step;

	// Split the name at the comma that best fills the first line, if it needs splitting.
	const int nameLength = static_cast<int>(std::strlen(name));
	int split = nameLength;
	if (Measure(canvas, name, nameLength, size) > room) {
		for (const char* c = std::strchr(name, ','); c != nullptr; c = std::strchr(c + 1, ',')) {
			if (split == nameLength || Measure(canvas, name, static_cast<int>(c - name), size) <= room) {
				split = static_cast<int>(c - name);
			}
		}
	}
	const float nameTop = top + (split < nameLength ? 0 : (block - size) / 2);
	Text(canvas, labelRight - Measure(canvas, name, split, size), nameTop, name, split, size, HeaderGold);
	if (split < nameLength) {
		const char* rest = name + split + 1;
		while (*rest == ' ') {
			++rest;
		}
		const int restLength = static_cast<int>(std::strlen(rest));
		Text(canvas, labelRight - Measure(canvas, rest, restLength, size), nameTop + step, rest, restLength, size, HeaderGold);
	}
	Text(canvas, right - countWidth, top + (block - size) / 2, line, len, size, HeaderGold);
	const float grailTop = top + block + size * 0.5f;
	Text(canvas, labelRight - Measure(canvas, "Grail", 5, size), grailTop, "Grail", 5, size, HeaderGold);
	len = std::snprintf(line, sizeof line, "%d/%d", view.filledCount, static_cast<int>(sizeof Slots / sizeof Slots[0]));
	Text(canvas, right - Measure(canvas, line, len, size), grailTop, line, len, size, HeaderGold);
}

static void __cdecl OnFrame(const D2RL::PluginContext*, const D2RL::Overlay::Frame* frame, void*) noexcept {
	if (frame == nullptr || frame->screenHeight <= 0) {
		return;
	}
	void* list = g_clipFix ? PushScreenClip(frame->screenWidth, frame->screenHeight) : nullptr;
	DrawFrame(frame);
	if (list != nullptr) {
		PopScreenClip(list);
	}
}

// ---------------------------------------------------------------- console and load

static char g_report[2048];

static void Note(const char* text) noexcept {
	const size_t used = std::strlen(g_report);
	std::snprintf(g_report + used, sizeof g_report - used, "%s\n", text);
	g_ctx->LogInfo(text);
}

static auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	command->plugin->WriteConsoleMessage(g_report);
	static constexpr const char* States[] = { "not set up", "needs a relaunch", "active", "tabs missing (add_shared_tabs too low)" };
	static constexpr const char* Tops[]   = { "unknown", "shared", "other" };
	char line[320];
	std::snprintf(line, sizeof line,
		"Setup: %s. Shared tabs: %d (add_shared_tabs %d). Grail block: tabs %d-%d (%d pages).\n"
		"Stash %s, top tab %s, shared tab %d. Filed %d/%zu, misplaced %d.",
		States[static_cast<int>(SetupState())], TotalTabs(), g_liveAddedTabs, g_cfg.firstTab, g_cfg.firstTab + BlockPages() - 1, BlockPages(),
		g_view.stashOpen ? "open" : "closed", Tops[static_cast<int>(g_view.top)], g_view.tab + 1, g_view.filledCount,
		sizeof Slots / sizeof Slots[0], g_view.misplacedCount);
	command->plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

static auto __cdecl DebugCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	g_debug = !g_debug;
	command->plugin->WriteConsoleMessage(g_debug ? "grail-tab: debug log on." : "grail-tab: debug log off.");
	return D2RL::ConsoleCommandResult::Handled;
}

template <typename Service>
static bool Query(const char* name, const Service** service, uint32_t needed) noexcept {
	const bool found = g_ctx->QueryService(service) == D2RL::ServiceQueryResult::Success && *service != nullptr;
	char line[128];
	std::snprintf(line, sizeof line, "%s: %s, size %u (need %u)", name, found ? "found" : "MISSING", found ? (*service)->serviceSize : 0, needed);
	Note(line);
	return found && (*service)->serviceSize >= needed;
}

static bool Check(const char* what, unsigned result) noexcept {
	if (result != 0) {
		char line[128];
		std::snprintf(line, sizeof line, "FAILED: %s (result %u)", what, result);
		Note(line);
	}
	return result == 0;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &Info;
}

// Always loads, so grail-status can say what's missing on this loader.
D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	g_ctx = context;

	if (!context->RegisterConsoleCommand("grail-status", StatusCommand, "Show Grail Tab's setup and state.")
		|| !context->RegisterConsoleCommand("grail-debug", DebugCommand, "Toggle Grail Tab's debug log.")) {
		Note("FAILED: console commands");
	}

	const D2RL::ItemInteractionService* interactions = nullptr;
	const D2RL::SharedEventService*     events       = nullptr;
	const D2RL::InputService*           input        = nullptr;
	bool ok = true;
	ok &= Query("widgets", &g_widgets, D2RL::WidgetServiceRequiredSize);
	ok &= Query("overlay", &g_overlay, D2RL::OverlayServiceRequiredSize);
	ok &= Query("threads", &g_threads, D2RL::ThreadServiceRequiredSize);
	ok &= Query("items", &g_items, D2RL::ItemServiceRequiredSize);
	ok &= Query("inventory", &g_inventory, D2RL::InventoryServiceRequiredSize);
	ok &= Query("item clicks", &interactions, D2RL::ItemInteractionServiceRequiredSize);
	ok &= Query("ui events", &events, D2RL::SharedEventServiceRequiredSize);
	ok &= Query("input", &input, D2RL::InputServiceRequiredSize);
	if (!D2RL::HasItemServiceCapability(g_items, D2RL::Items::ItemServiceCapability::SharedStashWrite)) {
		Note("Shared stash writes aren't available: Ctrl+click filing is off.");
		g_cfg.ctrlClick = false;
	}
	if (!ok) {
		Note("Grail Tab is disabled.");
		return true;
	}

	wchar_t exe[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (wchar_t* slash = n ? std::wcsrchr(exe, L'\\') : nullptr) {
		*slash = 0;
		std::swprintf(g_loaderToml, MAX_PATH, L"%ls\\d2rloader\\config\\d2rloader.toml", exe);
	}
	g_liveAddedTabs = ReadAddedTabs();
	LoadSettings();
	char line[160];
	std::snprintf(line, sizeof line, "add_shared_tabs %d at launch; first_tab %d", g_liveAddedTabs, g_cfg.firstTab);
	Note(line);

	const D2RL::ItemInteractions::ItemInteractionListener clicks {
		.structSize    = D2RL::ItemInteractions::ItemInteractionListenerSize,
		.callback      = OnItemClick,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)
		               | D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::PersonalStash)
		               | (interactions->supportedContainerMask & D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::SharedStash)),
	};
	D2RL::ItemInteractions::ListenerHandle clickHandle = D2RL::ItemInteractions::InvalidHandle;
	Check("click listener", static_cast<unsigned>(interactions->registerListener(context, &clicks, &clickHandle)));

	const D2RL::SharedEvents::UiMessageListener messages { .structSize = D2RL::SharedEvents::UiMessageListenerSize, .callback = OnUiMessage };
	D2RL::SharedEvents::ListenerHandle messageHandle = D2RL::SharedEvents::InvalidHandle;
	Check("ui listener", static_cast<unsigned>(events->registerUiMessageListener(context, &messages, &messageHandle)));

	const D2RL::SharedEvents::ItemTooltipListener tooltips {
		.structSize = D2RL::SharedEvents::ItemTooltipListenerSize,
		.region     = D2RL::SharedEvents::ItemTooltipRegion::ActionFooter,
		.position   = D2RL::SharedEvents::ItemTooltipPosition::Bottom,
		.callback   = OnTooltip,
	};
	D2RL::SharedEvents::ListenerHandle tooltipHandle = D2RL::SharedEvents::InvalidHandle;
	Check("tooltip listener", static_cast<unsigned>(events->registerItemTooltipListener(context, &tooltips, &tooltipHandle)));

	const D2RL::LifecycleService* lifecycle = nullptr;
	if (g_ctx->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
		const D2RL::Lifecycle::GameplayEventListener left {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = D2RL::Lifecycle::GameplayEventKind::GameLeft,
			.callback   = OnGameplay,
		};
		D2RL::Lifecycle::ListenerHandle leftHandle = D2RL::Lifecycle::InvalidHandle;
		Check("game-left listener", static_cast<unsigned>(lifecycle->registerGameplayEventListener(context, &left, &leftHandle)));
	}

	const D2RL::Overlay::CallbackRegistration frame {
		.structSize = D2RL::Overlay::CallbackRegistrationSize, .phase = D2RL::Overlay::Phase::AfterGameUi, .callback = OnFrame };
	D2RL::Overlay::CallbackHandle frameHandle = D2RL::Overlay::InvalidCallbackHandle;
	Check("overlay", static_cast<unsigned>(g_overlay->registerFrameCallback(context, &frame, &frameHandle)));

	const D2RL::Input::ActionRegistration action {
		.structSize       = D2RL::Input::ActionRegistrationSize,
		.logicalId        = "grail",
		.displayName      = "Grail Tab: jump to the grail and back (same as the chalice)",
		.category         = "Grail Tab",
		.defaultPrimary   = { .key = D2RL::Input::Key::None },   // the chalice is the main way in; bind a key if you like
		.defaultSecondary = { .key = D2RL::Input::Key::None },
		.callback         = OnKey,
	};
	D2RL::Input::ActionHandle actionHandle = D2RL::Input::InvalidHandle;
	Check("hotkey", static_cast<unsigned>(input->registerAction(context, &action, &actionHandle)));

	g_clipFix = context->CheckExpectedBytes(DrawListRva, DrawListGuard, sizeof DrawListGuard)
		&& context->CheckExpectedBytes(PushClipRva, PushClipGuard, sizeof PushClipGuard)
		&& context->CheckExpectedBytes(PopClipRva, PopClipGuard, sizeof PopClipGuard);
	Note(g_clipFix ? "Overlay clip fix: on (the game's code matched)." : "Overlay clip fix: off (the game's code didn't match); drawing may be invisible.");

	Note("Grail Tab loaded.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
