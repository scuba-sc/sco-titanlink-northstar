// plugin.cpp: TitanLink, a Northstar plugin (plugins v3) that is the Titanfall 2 side of
// sc-offline's TitanLink bridge (README.md). It
//   - opens sc-offline's channel as the peer, through sc_ipc.h only (link.cpp), and beats;
//   - starts a local match on TL_MSG_START_MATCH and leaves it when sc-offline closes the link;
//   - gives the mod's server script (mod/TitanLink) the pilot pose and Titan requests, and holds
//     sc-offline's buttons and camera on the local player;
//   - publishes the match's state (tl_tf_state) back.
// It acts only in the local match it started itself, with one player in it: in any other game
// (a public or private server, or a match the user started) it stays idle and says so.
#include "link.h"
#include "northstar_api.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef SCO_SDK_PIN
#error "SCO_SDK_PIN must be defined by the build (third_party/sco-sdk/SCO_SDK_PIN)"
#endif

namespace {

using namespace ns;

HMODULE   g_self = nullptr;
ISys*     g_sys = nullptr;
ServerSq  g_sq;
Engine    g_eng;
bool      g_engTried = false;
std::mutex g_lock;  // RunFrame and the script natives (both on the game thread in practice)

void Log(LogLevel level, const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_sys) g_sys->Log(reinterpret_cast<int64_t>(g_self), level, buf);
    else OutputDebugStringA(buf);
}

void LinkLog(const char* line) { Log(LogLevel::INFO, "%s", line); }

tl::Link g_link(L"Local\\SCO_titanlink.link", &LinkLog);

// ---- the local match ----------------------------------------------------------------------------

struct Match {
    bool     want = false;        // sc-offline asked for a match on this link
    char     map[64] = "", mode[32] = "";
    int      tries = 0;
    uint64_t triedAt = 0;
    uint64_t pendingAt = 0;       // we issued `map` at this time; the next server VM is ours
    bool     ours = false;        // the server VM in this process runs the match we started
    bool     crowded = false;     // more than one player in it: stay idle
    bool     settings = false;    // the local-match settings are applied
    bool     toldOther = false;   // told sc-offline we're in a game we didn't start
} g_m;

bool     g_serverVm = false, g_clientVm = false;
uint64_t g_uiVmAt = 0;
bool     g_faulted = false;

// What the script reported last (TL_Report), and when.
tl_tf_state g_tf = {};
uint64_t    g_tfAt = 0;

// Titan requests for the script, as counters it compares (the fork's scheme).
int32_t g_callSeq = 0, g_embarkSeq = 0, g_disembarkSeq = 0;
float   g_drop[3] = {}, g_dropYaw = 0;

// Held buttons: TL_BTN_* bit i holds kButtonCmds[i] (+cmd while set, -cmd on release).
const char* const kButtonCmds[10] = { "attack", "zoom", "reload", "melee", "offhand0",
                                      "offhand1", "offhand2", "ability", "weaponCycle", "useAndReload" };
uint32_t g_held = 0;
float    g_fovScale = 0;

void Cmd(const char* text) {
    if (!g_eng.cbufAddText) return;
    char line[256];
    snprintf(line, sizeof(line), "%s\n", text);
    g_eng.cbufAddText(g_eng.cbufGetCurrentPlayer(), line, cmd_source_t::kCommandSrcCode);
}

// In a game this plugin didn't start: a server elsewhere (public or private), or a match the user
// started here. Nothing is applied there and no match is started from it.
bool InOtherGame() { return (g_clientVm || g_serverVm) && !g_m.ours; }

bool Acting() { return g_m.ours && !g_m.crowded && !g_faulted && g_link.OwnerAlive(); }

void ReleaseButtons() {
    for (int i = 0; i < 10; ++i)
        if (g_held & (1u << i)) {
            char c[48];
            snprintf(c, sizeof(c), "-%s", kButtonCmds[i]);
            Cmd(c);
        }
    g_held = 0;
}

// The settings a local match needs, applied right before its `map` and only then:
//  - ns_auth_allow_insecure 1: the local listen server lets its own player in without asking
//    Northstar's master server to sign them in (no online service involved);
//  - ns_report_server_to_masterserver 0 (and the sp one): the match is never listed publicly;
//  - max_players 1 (playlist override): nobody else can join it.
// They are server-side settings: they never affect a server you join. RestoreSettings puts
// Northstar's defaults back when the match ends.
void ApplySettings() {
    Cmd("ns_report_server_to_masterserver 0");
    Cmd("ns_report_sp_server_to_masterserver 0");
    Cmd("ns_auth_allow_insecure 1");
    g_m.settings = true;
}

void RestoreSettings() {
    if (!g_m.settings) return;
    Cmd("ns_auth_allow_insecure 0");
    Cmd("ns_report_server_to_masterserver 1");
    Cmd("ns_report_sp_server_to_masterserver 0");
    g_m.settings = false;
    Log(LogLevel::INFO, "local match over: Northstar's defaults restored (ns_auth_allow_insecure 0, server reporting on)");
}

void StartMatch(uint64_t now) {
    g_m.triedAt = now;
    g_m.pendingAt = now;
    ++g_m.tries;
    ApplySettings();
    char c[128];
    Cmd("setplaylist private_match");
    Cmd("setplaylistvaroverrides max_players 1");
    snprintf(c, sizeof(c), "mp_gamemode %s", g_m.mode);  // both names checked against link.cpp's lists
    Cmd(c);
    snprintf(c, sizeof(c), "map %s", g_m.map);
    Cmd(c);
    Log(LogLevel::INFO, "starting the local match: %s on %s (try %d)", g_m.mode, g_m.map, g_m.tries);
    snprintf(c, sizeof(c), "starting the local match: %s on %s", g_m.mode, g_m.map);
    g_link.Send(c);
}

void HandleEvent(const tl::Event& e) {
    switch (e.type) {
    case tl::EventType::StartMatch:
        if (InOtherGame()) {
            Log(LogLevel::WARN, "sc-offline asked for a match, but this game is one TitanLink didn't start; staying idle");
            g_link.Send("in a game TitanLink didn't start: leave it (main menu), then open pilot mode again");
            return;
        }
        g_m.want = true;
        g_m.tries = 0;
        g_m.triedAt = 0;
        strcpy_s(g_m.map, e.map);
        strcpy_s(g_m.mode, e.mode);
        break;
    case tl::EventType::CallTitan:
        if (!Acting()) return;
        memcpy(g_drop, e.drop, sizeof(g_drop));
        g_dropYaw = e.yaw;
        ++g_callSeq;
        break;
    case tl::EventType::Embark:
        if (Acting()) ++g_embarkSeq;
        break;
    case tl::EventType::Disembark:
        if (Acting()) ++g_disembarkSeq;
        break;
    case tl::EventType::PilotOff:
        ReleaseButtons();
        break;
    }
}

void ApplyPilot(const tl::ScView& sc) {
    const bool pilot = Acting() && (sc.flags & TL_SC_PILOT);
    const uint32_t want = pilot ? sc.buttons : 0;
    for (int i = 0; i < 10; ++i) {
        const uint32_t bit = 1u << i;
        if ((want & bit) != (g_held & bit)) {
            char c[48];
            snprintf(c, sizeof(c), "%c%s", (want & bit) ? '+' : '-', kButtonCmds[i]);
            Cmd(c);
        }
    }
    g_held = want;
    if (!pilot) return;
    // Titanfall's cl_fovScale runs 1.0 to 1.7.
    const float fov = sc.fov_scale < 1.0f ? 1.0f : sc.fov_scale > 1.7f ? 1.7f : sc.fov_scale;
    if (std::fabs(fov - g_fovScale) > 0.002f) {
        g_fovScale = fov;
        char c[48];
        snprintf(c, sizeof(c), "cl_fovScale %.4f", fov);
        Cmd(c);
    }
    if (sc.pose_ok && g_eng.setViewAngles && !(g_tf.flags & TL_TF_BUSY)) {
        float a[3] = { sc.ang[0], sc.ang[1], sc.ang[2] };  // pitch already within +-89
        g_eng.setViewAngles(g_eng.engineClient, a);
    }
}

const char* Status() {
    if (g_faulted) return "TitanLink stopped after a fault (see Northstar's log)";
    if (g_m.crowded) return "idle: another player is in the match";
    if (InOtherGame()) return "idle: in a game TitanLink didn't start";
    if (g_m.ours) return "local match running; no shared picture (follow-up)";
    if (g_m.want) return "starting the local match";
    return "waiting for sc-offline to ask for a match";
}

void Tick() {
    const uint64_t now = GetTickCount64();
    if (!g_engTried) {
        g_engTried = true;
        if (!g_eng.Bind(GetModuleHandleA("engine.dll"))) Log(LogLevel::ERR, "engine.dll isn't loaded; TitanLink can't run commands");
    }
    std::lock_guard<std::mutex> lock(g_lock);
    g_link.Poll(now);
    if (g_link.TakeNewLink()) {
        g_m.want = false;
        g_m.toldOther = false;
        g_link.Send("TitanLink (Northstar) attached, SDK " SCO_SDK_PIN);
    }
    if (g_link.TakeClosed()) {
        ReleaseButtons();
        g_m.want = false;
        if (g_m.ours) {
            Log(LogLevel::INFO, "sc-offline closed the link: leaving the local match");
            Cmd("disconnect");
        }
    }
    tl::Event e;
    while (g_link.NextEvent(e)) HandleEvent(e);

    if (InOtherGame() && g_link.Attached() && !g_m.toldOther) {
        g_m.toldOther = true;
        g_m.want = false;
        g_link.Send("in a game TitanLink didn't start: staying idle there");
    }
    if (!InOtherGame()) g_m.toldOther = false;

    // Start the match once the main menu has been up a moment, retrying a stuck load a few times.
    if (g_m.want && !g_m.ours && !InOtherGame() && g_link.OwnerAlive() && g_eng.cbufAddText && g_uiVmAt &&
        now - g_uiVmAt > 4000 && g_m.tries < 5 && (!g_m.triedAt || now - g_m.triedAt > 45000))
        StartMatch(now);
    if (g_m.ours) g_m.want = false;

    const tl::ScView& sc = g_link.Sc();
    if (!g_link.OwnerAlive() || !(sc.flags & TL_SC_PILOT)) ReleaseButtons();
    ApplyPilot(sc);

    tl_tf_state out = {};
    if (Acting() && g_tfAt && now - g_tfAt < 2000) out = g_tf;
    else out.flags = 0;  // not in a match sc-offline may use
    out.pid = GetCurrentProcessId();
    out.clip = out.flags ? out.clip : -1;
    g_link.PublishTf(out);
    g_link.PublishFrame(Status());
}

// ---- script natives (server VM) ----------------------------------------------------------------

const tl::ScView& ScForScript() {
    static const tl::ScView none;
    return Acting() ? g_link.Sc() : none;
}

SQInteger SQ_OurMatch(HSQUIRRELVM v) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_sq.pushinteger(v, g_m.ours && !g_m.crowded && !g_faulted ? 1 : 0);
    return 1;
}
SQInteger SQ_Flags(HSQUIRRELVM v) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_sq.pushinteger(v, static_cast<SQInteger>(ScForScript().flags));
    return 1;
}
SQInteger SQ_Feet(HSQUIRRELVM v) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_sq.pushvector(v, ScForScript().feet);
    return 1;
}
SQInteger SQ_Vel(HSQUIRRELVM v) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_sq.pushvector(v, ScForScript().vel);
    return 1;
}
SQInteger SQ_Angles(HSQUIRRELVM v) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_sq.pushvector(v, ScForScript().ang);
    return 1;
}
SQInteger SQ_TitanCallSeq(HSQUIRRELVM v) { std::lock_guard<std::mutex> l(g_lock); g_sq.pushinteger(v, g_callSeq); return 1; }
SQInteger SQ_EmbarkSeq(HSQUIRRELVM v) { std::lock_guard<std::mutex> l(g_lock); g_sq.pushinteger(v, g_embarkSeq); return 1; }
SQInteger SQ_DisembarkSeq(HSQUIRRELVM v) { std::lock_guard<std::mutex> l(g_lock); g_sq.pushinteger(v, g_disembarkSeq); return 1; }
SQInteger SQ_TitanDrop(HSQUIRRELVM v) { std::lock_guard<std::mutex> l(g_lock); g_sq.pushvector(v, g_drop); return 1; }
SQInteger SQ_TitanDropYaw(HSQUIRRELVM v) { std::lock_guard<std::mutex> l(g_lock); g_sq.pushfloat(v, g_dropYaw); return 1; }

void Vec(HSQUIRRELVM v, SQInteger i, float* out) {
    const float* p = g_sq.getvector(v, i);
    if (p) memcpy(out, p, 12);
}

SQInteger SQ_Report(HSQUIRRELVM v) {
    tl_tf_state s = {};
    s.flags = static_cast<uint32_t>(g_sq.getinteger(v, 1));
    Vec(v, 2, s.origin);
    s.eye_z = g_sq.getfloat(v, 3);
    Vec(v, 4, s.titan_origin);
    s.titan_yaw = g_sq.getfloat(v, 5);
    s.clip = static_cast<int32_t>(g_sq.getinteger(v, 6));
    s.shots = static_cast<uint32_t>(g_sq.getinteger(v, 7));
    s.damage = g_sq.getfloat(v, 8);
    s.titan_health = g_sq.getfloat(v, 9);
    Vec(v, 10, s.floor_pt);
    if (const char* w = g_sq.getstring(v, 11)) strncpy_s(s.weapon, w, _TRUNCATE);
    s.zoom_frac = g_sq.getfloat(v, 12);
    s.zoom_fov = g_sq.getfloat(v, 13);
    const SQInteger players = g_sq.getinteger(v, 14);
    std::lock_guard<std::mutex> lock(g_lock);
    if (players > 1 && !g_m.crowded && g_m.ours) {
        g_m.crowded = true;
        ReleaseButtons();
        Log(LogLevel::WARN, "%ld players in the local match: TitanLink stops acting in it", players);
        g_link.Send("another player is in the local match: TitanLink stopped acting in it");
    }
    g_tf = s;
    g_tfAt = GetTickCount64();
    return 0;
}

SQInteger SQ_Log(HSQUIRRELVM v) {
    const char* s = g_sq.getstring(v, 1);
    Log(LogLevel::INFO, "[script] %s", s ? s : "");
    return 0;
}

struct Native {
    const char*   name;
    const char*   ret;
    eSQReturnType rt;
    const char*   args;
    SQFunction    fn;
};
const Native kNatives[] = {
    { "TL_OurMatch", "int", eSQReturnType::Integer, "", SQ_OurMatch },
    { "TL_Flags", "int", eSQReturnType::Integer, "", SQ_Flags },
    { "TL_Feet", "vector", eSQReturnType::Vector, "", SQ_Feet },
    { "TL_Vel", "vector", eSQReturnType::Vector, "", SQ_Vel },
    { "TL_Angles", "vector", eSQReturnType::Vector, "", SQ_Angles },
    { "TL_TitanCallSeq", "int", eSQReturnType::Integer, "", SQ_TitanCallSeq },
    { "TL_TitanDrop", "vector", eSQReturnType::Vector, "", SQ_TitanDrop },
    { "TL_TitanDropYaw", "float", eSQReturnType::Float, "", SQ_TitanDropYaw },
    { "TL_EmbarkSeq", "int", eSQReturnType::Integer, "", SQ_EmbarkSeq },
    { "TL_DisembarkSeq", "int", eSQReturnType::Integer, "", SQ_DisembarkSeq },
    { "TL_Report", "void", eSQReturnType::Default,
      "int flags, vector origin, float eyeZ, vector titanOrigin, float titanYaw, int clip, int shots, float damage, "
      "float titanHealth, vector floorPoint, string weapon, float zoomFrac, float zoomFov, int players",
      SQ_Report },
    { "TL_Log", "void", eSQReturnType::Default, "string text", SQ_Log },
};
SQFuncRegistration g_regs[sizeof(kNatives) / sizeof(kNatives[0])];

void RegisterNatives(CSquirrelVM* vm) {
    for (size_t i = 0; i < sizeof(kNatives) / sizeof(kNatives[0]); ++i) {
        SQFuncRegistration& r = g_regs[i];
        r = {};
        r.squirrelFuncName = kNatives[i].name;
        r.cppFuncName = kNatives[i].name;
        r.helpText = "";
        r.returnTypeString = kNatives[i].ret;
        r.returnType = kNatives[i].rt;
        r.argTypes = kNatives[i].args;
        r.funcPtr = kNatives[i].fn;
        g_sq.registerFunc(vm, &r, 1);
    }
}

// ---- Northstar's interfaces ---------------------------------------------------------------------

class PluginId final : public IPluginId {
public:
    const char* GetString(PluginString prop) override {
        switch (prop) {
        case PluginString::NAME: return "TitanLink";
        case PluginString::LOG_NAME: return "TITANLINK";
        case PluginString::DEPENDENCY_NAME: return "TitanLink";  // the mod's PluginDependencies constant
        }
        return nullptr;
    }
    int64_t GetField(PluginField prop) override {
        if (prop == PluginField::CONTEXT) return static_cast<int64_t>(PluginContext::CLIENT);  // never on dedicated servers
        if (prop == PluginField::COLOR) return 0xD0A030;
        return 0;
    }
};

// A fault in the frame update stops the plugin for the session instead of retrying blind.
void GuardedTick() {
    __try {
        Tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted = true;
        Log(LogLevel::ERR, "fault (0x%08lX) in TitanLink's frame update: TitanLink stops for this session", GetExceptionCode());
    }
}

class Callbacks final : public IPluginCallbacks {
public:
    void Init(HMODULE northstarModule, const PluginNorthstarData* initData, bool) override {
        if (initData) g_self = initData->pluginHandle;
        if (auto ci = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(northstarModule, "CreateInterface")))
            g_sys = static_cast<ISys*>(ci("NSSys001", nullptr));
        Log(LogLevel::INFO, "TitanLink loaded: sc_ipc wire %u, layout 0x%08X v%u, sco-core SDK %s", SC_IPC_VERSION,
            TL_LAYOUT_ID, TL_LAYOUT_VERSION, SCO_SDK_PIN);
    }
    void Finalize() override {}
    bool Unload() override {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_serverVm) return false;  // the server VM still holds our natives
        ReleaseButtons();
        RestoreSettings();
        g_link.Drop();
        return true;
    }
    void OnSqvmCreated(CSquirrelVM* vm) override {
        const ScriptContext ctx = VmContext(vm);
        std::lock_guard<std::mutex> lock(g_lock);
        if (ctx == ScriptContext::UI) { g_uiVmAt = GetTickCount64(); return; }
        if (ctx == ScriptContext::CLIENT) { g_clientVm = true; return; }
        if (ctx != ScriptContext::SERVER) return;
        g_serverVm = true;
        const uint64_t now = GetTickCount64();
        g_m.ours = g_m.pendingAt && now - g_m.pendingAt < 120000;
        g_m.pendingAt = 0;
        g_m.crowded = false;
        g_tfAt = 0;
        if (g_m.ours) Log(LogLevel::INFO, "the local match TitanLink started is loading");
        else Log(LogLevel::INFO, "a server TitanLink didn't start is loading here: TitanLink stays idle in it");
        if (!g_sq.registerFunc && !g_sq.Bind(GetModuleHandleA("server.dll"))) {
            Log(LogLevel::ERR, "server.dll isn't loaded; the TitanLink script won't have its functions");
            return;
        }
        RegisterNatives(vm);
    }
    void OnSqvmDestroying(CSquirrelVM* vm) override {
        const ScriptContext ctx = VmContext(vm);
        std::lock_guard<std::mutex> lock(g_lock);
        if (ctx == ScriptContext::CLIENT) g_clientVm = false;
        if (ctx != ScriptContext::SERVER) return;
        g_serverVm = false;
        if (g_m.ours) {
            ReleaseButtons();
            g_m.ours = false;
        }
        RestoreSettings();
        g_tfAt = 0;
    }
    void OnLibraryLoaded(HMODULE, const char*) override {}
    void RunFrame() override {
        if (!g_faulted) GuardedTick();
    }
};

PluginId  g_id;
Callbacks g_callbacks;

}  // namespace

bool ns::Engine::Bind(HMODULE engine) {
    auto* b = reinterpret_cast<uint8_t*>(engine);
    if (!b) return false;
    cbufGetCurrentPlayer = reinterpret_cast<decltype(cbufGetCurrentPlayer)>(b + 0x120630);
    cbufAddText = reinterpret_cast<decltype(cbufAddText)>(b + 0x1203B0);
    // The client's SetViewAngles: not bound by Northstar, so its first bytes must match.
    static const uint8_t kSetViewAngles[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x83, 0xC9, 0xFF, 0x48, 0x8B, 0xFA, 0xE8 };
    if (!memcmp(b + 0x51150, kSetViewAngles, sizeof(kSetViewAngles))) {
        setViewAngles = reinterpret_cast<decltype(setViewAngles)>(b + 0x51150);
        engineClient = b + 0x763B78;
    } else {
        Log(LogLevel::WARN, "engine.dll isn't the build TitanLink knows: the camera won't follow Star Citizen");
    }
    return true;
}

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* status) {
    if (status) *status = 0;
    if (!strcmp(name, "PluginId001")) return static_cast<IPluginId*>(&g_id);
    if (!strcmp(name, "PluginCallbacks001")) return static_cast<IPluginCallbacks*>(&g_callbacks);
    if (status) *status = 1;
    return nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
