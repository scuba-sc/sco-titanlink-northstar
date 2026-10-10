// link.h: the peer side of sc-offline's TitanLink channel (Local\SCO_titanlink.link), through
// sco-core's MIT wire headers only (third_party/sco-sdk: sc_ipc.h, sc_titanlink.h).
//
// Everything sc-offline writes is treated as hostile: sc_ipc.h bounds-checks the rings and
// blocks, and this file checks every value inside them before the game sees it (flags masked,
// coordinates finite and in range, names [a-z0-9_] and known, records of the wrong size or an
// unknown type skipped, a ring that fails validation ignored from then on, logged once).
//
// No game code here, so tests/link_selftest.cpp drives it against a channel it creates itself.
#pragma once
#include <windows.h>
#include <sc_ipc.h>
#include <sc_titanlink.h>
#include <cstdint>

namespace tl {

// The wire this plugin was written for. A vendored header with other values fails the build
// (static_asserts in link.cpp); a channel with other values is refused at attach, loudly.
constexpr uint32_t kWireVersion = 1;            // SC_IPC_VERSION
constexpr uint32_t kLayoutId = 0x314B4C54u;     // TL_LAYOUT_ID, "TLK1"
constexpr uint32_t kLayoutVersion = 1;          // TL_LAYOUT_VERSION
constexpr uint32_t kOwnerTimeoutMs = 4000;      // sc-offline's side treats us as gone after 4 s too
constexpr float kMaxCoord = 1000000.0f;         // docs/bridges.md: beyond +-1,000,000 is invalid
constexpr float kMaxSpeed = 100000.0f;          // units per second

using LogFn = void (*)(const char* line);

// tl_sc_state after validation. pose_ok is false unless every pose field passed, so a caller
// never sees a NaN, an infinity or a coordinate outside the match.
struct ScView {
    bool     fresh = false;   // a snapshot was read this poll (false: keep the last one)
    bool     pose_ok = false; // TL_SC_ANCHORED and eye/ang/feet/vel all valid
    uint32_t flags = 0;       // TL_SC_* only
    uint32_t buttons = 0;     // TL_BTN_* only, 0 unless pilot mode and Star Citizen in front
    float    eye[3] = {}, ang[3] = {}, feet[3] = {}, vel[3] = {};
    float    fov_scale = 1.0f; // 0.6 to 2 (1 when sc-offline sent something else)
    uint32_t view_w = 0, view_h = 0;
};

enum class EventType { StartMatch, CallTitan, Embark, Disembark, PilotOff };

struct Event {
    EventType type = EventType::PilotOff;
    char      map[64] = "";  // StartMatch: a known map, [a-z0-9_]
    char      mode[32] = ""; // StartMatch: a known mode
    float     drop[3] = {};  // CallTitan
    float     yaw = 0;       // CallTitan, -360 to 360
};

// Map and mode names a START_MATCH may carry: [a-z0-9_] and on these lists (link.cpp).
bool KnownMap(const char* s);
bool KnownMode(const char* s);

class Link {
public:
    Link(const wchar_t* name, LogFn log) : name_(name), log_(log) {}
    ~Link() { Drop(); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Attaches (or re-attaches after an epoch change), beats, and reads tl_sc_state. Call every
    // frame; attach attempts are throttled to one every 2 s. True while attached.
    bool Poll(uint64_t now_ms);

    bool Attached() const { return up_; }
    // Attached and sc-offline beat within kOwnerTimeoutMs.
    bool OwnerAlive() const { return up_ && ownerAlive_; }
    // True once after sc-offline closed the channel (SC_IPC_GONE): the link was ended on purpose.
    bool TakeClosed() { const bool c = closed_; closed_ = false; return c; }
    // True once after a (re)attach: a new link, so a new START_MATCH is allowed.
    bool TakeNewLink() { const bool n = newLink_; newLink_ = false; return n; }
    // True while sc-offline's channel speaks another wire or layout (refused, logged once).
    bool Refused() const { return refused_; }

    const ScView& Sc() const { return sc_; }

    // The next valid message sc-offline queued; invalid ones are skipped (and logged once per
    // kind). START_MATCH is accepted once per link.
    bool NextEvent(Event& out);

    // Our side's blocks. Values are cleaned the same way before they go out.
    void PublishTf(const tl_tf_state& s);
    void PublishFrame(const char* status);
    // A line for sc-offline's mod.log and tab (printable ASCII, cut to TL_MSG_LOG_MAX).
    void Send(const char* text);

    void Drop();

private:
    bool Attach(uint64_t now_ms);
    void ReadSc();
    void Logf(const char* fmt, ...);

    const wchar_t*   name_;
    LogFn            log_;
    HANDLE           map_ = nullptr;
    void*            base_ = nullptr;
    sc_ipc_chan      ch_ = {};
    sc_ipc_ring_view in_ = {}, out_ = {};
    bool             up_ = false, ownerAlive_ = false, closed_ = false, newLink_ = false;
    bool             refused_ = false, inDead_ = false, startSeen_ = false;
    uint32_t         refusedKey_[3] = {};
    uint32_t         skipped_ = 0;    // kinds of invalid records already logged (bit per kind)
    uint64_t         nextTry_ = 0, beatAt_ = 0;
    ScView           sc_;
};

}  // namespace tl
