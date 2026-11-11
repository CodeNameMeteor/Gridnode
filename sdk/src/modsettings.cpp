// Declared mod settings (gn_api setting_*, API 17): a mod declares each setting once (kind, default,
// range, label, description); the value lives in the mod's settings file (config.cpp) as text, with
// the label and description written above it, and GridNode draws a page of them for every mod in its
// menu (the mod can hide it). A mod without any UI code of its own still gets that page; without
// GridNode's menu the file is the way to change them.
//
// Locks: this file's lock is never held while calling config.cpp (whose writer calls back here for
// the comments with its own lock held).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "gridnode/mod_call.hpp"
#include "gridnode/config.hpp"
#include "gridnode/log.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host {
namespace {

struct Declared {
    config::Decl d;
    uintptr_t owner = 0; // an address in the code that declared it (taken out when that mod is unloaded)
};
struct ModPage {
    std::vector<Declared> settings; // in declaration order
    bool shown = true;
};
struct Listener {
    std::string mod, id; // id empty: any of the mod's settings
    gn_setting_fn fn;
    void* user;
};

std::mutex g_m;
std::map<std::string, ModPage> g_pages; // (g_m) by mod name
std::vector<Listener> g_listeners;      // (g_m)
// (g_m) Notifications queued and not yet run, by mod, id, fn and user: a slider being dragged sets the
// value every frame, and the listener needs to hear once per tick, not once per frame (a drag logged
// each value five times over, 2026-11-03). It reads the value when it runs, so it gets the latest.
std::vector<std::tuple<std::string, std::string, gn_setting_fn, void*>> g_pending;
// (g_m) The file comments of each mod's settings, kept after it is unloaded: its file is written once
// more then, and should keep its descriptions.
std::map<std::string, std::map<std::string, std::string>> g_comments;

// (g_m) A copy of the declaration, or false.
bool declOf(const std::string& mod, const std::string& id, config::Decl& out) {
    auto it = g_pages.find(mod);
    if (it == g_pages.end()) return false;
    for (const Declared& s : it->second.settings)
        if (s.d.id == id) {
            out = s.d;
            return true;
        }
    return false;
}

bool lookup(const char* mod, const char* id, config::Decl& out) {
    if (!mod || !id) return false;
    std::lock_guard<std::mutex> lock(g_m);
    return declOf(mod, id, out);
}

// The change, to the mod's listeners on the client thread (as hotkeys run); skipped when the
// listener went meanwhile (its mod unloaded).
struct Notify {
    std::string mod, id;
    gn_setting_fn fn;
    void* user;
    void operator()() const {
        {
            std::lock_guard<std::mutex> lock(g_m);
            const auto me = std::make_tuple(mod, id, fn, user);
            g_pending.erase(std::remove(g_pending.begin(), g_pending.end(), me), g_pending.end());
            if (std::none_of(g_listeners.begin(), g_listeners.end(), [this](const Listener& l) { return l.fn == fn && l.user == user; })) return;
        }
        modcall::Scope scope(fn);
        fn(id.c_str(), user);
    }
};

void changed(const std::string& mod, const std::string& id) {
    std::vector<Notify> calls;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Listener& l : g_listeners) {
            if (l.mod != mod || !(l.id.empty() || l.id == id)) continue;
            const auto key = std::make_tuple(mod, id, l.fn, l.user);
            if (std::find(g_pending.begin(), g_pending.end(), key) != g_pending.end()) continue; // one is on its way
            g_pending.push_back(key);
            calls.push_back({mod, id, l.fn, l.user});
        }
    }
    for (Notify& n : calls) client().post(std::move(n));
}

double current(const std::string& mod, const config::Decl& d) {
    bool found = false;
    const std::string v = configValue(mod, d.id, &found);
    return found ? config::parseNumber(d, v) : d.def;
}

std::string currentText(const std::string& mod, const config::Decl& d) {
    bool found = false;
    const std::string v = configValue(mod, d.id, &found);
    return found ? v : d.defText;
}

} // namespace

int settingAdd(const char* mod, const gn_setting* s, const void* caller) {
    if (!mod || !s || s->size < sizeof(gn_setting) || !s->id || !config::validKey(s->id)) return GN_ERR_ARGUMENT;
    config::Decl d;
    d.id = s->id;
    d.label = s->label ? s->label : "";
    d.description = s->description ? s->description : "";
    d.group = s->group ? s->group : "";
    d.kind = static_cast<config::Kind>(s->kind);
    d.def = s->def, d.min = s->min, d.max = s->max, d.step = s->step;
    for (const char* const* c = s->choices; c && *c; ++c) d.choices.push_back(*c);
    d.defText = s->def_text ? config::cleanValue(s->def_text) : "";
    d.flags = s->flags;
    if (!config::normalize(d)) {
        GN_LOG("[%s] setting %s: unknown kind %d (or a choice without choices)", mod, s->id, s->kind);
        return GN_ERR_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(g_m);
        ModPage& page = g_pages[mod];
        auto it = std::find_if(page.settings.begin(), page.settings.end(), [&](const Declared& x) { return x.d.id == d.id; });
        if (it != page.settings.end()) *it = {d, reinterpret_cast<uintptr_t>(caller)}; // declared again (a reload): the new one
        else page.settings.push_back({d, reinterpret_cast<uintptr_t>(caller)});
        g_comments[mod][d.id] = config::comment(d);
    }
    // Not in the file yet: the default goes in, so the file lists every setting with its description.
    bool found = false;
    const std::string v = configValue(mod, d.id, &found);
    if (!found) configSet(mod, d.id.c_str(), d.kind == config::Kind::Text ? d.defText.c_str() : config::format(d, d.def).c_str());
    else if (d.kind != config::Kind::Text && config::format(d, config::parseNumber(d, v)) != v)
        configSet(mod, d.id.c_str(), config::format(d, config::parseNumber(d, v)).c_str()); // out of range or not a number: put right
    return GN_OK;
}

double settingNumber(const char* mod, const char* id) {
    config::Decl d;
    if (!lookup(mod, id, d) || d.kind == config::Kind::Text) return std::nan("");
    return current(mod, d);
}

int settingText(const char* mod, const char* id, char* out, size_t size) {
    if (out && size) out[0] = 0;
    config::Decl d;
    if (!out || !size) return GN_ERR_ARGUMENT;
    if (!lookup(mod, id, d)) return GN_ERR_NOT_FOUND;
    const std::string v = d.kind == config::Kind::Text ? currentText(mod, d) : config::format(d, current(mod, d));
    const size_t n = std::min(v.size(), size - 1);
    memcpy(out, v.data(), n);
    out[n] = 0;
    return n == v.size() ? GN_OK : GN_ERR_FULL;
}

int settingSetNumber(const char* mod, const char* id, double v) {
    config::Decl d;
    if (!lookup(mod, id, d)) return GN_ERR_NOT_FOUND;
    if (d.kind == config::Kind::Text) return GN_ERR_ARGUMENT;
    const std::string text = config::format(d, v);
    bool found = false;
    if (configValue(mod, d.id, &found) == text && found) return GN_OK;
    configSet(mod, id, text.c_str());
    changed(mod, id);
    return GN_OK;
}

int settingSetText(const char* mod, const char* id, const char* v) {
    config::Decl d;
    if (!v) return GN_ERR_ARGUMENT;
    if (!lookup(mod, id, d)) return GN_ERR_NOT_FOUND;
    if (d.kind != config::Kind::Text) return settingSetNumber(mod, id, config::parseNumber(d, v));
    const std::string text = config::cleanValue(v);
    bool found = false;
    if (configValue(mod, d.id, &found) == text && found) return GN_OK;
    configSet(mod, id, text.c_str());
    changed(mod, id);
    return GN_OK;
}

int settingOnChange(const char* mod, const char* id, gn_setting_fn fn, void* user) {
    if (!mod || !fn) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    g_listeners.push_back({mod, id ? id : "", fn, user});
    return GN_OK;
}

int settingsPage(const char* mod, int shown) {
    if (!mod) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    g_pages[mod].shown = shown != 0;
    return GN_OK;
}

config::Comments settingComments(const std::string& mod) {
    config::Comments out;
    std::lock_guard<std::mutex> lock(g_m);
    auto it = g_comments.find(mod);
    if (it == g_comments.end()) return out;
    for (const auto& [id, text] : it->second) out.push_back({id, text});
    return out;
}

void forgetSettings(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& [mod, page] : g_pages)
        page.settings.erase(std::remove_if(page.settings.begin(), page.settings.end(), [&](const Declared& s) { return r.has(reinterpret_cast<void*>(s.owner)); }),
                            page.settings.end());
    for (auto it = g_pages.begin(); it != g_pages.end();) it = it->second.settings.empty() ? g_pages.erase(it) : std::next(it);
    g_listeners.erase(std::remove_if(g_listeners.begin(), g_listeners.end(), [&](const Listener& l) { return r.has(reinterpret_cast<void*>(l.fn)); }),
                      g_listeners.end());
}

// The pages, in the GridNode menu (render thread).
void drawSettingsPages() {
    std::vector<std::pair<std::string, std::vector<config::Decl>>> pages;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const auto& [mod, page] : g_pages) {
            if (!page.shown) continue;
            std::vector<config::Decl> ds;
            for (const Declared& s : page.settings)
                if (!(s.d.flags & GN_SETTING_HIDDEN)) ds.push_back(s.d);
            if (!ds.empty()) pages.push_back({mod, std::move(ds)});
        }
    }
    if (pages.empty()) return;
    if (!ImGui::CollapsingHeader("Mod settings")) return;
    static std::map<std::string, std::string> editing; // a text being typed, by "<mod>/<id>"
    for (const auto& [mod, ds] : pages) {
        ImGui::PushID(mod.c_str());
        if (ImGui::TreeNodeEx(mod.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            std::string group;
            for (const config::Decl& d : ds) {
                if (d.group != group) {
                    group = d.group;
                    if (!group.empty()) ImGui::SeparatorText(group.c_str());
                }
                ImGui::PushID(d.id.c_str());
                const char* label = d.label.empty() ? d.id.c_str() : d.label.c_str();
                const double v = d.kind == config::Kind::Text ? 0 : current(mod, d);
                switch (d.kind) {
                case config::Kind::Bool: {
                    bool b = v != 0;
                    if (ImGui::Checkbox(label, &b)) settingSetNumber(mod.c_str(), d.id.c_str(), b ? 1 : 0);
                    break;
                }
                case config::Kind::Int: {
                    int i = static_cast<int>(v);
                    const bool moved = d.min < d.max ? ImGui::SliderInt(label, &i, static_cast<int>(d.min), static_cast<int>(d.max))
                                                     : ImGui::InputInt(label, &i, d.step > 0 ? static_cast<int>(d.step) : 1);
                    if (moved) settingSetNumber(mod.c_str(), d.id.c_str(), i);
                    break;
                }
                case config::Kind::Float: {
                    float f = static_cast<float>(v);
                    const bool moved = d.min < d.max ? ImGui::SliderFloat(label, &f, static_cast<float>(d.min), static_cast<float>(d.max), "%.3g")
                                                     : ImGui::InputFloat(label, &f, static_cast<float>(d.step), static_cast<float>(d.step * 10), "%.3g");
                    if (moved) settingSetNumber(mod.c_str(), d.id.c_str(), f);
                    break;
                }
                case config::Kind::Choice: {
                    const int sel = static_cast<int>(v);
                    if (ImGui::BeginCombo(label, d.choices[size_t(sel)].c_str())) {
                        for (size_t c = 0; c < d.choices.size(); ++c)
                            if (ImGui::Selectable(d.choices[c].c_str(), int(c) == sel)) settingSetNumber(mod.c_str(), d.id.c_str(), double(c));
                        ImGui::EndCombo();
                    }
                    break;
                }
                case config::Kind::Text: {
                    const std::string key = mod + "/" + d.id;
                    auto it = editing.find(key);
                    std::string text = it != editing.end() ? it->second : currentText(mod, d);
                    char buf[512];
                    snprintf(buf, sizeof(buf), "%s", text.c_str());
                    if (ImGui::InputText(label, buf, sizeof(buf))) editing[key] = buf;
                    if (ImGui::IsItemDeactivatedAfterEdit()) { // written when the player leaves the box, not per letter
                        settingSetText(mod.c_str(), d.id.c_str(), buf);
                        editing.erase(key);
                    }
                    break;
                }
                }
                if (ImGui::BeginPopupContextItem("reset")) {
                    if (ImGui::MenuItem("Back to the default")) {
                        if (d.kind == config::Kind::Text) settingSetText(mod.c_str(), d.id.c_str(), d.defText.c_str());
                        else settingSetNumber(mod.c_str(), d.id.c_str(), d.def);
                    }
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                    const std::string tip = config::comment(d) + "\nRight-click: back to the default.";
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

} // namespace gn::host
