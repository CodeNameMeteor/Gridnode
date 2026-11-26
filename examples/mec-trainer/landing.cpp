#include "landing.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/ui.hpp"

namespace landing {
namespace {

// Options (mods/mec-trainer.ini, landing.*).
std::atomic<bool> g_on{false}, g_arc{true}, g_ring{true}, g_coords{true}, g_hud{true}, g_jumpArc{true};
std::atomic<float> g_ringRadius{1.f};

// Her fall, measured: gravity (m/s², negative) and top fall speed (m/s, negative; 0 = not seen yet). Saved so
// the next run starts with them. 9.81 until her own falls say otherwise.
constexpr float kDefaultGravity = -9.81f;
std::atomic<float> g_gravity{kDefaultGravity}, g_topFall{0.f};
std::atomic<bool> g_gravityMeasured{false};

// From the client tick.
std::atomic<bool> g_inAir{false}, g_flying{false};
std::atomic<float> g_lastGroundY{0.f};
std::atomic<int> g_state{0};
std::atomic<float> g_camX{0.f}, g_camZ{1.f}; // the camera's forward, flattened

// What a jump does from a hold (a ledge, a pipe, standing still): learnt from her own jumps, per movement state,
// because the game's animation decides the launch (no fixed number to read). Kept in two frames, the camera's
// (where you look) and her body's (where she faces); the one whose jumps agree better is used. (Server thread,
// saved as landing.jump.<state>.)
struct Launch {
    float cam[3] = {}, body[3] = {}; // forward, up, right (m/s) in each frame
    float camVar = 0.f, bodyVar = 0.f;
    int n = 0;
    bool useBody() const { return n >= 2 && bodyVar < camVar * 0.7f; }
};
std::mutex g_jumpM;
std::map<int, Launch> g_jumps; // (g_jumpM)

struct Sample {
    double t; // game seconds (the server tick's dt summed: follows the game's time scale, as her physics does)
    gn::Vec3 p;
};

struct Prediction {
    bool valid = false;       // an arc to show
    bool hit = false;         // the arc meets something
    std::vector<gn::Vec3> arc; // her feet along the arc, from now to the hit (or the end of the search)
    gn::Vec3 at{}, normal{0.f, 1.f, 0.f};
    float seconds = 0.f;      // game seconds to the hit
    float impact = 0.f;       // speed at the hit, m/s
    float fromY = 0.f;        // her height now
    float lastGroundY = 0.f;  // the game measures falls from it
    // After she lands: where she really came down, beside the last prediction (shown a few seconds).
    bool landed = false;
    gn::Vec3 landedAt{};
    ULONGLONG landedMs = 0;
    bool jumpRef = false;   // she jumped from a hold with a jump arc showing: where its ring was
    gn::Vec3 jumpAt{};
};

std::mutex g_m;
Prediction g_pred; // (g_m)
Prediction g_jump; // (g_m) where she lands if she jumps now (valid only while she holds still in a learnt state)
int g_jumpState = 0; // (g_m)

// Server thread only.
std::deque<Sample> g_samples;
double g_time = 0.0;
double g_airSince = -1.0;
int g_tick = 0;
bool g_wasInAir = false;
// A jump being learnt: the state she left and her frames at that moment; measured 0.15 s later.
struct Pending {
    bool on = false;
    int state = 0;
    double t = 0.0;
    float camF[2] = {}, bodyF[2] = {};
};
Pending g_pending;
bool g_haveTakeoffJump = false; // the jump arc's landing when she left the hold (server thread)
gn::Vec3 g_takeoffJump{};
int g_prevState = 0;
bool g_prevStill = false, g_prevInAir = false; // a hold counts while the game has her in the air (a ledge, a pipe), not standing
float g_prevCam[2] = {0.f, 1.f}, g_prevBody[2] = {0.f, 1.f};

gn::Vec3 add(const gn::Vec3& a, const gn::Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
gn::Vec3 scale(const gn::Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float length(const gn::Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
gn::Vec3 cross(const gn::Vec3& a, const gn::Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
gn::Vec3 normalize(const gn::Vec3& a) {
    const float l = length(a);
    return l > 1e-6f ? scale(a, 1.f / l) : gn::Vec3{0.f, 1.f, 0.f};
}

// A flat forward (x, z) and its right; local (forward, up, right) to world and back. Which side "right" is does
// not matter: the same formula goes both ways.
void flat(float x, float z, float out[2]) {
    const float l = std::sqrt(x * x + z * z);
    out[0] = l > 1e-4f ? x / l : 0.f, out[1] = l > 1e-4f ? z / l : 1.f;
}
void toLocal(const gn::Vec3& v, const float f[2], float out[3]) {
    out[0] = v.x * f[0] + v.z * f[1], out[1] = v.y, out[2] = v.x * f[1] - v.z * f[0];
}
gn::Vec3 toWorld(const float l[3], const float f[2]) {
    return {l[0] * f[0] + l[2] * f[1], l[1], l[0] * f[1] - l[2] * f[0]};
}
void learn(Launch& j, const float cam[3], const float body[3]) {
    auto dev = [](const float* a, const float* b) {
        return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
    };
    if (j.n == 0) {
        std::copy(cam, cam + 3, j.cam), std::copy(body, body + 3, j.body);
    } else {
        j.camVar = j.n == 1 ? dev(cam, j.cam) : j.camVar * 0.7f + dev(cam, j.cam) * 0.3f;
        j.bodyVar = j.n == 1 ? dev(body, j.body) : j.bodyVar * 0.7f + dev(body, j.body) * 0.3f;
        for (int i = 0; i < 3; ++i) j.cam[i] = j.cam[i] * 0.7f + cam[i] * 0.3f, j.body[i] = j.body[i] * 0.7f + body[i] * 0.3f;
    }
    ++j.n;
}

// Her velocity now: from the sample about 0.1 s back to the newest (her position moves in physics steps that do not
// line up with the ticks, so tick to tick it jitters). That is her velocity 0.05 s ago: gravity moves its y on.
bool velocity(gn::Vec3& v) {
    if (g_samples.size() < 3) return false;
    const Sample& last = g_samples.back();
    const Sample* from = &g_samples[g_samples.size() - 2];
    for (auto it = g_samples.rbegin() + 1; it != g_samples.rend(); ++it) {
        from = &*it;
        if (last.t - it->t >= 0.1) break;
    }
    const double span = last.t - from->t;
    if (span < 0.03) return false;
    v = scale(add(last.p, scale(from->p, -1.f)), static_cast<float>(1.0 / span));
    v.y += g_gravity.load() * static_cast<float>(span * 0.5);
    if (const float top = g_topFall.load(); top < 0.f) v.y = std::max(v.y, top);
    return true;
}

// Gravity and top fall speed from her own fall: the slope of her vertical speed over the last half second in the air.
// A wall run or a swing is not a fall (its vertical speed barely changes): slopes outside -3..-60 are not taken.
void measure() {
    if (g_airSince < 0.0 || g_time - g_airSince < 0.4 || g_samples.size() < 10) return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    float lastVy = 0.f;
    for (size_t i = 1; i < g_samples.size(); ++i) {
        const Sample &a = g_samples[i - 1], &b = g_samples[i];
        if (a.t < g_time - 0.5 || a.t < g_airSince) continue;
        const double dt = b.t - a.t;
        if (dt <= 1e-4) continue;
        const double t = (a.t + b.t) * 0.5, vy = (b.p.y - a.p.y) / dt;
        sx += t, sy += vy, sxx += t * t, sxy += t * vy, ++n;
        lastVy = static_cast<float>(vy);
    }
    if (n < 8) return;
    const double den = n * sxx - sx * sx;
    if (std::fabs(den) < 1e-9) return;
    const float slope = static_cast<float>((n * sxy - sx * sy) / den);
    if (slope < -3.f && slope > -60.f) {
        const float g = g_gravityMeasured ? g_gravity * 0.8f + slope * 0.2f : slope;
        g_gravity = g;
        g_gravityMeasured = true;
    } else if (std::fabs(slope) < 0.6f && lastVy < -15.f && g_time - g_airSince > 1.0) {
        // Falling fast and no longer speeding up: her top fall speed.
        const float top = g_topFall.load();
        g_topFall = top < 0.f ? top * 0.8f + lastVy * 0.2f : lastVy;
    }
}

// The arc from p with velocity v, swept with a sphere of her feet's size until it meets something.
void predict(const gn::Vec3& p0, const gn::Vec3& v0, Prediction& out) {
    constexpr float kRadius = 0.2f;     // a sphere, so a thin gap or an edge does not let the arc through
    constexpr float kMaxSeconds = 40.f; // ~7.8 km of free fall at 9.81: higher than anything in the city
    constexpr int kMaxSteps = 400;
    const float g = g_gravity.load(), top = g_topFall.load();
    gn::Vec3 p = p0, v = v0;
    float t = 0.f;
    out.arc.clear();
    out.arc.push_back(p);
    out.hit = false;
    for (int i = 0; i < kMaxSteps && t < kMaxSeconds && p.y > -400.f; ++i) {
        // Steps of about 2.5 m of path: fine near her, cheap on a long fall.
        const float dt = std::clamp(2.5f / std::max(length(v), 0.1f), 1.f / 30.f, 0.25f);
        gn::Vec3 nv = v;
        nv.y += g * dt;
        if (top < 0.f) nv.y = std::max(nv.y, top);
        const gn::Vec3 np = add(p, scale(add(v, nv), 0.5f * dt));
        gn::RayHit hit{};
        const gn::Vec3 a{p.x, p.y + kRadius, p.z}, b{np.x, np.y + kRadius, np.z};
        // The first step can touch the ground she is leaving: a hit right at the start of it is not a landing.
        if (gn::raycast(a, b, hit, kRadius) && !(i == 0 && hit.distance < 0.05f)) {
            const float f = std::clamp(hit.fraction, 0.f, 1.f);
            out.at = {hit.position[0], hit.position[1], hit.position[2]};
            out.normal = normalize({hit.normal[0], hit.normal[1], hit.normal[2]});
            out.seconds = t + dt * f;
            const gn::Vec3 vh = add(v, scale(add(nv, scale(v, -1.f)), f));
            out.impact = length(vh);
            out.arc.push_back(add(p, scale(add(np, scale(p, -1.f)), f)));
            out.hit = true;
            return;
        }
        p = np, v = nv, t += dt;
        out.arc.push_back(p);
    }
    out.seconds = t;
}

void ring(const gn::ui::View& view, const gn::Vec3& at, const gn::Vec3& n, float r, ImU32 col) {
    const gn::Vec3 side = std::fabs(n.y) < 0.9f ? gn::Vec3{0.f, 1.f, 0.f} : gn::Vec3{1.f, 0.f, 0.f};
    const gn::Vec3 t1 = normalize(cross(n, side)), t2 = cross(n, t1);
    const gn::Vec3 c = add(at, scale(n, 0.05f)); // just off the surface, so it is not hidden in it
    constexpr int kSegments = 40;
    gn::Vec3 prev{};
    for (int i = 0; i <= kSegments; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / kSegments;
        const gn::Vec3 q = add(c, add(scale(t1, std::cos(a) * r), scale(t2, std::sin(a) * r)));
        if (i) gn::ui::line3d(view, prev, q, col, 2.5f);
        prev = q;
    }
    gn::ui::line3d(view, add(c, scale(t1, -r * 0.3f)), add(c, scale(t1, r * 0.3f)), col, 2.f);
    gn::ui::line3d(view, add(c, scale(t2, -r * 0.3f)), add(c, scale(t2, r * 0.3f)), col, 2.f);
}

// Lines of text under a world point (centred, one under the other, with a shadow so they read on the sky).
void label(const gn::ui::View& view, const gn::Vec3& at, const char* const* lines, int count, ImU32 col) {
    ImVec2 s;
    if (!gn::ui::toScreen(view, at, s)) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float y = s.y + 8.f;
    for (int i = 0; i < count; ++i) {
        const ImVec2 size = ImGui::CalcTextSize(lines[i]);
        const ImVec2 p(s.x - size.x * 0.5f, y);
        dl->AddText(ImVec2(p.x + 1.f, p.y + 1.f), IM_COL32(0, 0, 0, 200), lines[i]);
        dl->AddText(p, col, lines[i]);
        y += size.y + 1.f;
    }
}

} // namespace

void loadSettings() {
    g_on = gn::config::getBool("landing.on", g_on);
    g_arc = gn::config::getBool("landing.arc", g_arc);
    g_ring = gn::config::getBool("landing.ring", g_ring);
    g_coords = gn::config::getBool("landing.coords", g_coords);
    g_hud = gn::config::getBool("landing.hud", g_hud);
    g_ringRadius = std::clamp(gn::config::getFloat("landing.ring_m", g_ringRadius), 0.2f, 5.f);
    const float g = gn::config::getFloat("landing.gravity", 0.f);
    if (g < -3.f && g > -60.f) g_gravity = g, g_gravityMeasured = true;
    const float top = gn::config::getFloat("landing.top_fall", 0.f);
    if (top < -15.f && top > -500.f) g_topFall = top;
    g_jumpArc = gn::config::getBool("landing.jump_arc", g_jumpArc);
    std::lock_guard<std::mutex> lock(g_jumpM);
    for (int st = 0; st < 64; ++st) {
        const std::string key = "landing.jump." + std::to_string(st);
        const std::string v = gn::config::get(key.c_str());
        Launch j;
        if (!v.empty() && sscanf(v.c_str(), "%d %f %f %f %f %f %f %f %f", &j.n, &j.cam[0], &j.cam[1], &j.cam[2], &j.body[0], &j.body[1],
                                 &j.body[2], &j.camVar, &j.bodyVar) == 9 && j.n > 0)
            g_jumps[st] = j;
    }
}

void saveSettings() {
    gn::config::setBool("landing.on", g_on);
    gn::config::setBool("landing.arc", g_arc);
    gn::config::setBool("landing.ring", g_ring);
    gn::config::setBool("landing.coords", g_coords);
    gn::config::setBool("landing.hud", g_hud);
    gn::config::setFloat("landing.ring_m", g_ringRadius);
    if (g_gravityMeasured) gn::config::setFloat("landing.gravity", g_gravity);
    if (g_topFall.load() < 0.f) gn::config::setFloat("landing.top_fall", g_topFall);
    gn::config::setBool("landing.jump_arc", g_jumpArc);
    std::lock_guard<std::mutex> lock(g_jumpM);
    for (const auto& [st, j] : g_jumps) {
        char b[160];
        snprintf(b, sizeof(b), "%d %.3f %.3f %.3f %.3f %.3f %.3f %.4f %.4f", j.n, j.cam[0], j.cam[1], j.cam[2], j.body[0], j.body[1], j.body[2],
                 j.camVar, j.bodyVar);
        gn::config::setString(("landing.jump." + std::to_string(st)).c_str(), b);
    }
}

void clientTick(const gn::Movement& m, bool flying) {
    g_inAir = m.on_ground != 0; // 1 while she is in the air (the field's name is historical)
    g_flying = flying;
    g_lastGroundY = m.last_ground_y;
    g_state = m.state;
    g_camX = m.camera_forward[0], g_camZ = m.camera_forward[2];
}

void serverTick(void* faith, float dt) {
    gn::Vec3 p;
    if (!g_on || g_flying || !faith || !gn::positionOf(faith, p)) {
        g_samples.clear(), g_airSince = -1.0, g_wasInAir = false;
        g_pending.on = false;
        std::lock_guard<std::mutex> lock(g_m);
        g_pred.valid = false, g_jump.valid = false;
        return;
    }
    g_time += std::max(dt, 0.f);
    // A teleport or a respawn is not movement: start again.
    if (!g_samples.empty() && length(add(p, scale(g_samples.back().p, -1.f))) > 50.f) g_samples.clear(), g_airSince = -1.0;
    g_samples.push_back({g_time, p});
    while (g_samples.size() > 2 && g_time - g_samples.front().t > 0.6) g_samples.pop_front();

    const bool inAir = g_inAir.load();
    if (inAir && !g_wasInAir) g_airSince = g_time;
    if (!inAir && g_wasInAir) { // she landed: keep the last prediction a moment, beside where she came down
        g_airSince = -1.0;
        std::lock_guard<std::mutex> lock(g_m);
        g_pred.landed = true, g_pred.landedAt = p, g_pred.landedMs = GetTickCount64();
        g_pred.jumpRef = g_haveTakeoffJump, g_pred.jumpAt = g_takeoffJump;
        if (g_haveTakeoffJump && !g_pred.hit) g_pred.valid = true, g_pred.arc.clear(); // a short jump: no fall arc was made
        g_haveTakeoffJump = false;
    }
    g_wasInAir = inAir;
    if (inAir) measure();

    // Jumps from a hold: when she leaves a state she was holding still in, what she does 0.15 s later is that state's
    // jump, if it is one (going up, or off sideways fast; letting go or climbing up is not).
    gn::Vec3 v{};
    const bool haveV = velocity(v);
    const bool still = haveV && length(v) < 1.5f;
    const int state = g_state.load();
    float cam[2], body[2] = {0.f, 1.f};
    flat(g_camX, g_camZ, cam);
    if (gn::Transform t; gn::transformOf(faith, t)) flat(t.forward[0], t.forward[2], body);
    if (state != g_prevState && g_prevStill && g_prevInAir) {
        g_pending.on = true, g_pending.state = g_prevState, g_pending.t = g_time;
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_haveTakeoffJump = g_jump.valid && g_jump.hit;
            if (g_haveTakeoffJump) g_takeoffJump = g_jump.at;
        }
        std::copy(g_prevCam, g_prevCam + 2, g_pending.camF), std::copy(g_prevBody, g_prevBody + 2, g_pending.bodyF);
    }
    if (g_pending.on && g_time - g_pending.t >= 0.15) {
        g_pending.on = false;
        if (inAir && haveV && g_time - g_pending.t < 0.4) {
            gn::Vec3 take = v;
            take.y -= g_gravity.load() * static_cast<float>(g_time - g_pending.t); // back to the moment she left
            if (take.y > 1.5f || std::sqrt(take.x * take.x + take.z * take.z) > 2.f) {
                float lc[3], lb[3];
                toLocal(take, g_pending.camF, lc), toLocal(take, g_pending.bodyF, lb);
                std::lock_guard<std::mutex> lock(g_jumpM);
                Launch& j = g_jumps[g_pending.state];
                learn(j, lc, lb);
                gn::log("landing: jump from state %d learnt (%d so far): %.1f m/s up, %.1f m/s off", g_pending.state, j.n, take.y,
                        std::sqrt(take.x * take.x + take.z * take.z));
            }
        }
    }
    g_prevState = state, g_prevStill = still, g_prevInAir = inAir;
    std::copy(cam, cam + 2, g_prevCam), std::copy(body, body + 2, g_prevBody);

    if (++g_tick % 3) return; // ten arcs a second is enough to follow her; each is up to 400 ray casts
    // If she jumps now: while she holds still in a state with a learnt jump.
    {
        Launch j;
        bool have = false;
        if (g_jumpArc && still && inAir) {
            std::lock_guard<std::mutex> lock(g_jumpM);
            if (auto it = g_jumps.find(state); it != g_jumps.end()) j = it->second, have = true;
        }
        Prediction jp;
        if (have) {
            predict(p, j.useBody() ? toWorld(j.body, body) : toWorld(j.cam, cam), jp);
            jp.valid = true, jp.fromY = p.y;
        }
        std::lock_guard<std::mutex> lock(g_m);
        g_jump = std::move(jp);
        g_jumpState = state;
    }
    if (!inAir || !haveV) return;
    Prediction next;
    predict(p, v, next);
    next.valid = true;
    next.fromY = p.y;
    next.lastGroundY = g_lastGroundY.load();
    std::lock_guard<std::mutex> lock(g_m);
    g_pred = std::move(next);
}

void draw() {
    if (!g_on) return;
    Prediction p, jp;
    {
        std::lock_guard<std::mutex> lock(g_m);
        jp = g_jump;
        if (g_pred.valid && g_pred.landed && GetTickCount64() - g_pred.landedMs > 4000) g_pred.valid = false;
        p = g_pred;
    }
    if (!p.valid && !jp.valid) return;
    gn::ui::View view;
    if (!gn::ui::view(view)) return;
    if (jp.valid) { // if she jumps now: yellow, dashed
        const ImU32 jc = IM_COL32(255, 220, 40, 255);
        if (g_arc)
            for (size_t i = 1; i < jp.arc.size(); ++i)
                if (i % 2) gn::ui::line3d(view, jp.arc[i - 1], jp.arc[i], IM_COL32(255, 220, 40, 210), 2.f);
        if (jp.hit) {
            if (g_ring) ring(view, jp.at, jp.normal, g_ringRadius, jc);
            if (g_coords) {
                char a[96], b[96];
                snprintf(a, sizeof(a), "if you jump: %.1f, %.1f, %.1f", jp.at.x, jp.at.y, jp.at.z);
                snprintf(b, sizeof(b), "%.1f s   %.1f m down%s", jp.seconds, jp.fromY - jp.at.y, jp.normal.y > 0.7f ? "" : "   (a wall)");
                const char* lines[] = {a, b};
                label(view, jp.at, lines, 2, jc);
            }
        }
    }
    if (!p.valid) return;
    if (p.landed && p.jumpRef) { // she jumped from a hold: how close the jump arc was
        if (g_ring) ring(view, p.jumpAt, {0.f, 1.f, 0.f}, g_ringRadius, IM_COL32(255, 220, 40, 255));
        gn::ui::marker(view, p.landedAt, IM_COL32(255, 255, 255, 255), 5.f);
        const float dx = p.landedAt.x - p.jumpAt.x, dz = p.landedAt.z - p.jumpAt.z;
        char b[96];
        snprintf(b, sizeof(b), "landed %.2f m from the jump prediction", std::sqrt(dx * dx + dz * dz));
        const char* lines[] = {b};
        label(view, p.jumpAt, lines, 1, IM_COL32(255, 255, 255, 230));
    }
    if (p.landed && !p.hit) return;
    const bool ground = p.hit && p.normal.y > 0.7f;
    const ImU32 col = !p.hit ? IM_COL32(200, 200, 200, 200) : ground ? IM_COL32(60, 230, 120, 255) : IM_COL32(255, 160, 40, 255);
    if (g_arc && !p.landed)
        for (size_t i = 1; i < p.arc.size(); ++i) gn::ui::line3d(view, p.arc[i - 1], p.arc[i], IM_COL32(80, 200, 255, 200), 2.f);
    if (!p.hit) return;
    if (g_ring) ring(view, p.at, p.normal, g_ringRadius, col);
    if (p.landed) {
        gn::ui::marker(view, p.landedAt, IM_COL32(255, 255, 255, 255), 5.f);
        const float dx = p.landedAt.x - p.at.x, dz = p.landedAt.z - p.at.z;
        char b[96];
        snprintf(b, sizeof(b), "landed %.2f m from the prediction", std::sqrt(dx * dx + dz * dz));
        const char* lines[] = {b};
        label(view, p.at, lines, 1, IM_COL32(255, 255, 255, 230));
        return;
    }
    if (g_coords) {
        char a[96], b[96];
        snprintf(a, sizeof(a), "%.1f, %.1f, %.1f", p.at.x, p.at.y, p.at.z);
        snprintf(b, sizeof(b), "%.1f s   %.1f m down%s", p.seconds, p.fromY - p.at.y, ground ? "" : "   (a wall)");
        const char* lines[] = {a, b};
        label(view, p.at, lines, 2, col);
    }
}

void hudLines() {
    if (!g_on || !g_hud) return;
    Prediction p;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_jump.valid && g_jump.hit)
            ImGui::TextColored(ImVec4(1.f, 0.86f, 0.16f, 1.f), "If you jump: (%.1f, %.1f, %.1f) in %.1f s, %.1f m down", g_jump.at.x, g_jump.at.y,
                               g_jump.at.z, g_jump.seconds, g_jump.fromY - g_jump.at.y);
        if (!g_pred.valid || g_pred.landed) return;
        p.hit = g_pred.hit, p.at = g_pred.at, p.normal = g_pred.normal, p.seconds = g_pred.seconds, p.impact = g_pred.impact;
        p.fromY = g_pred.fromY, p.lastGroundY = g_pred.lastGroundY;
    }
    if (!p.hit) {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.f), "Landing: nothing below within %.0f s (not streamed in yet?)", p.seconds);
        return;
    }
    ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.5f, 1.f), "Landing (%.1f, %.1f, %.1f) in %.1f s", p.at.x, p.at.y, p.at.z, p.seconds);
    ImGui::Text("Drop %.1f m (fall from last ground %.1f m), hits at %.0f m/s", p.fromY - p.at.y, p.lastGroundY - p.at.y, p.impact);
}

void options() {
    bool v = g_on;
    if (ImGui::Checkbox("Landing prediction", &v)) g_on = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While Faith is in the air: the arc she will follow, a ring where she comes down and its coordinates.\n"
                          "Works from any height (skydiving: is she high enough, where does she land). Green: ground, orange: a wall,\n"
                          "grey: nothing found yet (the ground below may not be streamed in). After landing, a white dot shows\n"
                          "where she really came down, for a few seconds.");
    if (!g_on) return;
    ImGui::Indent();
    v = g_arc;
    if (ImGui::Checkbox("Arc", &v)) g_arc = v;
    ImGui::SameLine();
    v = g_ring;
    if (ImGui::Checkbox("Ring", &v)) g_ring = v;
    ImGui::SameLine();
    v = g_coords;
    if (ImGui::Checkbox("Coordinates", &v)) g_coords = v;
    ImGui::SameLine();
    v = g_hud;
    if (ImGui::Checkbox("In Player info", &v)) g_hud = v;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The landing's coordinates, time and drop as lines in the Player info window.");
    float r = g_ringRadius;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderFloat("Ring size (m)", &r, 0.2f, 5.f, "%.1f")) g_ringRadius = r;
    v = g_jumpArc;
    if (ImGui::Checkbox("If I jump (learnt)", &v)) g_jumpArc = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Holding still on a ledge, a pipe or a beam: a yellow dashed arc and ring for where a jump from there lands.\n"
                          "The game's animation decides a jump, so the trainer learns each kind of hold from your own jumps: jump\n"
                          "off one once (looking where you want to go) and from then on it shows. More jumps make it more exact.");
    {
        std::string learnt;
        std::lock_guard<std::mutex> lock(g_jumpM);
        for (const auto& [st, j] : g_jumps)
            learnt += (learnt.empty() ? "" : ", ") + std::to_string(st) + " (" + std::to_string(j.n) + (j.useBody() ? ", body" : "") + ")";
        ImGui::SameLine();
        ImGui::TextDisabled("learnt: %s", learnt.empty() ? "none yet" : learnt.c_str());
        if (!learnt.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Forget jumps")) {
                for (const auto& [st, j] : g_jumps) gn::config::setString(("landing.jump." + std::to_string(st)).c_str(), "");
                g_jumps.clear();
            }
        }
    }
    const float top = g_topFall.load();
    ImGui::TextDisabled("Gravity %.2f m/s2 (%s), top fall speed %s", -g_gravity.load(), g_gravityMeasured ? "measured" : "default until she falls",
                        top < 0.f ? (std::to_string(static_cast<int>(-top)) + " m/s").c_str() : "not seen yet");
    ImGui::SameLine();
    if (ImGui::SmallButton("Measure again")) g_gravity = kDefaultGravity, g_gravityMeasured = false, g_topFall = 0.f;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget the measured gravity and top fall speed; the next falls measure them again.");
    ImGui::Unindent();
}

bool shown() { return g_on; }

} // namespace landing
