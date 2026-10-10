// link.cpp: see link.h.
#include "link.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// The vendored headers must be the wire this file was written for. A newer SDK that changes any of
// these fails here, at build time, instead of misreading the channel at run time.
static_assert(SC_IPC_VERSION == tl::kWireVersion, "sc_ipc.h speaks another wire version: update link.cpp for it");
static_assert(SC_IPC_HEADER_BYTES == 64 && SC_IPC_RING_BYTES == 192 && SC_IPC_BLOCK_BYTES == 16, "sc_ipc.h layout changed");
static_assert(TL_LAYOUT_ID == tl::kLayoutId, "sc_titanlink.h names another layout");
static_assert(TL_LAYOUT_VERSION == tl::kLayoutVersion, "sc_titanlink.h is another layout version: update link.cpp for it");
static_assert(TL_CHANNEL_BYTES == 0x10000u, "TitanLink channel size changed");
static_assert(TL_OFF_SC_STATE == 0x0100u && TL_OFF_TF_STATE == 0x0400u && TL_OFF_FRAME == 0x0600u, "TitanLink block offsets changed");
static_assert(TL_OFF_TO_PEER == 0x1000u && TL_OFF_FROM_PEER == 0x5100u, "TitanLink ring offsets changed");
static_assert(TL_RING_TO_PEER == 0x4000u && TL_RING_FROM_PEER == 0x4000u, "TitanLink ring sizes changed");
static_assert(sizeof(tl_sc_state) == 80 && sizeof(tl_tf_state) == 124 && sizeof(tl_frame) == 136, "TitanLink block sizes changed");
static_assert(sizeof(tl_msg_start_match) == 96 && sizeof(tl_msg_call_titan) == 16, "TitanLink message sizes changed");
static_assert(TL_MSG_START_MATCH == 1 && TL_MSG_CALL_TITAN == 2 && TL_MSG_EMBARK == 3 && TL_MSG_DISEMBARK == 4 &&
                  TL_MSG_PILOT_OFF == 5 && TL_MSG_LOG == 1 && TL_MSG_LOG_MAX == 200,
              "TitanLink message types changed");

namespace tl {
namespace {

constexpr uint32_t kScFlags = TL_SC_PILOT | TL_SC_FOCUSED | TL_SC_ANCHORED;
constexpr uint32_t kButtons = (1u << 10) - 1u;  // TL_BTN_ATTACK .. TL_BTN_X

// Kinds of skipped records, logged once per link each.
enum : uint32_t { kSkipSize = 1, kSkipType = 2, kSkipName = 4, kSkipValue = 8, kSkipRepeat = 16 };

const char* const kMaps[] = {
    "mp_angel_city", "mp_black_water_canal", "mp_coliseum", "mp_coliseum_column", "mp_colony02", "mp_complex3",
    "mp_crashsite3", "mp_drydock", "mp_eden", "mp_forwardbase_kodai", "mp_glitch", "mp_grave", "mp_homestead",
    "mp_lf_deck", "mp_lf_meadow", "mp_lf_stacks", "mp_lf_township", "mp_lf_traffic", "mp_lf_uma", "mp_relic02",
    "mp_rise", "mp_thaw", "mp_wargames",
};
const char* const kModes[] = {
    "tdm", "ps", "aitdm", "cp", "ctf", "lts", "ttdm", "ffa", "fra", "mfd", "coliseum", "speedball",
};

bool FiniteIn(const float* v, int n, float lim) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > lim) return false;
    return true;
}

float WrapDeg(float a) {
    a = std::fmod(a, 360.0f);
    if (a >= 180.0f) a -= 360.0f;
    if (a < -180.0f) a += 360.0f;
    return a;
}

// A name in a fixed array: NUL-terminated inside it, 1+ chars, [a-z0-9_] only.
bool CleanName(const char* s, size_t n) {
    const void* nul = memchr(s, 0, n);
    if (!nul || nul == s) return false;
    for (const char* p = s; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

bool OnList(const char* s, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(s, list[i])) return true;
    return false;
}

float Clamp(float v, float lo, float hi) { return !std::isfinite(v) ? lo : v < lo ? lo : v > hi ? hi : v; }

void CleanVec(float* v, int n) {
    if (!FiniteIn(v, n, kMaxCoord))
        for (int i = 0; i < n; ++i) v[i] = 0;
}

void Printable(char* s, size_t n) {
    s[n - 1] = 0;
    for (char* p = s; *p; ++p)
        if (*p < 0x20 || *p > 0x7e) *p = '?';
}

// One buffer for popped records: a record is never larger than its ring.
uint8_t g_rec[TL_RING_TO_PEER];

}  // namespace

bool KnownMap(const char* s) { return OnList(s, kMaps, sizeof(kMaps) / sizeof(kMaps[0])); }
bool KnownMode(const char* s) { return OnList(s, kModes, sizeof(kModes) / sizeof(kModes[0])); }

void Link::Logf(const char* fmt, ...) {
    if (!log_) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_(buf);
}

void Link::Drop() {
    if (base_) UnmapViewOfFile(base_);
    if (map_) CloseHandle(map_);
    base_ = nullptr;
    map_ = nullptr;
    ch_ = {};
    in_ = {};
    out_ = {};
    up_ = false;
    ownerAlive_ = false;
    sc_ = {};
}

bool Link::Attach(uint64_t now) {
    if (now < nextTry_) return false;
    nextTry_ = now + 2000;
    map_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name_);
    if (!map_) return false;  // sc-offline hasn't opened the bridge (F9) yet
    base_ = MapViewOfFile(map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!base_ || !VirtualQuery(base_, &mbi, sizeof(mbi))) {
        Logf("could not map sc-offline's channel (error %lu)", GetLastError());
        Drop();
        return false;
    }
    // The view's size comes from the OS, never from the mapping.
    int rc = sc_ipc_attach(base_, mbi.RegionSize, TL_LAYOUT_ID, TL_LAYOUT_VERSION, &ch_);
    if (rc == SC_IPC_MISMATCH) {
        // Name what the channel speaks, once per distinct value, and refuse to attach.
        const volatile sc_ipc_hdr* h = static_cast<const volatile sc_ipc_hdr*>(base_);
        const uint32_t key[3] = { h->version, h->layout_id, h->layout_version };
        if (!refused_ || memcmp(key, refusedKey_, sizeof(key)) != 0) {
            Logf("REFUSING to attach: sc-offline's channel speaks sc_ipc wire %u, layout 0x%08X version %u; this plugin "
                 "was built for wire %u, layout 0x%08X version %u (sco-core SDK %s). Update the side that is behind.",
                 key[0], key[1], key[2], SC_IPC_VERSION, TL_LAYOUT_ID, TL_LAYOUT_VERSION, SCO_SDK_PIN);
            memcpy(refusedKey_, key, sizeof(key));
        }
        refused_ = true;
        Drop();
        return false;
    }
    if (rc == SC_IPC_NOT_READY || rc == SC_IPC_BUSY) { nextTry_ = now + 250; Drop(); return false; }
    if (rc == SC_IPC_GONE) { Drop(); return false; }  // closed, not yet released by sc-offline
    if (rc != SC_IPC_OK) {
        Logf("sc-offline's channel failed validation (sc_ipc result %d); not attaching", rc);
        Drop();
        return false;
    }
    refused_ = false;
    if (ch_.bytes < TL_CHANNEL_BYTES) {
        Logf("REFUSING to attach: the channel is %llu bytes, the TitanLink layout needs %u",
             static_cast<unsigned long long>(ch_.bytes), TL_CHANNEL_BYTES);
        Drop();
        return false;
    }
    rc = sc_ipc_ring_attach(&ch_, TL_OFF_TO_PEER, &in_);
    if (rc == SC_IPC_OK) rc = sc_ipc_ring_attach(&ch_, TL_OFF_FROM_PEER, &out_);
    if (rc != SC_IPC_OK) {
        if (rc != SC_IPC_NOT_READY && rc != SC_IPC_EPOCH) Logf("the channel's rings failed validation (sc_ipc result %d)", rc);
        nextTry_ = now + 250;
        Drop();
        return false;
    }
    if (in_.direction != SC_IPC_TO_PEER || out_.direction != SC_IPC_FROM_PEER) {
        Logf("REFUSING to attach: the channel's rings face the wrong way");
        Drop();
        return false;
    }
    up_ = true;
    newLink_ = true;
    inDead_ = false;
    startSeen_ = false;
    skipped_ = 0;
    beatAt_ = 0;
    Logf("attached to sc-offline's channel (epoch %llu)", static_cast<unsigned long long>(ch_.epoch));
    return true;
}

bool Link::Poll(uint64_t now) {
    if (up_) {
        const int rc = sc_ipc_check(&ch_);
        if (rc == SC_IPC_EPOCH) {
            Logf("sc-offline re-created the channel; attaching again");
            Drop();
            nextTry_ = 0;
        } else if (rc == SC_IPC_GONE) {
            Logf("sc-offline closed the link");
            closed_ = true;
            Drop();
        }
    }
    if (!up_ && !Attach(now)) return false;
    if (now - beatAt_ >= 250) {
        beatAt_ = now;
        sc_ipc_peer_beat(&ch_, GetCurrentProcessId(), now);
    }
    uint32_t age = 0;
    ownerAlive_ = sc_ipc_owner_age_ms(&ch_, now, &age) == SC_IPC_OK && age < kOwnerTimeoutMs;
    ReadSc();
    return true;
}

void Link::ReadSc() {
    tl_sc_state s = {};
    int rc = SC_IPC_BUSY;
    for (int i = 0; i < 4 && rc == SC_IPC_BUSY; ++i) rc = sc_ipc_block_read(&ch_, TL_OFF_SC_STATE, &s, sizeof(s));
    sc_.fresh = rc == SC_IPC_OK;
    if (!sc_.fresh) {
        if (rc == SC_IPC_MISMATCH && !(skipped_ & kSkipSize)) {
            skipped_ |= kSkipSize;
            Logf("sc-offline's state block has another size; ignoring it");
        }
        if (rc != SC_IPC_BUSY) sc_ = {};  // never written, or unusable: nothing to act on
        return;
    }
    ScView v;
    v.fresh = true;
    v.flags = s.flags & kScFlags;
    v.buttons = (v.flags & TL_SC_PILOT) && (v.flags & TL_SC_FOCUSED) ? s.buttons & kButtons : 0;
    v.pose_ok = (v.flags & TL_SC_ANCHORED) && FiniteIn(s.eye, 3, kMaxCoord) && FiniteIn(s.feet, 3, kMaxCoord) &&
                FiniteIn(s.vel, 3, kMaxSpeed) && FiniteIn(s.ang, 3, 3600.0f);
    if (v.pose_ok) {
        memcpy(v.eye, s.eye, sizeof(v.eye));
        memcpy(v.feet, s.feet, sizeof(v.feet));
        memcpy(v.vel, s.vel, sizeof(v.vel));
        v.ang[0] = Clamp(WrapDeg(s.ang[0]), -89.0f, 89.0f);
        v.ang[1] = WrapDeg(s.ang[1]);
        v.ang[2] = WrapDeg(s.ang[2]);
    } else {
        v.flags &= ~TL_SC_ANCHORED;
    }
    v.fov_scale = std::isfinite(s.fov_scale) && s.fov_scale >= 0.6f && s.fov_scale <= 2.0f ? s.fov_scale : 1.0f;
    v.view_w = s.view_w <= 16384 ? s.view_w : 0;
    v.view_h = s.view_h <= 16384 ? s.view_h : 0;
    sc_ = v;
}

bool Link::NextEvent(Event& out) {
    while (up_ && !inDead_) {
        uint32_t type = 0, n = sizeof(g_rec);
        const int rc = sc_ipc_ring_pop(&in_, &type, g_rec, &n);
        if (rc == SC_IPC_EMPTY || rc == SC_IPC_EPOCH || rc == SC_IPC_GONE) return false;  // Poll handles the last two
        if (rc != SC_IPC_OK) {
            inDead_ = true;
            Logf("sc-offline's message ring failed validation (sc_ipc result %d); ignoring it until the next link", rc);
            return false;
        }
        Event e;
        uint32_t skip = 0;
        switch (type) {
        case TL_MSG_START_MATCH: {
            if (n != sizeof(tl_msg_start_match)) { skip = kSkipSize; break; }
            tl_msg_start_match m;
            memcpy(&m, g_rec, sizeof(m));
            if (!CleanName(m.map, sizeof(m.map)) || !CleanName(m.mode, sizeof(m.mode)) || !KnownMap(m.map) ||
                !KnownMode(m.mode)) {
                skip = kSkipName;
                break;
            }
            if (startSeen_) { skip = kSkipRepeat; break; }
            startSeen_ = true;
            e.type = EventType::StartMatch;
            strcpy_s(e.map, m.map);
            strcpy_s(e.mode, m.mode);
            break;
        }
        case TL_MSG_CALL_TITAN: {
            if (n != sizeof(tl_msg_call_titan)) { skip = kSkipSize; break; }
            tl_msg_call_titan m;
            memcpy(&m, g_rec, sizeof(m));
            if (!FiniteIn(m.drop, 3, kMaxCoord) || !std::isfinite(m.yaw) || std::fabs(m.yaw) > 360.0f) { skip = kSkipValue; break; }
            e.type = EventType::CallTitan;
            memcpy(e.drop, m.drop, sizeof(e.drop));
            e.yaw = m.yaw;
            break;
        }
        case TL_MSG_EMBARK:
        case TL_MSG_DISEMBARK:
        case TL_MSG_PILOT_OFF:
            if (n != 0) { skip = kSkipSize; break; }
            e.type = type == TL_MSG_EMBARK ? EventType::Embark : type == TL_MSG_DISEMBARK ? EventType::Disembark : EventType::PilotOff;
            break;
        default:
            skip = kSkipType;
        }
        if (!skip) {
            out = e;
            return true;
        }
        if (!(skipped_ & skip)) {
            skipped_ |= skip;
            static const char* const why[] = { "", "of the wrong size", "of an unknown type", "with a map or mode this plugin doesn't know",
                                               "with values out of range", "repeating START_MATCH on the same link" };
            unsigned long bit = 0;
            _BitScanForward(&bit, skip);
            Logf("skipped a message %s (type %u, %u bytes); further ones like it are skipped quietly", why[bit + 1], type, n);
        }
    }
    return false;
}

void Link::PublishTf(const tl_tf_state& in) {
    if (!up_) return;
    tl_tf_state s = in;
    s.flags &= TL_TF_FLAGS;
    CleanVec(s.origin, 3);
    CleanVec(s.titan_origin, 3);
    CleanVec(s.floor_pt, 3);
    s.eye_z = Clamp(s.eye_z, -1000.0f, 1000.0f);
    s.titan_yaw = std::isfinite(s.titan_yaw) ? WrapDeg(s.titan_yaw) : 0.0f;
    s.damage = Clamp(s.damage, 0.0f, 100000.0f);
    s.titan_health = Clamp(s.titan_health, 0.0f, 1.0f);
    s.zoom_frac = Clamp(s.zoom_frac, 0.0f, 1.0f);
    s.zoom_fov = Clamp(s.zoom_fov, 0.0f, 180.0f);
    Printable(s.weapon, sizeof(s.weapon));
    sc_ipc_block_write(&ch_, TL_OFF_TF_STATE, &s, sizeof(s));
}

void Link::PublishFrame(const char* status) {
    if (!up_) return;
    tl_frame f = {};  // no shared picture (README: follow-ups): both handles 0
    strncpy_s(f.status, status ? status : "", _TRUNCATE);
    Printable(f.status, sizeof(f.status));
    sc_ipc_block_write(&ch_, TL_OFF_FRAME, &f, sizeof(f));
}

void Link::Send(const char* text) {
    if (!up_ || !text || !*text) return;
    char line[TL_MSG_LOG_MAX + 1];
    strncpy_s(line, text, _TRUNCATE);
    Printable(line, sizeof(line));
    // Best effort: a full ring (sc-offline not reading) drops the line.
    sc_ipc_ring_push(&out_, TL_MSG_LOG, line, static_cast<uint32_t>(strlen(line)));
}

}  // namespace tl
