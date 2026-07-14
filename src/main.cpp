#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

#include "network/resource_cache.h"
#include "network/websocket.h"
#include "network/url.h"
#include "html/parser.h"
#include "html/resources.h"
#include "network/text_decode.h"
#include "layout/scroll.h"
#include "render/renderer.h"
#include "render/canvas_renderer.h"
#include "platform/form_state.h"
#include "platform/resource.h"
#include "platform/updater.h"
#include "platform/chrome.h"
#include "platform/chrome_theme.h"
#include "platform/box_painter.h"
#include "platform/downloads.h"
#include "platform/profile.h"
#include "js/engine.h"
#include "js/dom_bridge.h"

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <deque>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <ctime>
#include <cmath>

static Semaphore g_imageFetchGate(6);

// ─── control IDs ─────────────────────────────────────────────────────────────
enum : int { IDC_BACK = 101, IDC_FWRD, IDC_REFR, IDC_STOP, IDC_HOME, IDC_URL, IDC_FIND };

// ─── custom messages ──────────────────────────────────────────────────────────
constexpr UINT WM_PAGE_READY  = WM_USER + 1;
constexpr UINT WM_IMAGE_READY = WM_USER + 2;
constexpr UINT WM_NEWTAB_NAVIGATE = WM_USER + 3;
constexpr UINT WM_GAMEPAD_POLL = WM_USER + 4;
constexpr UINT WM_PIP_CLOSED = WM_USER + 5;
constexpr UINT_PTR TIMER_MAIN = 1;
constexpr UINT_PTR TIMER_FULLSCREEN_CURSOR = 2;
constexpr UINT_PTR TIMER_GAMEPAD = 3;
constexpr UINT_PTR TIMER_MEDIA_SESSION = 4;

// ─── globals ─────────────────────────────────────────────────────────────────
static HWND     g_hwnd;
static HWND     g_hwndBack, g_hwndFwrd, g_hwndRefr, g_hwndStop, g_hwndHome, g_hwndUrl;
static HWND     g_hwndUrlBadge;
static HWND     g_hwndStatus;
static HWND     g_hwndFind;
static bool     g_findVisible = false;
static Renderer g_renderer;
const Node* g_hoverNode = nullptr;
std::map<const Node*, float> g_elementScrollY;
static std::vector<ScrollableRegion> g_scrollables;
static bool g_windowFullscreen = false;
static LONG_PTR g_fullscreenStyle = 0;
static LONG_PTR g_fullscreenExStyle = 0;
static WINDOWPLACEMENT g_fullscreenPlacement{ sizeof(g_fullscreenPlacement) };
static int g_fullscreenTab = -1;
static Node* g_fullscreenElement = nullptr;
static HWND g_fullscreenRestoreFocus = nullptr;
static bool g_fullscreenMouseHidden = false;
static bool g_pointerLocked = false;
static Node* g_pointerLockElement = nullptr;
static POINT g_pointerLockCenter{};
static bool g_pointerLockWarping = false;
static HWND g_pointerLockRestoreFocus = nullptr;
static bool g_pipActive = false;
static HWND g_pipHwnd = nullptr;
static Node* g_pipSourceElement = nullptr;
static RECT g_pipRect{ 80, 80, 480, 350 };
static bool g_modalDialogActive = false;
static Node* g_modalDialog = nullptr;
static std::vector<Node*> g_topLayerNodes;
static Node* g_activePopover = nullptr;
static bool g_dragActive = false;
static std::vector<std::string> g_dragFiles;
static std::string g_lastClipboardHtml;
static std::vector<uint8_t> g_lastClipboardImage;
static bool g_gamepadConnected[4] = {};
static DWORD g_gamepadPacket[4] = {};
static HMODULE g_xinputModule = nullptr;
static std::string g_mediaSessionAction;
static bool g_notificationPermissionGranted = true;
static UINT g_nextNotificationId = 1;
static std::map<UINT, std::string> g_notifications;

struct PlatformEventRecord {
    std::string type;
    std::string detail;
    long long tick = 0;
};

struct GamepadSnapshot {
    bool connected = false;
    DWORD packet = 0;
    WORD buttons = 0;
    BYTE leftTrigger = 0;
    BYTE rightTrigger = 0;
    SHORT leftX = 0;
    SHORT leftY = 0;
    SHORT rightX = 0;
    SHORT rightY = 0;
};

struct NotificationRecord {
    UINT id = 0;
    std::string title;
    std::string body;
    bool shown = false;
    bool clicked = false;
    bool closed = false;
};

struct DragPayload {
    std::vector<std::string> files;
    POINT screenPoint{};
    POINT clientPoint{};
    bool hasFiles = false;
};

struct PermissionStateRecord {
    std::string name;
    std::string state;
    long long updated = 0;
};

struct SharePayload {
    std::string title;
    std::string text;
    std::string url;
    std::vector<std::string> files;
};

struct ScreenDetailsRecord {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int workWidth = 0;
    int workHeight = 0;
    int dpi = 96;
    bool primary = true;
};

struct GeoPositionRecord {
    double latitude = 0.0;
    double longitude = 0.0;
    double accuracy = 0.0;
    long long timestamp = 0;
};

struct BatteryRecord {
    double level = 1.0;
    bool charging = true;
    int chargingTime = 0;
    int dischargingTime = 0;
};

struct SensorRecord {
    std::string type;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    long long timestamp = 0;
    bool active = false;
};

struct ExternalDeviceRecord {
    std::string kind;
    std::string name;
    std::string id;
    bool open = false;
};

struct ContactRecord {
    std::string name;
    std::string email;
    std::string tel;
};

static std::deque<PlatformEventRecord> g_platformEvents;
static GamepadSnapshot g_gamepads[4];
static std::map<UINT, NotificationRecord> g_notificationRecords;
static DragPayload g_dragPayload;
static std::string g_lastPickedFileList;
static std::string g_pointerLockLastDelta;
static std::map<std::string, PermissionStateRecord> g_permissions;
static SharePayload g_lastSharePayload;
static std::string g_lastSavedFile;
static int g_appBadge = 0;
static bool g_wakeLockActive = false;
static bool g_screenWakeLockActive = false;
static bool g_launchQueuePending = false;
static std::vector<std::string> g_launchQueueFiles;
static std::vector<ScreenDetailsRecord> g_screenDetails;
static std::map<std::string, std::string> g_protocolHandlers;
static std::vector<std::string> g_fileHandlers;
static GeoPositionRecord g_geoPosition{ 37.7749, -122.4194, 65.0, 0 };
static BatteryRecord g_battery;
static bool g_idleDetectorActive = false;
static bool g_userIdle = false;
static bool g_screenLocked = false;
static DWORD g_lastInputTick = 0;
static std::map<std::string, SensorRecord> g_sensors;
static std::vector<ExternalDeviceRecord> g_externalDevices;
static std::vector<ContactRecord> g_contacts;
static unsigned long long g_storageUsageBytes = 0;
static unsigned long long g_storageQuotaBytes = 512ull * 1024ull * 1024ull;
static std::vector<int> g_vibrationPattern;

// BrowserChrome owns tabs, form state, JS engine, updater.
static BrowserChrome g_chrome;
static auto& g_tabs      = g_chrome.state.tabs;
static auto& g_activeTab = g_chrome.state.activeTab;
static auto& g_formState = g_chrome.state.form;
static auto& g_updater   = g_chrome.state.updater;
static auto& g_js        = g_chrome.state.js;

// PendingPageScript and the kMax*PerTimerTick budget constants now live in
// platform/chrome.h (shared with Linux/macOS's BrowserChrome::pumpJs()) —
// Windows still runs its own independent pipeline below (doesn't call
// BrowserChrome::onPageReady/pumpJs), just reusing the same type/constants
// rather than redeclaring them.
static std::deque<PendingPageScript> g_pendingPageScripts;

static HCURSOR  g_cursorArrow, g_cursorHand, g_cursorIBeam, g_cursorSizeAll,
                g_cursorNo, g_cursorCross, g_cursorHelp;
static HFONT    g_uiFont = nullptr;
static HFONT    g_iconFont = nullptr; // Segoe MDL2 Assets, for the nav toolbar glyphs
static HFONT    g_urlFont = nullptr;
static HBRUSH   g_toolbarBrush = nullptr;
static HBRUSH   g_statusBrush = nullptr;
static HBRUSH   g_editBrush = nullptr;
static HBRUSH   g_windowBrush = nullptr;
static HICON    g_appIconLarge = nullptr;
static HICON    g_appIconSmall = nullptr;
static int      g_appIconResourceId = 0;
static vertex::profile::ProfilePaths g_profilePaths;
static std::vector<vertex::downloads::DownloadRecord> g_downloads;

static constexpr COLORREF ToColorRef(vertex::chrome_theme::Rgb c) {
    return RGB(c.r, c.g, c.b);
}

static constexpr COLORREF kChromeInk     = ToColorRef(vertex::chrome_theme::Ink);
static constexpr COLORREF kChromePanel   = ToColorRef(vertex::chrome_theme::Panel);
static constexpr COLORREF kChromeRail    = ToColorRef(vertex::chrome_theme::Rail);
static constexpr COLORREF kChromeActive  = ToColorRef(vertex::chrome_theme::Active);
static constexpr COLORREF kChromeHover   = ToColorRef(vertex::chrome_theme::Hover);
static constexpr COLORREF kChromePressed = ToColorRef(vertex::chrome_theme::Pressed);
static constexpr COLORREF kChromeDisabled = ToColorRef(vertex::chrome_theme::Disabled);
static constexpr COLORREF kChromeAccent  = ToColorRef(vertex::chrome_theme::Accent);
static constexpr COLORREF kChromeAccentSoft = ToColorRef(vertex::chrome_theme::AccentSoft);
static constexpr COLORREF kChromeQuiet   = ToColorRef(vertex::chrome_theme::Quiet);
static constexpr COLORREF kChromeDisabledText = ToColorRef(vertex::chrome_theme::DisabledText);
static constexpr COLORREF kChromeLine    = ToColorRef(vertex::chrome_theme::Line);

// ─── layout constants ─────────────────────────────────────────────────────────
// Layout constants from ChromeLayout (shared with chrome.h).
static bool IsSystemDarkMode() {
    DWORD appsUseLightTheme = 1;
    DWORD size = sizeof(appsUseLightTheme);
    LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &appsUseLightTheme,
        &size);
    return status == ERROR_SUCCESS && appsUseLightTheme == 0;
}

static int ThemedIconResourceId() {
    return IsSystemDarkMode() ? IDI_VERTEX_APP_LIGHT : IDI_VERTEX_APP;
}

static void LoadThemedAppIcons(HINSTANCE instance) {
    int iconId = ThemedIconResourceId();
    if (g_appIconResourceId == iconId && g_appIconLarge && g_appIconSmall)
        return;
    if (g_appIconLarge) {
        DestroyIcon(g_appIconLarge);
        g_appIconLarge = nullptr;
    }
    if (g_appIconSmall) {
        DestroyIcon(g_appIconSmall);
        g_appIconSmall = nullptr;
    }
    g_appIconLarge = (HICON)LoadImageW(
        instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_appIconSmall = (HICON)LoadImageW(
        instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    g_appIconResourceId = iconId;
}

static void ApplyThemedWindowIcon(HWND hwnd) {
    HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    LoadThemedAppIcons(instance);
    if (g_appIconLarge)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_appIconLarge);
    if (g_appIconSmall)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_appIconSmall);
    if (g_appIconSmall)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL2, (LPARAM)g_appIconSmall);
}

constexpr int TAB_H     = vertex::chrome_theme::TabHeight;
constexpr int TOOLBAR_H = vertex::chrome_theme::ToolbarHeight;
constexpr int STATUS_H  = vertex::chrome_theme::StatusHeight;
constexpr int FIND_H    = 34;   // find bar height
constexpr int TOP_INSET = TAB_H + TOOLBAR_H;  // total above content
constexpr int BTN_W     = vertex::chrome_theme::ButtonWidth;
constexpr int BTN_H     = vertex::chrome_theme::ButtonHeight;
constexpr int MARGIN    = vertex::chrome_theme::Margin;
constexpr int GAP       = vertex::chrome_theme::Gap;
constexpr int CORNER_R  = vertex::chrome_theme::CornerRadius;
constexpr int URL_BADGE_W = 28;
static HWND g_hoverChromeButton = nullptr;

// ─── active tab helpers ───────────────────────────────────────────────────────
static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ─── string helpers ───────────────────────────────────────────────────────────
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring UrlBadgeText(const std::string& url) {
    if (url.rfind("vertex://", 0) == 0 || url.rfind("felix://", 0) == 0) return L"H";
    if (url.rfind("https://", 0) == 0) return L"S";
    if (url.rfind("http://", 0) == 0) return L"i";
    return L"?";
}

static void SetUrlBadge(const std::string& url) {
    if (g_hwndUrlBadge)
        SetWindowTextW(g_hwndUrlBadge, UrlBadgeText(url).c_str());
}

static void SetUrlBar(const std::string& url) {
    SetWindowTextW(g_hwndUrl, ToWide(url).c_str());
    SetUrlBadge(url);
}
static void SetUrlBarForTab(const Tab& tab) {
    SetUrlBar(tab.displayUrl.empty() ? tab.url : tab.displayUrl);
}
static void SetStatus(const std::string& s) {
    // Show updater status when not hovering a link.
    std::string effective = (s.empty() && !g_updater.statusMessage.empty())
        ? g_updater.statusMessage
        : s;
    static std::string lastStatus;
    if (effective == lastStatus) return;
    lastStatus = effective;
    SetWindowTextW(g_hwndStatus, ToWide(effective).c_str());
}

static std::string HtmlEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string NowSecondsString() {
    return std::to_string((long long)std::time(nullptr));
}

static std::string AppPageCss() {
    return "body{font:16px system-ui,sans-serif;background:#f7f2e8;color:#151515;margin:0;padding:36px;}"
           "main{max-width:900px;margin:auto;}h1{font-size:32px;margin:0 0 20px;}"
           ".item{background:white;border:1px solid #ded6c8;margin:10px 0;padding:14px 16px;}"
           ".name{font-weight:700;color:#245bd8}.meta{color:#555;margin-top:6px}.bad{color:#a12626}"
           "code{background:#fff;padding:2px 5px;border:1px solid #ded6c8}";
}

static std::string JoinStrings(const std::vector<std::string>& values, const std::string& sep);
static std::string NodeLabel(Node* node);

static void AppendHistoryRecord(const std::string& url, const std::string& title = "") {
    if (url.empty() || url.rfind("vertex://", 0) == 0 || url.rfind("felix://", 0) == 0)
        return;
    vertex::profile::AppendTsvRow(g_profilePaths.historyFile,
        { NowSecondsString(), url, title });
}

static void AppendBookmarkRecord(const std::string& url, const std::string& title) {
    if (url.empty()) return;
    vertex::profile::AppendTsvRow(g_profilePaths.bookmarksFile,
        { NowSecondsString(), url, title.empty() ? url : title });
}

static void AppendDownloadRecord(const vertex::downloads::DownloadRecord& record) {
    vertex::profile::AppendTsvRow(g_profilePaths.downloadsFile,
        { NowSecondsString(), record.url, record.path, record.filename,
          record.contentType, std::to_string(record.bytes),
          record.success ? "1" : "0", record.error });
}

static void LoadDownloadRecords() {
    g_downloads.clear();
    auto rows = vertex::profile::ReadTsvRows(g_profilePaths.downloadsFile);
    for (const auto& row : rows) {
        if (row.size() < 8) continue;
        vertex::downloads::DownloadRecord record;
        record.url = row[1];
        record.path = row[2];
        record.filename = row[3];
        record.contentType = row[4];
        try { record.bytes = (size_t)std::stoull(row[5]); } catch (...) { record.bytes = 0; }
        record.success = row[6] == "1";
        record.error = row[7];
        g_downloads.push_back(record);
    }
}

static std::string HistoryPageHtml() {
    std::string html = "<html><head><title>History</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>History</h1>";
    auto rows = vertex::profile::ReadTsvRows(g_profilePaths.historyFile);
    bool any = false;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        if (it->size() < 2) continue;
        std::string url = (*it)[1];
        std::string title = it->size() > 2 && !(*it)[2].empty() ? (*it)[2] : url;
        html += "<div class=\"item\"><a class=\"name\" href=\"" + HtmlEscape(url) + "\">"
            + HtmlEscape(title) + "</a><div class=\"meta\">" + HtmlEscape(url) + "</div></div>";
        any = true;
    }
    if (!any) html += "<p>No history yet.</p>";
    html += "</main></body></html>";
    return html;
}

static std::string BookmarksPageHtml() {
    std::string html = "<html><head><title>Bookmarks</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Bookmarks</h1>";
    auto rows = vertex::profile::ReadTsvRows(g_profilePaths.bookmarksFile);
    bool any = false;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        if (it->size() < 2) continue;
        std::string url = (*it)[1];
        std::string title = it->size() > 2 && !(*it)[2].empty() ? (*it)[2] : url;
        html += "<div class=\"item\"><a class=\"name\" href=\"" + HtmlEscape(url) + "\">"
            + HtmlEscape(title) + "</a><div class=\"meta\">" + HtmlEscape(url) + "</div></div>";
        any = true;
    }
    if (!any) html += "<p>No bookmarks yet.</p>";
    html += "</main></body></html>";
    return html;
}

static std::string SettingsPageHtml() {
    std::string html = "<html><head><title>Settings</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Settings</h1>"
        "<div class=\"item\"><div class=\"name\">Profile</div>"
        "<div class=\"meta\"><code>" + HtmlEscape(g_profilePaths.profileRoot) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">Cache</div>"
        "<div class=\"meta\"><code>" + HtmlEscape(g_profilePaths.cacheProfileRoot) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">Controls</div>"
        "<div class=\"meta\"><a href=\"vertex://history\">History</a> · "
        "<a href=\"vertex://bookmarks\">Bookmarks</a> · "
        "<a href=\"vertex://downloads\">Downloads</a> · "
        "<a href=\"vertex://site-data\">Site data</a> · "
        "<a href=\"vertex://platform-features\">Platform features</a></div></div>"
        "<div class=\"item\"><div class=\"name\">Current defaults</div>"
        "<div class=\"meta\">JavaScript on · Images on · Cache on · Search engine Bing</div></div>"
        "</main></body></html>";
    return html;
}

static std::string SiteDataPageHtml() {
    auto history = vertex::profile::ReadTsvRows(g_profilePaths.historyFile);
    auto bookmarks = vertex::profile::ReadTsvRows(g_profilePaths.bookmarksFile);
    auto downloads = vertex::profile::ReadTsvRows(g_profilePaths.downloadsFile);
    std::string html = "<html><head><title>Site Data</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Site Data</h1>"
        "<div class=\"item\"><div class=\"name\">Storage root</div><div class=\"meta\"><code>"
        + HtmlEscape(g_profilePaths.profileRoot) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">History entries</div><div class=\"meta\">"
        + std::to_string(history.size()) + "</div></div>"
        "<div class=\"item\"><div class=\"name\">Bookmarks</div><div class=\"meta\">"
        + std::to_string(bookmarks.size()) + "</div></div>"
        "<div class=\"item\"><div class=\"name\">Downloads</div><div class=\"meta\">"
        + std::to_string(downloads.size()) + "</div></div>"
        "<div class=\"item\"><div class=\"name\">Local storage</div><div class=\"meta\"><code>"
        + HtmlEscape(g_profilePaths.localStorageDir) + "</code></div></div>"
        "<div class=\"item\"><div class=\"name\">Cookies</div><div class=\"meta\"><code>"
        + HtmlEscape(g_profilePaths.cookiesFile) + "</code></div></div>"
        "</main></body></html>";
    return html;
}

static std::string PlatformFeaturesPageHtml() {
    std::string html = "<html><head><title>Platform Features</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Platform Features</h1>";
    auto item = [&](const std::string& name, const std::string& value) {
        html += "<div class=\"item\"><div class=\"name\">" + HtmlEscape(name)
            + "</div><div class=\"meta\">" + HtmlEscape(value) + "</div></div>";
    };
    item("Fullscreen", g_windowFullscreen ? "active" : "inactive");
    item("Pointer lock", g_pointerLocked ? g_pointerLockLastDelta : "inactive");
    item("Picture in Picture", g_pipActive ? NodeLabel(g_pipSourceElement) : "inactive");
    item("Modal dialog", g_modalDialogActive ? NodeLabel(g_modalDialog) : "inactive");
    item("Popover", g_activePopover ? NodeLabel(g_activePopover) : "inactive");
    item("Drag files", g_dragPayload.hasFiles ? JoinStrings(g_dragPayload.files, " | ") : "none");
    item("Clipboard HTML", g_lastClipboardHtml.empty() ? "empty" : std::to_string(g_lastClipboardHtml.size()) + " bytes");
    item("Clipboard image", g_lastClipboardImage.empty() ? "empty" : std::to_string(g_lastClipboardImage.size()) + " bytes");
    item("File picker", g_lastPickedFileList.empty() ? "none" : g_lastPickedFileList);
    item("Media session", g_mediaSessionAction.empty() ? "none" : g_mediaSessionAction);
    item("Notifications", std::to_string(g_notificationRecords.size()));
    item("Permissions", std::to_string(g_permissions.size()));
    item("Share", g_lastSharePayload.title.empty() ? "none" : g_lastSharePayload.title);
    item("Saved file", g_lastSavedFile.empty() ? "none" : g_lastSavedFile);
    item("App badge", std::to_string(g_appBadge));
    item("Wake lock", g_wakeLockActive ? (g_screenWakeLockActive ? "screen" : "system") : "none");
    item("Launch queue", g_launchQueuePending ? JoinStrings(g_launchQueueFiles, " | ") : "empty");
    item("Screens", std::to_string(g_screenDetails.size()));
    item("Protocol handlers", std::to_string(g_protocolHandlers.size()));
    item("File handlers", JoinStrings(g_fileHandlers, " | "));
    item("Geolocation", std::to_string(g_geoPosition.latitude) + ", " + std::to_string(g_geoPosition.longitude));
    item("Battery", std::to_string((int)(g_battery.level * 100)) + "% " + (g_battery.charging ? "charging" : "discharging"));
    item("Idle detector", g_idleDetectorActive ? (g_userIdle ? "idle" : "active") : "inactive");
    item("Screen lock", g_screenLocked ? "locked" : "unlocked");
    item("Storage", std::to_string(g_storageUsageBytes) + " / " + std::to_string(g_storageQuotaBytes));
    item("Contacts", std::to_string(g_contacts.size()));
    item("External devices", std::to_string(g_externalDevices.size()));
    item("Vibration", std::to_string(g_vibrationPattern.size()) + " segment(s)");
    for (const auto& [type, sensor] : g_sensors)
        item("Sensor " + type, std::to_string(sensor.x) + ", " + std::to_string(sensor.y) + ", " + std::to_string(sensor.z));
    for (const auto& dev : g_externalDevices)
        item("Device " + dev.kind, dev.name + " " + (dev.open ? "open" : "closed"));
    for (const auto& [name, perm] : g_permissions)
        item("Permission " + name, perm.state);
    for (const auto& [scheme, url] : g_protocolHandlers)
        item("Protocol " + scheme, url);
    for (int i = 0; i < 4; ++i) {
        item("Gamepad " + std::to_string(i), g_gamepads[i].connected
            ? ("buttons=" + std::to_string(g_gamepads[i].buttons) + " packet=" + std::to_string(g_gamepads[i].packet))
            : "disconnected");
    }
    size_t start = g_platformEvents.size() > 20 ? g_platformEvents.size() - 20 : 0;
    for (size_t i = start; i < g_platformEvents.size(); ++i) {
        item("Event " + std::to_string(i), g_platformEvents[i].type + " " + g_platformEvents[i].detail);
    }
    html += "</main></body></html>";
    return html;
}

static std::string DownloadsPageHtml() {
    std::string html =
        "<html><head><title>Downloads</title><style>" + AppPageCss()
        + "</style></head><body><main><h1>Downloads</h1>";
    if (g_downloads.empty()) {
        html += "<p>No downloads yet.</p>";
    } else {
        for (auto it = g_downloads.rbegin(); it != g_downloads.rend(); ++it) {
            html += "<div class=\"item\"><div class=\"name\">" + HtmlEscape(it->filename) + "</div>";
            html += "<div class=\"meta\">" + HtmlEscape(it->path) + "</div>";
            if (it->success) {
                html += "<div class=\"meta\">" + std::to_string(it->bytes) + " bytes from "
                    + HtmlEscape(it->url) + "</div>";
            } else {
                html += "<div class=\"bad\">" + HtmlEscape(it->error) + "</div>";
            }
            html += "</div>";
        }
    }
    html += "</main></body></html>";
    return html;
}

static void UpdatePerfStatusMaybe() {
    if (getenv("VERTEX_PERF") == nullptr) return;
    const RendererTimings render = g_renderer.LastTimings();
    const ResourceCacheStats resources = ResourceCache::instance().stats();
    const JsScriptStats js = g_js.scriptStats();
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
        "style %.1fms layout %.1fms%s paint %.1fms | js parse %.1fms run %.1fms %zu/%zu | fetch %.1fms req %llu hit %llu net %llu %.1fMB",
        render.styleMs,
        render.layoutMs,
        render.layoutReused ? " cached" : "",
        render.paintMs,
        js.parseMs,
        js.compileRunMs,
        js.scriptsExecuted,
        js.scriptsAttempted,
        resources.fetchMs,
        static_cast<unsigned long long>(resources.requests),
        static_cast<unsigned long long>(resources.cacheHits),
        static_cast<unsigned long long>(resources.networkFetches),
        resources.bytesFetched / (1024.0 * 1024.0));
    SetStatus(buffer);
}
static void SetBrowserCursor(HCURSOR cursor) {
    static HCURSOR lastCursor = nullptr;
    if (cursor == lastCursor) return;
    lastCursor = cursor;
    SetCursor(cursor);
}

static HCURSOR CursorFromCss(int cursor) {
    switch (cursor) {
    case 1: return g_cursorHand;
    case 2: return g_cursorIBeam;
    case 3:
    case 5:
    case 6: return g_cursorSizeAll;
    case 4: return g_cursorNo;
    case 7: return g_cursorCross;
    case 8: return g_cursorHelp;
    default: return g_cursorArrow;
    }
}
static void UpdateTitle() {
    std::wstring t = ToWide(CurTab().title);
    if (t.empty()) t = L"New Tab";
    SetWindowTextW(g_hwnd, (t + L" \x2014 Vertex").c_str());
}

static void LayoutControls();
static void ClampScroll();
static void UpdateScrollbar();
static RECT ContentPaintRect();
static void InvalidateContent();

static bool AnyTabLoading() {
    for (const auto& tab : g_tabs)
        if (tab.loading) return true;
    return false;
}

static int ChromeTopInset() {
    return g_windowFullscreen ? 0 : TOP_INSET;
}

static int ChromeBottomInset() {
    return g_windowFullscreen ? 0 : STATUS_H + (g_findVisible ? FIND_H : 0);
}

static void UpdateFullscreenChromeVisibility() {
    const int show = g_windowFullscreen ? SW_HIDE : SW_SHOW;
    ShowWindow(g_hwndBack, show);
    ShowWindow(g_hwndFwrd, show);
    ShowWindow(g_hwndRefr, show);
    ShowWindow(g_hwndStop, show);
    ShowWindow(g_hwndHome, show);
    ShowWindow(g_hwndUrlBadge, show);
    ShowWindow(g_hwndUrl, show);
    ShowWindow(g_hwndStatus, show);
    ShowWindow(g_hwndFind, (!g_windowFullscreen && g_findVisible) ? SW_SHOW : SW_HIDE);
}

static void NotifyFullscreenChanged() {
    try {
        g_js.dispatchDocumentEvent("fullscreenchange");
    } catch (...) {
        OutputDebugStringA("[Fullscreen] fullscreenchange dispatch failed\n");
    }
}

static MONITORINFO MonitorInfoForWindow(HWND hwnd) {
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    return mi;
}

static void ShowFullscreenCursor(bool show) {
    if (show == !g_fullscreenMouseHidden) return;
    ShowCursor(show ? TRUE : FALSE);
    g_fullscreenMouseHidden = !show;
}

static void ArmFullscreenCursorTimer() {
    if (!g_windowFullscreen) return;
    ShowFullscreenCursor(true);
    SetTimer(g_hwnd, TIMER_FULLSCREEN_CURSOR, 1800, NULL);
}

static void FitFullscreenToCurrentMonitor() {
    if (!g_hwnd || !g_windowFullscreen) return;
    MONITORINFO mi = MonitorInfoForWindow(g_hwnd);
    SetWindowPos(g_hwnd, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    g_renderer.InvalidateLayout();
    ClampScroll();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ExitWindowFullscreen(bool dispatchEvent = true) {
    if (!g_hwnd || !g_windowFullscreen) return;

    g_windowFullscreen = false;
    g_fullscreenTab = -1;
    g_fullscreenElement = nullptr;
    KillTimer(g_hwnd, TIMER_FULLSCREEN_CURSOR);
    ShowFullscreenCursor(true);

    SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_fullscreenStyle);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, g_fullscreenExStyle);
    SetWindowPlacement(g_hwnd, &g_fullscreenPlacement);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

    UpdateFullscreenChromeVisibility();
    LayoutControls();
    g_renderer.InvalidateLayout();
    ClampScroll();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_fullscreenRestoreFocus && IsWindow(g_fullscreenRestoreFocus))
        SetFocus(g_fullscreenRestoreFocus);
    g_fullscreenRestoreFocus = nullptr;
    if (dispatchEvent) NotifyFullscreenChanged();
}

static void EnterWindowFullscreen(Node* element) {
    if (!g_hwnd) return;
    if (g_windowFullscreen) {
        g_fullscreenElement = element;
        g_fullscreenTab = g_activeTab;
        FitFullscreenToCurrentMonitor();
        NotifyFullscreenChanged();
        return;
    }

    g_fullscreenRestoreFocus = GetFocus();
    g_fullscreenStyle = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    g_fullscreenExStyle = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    g_fullscreenPlacement = WINDOWPLACEMENT{ sizeof(g_fullscreenPlacement) };
    GetWindowPlacement(g_hwnd, &g_fullscreenPlacement);

    MONITORINFO mi = MonitorInfoForWindow(g_hwnd);
    g_windowFullscreen = true;
    g_fullscreenTab = g_activeTab;
    g_fullscreenElement = element;

    LONG_PTR style = g_fullscreenStyle;
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_VSCROLL);
    LONG_PTR exStyle = g_fullscreenExStyle;
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, style);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(g_hwnd, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

    UpdateFullscreenChromeVisibility();
    LayoutControls();
    g_renderer.InvalidateLayout();
    ClampScroll();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    SetFocus(g_hwnd);
    ArmFullscreenCursorTimer();
    NotifyFullscreenChanged();
}

static void ToggleWindowFullscreen() {
    if (g_windowFullscreen) ExitWindowFullscreen();
    else EnterWindowFullscreen(nullptr);
}

static void DispatchPlatformEvent(const std::string& name) {
    g_platformEvents.push_back({ name, {}, (long long)GetTickCount64() });
    while (g_platformEvents.size() > 256) g_platformEvents.pop_front();
    try {
        g_js.dispatchWindowEvent(name);
    } catch (...) {
        OutputDebugStringA(("[Platform] event failed: " + name + "\n").c_str());
    }
}

static void DispatchPlatformEventDetail(const std::string& name, const std::string& detail) {
    g_platformEvents.push_back({ name, detail, (long long)GetTickCount64() });
    while (g_platformEvents.size() > 256) g_platformEvents.pop_front();
    SetStatus(name + (detail.empty() ? "" : (": " + detail)));
    try {
        g_js.dispatchWindowEvent(name);
    } catch (...) {
        OutputDebugStringA(("[Platform] event failed: " + name + "\n").c_str());
    }
}

static std::string JoinStrings(const std::vector<std::string>& values, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += sep;
        out += values[i];
    }
    return out;
}

static std::string NodeLabel(Node* node) {
    if (!node) return "null";
    std::string label = node->tagName.empty() ? "node" : node->tagName;
    std::string id = node->attr("id");
    if (!id.empty()) label += "#" + id;
    std::string cls = node->attr("class");
    if (!cls.empty()) label += "." + cls;
    return label;
}

static void PushTopLayer(Node* node) {
    if (!node) return;
    g_topLayerNodes.erase(std::remove(g_topLayerNodes.begin(), g_topLayerNodes.end(), node), g_topLayerNodes.end());
    g_topLayerNodes.push_back(node);
}

static void RemoveTopLayer(Node* node) {
    g_topLayerNodes.erase(std::remove(g_topLayerNodes.begin(), g_topLayerNodes.end(), node), g_topLayerNodes.end());
    if (g_modalDialog == node) {
        g_modalDialog = nullptr;
        g_modalDialogActive = false;
    }
    if (g_activePopover == node)
        g_activePopover = nullptr;
}

static void ShowModalDialog(Node* dialog) {
    if (!dialog) return;
    g_modalDialog = dialog;
    g_modalDialogActive = true;
    dialog->attrs["open"] = "";
    dialog->attrs["data-vertex-modal"] = "true";
    PushTopLayer(dialog);
    g_renderer.InvalidateLayout();
    InvalidateContent();
    DispatchPlatformEventDetail("vertexmodalchange", "open " + NodeLabel(dialog));
}

static void CloseModalDialog(Node* dialog) {
    if (!dialog) dialog = g_modalDialog;
    if (!dialog) return;
    dialog->attrs.erase("open");
    dialog->attrs.erase("data-vertex-modal");
    RemoveTopLayer(dialog);
    g_renderer.InvalidateLayout();
    InvalidateContent();
    DispatchPlatformEventDetail("close", NodeLabel(dialog));
}

static void ShowPlatformPopover(Node* popover) {
    if (!popover) return;
    if (g_activePopover && g_activePopover != popover)
        g_activePopover->attrs.erase("popover-open");
    g_activePopover = popover;
    popover->attrs["popover-open"] = "";
    popover->attrs["data-vertex-top-layer"] = "popover";
    PushTopLayer(popover);
    g_renderer.InvalidateLayout();
    InvalidateContent();
    DispatchPlatformEventDetail("toggle", "popover open " + NodeLabel(popover));
}

static void HidePlatformPopover(Node* popover = nullptr) {
    if (!popover) popover = g_activePopover;
    if (!popover) return;
    popover->attrs.erase("popover-open");
    popover->attrs.erase("data-vertex-top-layer");
    RemoveTopLayer(popover);
    g_renderer.InvalidateLayout();
    InvalidateContent();
    DispatchPlatformEventDetail("toggle", "popover closed " + NodeLabel(popover));
}

static void CenterPointerLockCursor() {
    if (!g_pointerLocked || !g_hwnd) return;
    RECT rc = ContentPaintRect();
    POINT pt{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    ClientToScreen(g_hwnd, &pt);
    g_pointerLockCenter = pt;
    g_pointerLockWarping = true;
    SetCursorPos(pt.x, pt.y);
    g_pointerLockWarping = false;
}

static void EnterPointerLock(Node* target) {
    if (!g_hwnd) return;
    g_pointerLocked = true;
    g_pointerLockElement = target;
    g_pointerLockRestoreFocus = GetFocus();
    SetCapture(g_hwnd);
    ShowCursor(FALSE);
    RECT clip = ContentPaintRect();
    MapWindowPoints(g_hwnd, NULL, reinterpret_cast<POINT*>(&clip), 2);
    ClipCursor(&clip);
    CenterPointerLockCursor();
    DispatchPlatformEventDetail("pointerlockchange", "locked " + NodeLabel(target));
}

static void ExitPointerLock(bool fireEvent = true) {
    if (!g_pointerLocked) return;
    g_pointerLocked = false;
    g_pointerLockElement = nullptr;
    ReleaseCapture();
    ClipCursor(nullptr);
    ShowCursor(TRUE);
    if (g_pointerLockRestoreFocus && IsWindow(g_pointerLockRestoreFocus))
        SetFocus(g_pointerLockRestoreFocus);
    g_pointerLockRestoreFocus = nullptr;
    if (fireEvent) DispatchPlatformEventDetail("pointerlockchange", "unlocked");
}

static LRESULT CALLBACK PipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_pipActive = false;
        g_pipSourceElement = nullptr;
        PostMessageW(g_hwnd, WM_PIP_CLOSED, 0, 0);
        return 0;
    case WM_SIZE:
        GetWindowRect(hwnd, &g_pipRect);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(ps.hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(ps.hdc, RGB(255,255,255));
        SetBkMode(ps.hdc, TRANSPARENT);
        DrawTextW(ps.hdc, L"Picture in Picture", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsurePipWindow() {
    if (g_pipHwnd) return;
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(g_hwnd, GWLP_HINSTANCE);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = PipWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"VertexPipWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    g_pipHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"VertexPipWindow", L"Picture in Picture",
        WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU,
        g_pipRect.left, g_pipRect.top,
        g_pipRect.right - g_pipRect.left,
        g_pipRect.bottom - g_pipRect.top,
        g_hwnd, NULL, hi, NULL);
}

static void EnterPictureInPicture(Node* source) {
    EnsurePipWindow();
    g_pipActive = true;
    g_pipSourceElement = source;
    if (source) source->attrs["data-vertex-pip"] = "true";
    SetWindowPos(g_pipHwnd, HWND_TOPMOST,
        g_pipRect.left, g_pipRect.top,
        g_pipRect.right - g_pipRect.left,
        g_pipRect.bottom - g_pipRect.top,
        SWP_SHOWWINDOW);
    InvalidateRect(g_pipHwnd, nullptr, TRUE);
    DispatchPlatformEventDetail("enterpictureinpicture", NodeLabel(source));
}

static void ExitPictureInPicture() {
    if (!g_pipActive) return;
    g_pipActive = false;
    if (g_pipSourceElement) g_pipSourceElement->attrs.erase("data-vertex-pip");
    g_pipSourceElement = nullptr;
    if (g_pipHwnd) ShowWindow(g_pipHwnd, SW_HIDE);
    DispatchPlatformEventDetail("leavepictureinpicture", "closed");
}

static std::wstring FilePickerFilter(const std::vector<std::wstring>& filters) {
    if (filters.empty()) return L"All Files\0*.*\0\0";
    std::wstring out;
    for (const auto& f : filters) {
        out += f;
        out.push_back(L'\0');
        out += f;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

static std::vector<std::string> ShowOpenFilePicker(bool multi, const std::vector<std::wstring>& filters = {}) {
    std::vector<std::string> files;
    std::vector<wchar_t> buffer(32768, L'\0');
    std::wstring filter = FilePickerFilter(filters);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = (DWORD)buffer.size();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) return files;
    wchar_t* p = buffer.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (!multi || *p == L'\0') {
        files.push_back(ToUtf8(first));
        g_lastPickedFileList = JoinStrings(files, "|");
        DispatchPlatformEventDetail("filepickerchange", g_lastPickedFileList);
        return files;
    }
    while (*p) {
        std::wstring name = p;
        files.push_back(ToUtf8(first + L"\\" + name));
        p += name.size() + 1;
    }
    g_lastPickedFileList = JoinStrings(files, "|");
    DispatchPlatformEventDetail("filepickerchange", g_lastPickedFileList);
    return files;
}

static void WriteClipboardText(const std::string& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (h) {
        memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

static std::string ReadClipboardText() {
    std::string out;
    if (!OpenClipboard(g_hwnd)) return out;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        if (char* p = static_cast<char*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

static void WriteClipboardHtml(const std::string& html) {
    g_lastClipboardHtml = html;
    UINT htmlFormat = RegisterClipboardFormatW(L"HTML Format");
    std::string prefix = "Version:0.9\r\nStartHTML:00000097\r\nEndHTML:%08u\r\nStartFragment:00000131\r\nEndFragment:%08u\r\n<html><body><!--StartFragment-->";
    std::string suffix = "<!--EndFragment--></body></html>";
    unsigned endFragment = (unsigned)(131 + html.size());
    unsigned endHtml = (unsigned)(131 + html.size() + suffix.size());
    char header[256] = {};
    std::snprintf(header, sizeof(header), prefix.c_str(), endHtml, endFragment);
    std::string payload = std::string(header) + html + suffix;
    std::string plain = html;
    bool inTag = false;
    std::string plainOut;
    for (char c : plain) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) plainOut.push_back(c);
    }
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, payload.size() + 1);
        if (h) {
            memcpy(GlobalLock(h), payload.c_str(), payload.size() + 1);
            GlobalUnlock(h);
            SetClipboardData(htmlFormat, h);
        }
        HGLOBAL textHandle = GlobalAlloc(GMEM_MOVEABLE, plainOut.size() + 1);
        if (textHandle) {
            memcpy(GlobalLock(textHandle), plainOut.c_str(), plainOut.size() + 1);
            GlobalUnlock(textHandle);
            SetClipboardData(CF_TEXT, textHandle);
        }
        CloseClipboard();
    }
    DispatchPlatformEventDetail("clipboardchange", "html " + std::to_string(html.size()) + " bytes");
}

static void StoreClipboardImage(std::vector<uint8_t> bytes) {
    g_lastClipboardImage = std::move(bytes);
    DispatchPlatformEventDetail("clipboardchange", "image " + std::to_string(g_lastClipboardImage.size()) + " bytes");
}

static void BeginFileDrag(HDROP drop) {
    g_dragFiles.clear();
    GetCursorPos(&g_dragPayload.screenPoint);
    g_dragPayload.clientPoint = g_dragPayload.screenPoint;
    ScreenToClient(g_hwnd, &g_dragPayload.clientPoint);
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH] = {};
        DragQueryFileW(drop, i, path, MAX_PATH);
        g_dragFiles.push_back(ToUtf8(path));
    }
    DragFinish(drop);
    g_dragActive = !g_dragFiles.empty();
    g_dragPayload.files = g_dragFiles;
    g_dragPayload.hasFiles = g_dragActive;
    std::string detail = std::to_string(g_dragFiles.size()) + " file(s) at "
        + std::to_string(g_dragPayload.clientPoint.x) + "," + std::to_string(g_dragPayload.clientPoint.y);
    DispatchPlatformEventDetail("dragenter", detail);
    DispatchPlatformEventDetail("dragover", detail);
    DispatchPlatformEventDetail("drop", JoinStrings(g_dragFiles, "|"));
    g_dragActive = false;
}

using XInputGetStateFn = DWORD (WINAPI*)(DWORD, void*);
static XInputGetStateFn LoadXInputGetState() {
    if (!g_xinputModule) {
        g_xinputModule = LoadLibraryW(L"xinput1_4.dll");
        if (!g_xinputModule) g_xinputModule = LoadLibraryW(L"xinput9_1_0.dll");
    }
    return g_xinputModule ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(g_xinputModule, "XInputGetState")) : nullptr;
}

static void PollGamepads() {
    auto getState = LoadXInputGetState();
    if (!getState) return;
    struct XGamepad { WORD buttons; BYTE leftTrigger; BYTE rightTrigger; SHORT leftX; SHORT leftY; SHORT rightX; SHORT rightY; };
    struct XState { DWORD packet; XGamepad gamepad; };
    for (DWORD i = 0; i < 4; ++i) {
        XState state{};
        DWORD ok = getState(i, &state);
        bool connected = ok == 0;
        if (connected != g_gamepadConnected[i]) {
            g_gamepadConnected[i] = connected;
            g_gamepads[i].connected = connected;
            DispatchPlatformEventDetail(connected ? "gamepadconnected" : "gamepaddisconnected", "index " + std::to_string(i));
        }
        if (connected && state.packet != g_gamepadPacket[i]) {
            g_gamepadPacket[i] = state.packet;
            g_gamepads[i].packet = state.packet;
            g_gamepads[i].buttons = state.gamepad.buttons;
            g_gamepads[i].leftTrigger = state.gamepad.leftTrigger;
            g_gamepads[i].rightTrigger = state.gamepad.rightTrigger;
            g_gamepads[i].leftX = state.gamepad.leftX;
            g_gamepads[i].leftY = state.gamepad.leftY;
            g_gamepads[i].rightX = state.gamepad.rightX;
            g_gamepads[i].rightY = state.gamepad.rightY;
            DispatchPlatformEventDetail("gamepadinput", "index " + std::to_string(i) + " buttons " + std::to_string(state.gamepad.buttons));
        }
    }
}

static void ShowPlatformNotification(const std::string& title, const std::string& body) {
    if (!g_notificationPermissionGranted) return;
    UINT id = g_nextNotificationId++;
    g_notifications[id] = title + "\n" + body;
    g_notificationRecords[id] = { id, title, body, true, false, false };
    SetStatus("Notification: " + title);
    FlashWindow(g_hwnd, TRUE);
    DispatchPlatformEventDetail("notificationshow", std::to_string(id) + " " + title);
}

static void HandleMediaCommand(WPARAM wp, LPARAM lp) {
    int cmd = GET_APPCOMMAND_LPARAM(lp);
    switch (cmd) {
    case APPCOMMAND_MEDIA_PLAY: g_mediaSessionAction = "play"; break;
    case APPCOMMAND_MEDIA_PAUSE: g_mediaSessionAction = "pause"; break;
    case APPCOMMAND_MEDIA_PLAY_PAUSE: g_mediaSessionAction = "playpause"; break;
    case APPCOMMAND_MEDIA_NEXTTRACK: g_mediaSessionAction = "nexttrack"; break;
    case APPCOMMAND_MEDIA_PREVIOUSTRACK: g_mediaSessionAction = "previoustrack"; break;
    case APPCOMMAND_MEDIA_STOP: g_mediaSessionAction = "stop"; break;
    default: g_mediaSessionAction.clear(); break;
    }
    if (!g_mediaSessionAction.empty())
        DispatchPlatformEvent("mediasessionaction");
}

static void DismissTransientTopLayer() {
    if (g_activePopover) {
        HidePlatformPopover();
        return;
    }
    if (g_modalDialogActive) {
        CloseModalDialog(g_modalDialog);
        return;
    }
    if (g_pipActive) {
        ExitPictureInPicture();
        return;
    }
    if (g_pointerLocked) {
        ExitPointerLock();
        return;
    }
}

static void SetPermissionState(const std::string& name, const std::string& state) {
    g_permissions[name] = { name, state, (long long)GetTickCount64() };
    DispatchPlatformEventDetail("permissionchange", name + "=" + state);
}

static std::string QueryPermissionState(const std::string& name) {
    auto it = g_permissions.find(name);
    if (it != g_permissions.end()) return it->second.state;
    if (name == "notifications") return g_notificationPermissionGranted ? "granted" : "denied";
    if (name == "clipboard-read" || name == "clipboard-write") return "granted";
    if (name == "fullscreen" || name == "pointer-lock" || name == "gamepad") return "granted";
    return "prompt";
}

static void RefreshScreenDetails() {
    g_screenDetails.clear();
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM) -> BOOL {
            MONITORINFOEXW mi{ sizeof(mi) };
            if (!GetMonitorInfoW(mon, &mi)) return TRUE;
            HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, nullptr, nullptr);
            int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
            if (dc) DeleteDC(dc);
            g_screenDetails.push_back({
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                mi.rcWork.right - mi.rcWork.left,
                mi.rcWork.bottom - mi.rcWork.top,
                dpi,
                (mi.dwFlags & MONITORINFOF_PRIMARY) != 0
            });
            return TRUE;
        }, 0);
    DispatchPlatformEventDetail("screenschange", std::to_string(g_screenDetails.size()) + " screen(s)");
}

static std::string ShowSaveFilePicker(const std::string& suggestedName = "vertex-download.txt") {
    wchar_t file[MAX_PATH] = {};
    std::wstring suggested = ToWide(suggestedName);
    wcsncpy_s(file, suggested.c_str(), MAX_PATH - 1);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Text Files\0*.txt\0All Files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return {};
    g_lastSavedFile = ToUtf8(file);
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::string body = "Saved by Vertex platform file picker\r\n";
        DWORD written = 0;
        WriteFile(h, body.data(), (DWORD)body.size(), &written, nullptr);
        CloseHandle(h);
    }
    DispatchPlatformEventDetail("savefile", g_lastSavedFile);
    return g_lastSavedFile;
}

static void SharePlatformPayload(const SharePayload& payload) {
    g_lastSharePayload = payload;
    std::string text = payload.title + "\n" + payload.text + "\n" + payload.url;
    if (!payload.files.empty()) text += "\n" + JoinStrings(payload.files, "\n");
    WriteClipboardText(text);
    DispatchPlatformEventDetail("share", payload.title.empty() ? "shared" : payload.title);
}

static void SetAppBadge(int value) {
    g_appBadge = std::max(0, value);
    SetStatus(g_appBadge > 0 ? ("Badge: " + std::to_string(g_appBadge)) : "Badge cleared");
    DispatchPlatformEventDetail("badgechange", std::to_string(g_appBadge));
}

static void RequestWakeLock(const std::string& type) {
    g_wakeLockActive = true;
    g_screenWakeLockActive = type == "screen";
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | (g_screenWakeLockActive ? ES_DISPLAY_REQUIRED : 0));
    DispatchPlatformEventDetail("wakelockchange", type + " acquired");
}

static void ReleaseWakeLock() {
    if (!g_wakeLockActive) return;
    g_wakeLockActive = false;
    g_screenWakeLockActive = false;
    SetThreadExecutionState(ES_CONTINUOUS);
    DispatchPlatformEventDetail("wakelockchange", "released");
}

static void PrintCurrentPage() {
    PRINTDLGW pd{ sizeof(pd) };
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
    pd.hwndOwner = g_hwnd;
    if (PrintDlgW(&pd)) {
        DOCINFOW di{ sizeof(di) };
        di.lpszDocName = L"Vertex Page";
        if (StartDocW(pd.hDC, &di) > 0) {
            StartPage(pd.hDC);
            RECT rc{ 200, 200, 5200, 7600 };
            std::wstring text = ToWide(CurTab().title + "\n" + CurTab().url);
            DrawTextW(pd.hDC, text.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK);
            EndPage(pd.hDC);
            EndDoc(pd.hDC);
        }
        DeleteDC(pd.hDC);
        DispatchPlatformEventDetail("afterprint", CurTab().url);
    }
}

static void RegisterProtocolHandlerLocal(const std::string& scheme, const std::string& url) {
    g_protocolHandlers[scheme] = url;
    DispatchPlatformEventDetail("protocolhandlerchange", scheme + " -> " + url);
}

static void RegisterFileHandlerLocal(const std::string& extension) {
    if (std::find(g_fileHandlers.begin(), g_fileHandlers.end(), extension) == g_fileHandlers.end())
        g_fileHandlers.push_back(extension);
    DispatchPlatformEventDetail("filehandlerchange", extension);
}

static void EnqueueLaunchFiles(std::vector<std::string> files) {
    g_launchQueueFiles = std::move(files);
    g_launchQueuePending = !g_launchQueueFiles.empty();
    if (g_launchQueuePending)
        DispatchPlatformEventDetail("launch", JoinStrings(g_launchQueueFiles, "|"));
}

static void UpdateLastInputTick() {
    g_lastInputTick = GetTickCount();
    if (g_userIdle) {
        g_userIdle = false;
        DispatchPlatformEventDetail("idlechange", "active");
    }
}

static GeoPositionRecord QueryGeolocation() {
    g_geoPosition.timestamp = (long long)GetTickCount64();
    DispatchPlatformEventDetail("geolocation", std::to_string(g_geoPosition.latitude) + "," + std::to_string(g_geoPosition.longitude));
    return g_geoPosition;
}

static BatteryRecord QueryBattery() {
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        if (ps.BatteryLifePercent != 255)
            g_battery.level = ps.BatteryLifePercent / 100.0;
        g_battery.charging = ps.ACLineStatus == 1;
        g_battery.chargingTime = ps.BatteryLifeTime == (DWORD)-1 ? 0 : (int)ps.BatteryLifeTime;
        g_battery.dischargingTime = ps.BatteryLifeTime == (DWORD)-1 ? 0 : (int)ps.BatteryLifeTime;
    }
    DispatchPlatformEventDetail("batterystatus", std::to_string((int)(g_battery.level * 100)) + "%");
    return g_battery;
}

static void StartIdleDetector() {
    g_idleDetectorActive = true;
    g_lastInputTick = GetTickCount();
    DispatchPlatformEventDetail("idlechange", "watching");
}

static void PollIdleDetector() {
    if (!g_idleDetectorActive) return;
    LASTINPUTINFO li{ sizeof(li) };
    if (GetLastInputInfo(&li)) {
        DWORD idleMs = GetTickCount() - li.dwTime;
        bool idle = idleMs > 60000;
        if (idle != g_userIdle) {
            g_userIdle = idle;
            DispatchPlatformEventDetail("idlechange", idle ? "idle" : "active");
        }
    }
}

static void UpdateSensor(const std::string& type, double x, double y, double z) {
    g_sensors[type] = { type, x, y, z, (long long)GetTickCount64(), true };
    DispatchPlatformEventDetail("sensorreading", type + " " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z));
}

static void SampleSyntheticSensors() {
    double t = GetTickCount64() / 1000.0;
    UpdateSensor("accelerometer", std::sin(t) * 0.2, std::cos(t) * 0.2, 9.8);
    UpdateSensor("gyroscope", std::sin(t) * 0.01, std::cos(t) * 0.01, 0.0);
    UpdateSensor("magnetometer", 22.0 + std::sin(t), 5.0 + std::cos(t), -41.0);
    UpdateSensor("ambient-light", 350.0 + std::sin(t) * 50.0, 0.0, 0.0);
}

static void VibratePattern(std::vector<int> pattern) {
    g_vibrationPattern = std::move(pattern);
    for (int ms : g_vibrationPattern) {
        if (ms > 0) MessageBeep(MB_OK);
    }
    DispatchPlatformEventDetail("vibration", std::to_string(g_vibrationPattern.size()) + " segment(s)");
}

static void SeedContacts() {
    if (!g_contacts.empty()) return;
    g_contacts.push_back({ "Ada Lovelace", "ada@example.test", "+1-555-0101" });
    g_contacts.push_back({ "Grace Hopper", "grace@example.test", "+1-555-0102" });
    g_contacts.push_back({ "Katherine Johnson", "katherine@example.test", "+1-555-0103" });
}

static std::vector<ContactRecord> SelectContacts() {
    SeedContacts();
    DispatchPlatformEventDetail("contactsselect", std::to_string(g_contacts.size()) + " contact(s)");
    return g_contacts;
}

static void AddExternalDevice(const std::string& kind, const std::string& name) {
    std::string id = kind + ":" + std::to_string(g_externalDevices.size() + 1);
    g_externalDevices.push_back({ kind, name, id, true });
    DispatchPlatformEventDetail(kind + "connect", name + " " + id);
}

static void SeedExternalDevices() {
    if (!g_externalDevices.empty()) return;
    AddExternalDevice("serial", "COM1 Debug Port");
    AddExternalDevice("hid", "Vertex Test HID");
    AddExternalDevice("usb", "Vertex USB Device");
    AddExternalDevice("bluetooth", "Vertex BLE Sensor");
}

static void CloseExternalDevices() {
    for (auto& dev : g_externalDevices) dev.open = false;
    DispatchPlatformEventDetail("deviceclose", std::to_string(g_externalDevices.size()) + " device(s)");
}

static void EstimateStorage() {
    ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFree{};
    std::wstring root = ToWide(g_profilePaths.profileRoot);
    if (!root.empty()) GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, &totalFree);
    g_storageUsageBytes = 0;
    auto rows = vertex::profile::ReadTsvRows(g_profilePaths.historyFile);
    for (const auto& row : rows)
        for (const auto& cell : row) g_storageUsageBytes += cell.size();
    if (totalBytes.QuadPart > 0)
        g_storageQuotaBytes = std::min<unsigned long long>(totalBytes.QuadPart / 10, 2ull * 1024ull * 1024ull * 1024ull);
    DispatchPlatformEventDetail("storageestimate", std::to_string(g_storageUsageBytes) + "/" + std::to_string(g_storageQuotaBytes));
}

// ─── scrollbar ───────────────────────────────────────────────────────────────
static int ViewportH() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    return rc.bottom - rc.top - ChromeTopInset() - ChromeBottomInset();
}
static void UpdateScrollbar() {
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin  = 0;
    si.nMax  = (int)CurTab().docHeight;
    si.nPage = (UINT)std::max(0, ViewportH());
    si.nPos  = (int)CurTab().scrollY;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}
static void ClampScroll() {
    float maxY = std::max(0.f, CurTab().docHeight - (float)ViewportH());
    CurTab().scrollY = std::max(0.f, std::min(CurTab().scrollY, maxY));
}

static RECT ContentPaintRect() {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    rc.top = ChromeTopInset();
    rc.bottom = std::max(rc.top, rc.bottom - ChromeBottomInset());
    return rc;
}

static void InvalidateContent() {
    if (!g_hwnd) return;
    RECT rc = ContentPaintRect();
    InvalidateRect(g_hwnd, &rc, FALSE);
}

static RECT HoverRegionToClientRect(const HitRegion& region) {
    RECT content = ContentPaintRect();
    RECT rc{};
    rc.left = (LONG)(region.x - 3.f);
    rc.top = (LONG)(region.y - CurTab().scrollY + (float)ChromeTopInset() - 3.f);
    rc.right = (LONG)(region.x + region.w + 4.f);
    rc.bottom = (LONG)(region.y + region.h - CurTab().scrollY + (float)ChromeTopInset() + 4.f);
    rc.left = std::max(content.left, rc.left);
    rc.top = std::max(content.top, rc.top);
    rc.right = std::min(content.right, rc.right);
    rc.bottom = std::min(content.bottom, rc.bottom);
    return rc;
}

static void InvalidateHoverRegions(const HitRegion* oldRegion, const HitRegion* newRegion) {
    if (!g_hwnd) return;
    if (!oldRegion || !newRegion) {
        InvalidateContent();
        return;
    }
    // Invalidate both regions separately to ensure both repaint correctly,
    // even if they're far apart or one is partially off-screen.
    RECT oldRect = HoverRegionToClientRect(*oldRegion);
    RECT newRect = HoverRegionToClientRect(*newRegion);
    if (oldRect.right > oldRect.left && oldRect.bottom > oldRect.top)
        InvalidateRect(g_hwnd, &oldRect, FALSE);
    if (newRect.right > newRect.left && newRect.bottom > newRect.top)
        InvalidateRect(g_hwnd, &newRect, FALSE);
}

static void ClearPendingPageScriptsForTab(int tabIdx) {
    g_pendingPageScripts.erase(
        std::remove_if(g_pendingPageScripts.begin(), g_pendingPageScripts.end(),
            [tabIdx](const PendingPageScript& job) { return job.tabIdx == tabIdx; }),
        g_pendingPageScripts.end());
}

static bool PendingPageScriptStillCurrent(const PendingPageScript& job) {
    return job.tabIdx >= 0
        && job.tabIdx < (int)g_tabs.size()
        && g_tabs[job.tabIdx].page
        && g_tabs[job.tabIdx].page->url == job.pageUrl;
}

static bool PendingPageScriptWaitingForFetch(const PendingPageScript& job) {
    return job.fetchBeforeRun && job.source.empty() && !job.filename.empty();
}

static void RequeueFetchedPageScript(HWND hwnd, PendingPageScript job, FetchResult res) {
    if (!PendingPageScriptStillCurrent(job)) return;
    if (res.success && !res.body.empty())
        job.source = DecodeTextToUtf8(res.body, res.contentType);
    job.fetchBeforeRun = false;
    g_pendingPageScripts.push_back(std::move(job));
    SetTimer(hwnd, TIMER_MAIN, 16, NULL);
}

static void RunPendingPageScripts(HWND hwnd) {
    size_t ran = 0;
    while (!g_pendingPageScripts.empty() && ran < kMaxScriptsPerTimerTick) {
        PendingPageScript job = std::move(g_pendingPageScripts.front());
        g_pendingPageScripts.pop_front();
        if (!PendingPageScriptStillCurrent(job)) continue;
        try {
            if (job.dispatchLoadEvents) {
                g_js.dispatchDocumentEvent("DOMContentLoaded");
                g_js.dispatchWindowEvent("load");
            } else if (PendingPageScriptWaitingForFetch(job)) {
                FetchResourceAsync(job.filename, 1024 * 1024, ResourceKind::Script,
                    [hwnd, job = std::move(job)](FetchResult res) mutable {
                        RequeueFetchedPageScript(hwnd, std::move(job), std::move(res));
                    });
            } else {
                std::string source = std::move(job.source);
                if (!source.empty())
                    g_js.runScript(source, job.filename);
            }
        } catch (...) {
            OutputDebugStringA("[JS] Pending page script failed; continuing\n");
        }
        ++ran;
    }
    if (!g_pendingPageScripts.empty())
        SetTimer(hwnd, TIMER_MAIN, 16, NULL);
}

// Home page HTML comes from HomePageHtml() in browser_core.h.

// ─── title extraction ────────────────────────────────────────────────────────
static std::string ExtractTitle(const Node* root) {
    if (!root) return {};
    std::function<std::string(const Node*)> find = [&](const Node* n) -> std::string {
        if (!n) return {};
        if (n->type == NodeType::Element && n->tagName == "title") {
            std::string t;
            for (auto& c : n->children)
                if (c->type == NodeType::Text) t += c->text;
            while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
            while (!t.empty() && isspace((unsigned char)t.back()))  t.pop_back();
            return t;
        }
        for (auto& c : n->children) {
            auto r = find(c.get());
            if (!r.empty()) return r;
        }
        return {};
    };
    return find(root);
}

// ─── address-bar input: URL vs. search query ──────────────────────────────────
// UrlEncodeQuery, LooksLikeUrl, TabPushHistory, UrlFragment, UrlWithoutFragment
// now come from browser_core.h via chrome.h.

// ─── navigation ──────────────────────────────────────────────────────────────
static void Navigate(int tabIdx, const std::string& rawUrl, bool pushHistory = true);
static void Navigate(const std::string& rawUrl, bool push = true) {
    Navigate(g_activeTab, rawUrl, push);
}

static void ShowInternalPage(const std::string& url,
                             const std::string& title,
                             const std::string& html,
                             bool pushHistory = true) {
    Tab& tab = CurTab();
    tab.page.reset(new Page{ url, ParseHtml(html), {} });
    tab.url = url;
    tab.title = title;
    tab.loading = false;
    tab.scrollY = 0.f;
    tab.pendingFragment.clear();
    tab.fragmentScrollPending = false;
    if (pushHistory) TabPushHistory(tab, url);
    EnableWindow(g_hwndStop, FALSE);
    EnableWindow(g_hwndRefr, TRUE);
    SetUrlBar(url);
    UpdateTitle();
    g_renderer.InvalidateLayout();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ShowDownloadsPage(bool pushHistory = true) {
    ShowInternalPage("vertex://downloads", "Downloads", DownloadsPageHtml(), pushHistory);
}

static void ShowBookmarksPage(bool pushHistory = true) {
    ShowInternalPage("vertex://bookmarks", "Bookmarks", BookmarksPageHtml(), pushHistory);
}

static void ShowSettingsPage(bool pushHistory = true) {
    ShowInternalPage("vertex://settings", "Settings", SettingsPageHtml(), pushHistory);
}

static void ShowSiteDataPage(bool pushHistory = true) {
    ShowInternalPage("vertex://site-data", "Site Data", SiteDataPageHtml(), pushHistory);
}

static void ShowPlatformFeaturesPage(bool pushHistory = true) {
    ShowInternalPage("vertex://platform-features", "Platform Features", PlatformFeaturesPageHtml(), pushHistory);
}

static void StartDownload(const std::string& url, const std::string& downloadName = "") {
    if (url.empty()) return;
    SetStatus("Downloading " + url);
    SetTimer(g_hwnd, TIMER_MAIN, 16, NULL);
    FetchResourceAsync(url, 256 * 1024 * 1024, ResourceKind::Other,
        [url, downloadName](FetchResult res) {
            auto record = vertex::downloads::SaveFetchedBody(url, res, downloadName);
            g_downloads.push_back(record);
            AppendDownloadRecord(record);
            SetStatus(record.success ? ("Saved " + record.filename)
                                     : ("Download failed: " + record.error));
            if (CurTab().url == "vertex://downloads")
                ShowDownloadsPage(false);
        });
}

static void Navigate(int tabIdx, const std::string& rawUrl, bool pushHistory) {
    if (tabIdx < 0 || tabIdx >= (int)g_tabs.size()) return;
    if (g_windowFullscreen && tabIdx == g_activeTab)
        ExitWindowFullscreen(false);
    Tab& tab = g_tabs[tabIdx];
    if (tab.loading) return;
    ClearPendingPageScriptsForTab(tabIdx);

    std::string url = rawUrl;
    tab.displayUrl.clear();

    // ── built-in: home ──────────────────────────────────────────────────
    if (url.empty() || url == "vertex://home" || url == "felix://home") {
        url = "vertex://home";
        tab.page.reset(new Page{ url, ParseHtml(HomePageHtml()), {} });
        tab.url     = url;
        tab.title   = "Vertex";
        tab.loading = false;
        tab.scrollY = 0.f;
        tab.pendingFragment.clear();
        tab.fragmentScrollPending = false;
        if (pushHistory) TabPushHistory(tab, url);
        if (tabIdx == g_activeTab) {
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            SetUrlBar(url);
            UpdateTitle();
            g_renderer.InvalidateLayout();
            UpdateScrollbar();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        return;
    }

    // ── built-in: history ──────────────────────────────────────────────
    if (url == "vertex://history" || url == "felix://history") {
        ShowInternalPage("vertex://history", "History", HistoryPageHtml(), pushHistory);
        return;
    }

    if (url == "vertex://downloads") {
        ShowDownloadsPage(pushHistory);
        return;
    }
    if (url == "vertex://bookmarks") {
        ShowBookmarksPage(pushHistory);
        return;
    }
    if (url == "vertex://settings") {
        ShowSettingsPage(pushHistory);
        return;
    }
    if (url == "vertex://site-data") {
        ShowSiteDataPage(pushHistory);
        return;
    }
    if (url == "vertex://platform-features") {
        ShowPlatformFeaturesPage(pushHistory);
        return;
    }

    // If it's a URL, ensure it has a scheme; otherwise treat it as a search
    // query and route it to DuckDuckGo's server-rendered results page.
    std::string displayUrl = url;
    if (LooksLikeUrl(url)) {
        if (url.find("://") == std::string::npos)
            url = "https://" + url;
        displayUrl = url;
    } else {
        displayUrl = url;
        url = "https://duckduckgo.com/html/?q=" + UrlEncodeQuery(url);
    }

    tab.loading    = true;
    tab.url        = url;
    tab.displayUrl = displayUrl;
    tab.title      = "Loading…";
    tab.pendingFragment = UrlFragment(url);
    tab.fragmentScrollPending = !tab.pendingFragment.empty();
    if (pushHistory) {
        TabPushHistory(tab, url);
        AppendHistoryRecord(url);
    }

    if (tabIdx == g_activeTab) {
        EnableWindow(g_hwndStop, TRUE);
        EnableWindow(g_hwndRefr, FALSE);
        SetUrlBar(displayUrl);
        UpdateTitle();
        SetTimer(g_hwnd, TIMER_MAIN, 16, NULL);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }

    HWND hwnd = g_hwnd;
    std::string fetchUrl = UrlWithoutFragment(url);
    std::thread([hwnd, url, fetchUrl, tabIdx]() {
        auto* p = new Page;
        p->url   = url;
        try {
            auto res = FetchResourceCached(fetchUrl, 12 * 1024 * 1024, ResourceKind::Document);
            if (res.success) {
                p->dom = ParseHtml(DecodeTextToUtf8(res.body, res.contentType, true));
                if (!res.finalUrl.empty() && res.finalUrl != url)
                    p->url = res.finalUrl;
                LoadExternalStylesheets(p->dom, p->url);
                LoadExternalScriptSources(p->dom, p->url);
            } else {
                p->error = res.error;
            }
        } catch (...) {
            p->dom.reset();
            p->error = "Failed to load page (internal error).";
        }
        auto* pm = new PageMsg{ tabIdx, p };
        PostMessageW(hwnd, WM_PAGE_READY, 0, (LPARAM)pm);
    }).detach();
}

static void GoBack() {
    Tab& tab = CurTab();
    if (tab.histIdx > 0) Navigate(g_activeTab, tab.history[--tab.histIdx], false);
}
static void GoForward() {
    Tab& tab = CurTab();
    if (tab.histIdx + 1 < (int)tab.history.size())
        Navigate(g_activeTab, tab.history[++tab.histIdx], false);
}

// ─── tab management ───────────────────────────────────────────────────────────
static void NewTab(const std::string& url = "vertex://home") {
    g_tabs.emplace_back();
    int idx = (int)g_tabs.size() - 1;
    g_activeTab = idx;
    InvalidateRect(g_hwnd, NULL, FALSE);
    Navigate(idx, url);
}

static void CloseTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    if (g_windowFullscreen && idx == g_activeTab)
        ExitWindowFullscreen(false);
    if (g_tabs.size() <= 1) {
        Navigate("vertex://home");
        return;
    }
    g_tabs.erase(g_tabs.begin() + idx);
    if (g_activeTab >= (int)g_tabs.size())
        g_activeTab = (int)g_tabs.size() - 1;
    SetUrlBarForTab(CurTab());
    UpdateTitle();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void SwitchTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    if (g_windowFullscreen && idx != g_activeTab)
        ExitWindowFullscreen(false);
    g_activeTab = idx;
    const Tab& tab = CurTab();
    SetUrlBarForTab(tab);
    UpdateTitle();
    if (tab.loading) {
        EnableWindow(g_hwndStop, TRUE);
        EnableWindow(g_hwndRefr, FALSE);
    } else {
        EnableWindow(g_hwndStop, FALSE);
        EnableWindow(g_hwndRefr, TRUE);
    }
    ClampScroll();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ─── control layout ──────────────────────────────────────────────────────────
static void LayoutControls() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (g_windowFullscreen) {
        UpdateFullscreenChromeVisibility();
        return;
    }
    int btnY = TAB_H + (TOOLBAR_H - BTN_H) / 2;
    int x    = MARGIN;
    SetWindowPos(g_hwndBack, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP;
    SetWindowPos(g_hwndFwrd, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP + 4;
    SetWindowPos(g_hwndRefr, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP;
    SetWindowPos(g_hwndStop, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    x += BTN_W + GAP + 4;
    SetWindowPos(g_hwndHome, NULL, x, btnY, BTN_W, BTN_H, SWP_NOZORDER);
    int urlX = x + BTN_W + MARGIN + 2;
    int urlW = w - urlX - MARGIN;
    SetWindowPos(g_hwndUrlBadge, NULL, urlX + 5, btnY + 4, URL_BADGE_W - 6, BTN_H - 8, SWP_NOZORDER);
    SetWindowPos(g_hwndUrl,    NULL, urlX + URL_BADGE_W, btnY, urlW - URL_BADGE_W - 6, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFind,   NULL, 0, h - STATUS_H - FIND_H, w, FIND_H, SWP_NOZORDER);
}

static void DrawChromeButton(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focus = (dis->itemState & ODS_FOCUS) != 0;
    bool hover = (dis->hwndItem == g_hoverChromeButton);

    COLORREF fill = disabled ? kChromeDisabled
        : pressed ? kChromePressed
        : hover ? kChromeHover
        : kChromeActive;
    COLORREF stroke = focus ? kChromeAccent : kChromeLine;
    COLORREF text = disabled ? kChromeDisabledText : kChromeInk;

    HBRUSH bg = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focus ? 2 : 1, stroke);
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, CORNER_R, CORNER_R);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bg);

    wchar_t label[16] = {};
    GetWindowTextW(dis->hwndItem, label, 16);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HFONT oldFont = g_iconFont ? (HFONT)SelectObject(dc, g_iconFont) : nullptr;
    if (pressed) OffsetRect(&r, 0, 1);
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    if (oldFont) SelectObject(dc, oldFont);
}

static void InvalidateControlFrame(HWND control) {
    if (!g_hwnd || !control) return;
    RECT r{};
    GetWindowRect(control, &r);
    MapWindowPoints(NULL, g_hwnd, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, 4, 4);
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void DrawEditFrame(HDC dc, HWND edit) {
    if (!edit) return;
    RECT r{};
    GetWindowRect(edit, &r);
    MapWindowPoints(NULL, g_hwnd, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, 2, 2);
    bool focused = (GetFocus() == edit);

    HBRUSH bg = CreateSolidBrush(focused ? kChromeActive : kChromeHover);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? kChromeAccent : kChromeLine);
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (focused) {
        RECT halo = r;
        InflateRect(&halo, 2, 2);
        HPEN haloPen = CreatePen(PS_SOLID, 1, kChromeAccentSoft);
        HGDIOBJ oldHaloPen = SelectObject(dc, haloPen);
        HGDIOBJ oldHaloBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, halo.left, halo.top, halo.right, halo.bottom, CORNER_R + 4, CORNER_R + 4);
        SelectObject(dc, oldHaloBrush);
        SelectObject(dc, oldHaloPen);
        DeleteObject(haloPen);
    }
    RoundRect(dc, r.left, r.top, r.right, r.bottom, CORNER_R + 2, CORNER_R + 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bg);
}

static void DrawUrlFrame(HDC dc) {
    if (!g_hwndUrl || !g_hwndUrlBadge) return;
    RECT r{};
    GetWindowRect(g_hwndUrlBadge, &r);
    RECT edit{};
    GetWindowRect(g_hwndUrl, &edit);
    if (edit.left < r.left) r.left = edit.left;
    if (edit.top < r.top) r.top = edit.top;
    if (edit.right > r.right) r.right = edit.right;
    if (edit.bottom > r.bottom) r.bottom = edit.bottom;
    MapWindowPoints(NULL, g_hwnd, reinterpret_cast<POINT*>(&r), 2);
    InflateRect(&r, 3, 2);
    bool focused = (GetFocus() == g_hwndUrl);

    HBRUSH bg = CreateSolidBrush(focused ? kChromeActive : kChromeHover);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? kChromeAccent : kChromeLine);
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (focused) {
        RECT halo = r;
        InflateRect(&halo, 2, 2);
        HPEN haloPen = CreatePen(PS_SOLID, 1, kChromeAccentSoft);
        HGDIOBJ oldHaloPen = SelectObject(dc, haloPen);
        HGDIOBJ oldHaloBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, halo.left, halo.top, halo.right, halo.bottom, CORNER_R + 4, CORNER_R + 4);
        SelectObject(dc, oldHaloBrush);
        SelectObject(dc, oldHaloPen);
        DeleteObject(haloPen);
    }
    RoundRect(dc, r.left, r.top, r.right, r.bottom, CORNER_R + 2, CORNER_R + 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(bg);
}

static void DrawLoadingAccent(HDC dc) {
    if (!AnyTabLoading()) return;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    int w = rc.right - rc.left;
    if (w <= 0) return;
    float phase = (float)((GetTickCount() % 1400) / 1400.0);
    int segW = std::max(96, w / 5);
    int x = (int)((w + segW) * phase) - segW;
    RECT rail{ 0, TOP_INSET - 3, w, TOP_INSET };
    HBRUSH railBrush = CreateSolidBrush(kChromeAccentSoft);
    FillRect(dc, &rail, railBrush);
    DeleteObject(railBrush);
    RECT seg{ x, TOP_INSET - 3, std::min(w, x + segW), TOP_INSET };
    if (seg.right > 0) {
        if (seg.left < 0) seg.left = 0;
        HBRUSH accentBrush = CreateSolidBrush(kChromeAccent);
        FillRect(dc, &seg, accentBrush);
        DeleteObject(accentBrush);
    }
}

LRESULT CALLBACK ChromeButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hoverChromeButton != hwnd) {
        HWND old = g_hoverChromeButton;
        g_hoverChromeButton = hwnd;
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE && g_hoverChromeButton == hwnd) {
        g_hoverChromeButton = nullptr;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── URL bar subclass ─────────────────────────────────────────────────────────
LRESULT CALLBACK UrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        wchar_t buf[2048] = {};
        GetWindowTextW(hwnd, buf, 2048);
        Navigate(ToUtf8(buf));
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r') return 0;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        InvalidateControlFrame(hwnd);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── find bar helpers ─────────────────────────────────────────────────────────
static void ShowFind(bool show) {
    g_findVisible = show;
    ShowWindow(g_hwndFind, (show && !g_windowFullscreen) ? SW_SHOW : SW_HIDE);
    if (show) {
        SetFocus(g_hwndFind);
        SendMessageW(g_hwndFind, EM_SETSEL, 0, -1);
    } else {
        g_renderer.SetSearchQuery(L"");
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    // Re-layout status bar (find bar sits above it)
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFind,   NULL, 0, h - STATUS_H - FIND_H, w, FIND_H, SWP_NOZORDER);
}

static void FindNextInPage(bool backwards) {
    if (!g_findVisible) ShowFind(true);
    wchar_t buf[512] = {};
    GetWindowTextW(g_hwndFind, buf, 512);
    std::wstring query = buf;
    if (query.empty()) return;
    g_renderer.SetSearchQuery(query);
    float hitY = 0.f;
    if (g_renderer.FindTextY(query, CurTab().scrollY, backwards, hitY)) {
        CurTab().scrollY = std::max(0.f, hitY - 24.f);
        ClampScroll();
        UpdateScrollbar();
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

LRESULT CALLBACK FindProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { ShowFind(false); return 0; }
        if (wp == VK_RETURN) { return 0; }
    }
    if (msg == WM_CHAR || (msg == WM_KEYDOWN && wp != VK_ESCAPE)) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        // Update search query from current text
        wchar_t buf[512] = {};
        GetWindowTextW(hwnd, buf, 512);
        g_renderer.SetSearchQuery(buf);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return r;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        InvalidateControlFrame(hwnd);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── build tab entries for renderer ──────────────────────────────────────────
static std::vector<TabEntry> BuildTabEntries() {
    std::vector<TabEntry> entries;
    entries.reserve(g_tabs.size());
    for (int i = 0; i < (int)g_tabs.size(); i++) {
        TabEntry e;
        e.title   = ToWide(g_tabs[i].title.empty() ? "New Tab" : g_tabs[i].title);
        e.active  = (i == g_activeTab);
        e.loading = g_tabs[i].loading;
        e.loadingProgress = (float)((GetTickCount() % 1200) / 1200.0);
        entries.push_back(std::move(e));
    }
    return entries;
}

// ─── WndProc ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_KEYDOWN || msg == WM_CHAR)
        UpdateLastInputTick();
    switch (msg) {

    case WM_CREATE: {
        g_cursorArrow = LoadCursor(NULL, IDC_ARROW);
        g_cursorHand  = LoadCursor(NULL, IDC_HAND);
        g_cursorIBeam = LoadCursor(NULL, IDC_IBEAM);
        g_cursorSizeAll = LoadCursor(NULL, IDC_SIZEALL);
        g_cursorNo = LoadCursor(NULL, IDC_NO);
        g_cursorCross = LoadCursor(NULL, IDC_CROSS);
        g_cursorHelp = LoadCursor(NULL, IDC_HELP);
        ApplyThemedWindowIcon(hwnd);

        HINSTANCE hi = GetModuleHandleW(NULL);
        auto btn = [&](LPCWSTR t, int id) {
            return CreateWindowW(L"BUTTON", t,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                0,0,0,0, hwnd, (HMENU)(intptr_t)id, hi, NULL);
        };
        // Segoe MDL2 Assets glyphs (Windows' own native icon font — see g_iconFont).
        g_hwndBack = btn(L"\xE72B", IDC_BACK);
        g_hwndFwrd = btn(L"\xE72A", IDC_FWRD);
        g_hwndRefr = btn(L"\xE72C", IDC_REFR);
        g_hwndStop = btn(L"\xE711", IDC_STOP);
        g_hwndHome = btn(L"\xE10F", IDC_HOME);
        SetWindowSubclass(g_hwndBack, ChromeButtonProc, 11, 0);
        SetWindowSubclass(g_hwndFwrd, ChromeButtonProc, 12, 0);
        SetWindowSubclass(g_hwndRefr, ChromeButtonProc, 13, 0);
        SetWindowSubclass(g_hwndStop, ChromeButtonProc, 14, 0);
        SetWindowSubclass(g_hwndHome, ChromeButtonProc, 15, 0);

        g_hwndUrlBadge = CreateWindowW(L"STATIC", L"H",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_CENTER | SS_CENTERIMAGE,
            0,0,0,0, hwnd, NULL, hi, NULL);
        g_hwndUrl = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_URL, hi, NULL);
        g_hwndStatus = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
            0,0,0,0, hwnd, NULL, hi, NULL);

        g_hwndFind = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_FIND, hi, NULL);

        EnableWindow(g_hwndStop, FALSE);
        SetWindowSubclass(g_hwndUrl,  UrlProc,  1, 0);
        SetWindowSubclass(g_hwndFind, FindProc, 2, 0);

        g_uiFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_iconFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        g_urlFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_toolbarBrush = CreateSolidBrush(kChromePanel);
        g_statusBrush = CreateSolidBrush(kChromeRail);
        g_editBrush = CreateSolidBrush(kChromeActive);
        SendMessageW(g_hwndUrl, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        SendMessageW(g_hwndFind, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        if (g_iconFont) {
            SendMessageW(g_hwndBack, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
            SendMessageW(g_hwndFwrd, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
            SendMessageW(g_hwndRefr, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
            SendMessageW(g_hwndStop, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
            SendMessageW(g_hwndHome, WM_SETFONT, (WPARAM)g_iconFont, TRUE);
        }
        if (g_uiFont) {
            SendMessageW(g_hwndUrlBadge, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_hwndStatus, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        }
        if (g_urlFont) {
            SendMessageW(g_hwndUrl, WM_SETFONT, (WPARAM)g_urlFont, TRUE);
            SendMessageW(g_hwndFind, WM_SETFONT, (WPARAM)g_urlFont, TRUE);
        }
        DragAcceptFiles(hwnd, TRUE);
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        SetTimer(hwnd, TIMER_GAMEPAD, 250, NULL);
        SetTimer(hwnd, TIMER_MEDIA_SESSION, 1000, NULL);
        SetPermissionState("notifications", "granted");
        SetPermissionState("clipboard-read", "granted");
        SetPermissionState("clipboard-write", "granted");
        SetPermissionState("fullscreen", "granted");
        SetPermissionState("pointer-lock", "granted");
        SetPermissionState("gamepad", "granted");
        SetPermissionState("geolocation", "granted");
        SetPermissionState("camera", "prompt");
        SetPermissionState("microphone", "prompt");
        SetPermissionState("sensors", "granted");
        SetPermissionState("contacts", "granted");
        SetPermissionState("serial", "granted");
        SetPermissionState("hid", "granted");
        SetPermissionState("usb", "granted");
        SetPermissionState("bluetooth", "prompt");
        RegisterProtocolHandlerLocal("web+vertex", "vertex://protocol-handler?url=%s");
        RegisterFileHandlerLocal(".html");
        RegisterFileHandlerLocal(".txt");
        RefreshScreenDetails();
        QueryBattery();
        EstimateStorage();
        SeedContacts();
        SeedExternalDevices();

        g_renderer.SetImageRequestCallback([hwnd](std::string url) {
            FetchResourceAsync(url, 32 * 1024 * 1024, ResourceKind::Image,
                [hwnd, url](FetchResult res) {
                auto* m = new ImageMsg;
                m->url = url;
                if (res.success && !res.body.empty()) {
                    m->bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
                }
                PostMessageW(hwnd, WM_IMAGE_READY, 0, (LPARAM)m);
            });
            SetTimer(hwnd, TIMER_MAIN, 16, NULL);
        });

        g_renderer.Init(hwnd);
        g_renderer.SetPrefersDarkScheme(IsSystemDarkMode());

        // Start with one tab
        g_tabs.emplace_back();
        Navigate(std::string("vertex://home"));
        return 0;
    }

    case WM_SIZE: {
        UINT w = LOWORD(lp), h = HIWORD(lp);
        LayoutControls();
        g_renderer.Resize(w, h);
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DISPLAYCHANGE:
        FitFullscreenToCurrentMonitor();
        RefreshScreenDetails();
        return 0;

    case WM_INPUT: {
        if (!g_pointerLocked) break;
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        std::vector<BYTE> data(size);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, data.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(data.data());
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                LONG dx = raw->data.mouse.lLastX;
                LONG dy = raw->data.mouse.lLastY;
                if (dx || dy) {
                    g_pointerLockLastDelta = "dx=" + std::to_string(dx) + " dy=" + std::to_string(dy);
                    DispatchPlatformEventDetail("mousemove", g_pointerLockLastDelta);
                }
            }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        if (g_windowFullscreen) {
            FitFullscreenToCurrentMonitor();
        } else if (RECT* suggested = reinterpret_cast<RECT*>(lp)) {
            SetWindowPos(hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        g_renderer.SetPaintDirtyRect(ps.rcPaint);
        bool repaintChrome = !g_windowFullscreen && ps.rcPaint.top < TOP_INSET;

        auto tabs = BuildTabEntries();
        Tab& cur = CurTab();
        bool repaintForFragment = false;
        if (cur.page && cur.page->dom) {
            CurTab().docHeight = g_renderer.Paint(
                cur.page->dom, cur.scrollY, cur.page->url,
                (float)ChromeTopInset(), g_windowFullscreen ? 0.f : (float)TAB_H, &tabs, repaintChrome);
            if (cur.fragmentScrollPending) {
                cur.fragmentScrollPending = false;
                float anchorY = 0.f;
                if (!cur.pendingFragment.empty()
                    && g_renderer.GetAnchorY(cur.pendingFragment, anchorY)) {
                    cur.docHeight = FragmentReachableDocumentHeight(
                        cur.docHeight, anchorY, (float)ViewportH());
                    cur.scrollY = std::max(0.f, anchorY - 16.f);
                    ClampScroll();
                    repaintForFragment = true;
                }
            }
        } else {
            g_renderer.Paint(nullptr, 0.f, {},
                (float)ChromeTopInset(), g_windowFullscreen ? 0.f : (float)TAB_H, &tabs, repaintChrome);
        }
        if (repaintChrome) {
            DrawUrlFrame(ps.hdc);
            DrawLoadingAccent(ps.hdc);
        }
        if (g_findVisible && ps.rcPaint.bottom >= ContentPaintRect().bottom)
            DrawEditFrame(ps.hdc, g_hwndFind);
        UpdateScrollbar();
        UpdatePerfStatusMaybe();
        EndPaint(hwnd, &ps);
        if (repaintForFragment) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawChromeButton(dis);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, kChromePanel);
        return (LRESULT)g_toolbarBrush;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kChromeInk);
        SetBkColor(dc, kChromeActive);
        return (LRESULT)g_editBrush;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_hwndUrlBadge) {
            SetTextColor(dc, kChromeAccent);
            SetBkColor(dc, kChromeHover);
            return (LRESULT)g_editBrush;
        }
        SetTextColor(dc, kChromeQuiet);
        SetBkColor(dc, kChromeRail);
        return (LRESULT)g_statusBrush;
    }

    case WM_PAGE_READY: {
        auto* pm = reinterpret_cast<PageMsg*>(lp);
        int idx  = pm->tabIdx;
        Page* p  = pm->page;
        delete pm;

        if (idx >= 0 && idx < (int)g_tabs.size()) {
            Tab& tab   = g_tabs[idx];
            tab.page.reset(p);
            tab.scrollY = 0.f;
            tab.loading = false;
            if (p->dom) {
                tab.url = p->url;
                std::string title = ExtractTitle(p->dom.get());
                tab.title = title.empty() ? p->url : title;
            } else {
                std::string html = "<html><body><h2>Error</h2><p>"
                    + p->error + "</p></body></html>";
                tab.page->dom = ParseHtml(html);
                tab.title = "Error";
            }
        }

        if (idx == g_activeTab) {
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            SetUrlBarForTab(CurTab());
            UpdateTitle();
            g_renderer.InvalidateLayout();
            ClampScroll();
            UpdateScrollbar();

        }

        // Run <script> tags in the loaded page.
        // Pass 1: inline scripts and non-defer/async external scripts (blocking).
        // Pass 2: deferred scripts (after DOM is ready).
        if (idx >= 0 && idx < (int)g_tabs.size() && g_tabs[idx].page && g_tabs[idx].page->dom) {
            try {
                auto repaint = []() {
                    g_renderer.InvalidateLayout();
                    InvalidateContent();
                };
                DomBridgeCallbacks callbacks;
                callbacks.repaintOnly = []() {
                    InvalidateContent();
                };
                callbacks.navigate = [idx](const std::string& url, bool replace) {
                    if (idx != g_activeTab) return;
                    Navigate(g_activeTab, url, !replace);
                };
                callbacks.scrollTo = [idx](float, float y) {
                    if (idx != g_activeTab) return;
                    CurTab().scrollY = y;
                    ClampScroll();
                    UpdateScrollbar();
                    InvalidateContent();
                };
                callbacks.scrollBy = [idx](float, float dy) {
                    if (idx != g_activeTab) return;
                    CurTab().scrollY += dy;
                    ClampScroll();
                    UpdateScrollbar();
                    InvalidateContent();
                };
                callbacks.getCanvasSurface = [](Node* n) {
                    return g_renderer.GetOrCreateCanvasSurface(n);
                };
                callbacks.mediaPlay = [](Node* n) { return g_renderer.MediaPlay(n); };
                callbacks.mediaPause = [](Node* n) { g_renderer.MediaPause(n); };
                callbacks.mediaSetCurrentTime = [](Node* n, double v) { g_renderer.MediaSetCurrentTime(n, v); };
                callbacks.mediaCurrentTime = [](Node* n) { return g_renderer.MediaCurrentTime(n); };
                callbacks.mediaDuration = [](Node* n) { return g_renderer.MediaDuration(n); };
                callbacks.mediaSetVolume = [](Node* n, double v) { g_renderer.MediaSetVolume(n, v); };
                callbacks.mediaVolume = [](Node* n) { return g_renderer.MediaVolume(n); };
                callbacks.mediaSetMuted = [](Node* n, bool v) { g_renderer.MediaSetMuted(n, v); };
                callbacks.mediaMuted = [](Node* n) { return g_renderer.MediaMuted(n); };
                callbacks.mediaPaused = [](Node* n) { return g_renderer.MediaPaused(n); };
                callbacks.scrollIntoView = [idx](Node* target) {
                    if (idx != g_activeTab || !target) return;
                    const LayoutBox* root = g_renderer.GetLayoutRoot();
                    if (!root) return;
                    std::function<bool(const LayoutBox*, float&)> findBox =
                        [&](const LayoutBox* box, float& y) -> bool {
                            if (!box) return false;
                            if (box->node == target) {
                                y = box->y;
                                return true;
                            }
                            for (const auto& child : box->kids)
                                if (findBox(child.get(), y)) return true;
                            for (const auto& line : box->lines) {
                                for (const auto& frag : line.frags) {
                                    if (frag.src && frag.src->node == target) {
                                        y = frag.y;
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };
                    float y = 0.f;
                    if (!findBox(root, y)) return;
                    CurTab().scrollY = std::max(0.f, y - 16.f);
                    ClampScroll();
                    UpdateScrollbar();
                    InvalidateContent();
                };
                g_js.setDocument(g_tabs[idx].page->dom, repaint, g_tabs[idx].page->url, std::move(callbacks));
                struct ScriptEntry { std::string source; std::string filename; bool fetchBeforeRun = false; };
                std::vector<ScriptEntry> deferred;
                std::vector<ScriptEntry> blocking;
                const std::string pageUrl = g_tabs[idx].page->url;
                ClearPendingPageScriptsForTab(idx);
                std::vector<const Node*> stack;
                stack.push_back(g_tabs[idx].page->dom.get());
                size_t scriptCount = 0;
                size_t totalScriptBytes = 0;
                while (!stack.empty()) {
                    const Node* n = stack.back();
                    stack.pop_back();
                    if (!n) continue;
                    if (n->type == NodeType::Element && n->tagName == "script") {
                        std::string type = n->attr("type");
                        bool skip = (!type.empty() && type != "text/javascript"
                                     && type != "application/javascript" && type != "module");
                        bool isDefer = !n->attr("defer").empty() || !n->attr("async").empty();
                        std::string source;
                        std::string srcUrl = n->attr("src");
                        std::string filename = "inline";
                        std::string preloadedFilename = n->attr("__vertex_script_filename");
                        if (!preloadedFilename.empty() && !skip) {
                            filename = preloadedFilename;
                            for (auto& c : n->children)
                                if (c->type == NodeType::Text) source += c->text;
                        } else if (!srcUrl.empty() && !skip) {
                            std::string resolved = ResolveUrlAgainstBase(srcUrl, g_tabs[idx].page->url);
                            filename = resolved;
                        } else {
                            for (auto& c : n->children)
                                if (c->type == NodeType::Text) source += c->text;
                        }
                        const bool fetchBeforeRun = !srcUrl.empty() && preloadedFilename.empty() && !skip;
                        totalScriptBytes += source.size();
                        if (!skip && (!source.empty() || fetchBeforeRun) && scriptCount < 192 && totalScriptBytes <= 2 * 1024 * 1024) {
                            if (isDefer && !srcUrl.empty())
                                deferred.push_back({ source, filename, fetchBeforeRun });
                            else
                                blocking.push_back({ source, filename, fetchBeforeRun });
                        }
                        ++scriptCount;
                    }
                    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                        stack.push_back(it->get());
                }
                for (auto& script : blocking)
                    g_pendingPageScripts.push_back({ idx, pageUrl, std::move(script.source), std::move(script.filename), false, script.fetchBeforeRun });
                for (auto& script : deferred)
                    g_pendingPageScripts.push_back({ idx, pageUrl, std::move(script.source), std::move(script.filename), false, script.fetchBeforeRun });
                g_pendingPageScripts.push_back({ idx, pageUrl, {}, "__vertex_load_events__", true, false });
                // Set up timer for macrotasks / setTimeout
                SetTimer(hwnd, TIMER_MAIN, 16, NULL);
            } catch (...) {
                OutputDebugStringA("[JS] Page script setup failed; continuing without page scripts\n");
            }
        }

        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_IMAGE_READY: {
        auto* m = reinterpret_cast<ImageMsg*>(lp);
        g_renderer.ReceiveImage(m->url, m->bytes);
        delete m;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP:     CurTab().scrollY -= 30.f;                    break;
        case SB_LINEDOWN:   CurTab().scrollY += 30.f;                    break;
        case SB_PAGEUP:     CurTab().scrollY -= (float)si.nPage;         break;
        case SB_PAGEDOWN:   CurTab().scrollY += (float)si.nPage;         break;
        case SB_THUMBTRACK: CurTab().scrollY  = (float)si.nTrackPos;     break;
        }
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        float delta = GET_WHEEL_DELTA_WPARAM(wp) * 0.5f;
        // Check if cursor is inside a scrollable container.
        bool scrolledElement = false;
        for (auto& sr : g_scrollables) {
            if (pt.x >= sr.x && pt.x <= sr.x + sr.w && pt.y >= sr.y && pt.y <= sr.y + sr.h) {
                float maxScroll = std::max(0.f, sr.contentH - sr.h);
                float& elScroll = g_elementScrollY[sr.node];
                elScroll = std::max(0.f, std::min(elScroll - delta, maxScroll));
                scrolledElement = true;
                break;
            }
        }
        if (!scrolledElement) {
            CurTab().scrollY -= delta;
            ClampScroll();
            UpdateScrollbar();
        }
        InvalidateContent();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        // Check tab strip
        if (py < TAB_H) {
            if (g_windowFullscreen) return 0;
            int closeIdx = -1;
            if (g_renderer.HitTestTabClose((float)px, (float)py, closeIdx)) {
                CloseTab(closeIdx);
            } else {
                int tidx = g_renderer.HitTestTab((float)px, (float)py);
                if (tidx == -1) {
                    NewTab();
                } else if (tidx >= 0) {
                    SwitchTab(tidx);
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (g_activePopover && py >= ChromeTopInset()) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            if (href.empty()) HidePlatformPopover();
        }
        if (py >= ChromeTopInset()) {
            // Check if an input was clicked.
            if (g_renderer.GetLayoutRoot()) {
                Node* input = FormState::hitTestInput(*g_renderer.GetLayoutRoot(),
                    (float)px, (float)py, CurTab().scrollY, (float)ChromeTopInset());
                if (input) {
                    g_formState.focus(input);
                    InvalidateContent();
                    return 0;
                }
                g_formState.blur();
            }
            std::string href = g_renderer.HitTest((float)px, (float)py);
            if (!href.empty()) {
                if (g_renderer.LastHitWasDownload())
                    StartDownload(href, g_renderer.LastHitDownloadName());
                else
                    Navigate(href);
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py >= ChromeTopInset()) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            HMENU menu = CreatePopupMenu();
            if (!href.empty()) {
                AppendMenuW(menu, MF_STRING, 9001, L"Open Link");
                AppendMenuW(menu, MF_STRING, 9002, L"Open Link in New Tab");
                AppendMenuW(menu, MF_STRING, 9003, L"Copy Link");
                AppendMenuW(menu, MF_STRING, 9004, L"Save Link As");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            }
            AppendMenuW(menu, MF_STRING, 9010, L"Back");
            AppendMenuW(menu, MF_STRING, 9011, L"Forward");
            AppendMenuW(menu, MF_STRING, 9012, L"Reload");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, 9020, L"Add Bookmark");
            AppendMenuW(menu, MF_STRING, 9021, L"View Bookmarks");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, 9030, L"Toggle Fullscreen");
            AppendMenuW(menu, MF_STRING, 9031, L"Pointer Lock");
            AppendMenuW(menu, MF_STRING, 9032, L"Picture in Picture");
            AppendMenuW(menu, MF_STRING, 9033, L"Show File Picker");
            AppendMenuW(menu, MF_STRING, 9034, L"Test Notification");
            AppendMenuW(menu, MF_STRING, 9035, L"Share Page");
            AppendMenuW(menu, MF_STRING, 9036, L"Save File Picker");
            AppendMenuW(menu, MF_STRING, 9037, L"Toggle Wake Lock");
            AppendMenuW(menu, MF_STRING, 9038, L"Print Page");
            AppendMenuW(menu, MF_STRING, 9039, L"Platform Features");
            AppendMenuW(menu, MF_STRING, 9040, L"Geolocation");
            AppendMenuW(menu, MF_STRING, 9041, L"Battery");
            AppendMenuW(menu, MF_STRING, 9042, L"Sample Sensors");
            AppendMenuW(menu, MF_STRING, 9043, L"Vibrate");
            AppendMenuW(menu, MF_STRING, 9044, L"Contacts");
            AppendMenuW(menu, MF_STRING, 9045, L"Storage Estimate");
            POINT pt = {px, py};
            ClientToScreen(hwnd, &pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            switch (cmd) {
                case 9001: Navigate(href); break;
                case 9002: NewTab(href); break;
                case 9003: {
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, href.size() + 1);
                        if (h) { memcpy(GlobalLock(h), href.c_str(), href.size() + 1); GlobalUnlock(h);
                            SetClipboardData(CF_TEXT, h); }
                        CloseClipboard();
                    }
                    break;
                }
                case 9004: StartDownload(href); break;
                case 9010: GoBack(); break;
                case 9011: GoForward(); break;
                case 9012: if (!CurTab().loading) Navigate(CurTab().url, false); break;
                case 9020: {
                    AppendBookmarkRecord(CurTab().url, CurTab().title);
                    SetStatus("Bookmarked " + CurTab().url);
                    break;
                }
                case 9021: {
                    ShowBookmarksPage();
                    break;
                }
                case 9030: ToggleWindowFullscreen(); break;
                case 9031: EnterPointerLock(nullptr); break;
                case 9032: EnterPictureInPicture(nullptr); break;
                case 9033: {
                    auto files = ShowOpenFilePicker(true);
                    SetStatus(files.empty() ? "No file selected" : ("Selected " + std::to_string(files.size()) + " file(s)"));
                    break;
                }
                case 9034: ShowPlatformNotification("Vertex", "Notification API platform path"); break;
                case 9035: SharePlatformPayload({ CurTab().title, "Shared from Vertex", CurTab().url, {} }); break;
                case 9036: ShowSaveFilePicker(); break;
                case 9037: if (g_wakeLockActive) ReleaseWakeLock(); else RequestWakeLock("screen"); break;
                case 9038: PrintCurrentPage(); break;
                case 9039: ShowPlatformFeaturesPage(); break;
                case 9040: QueryGeolocation(); break;
                case 9041: QueryBattery(); break;
                case 9042: SampleSyntheticSensors(); break;
                case 9043: VibratePattern({ 80, 40, 80 }); break;
                case 9044: SelectContacts(); break;
                case 9045: EstimateStorage(); break;
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (g_windowFullscreen) ArmFullscreenCursorTimer();
        if (py >= ChromeTopInset()) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            int cursor = g_renderer.CursorAt((float)px, (float)py,
                CurTab().scrollY, (float)ChromeTopInset());
            SetBrowserCursor(CursorFromCss(cursor));
            SetStatus(href);
            // Track :hover node for CSS hover styles (throttled adaptively based on page complexity).
            if (g_renderer.GetLayoutRoot() && g_renderer.UsesHoverStyles()) {
                static DWORD lastHoverTick = 0;
                DWORD now = GetTickCount();
                // Adaptive throttle: 33ms (30Hz) for simple pages, 50ms (20Hz) for complex ones
                size_t candidateCount = g_renderer.HoverCandidateCount();
                DWORD throttleMs = (candidateCount > 500) ? 50 : 33;
                if (now - lastHoverTick >= throttleMs) {
                    HitRegion oldHoverRegion{};
                    bool hadOldHoverRegion = g_renderer.LastHoverRegion(oldHoverRegion);
                    const Node* hover = g_renderer.HoverNodeAt(
                            (float)px, (float)py, CurTab().scrollY, (float)ChromeTopInset());
                    lastHoverTick = now;
                    if (hover != g_hoverNode) {
                        HitRegion newHoverRegion{};
                        bool hasNewHoverRegion = g_renderer.LastHoverRegion(newHoverRegion);
                        g_hoverNode = hover;
                        // Only invalidate paint, not layout — hover mostly affects
                        // colors/opacity, not geometry. Full layout rebuild is expensive.
                        InvalidateHoverRegions(
                            hadOldHoverRegion ? &oldHoverRegion : nullptr,
                            hasNewHoverRegion ? &newHoverRegion : nullptr);
                    }
                }
            } else if (g_hoverNode) {
                g_hoverNode = nullptr;
                InvalidateContent();
            }
        } else {
            SetBrowserCursor(g_cursorArrow);
            SetStatus({});
            if (g_hoverNode) {
                g_hoverNode = nullptr;
                InvalidateContent();
            }
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BACK: GoBack();    break;
        case IDC_FWRD: GoForward(); break;
        case IDC_HOME: Navigate("vertex://home"); break;
        case IDC_REFR:
            if (CurTab().page) Navigate(CurTab().url, false);
            break;
        case IDC_STOP:
            CurTab().loading = false;
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_FULLSCREEN_CURSOR) {
            KillTimer(hwnd, TIMER_FULLSCREEN_CURSOR);
            if (g_windowFullscreen) ShowFullscreenCursor(false);
            return 0;
        }
        if (wp == TIMER_GAMEPAD) {
            PollGamepads();
            return 0;
        }
        if (wp == TIMER_MEDIA_SESSION) {
            if (!g_mediaSessionAction.empty())
                SetStatus("Media: " + g_mediaSessionAction);
            PollIdleDetector();
            return 0;
        }
        resetDomDirtyCoalesce(); // Allow next batch of DOM mutations to trigger repaint.
        if (DrainResourceCompletions(kMaxResourceCompletionsPerTimerTick) > 0) {
            InvalidateContent();
        }
        DrainWebSocketEvents(kMaxWebSocketEventsPerTimerTick);
        RunPendingPageScripts(hwnd);
        try {
            g_js.runMacrotasks(kMaxMacrotasksPerTimerTick);
        } catch (...) {
            OutputDebugStringA("[JS] Macrotask pump failed; timer stopped\n");
            KillTimer(hwnd, TIMER_MAIN);
        }
        if (AnyTabLoading()) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            rc.bottom = ChromeTopInset();
            InvalidateRect(hwnd, &rc, FALSE);
        } else if (g_pendingPageScripts.empty() && !g_js.hasPendingMacrotasks()
                   && !HasPendingResourceCompletions() && !HasOpenWebSockets()) {
            KillTimer(hwnd, TIMER_MAIN);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ApplyThemedWindowIcon(hwnd);
        FitFullscreenToCurrentMonitor();
        return 0;

    case WM_ACTIVATEAPP:
        if (!wp && g_windowFullscreen)
            ShowFullscreenCursor(true);
        if (!wp && g_pointerLocked)
            ExitPointerLock(false);
        if (wp && g_windowFullscreen)
            FitFullscreenToCurrentMonitor();
        if (wp && !g_notificationRecords.empty()) {
            auto it = g_notificationRecords.rbegin();
            it->second.clicked = true;
            DispatchPlatformEventDetail("notificationclick", std::to_string(it->second.id) + " " + it->second.title);
        }
        return 0;

    case WM_DROPFILES:
        BeginFileDrag((HDROP)wp);
        EnqueueLaunchFiles(g_dragFiles);
        return 0;

    case WM_APPCOMMAND:
        HandleMediaCommand(wp, lp);
        return 0;

    case WM_PIP_CLOSED:
        DispatchPlatformEvent("leavepictureinpicture");
        return 0;

    case WM_GETMINMAXINFO:
        if (g_windowFullscreen) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            MONITORINFO mi = MonitorInfoForWindow(hwnd);
            mmi->ptMaxPosition.x = mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcMonitor.right - mi.rcMonitor.left;
            mmi->ptMaxSize.y = mi.rcMonitor.bottom - mi.rcMonitor.top;
            return 0;
        }
        break;

    case WM_DESTROY:
        DragAcceptFiles(hwnd, FALSE);
        ExitPointerLock(false);
        ExitPictureInPicture();
        if (g_xinputModule) { FreeLibrary(g_xinputModule); g_xinputModule = nullptr; }
        if (g_uiFont) { DeleteObject(g_uiFont); g_uiFont = nullptr; }
        if (g_iconFont) { DeleteObject(g_iconFont); g_iconFont = nullptr; }
        if (g_urlFont) { DeleteObject(g_urlFont); g_urlFont = nullptr; }
        if (g_toolbarBrush) { DeleteObject(g_toolbarBrush); g_toolbarBrush = nullptr; }
        if (g_statusBrush) { DeleteObject(g_statusBrush); g_statusBrush = nullptr; }
        if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
        if (g_windowBrush) { DeleteObject(g_windowBrush); g_windowBrush = nullptr; }
        if (g_appIconLarge) { DestroyIcon(g_appIconLarge); g_appIconLarge = nullptr; }
        if (g_appIconSmall) { DestroyIcon(g_appIconSmall); g_appIconSmall = nullptr; }
        g_appIconResourceId = 0;
        PostQuitMessage(0);
        return 0;
    }

    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && g_windowFullscreen) {
        ExitWindowFullscreen();
        return 0;
    }

    // Form input keyboard handling: route chars to focused input.
    if (g_formState.focusedInput) {
        if (msg == WM_CHAR) {
            if (wp == '\r') {
                // Enter in a form input: submit the form via GET.
                std::string url = g_formState.buildFormQuery();
                if (!url.empty()) {
                    g_formState.blur();
                    Navigate(url);
                }
            } else if (wp == '\b') {
                g_formState.backspace();
                InvalidateContent();
            } else if (wp >= 32) {
                g_formState.insertChar((char)wp);
                InvalidateContent();
            }
            return 0;
        }
        if (msg == WM_KEYDOWN) {
            if (wp == VK_LEFT && g_formState.cursorPos > 0) {
                g_formState.cursorPos--;
                InvalidateContent();
                return 0;
            }
            if (wp == VK_RIGHT) {
                std::string v = g_formState.getValue(g_formState.focusedInput);
                if (g_formState.cursorPos < v.size()) g_formState.cursorPos++;
                InvalidateContent();
                return 0;
            }
            if (wp == VK_DELETE) {
                g_formState.deleteChar();
                InvalidateContent();
                return 0;
            }
            if (wp == VK_HOME) { g_formState.cursorPos = 0; InvalidateContent(); return 0; }
            if (wp == VK_END) {
                g_formState.cursorPos = g_formState.getValue(g_formState.focusedInput).size();
                InvalidateContent(); return 0;
            }
            if (wp == VK_ESCAPE) { g_formState.blur(); InvalidateContent(); return 0; }
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── entry point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetCurrentProcessExplicitAppUserModelID(L"hackclubium.Vertex");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_profilePaths = vertex::profile::DefaultPaths();
    vertex::profile::EnsureDirectories(g_profilePaths);
    LoadDownloadRecords();

    // Auto-update: apply a previously downloaded update, then check for new ones.
    char exeBuf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
    std::string exePath(exeBuf);
    Updater::applyPendingUpdate(exePath);
    g_updater.onStatusChanged = []() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); };
    g_updater.checkForUpdateAsync(exePath);

    WNDCLASSEXW wc   = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"VertexBrowser";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    g_windowBrush = CreateSolidBrush(kChromePanel);
    wc.hbrBackground = g_windowBrush;
    LoadThemedAppIcons(hInst);
    wc.hIcon         = g_appIconLarge;
    wc.hIconSm       = g_appIconSmall;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0,
        L"VertexBrowser", L"Vertex",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl    = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt     = (GetKeyState(VK_MENU)    & 0x8000) != 0;
            bool shift   = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool handled = false;

            if (ctrl && (!shift || msg.wParam == VK_TAB)) {
                if (msg.wParam == 'L') {
                    SetFocus(g_hwndUrl);
                    SendMessageW(g_hwndUrl, EM_SETSEL, 0, -1);
                    handled = true;
                } else if (msg.wParam == 'T') {
                    NewTab();
                    handled = true;
                } else if (msg.wParam == 'W') {
                    CloseTab(g_activeTab);
                    handled = true;
                } else if (msg.wParam == VK_TAB) {
                    int next = (g_activeTab + (shift ? -1 : 1) + (int)g_tabs.size())
                             % (int)g_tabs.size();
                    SwitchTab(next);
                    handled = true;
                } else if (msg.wParam >= '1' && msg.wParam <= '9') {
                    SwitchTab((int)(msg.wParam - '1'));
                    handled = true;
                } else if (msg.wParam == 'R' || msg.wParam == VK_F5) {
                    if (!CurTab().loading) Navigate(CurTab().url, false);
                    handled = true;
                } else if (msg.wParam == 'H') {
                    Navigate("vertex://history");
                    handled = true;
                } else if (msg.wParam == 'J') {
                    Navigate("vertex://downloads");
                    handled = true;
                } else if (msg.wParam == 'B') {
                    Navigate("vertex://bookmarks");
                    handled = true;
                } else if (msg.wParam == 'F') {
                    ShowFind(!g_findVisible);
                    handled = true;
                } else if (msg.wParam == 'G') {
                    FindNextInPage(shift);
                    handled = true;
                } else if (msg.wParam == VK_OEM_PLUS || msg.wParam == '=') {
                    g_renderer.SetZoom(g_renderer.GetZoom() + 0.1f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                } else if (msg.wParam == VK_OEM_MINUS) {
                    g_renderer.SetZoom(g_renderer.GetZoom() - 0.1f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                } else if (msg.wParam == '0') {
                    g_renderer.SetZoom(1.f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                }
            }

            if (!handled && alt && msg.wParam == VK_RETURN) {
                ToggleWindowFullscreen();
                handled = true;
            }
            if (!handled && msg.wParam == VK_F11) {
                ToggleWindowFullscreen();
                handled = true;
            }
            if (!handled && msg.wParam == VK_F5) {
                if (!CurTab().loading) Navigate(CurTab().url, false);
                handled = true;
            }
            if (!handled && msg.wParam == VK_F12 && g_updater.updateAvailable) {
                char exeBuf[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
                Updater::restartToUpdate(exeBuf);
                handled = true;
            }
            if (!handled && alt) {
                if (msg.wParam == VK_LEFT)  { GoBack();    handled = true; }
                if (msg.wParam == VK_RIGHT) { GoForward(); handled = true; }
            }
            if (!handled && ctrl && shift) {
                if (msg.wParam == 'P') {
                    EnterPointerLock(nullptr);
                    handled = true;
                } else if (msg.wParam == 'I') {
                    EnterPictureInPicture(nullptr);
                    handled = true;
                } else if (msg.wParam == 'O') {
                    auto files = ShowOpenFilePicker(true);
                    SetStatus(files.empty() ? "No file selected" : ("Selected " + std::to_string(files.size()) + " file(s)"));
                    handled = true;
                } else if (msg.wParam == 'N') {
                    ShowPlatformNotification("Vertex", "Notification API keyboard path");
                    handled = true;
                } else if (msg.wParam == 'C') {
                    WriteClipboardHtml("<b>Vertex rich clipboard</b>");
                    handled = true;
                } else if (msg.wParam == 'V') {
                    SetStatus("Clipboard: " + ReadClipboardText());
                    handled = true;
                } else if (msg.wParam == 'D') {
                    ShowModalDialog(CurTab().page && CurTab().page->dom ? CurTab().page->dom.get() : nullptr);
                    handled = true;
                } else if (msg.wParam == 'Y') {
                    ShowPlatformPopover(CurTab().page && CurTab().page->dom ? CurTab().page->dom.get() : nullptr);
                    handled = true;
                } else if (msg.wParam == 'G') {
                    PollGamepads();
                    handled = true;
                } else if (msg.wParam == 'S') {
                    SharePlatformPayload({ CurTab().title, "Shared from Vertex", CurTab().url, {} });
                    handled = true;
                } else if (msg.wParam == 'A') {
                    SetAppBadge(g_appBadge + 1);
                    handled = true;
                } else if (msg.wParam == 'W') {
                    if (g_wakeLockActive) ReleaseWakeLock(); else RequestWakeLock("screen");
                    handled = true;
                } else if (msg.wParam == 'X') {
                    ShowSaveFilePicker();
                    handled = true;
                } else if (msg.wParam == 'R') {
                    RefreshScreenDetails();
                    handled = true;
                } else if (msg.wParam == 'M') {
                    ShowPlatformFeaturesPage();
                    handled = true;
                } else if (msg.wParam == 'Q') {
                    PrintCurrentPage();
                    handled = true;
                } else if (msg.wParam == 'E') {
                    QueryGeolocation();
                    handled = true;
                } else if (msg.wParam == 'K') {
                    QueryBattery();
                    handled = true;
                } else if (msg.wParam == 'U') {
                    SampleSyntheticSensors();
                    handled = true;
                } else if (msg.wParam == 'Z') {
                    VibratePattern({ 80, 40, 80, 40, 160 });
                    handled = true;
                } else if (msg.wParam == 'T') {
                    SelectContacts();
                    handled = true;
                } else if (msg.wParam == 'L') {
                    StartIdleDetector();
                    handled = true;
                } else if (msg.wParam == 'H') {
                    SeedExternalDevices();
                    handled = true;
                } else if (msg.wParam == 'J') {
                    EstimateStorage();
                    handled = true;
                }
            }
            if (!handled && msg.wParam == VK_ESCAPE) {
                if (g_windowFullscreen) {
                    ExitWindowFullscreen();
                    handled = true;
                } else if (g_pointerLocked || g_activePopover || g_modalDialogActive || g_pipActive) {
                    DismissTransientTopLayer();
                    handled = true;
                } else if (g_findVisible) {
                    ShowFind(false);
                    handled = true;
                } else if (CurTab().loading) {
                    CurTab().loading = false;
                    CurTab().title   = CurTab().url;
                    EnableWindow(g_hwndStop, FALSE);
                    EnableWindow(g_hwndRefr, TRUE);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                }
            }

            if (handled) continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
