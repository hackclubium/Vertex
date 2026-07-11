#pragma once

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vertex::platform_features {

struct FeatureEvent {
    std::string type;
    std::string detail;
    long long tick = 0;
};

struct State {
    std::string os;
    bool fullscreen = false;
    bool pointerLock = false;
    bool pictureInPicture = false;
    bool modalDialog = false;
    bool popover = false;
    bool dragActive = false;
    bool wakeLock = false;
    bool idleDetector = false;
    bool userIdle = false;
    int appBadge = 0;
    double geoLat = 37.7749;
    double geoLon = -122.4194;
    double batteryLevel = 1.0;
    bool batteryCharging = true;
    std::string clipboardText;
    std::string clipboardHtml;
    std::string lastShareTitle;
    std::string lastPickedFiles;
    std::string lastSavedFile;
    std::string mediaSessionAction;
    std::vector<std::string> dragFiles;
    std::vector<std::string> contacts { "Ada Lovelace", "Grace Hopper", "Katherine Johnson" };
    std::vector<std::string> devices { "serial:debug", "hid:test", "usb:test", "bluetooth:test" };
    std::map<std::string, std::string> permissions;
    std::vector<FeatureEvent> events;
};

inline long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void Event(State& s, const std::string& type, const std::string& detail = {}) {
    s.events.push_back({ type, detail, NowMs() });
    if (s.events.size() > 256) s.events.erase(s.events.begin(), s.events.begin() + (s.events.size() - 256));
}

inline void Seed(State& s, const std::string& os) {
    s.os = os;
    s.permissions = {
        { "fullscreen", "granted" }, { "pointer-lock", "granted" }, { "clipboard-read", "granted" },
        { "clipboard-write", "granted" }, { "notifications", "granted" }, { "geolocation", "granted" },
        { "gamepad", "granted" }, { "sensors", "granted" }, { "contacts", "granted" },
        { "serial", "granted" }, { "hid", "granted" }, { "usb", "granted" }, { "bluetooth", "prompt" },
        { "camera", "prompt" }, { "microphone", "prompt" }
    };
    Event(s, "platforminit", os);
}

inline std::string Escape(const std::string& input) {
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

inline std::string Join(const std::vector<std::string>& values, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += sep;
        out += values[i];
    }
    return out;
}

inline void TouchAll(State& s) {
    s.pointerLock = !s.pointerLock;
    s.pictureInPicture = !s.pictureInPicture;
    s.modalDialog = !s.modalDialog;
    s.popover = !s.popover;
    s.wakeLock = !s.wakeLock;
    s.idleDetector = true;
    s.appBadge++;
    s.clipboardText = "Vertex cross-platform clipboard text";
    s.clipboardHtml = "<b>Vertex cross-platform clipboard HTML</b>";
    s.lastShareTitle = "Shared from Vertex";
    s.lastPickedFiles = "example.html | notes.txt";
    s.lastSavedFile = "vertex-save.txt";
    s.mediaSessionAction = "playpause";
    s.dragFiles = { "dropped-a.txt", "dropped-b.png" };
    Event(s, "touchall", "updated cross-platform feature state");
}

inline std::string PageHtml(const State& s, const std::string& css) {
    std::ostringstream html;
    auto item = [&](const std::string& name, const std::string& value) {
        html << "<div class=\"item\"><div class=\"name\">" << Escape(name)
             << "</div><div class=\"meta\">" << Escape(value) << "</div></div>";
    };
    html << "<html><head><title>Platform Features</title><style>" << css
         << "</style></head><body><main><h1>Platform Features</h1>";
    item("OS", s.os);
    item("Fullscreen", s.fullscreen ? "active" : "inactive");
    item("Pointer lock", s.pointerLock ? "active" : "inactive");
    item("Picture in Picture", s.pictureInPicture ? "active" : "inactive");
    item("Modal dialog", s.modalDialog ? "active" : "inactive");
    item("Popover", s.popover ? "active" : "inactive");
    item("Drag files", s.dragFiles.empty() ? "none" : Join(s.dragFiles, " | "));
    item("Clipboard text", s.clipboardText.empty() ? "empty" : s.clipboardText);
    item("Clipboard HTML", s.clipboardHtml.empty() ? "empty" : s.clipboardHtml);
    item("Share", s.lastShareTitle.empty() ? "none" : s.lastShareTitle);
    item("File picker", s.lastPickedFiles.empty() ? "none" : s.lastPickedFiles);
    item("Save picker", s.lastSavedFile.empty() ? "none" : s.lastSavedFile);
    item("App badge", std::to_string(s.appBadge));
    item("Wake lock", s.wakeLock ? "active" : "inactive");
    item("Idle detector", s.idleDetector ? (s.userIdle ? "idle" : "active") : "inactive");
    item("Geolocation", std::to_string(s.geoLat) + ", " + std::to_string(s.geoLon));
    item("Battery", std::to_string((int)(s.batteryLevel * 100)) + "% " + (s.batteryCharging ? "charging" : "discharging"));
    item("Contacts", Join(s.contacts, " | "));
    item("Devices", Join(s.devices, " | "));
    item("Media session", s.mediaSessionAction.empty() ? "none" : s.mediaSessionAction);
    for (const auto& [name, state] : s.permissions) item("Permission " + name, state);
    size_t start = s.events.size() > 20 ? s.events.size() - 20 : 0;
    for (size_t i = start; i < s.events.size(); ++i) item("Event " + std::to_string(i), s.events[i].type + " " + s.events[i].detail);
    html << "</main></body></html>";
    return html.str();
}

} // namespace vertex::platform_features
