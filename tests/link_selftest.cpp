// link_selftest: drives link.cpp against a TitanLink channel this test creates itself (playing
// sc-offline, the owner, through sc_ipc.h), with hostile values: a layout mismatch, a state block
// full of NaNs and stray bits, malformed and unknown messages, a corrupted ring, and a close.
// Time is passed in explicitly, so nothing here waits.
#include "link.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

int g_fail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            ++g_fail;                                               \
        }                                                           \
    } while (0)

int g_refusals = 0;
void Log(const char* line) {
    printf("  link: %s\n", line);
    if (strstr(line, "REFUSING")) ++g_refusals;
}

struct Owner {
    HANDLE           map = nullptr;
    uint8_t*         base = nullptr;
    sc_ipc_chan      ch = {};
    sc_ipc_ring_view to = {}, from = {};

    bool Create(const wchar_t* name) {
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, TL_CHANNEL_BYTES, name);
        base = map ? static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, TL_CHANNEL_BYTES)) : nullptr;
        return base != nullptr;
    }
    bool Init(uint32_t layoutVersion, uint64_t now) {
        return sc_ipc_init(base, TL_CHANNEL_BYTES, GetCurrentProcessId(), TL_LAYOUT_ID, layoutVersion, now, &ch) == SC_IPC_OK &&
               sc_ipc_ring_init(&ch, TL_OFF_TO_PEER, TL_RING_TO_PEER, SC_IPC_TO_PEER, &to) == SC_IPC_OK &&
               sc_ipc_ring_init(&ch, TL_OFF_FROM_PEER, TL_RING_FROM_PEER, SC_IPC_FROM_PEER, &from) == SC_IPC_OK;
    }
    void Push(uint32_t type, const void* p, uint32_t n) { CHECK(sc_ipc_ring_push(&to, type, p, n) == SC_IPC_OK); }
    void State(const tl_sc_state& s) { CHECK(sc_ipc_block_write(&ch, TL_OFF_SC_STATE, &s, sizeof(s)) == SC_IPC_OK); }
};

tl_msg_start_match Start(const char* map, const char* mode) {
    tl_msg_start_match m = {};
    strncpy_s(m.map, map, _TRUNCATE);
    strncpy_s(m.mode, mode, _TRUNCATE);
    return m;
}

}  // namespace

int main() {
    wchar_t name[96];
    swprintf_s(name, L"Local\\SCO_titanlink_selftest_%lu", GetCurrentProcessId());
    Owner o;
    if (!o.Create(name)) { printf("FAIL: could not create the test mapping\n"); return 1; }
    uint64_t t = 1000000;
    tl::Link link(name, &Log);

    // 1. A channel of another layout version is refused, loudly, and never attached.
    CHECK(o.Init(TL_LAYOUT_VERSION + 1, t));
    CHECK(!link.Poll(t));
    CHECK(link.Refused());
    CHECK(g_refusals == 1);
    CHECK(!link.Attached());
    // The same mismatch again is not logged again.
    t += 3000;
    CHECK(!link.Poll(t));
    CHECK(g_refusals == 1);

    // 2. The right layout attaches (after the attach throttle) and counts as a new link.
    CHECK(o.Init(TL_LAYOUT_VERSION, t));
    t += 3000;
    sc_ipc_owner_beat(&o.ch, t);
    CHECK(link.Poll(t));
    CHECK(link.Attached() && !link.Refused());
    CHECK(link.TakeNewLink());
    CHECK(link.OwnerAlive());
    uint32_t peerAge = 99;
    CHECK(sc_ipc_peer_age_ms(&o.ch, t, &peerAge) == SC_IPC_OK && peerAge == 0);  // we beat

    // 3. A hostile state block: every bit set, NaN eye, so no pose and only known bits.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    tl_sc_state s = {};
    s.flags = 0xFFFFFFFFu;
    s.buttons = 0xFFFFFFFFu;
    s.eye[0] = nan;
    s.fov_scale = 50.0f;
    s.view_w = 1u << 30;
    o.State(s);
    CHECK(link.Poll(t));
    CHECK(link.Sc().fresh);
    CHECK(link.Sc().flags == (TL_SC_PILOT | TL_SC_FOCUSED));
    CHECK(!link.Sc().pose_ok);
    CHECK(link.Sc().buttons == 0x3FFu);
    CHECK(link.Sc().fov_scale == 1.0f);
    CHECK(link.Sc().view_w == 0);
    CHECK(std::isfinite(link.Sc().eye[0]));

    // A good pose with pitch past straight down: valid, pitch clamped, yaw wrapped.
    s = {};
    s.flags = TL_SC_PILOT | TL_SC_ANCHORED;  // not in front: no buttons
    s.buttons = TL_BTN_ATTACK;
    s.ang[0] = 120.0f;
    s.ang[1] = 540.0f;
    s.feet[0] = 100.0f;
    s.fov_scale = 1.2f;
    o.State(s);
    CHECK(link.Poll(t));
    CHECK(link.Sc().pose_ok);
    CHECK(link.Sc().buttons == 0);
    CHECK(link.Sc().ang[0] == 89.0f);
    CHECK(std::fabs(link.Sc().ang[1] - (-180.0f)) < 0.001f);
    CHECK(link.Sc().feet[0] == 100.0f);
    // A coordinate past +-1,000,000 makes the pose invalid.
    s.feet[2] = 2.0e6f;
    o.State(s);
    CHECK(link.Poll(t));
    CHECK(!link.Sc().pose_ok && !(link.Sc().flags & TL_SC_ANCHORED));

    // 4. Messages: only the valid ones come out, in order.
    tl_msg_start_match m = Start("mp_coliseum", "tdm");
    o.Push(TL_MSG_START_MATCH, &m, sizeof(m) - 1);                 // wrong size
    tl_msg_start_match bad = Start("mp_x;quit", "tdm");
    o.Push(TL_MSG_START_MATCH, &bad, sizeof(bad));                  // not [a-z0-9_]
    bad = Start("mp_nowhere", "tdm");
    o.Push(TL_MSG_START_MATCH, &bad, sizeof(bad));                  // not a known map
    memset(&bad, 'a', sizeof(bad));
    o.Push(TL_MSG_START_MATCH, &bad, sizeof(bad));                  // no NUL
    o.Push(77, "x", 1);                                             // unknown type
    tl_msg_call_titan ct = { { std::numeric_limits<float>::infinity(), 0, 0 }, 0 };
    o.Push(TL_MSG_CALL_TITAN, &ct, sizeof(ct));                     // infinite drop
    o.Push(TL_MSG_EMBARK, "abcd", 4);                               // payload where none belongs
    o.Push(TL_MSG_START_MATCH, &m, sizeof(m));                      // valid
    o.Push(TL_MSG_START_MATCH, &m, sizeof(m));                      // repeat on the same link
    ct = { { 10, 20, 30 }, 90 };
    o.Push(TL_MSG_CALL_TITAN, &ct, sizeof(ct));                     // valid
    o.Push(TL_MSG_PILOT_OFF, nullptr, 0);                           // valid
    tl::Event e;
    CHECK(link.NextEvent(e) && e.type == tl::EventType::StartMatch && !strcmp(e.map, "mp_coliseum") && !strcmp(e.mode, "tdm"));
    CHECK(link.NextEvent(e) && e.type == tl::EventType::CallTitan && e.drop[2] == 30.0f && e.yaw == 90.0f);
    CHECK(link.NextEvent(e) && e.type == tl::EventType::PilotOff);
    CHECK(!link.NextEvent(e));

    // 5. What we publish is cleaned: flags masked, NaN coordinates zeroed, text printable.
    tl_tf_state tf = {};
    tf.flags = 0xFFu;
    tf.origin[1] = nan;
    tf.titan_health = 7.0f;
    strcpy_s(tf.weapon, "gun\x01");
    link.PublishTf(tf);
    tl_tf_state got = {};
    CHECK(sc_ipc_block_read(&o.ch, TL_OFF_TF_STATE, &got, sizeof(got)) == SC_IPC_OK);
    CHECK(got.flags == TL_TF_FLAGS && got.origin[1] == 0.0f && got.titan_health == 1.0f && !strcmp(got.weapon, "gun?"));
    link.Send("hello\x7f");
    char line[TL_MSG_LOG_MAX + 1] = {};
    uint32_t type = 0, n = TL_MSG_LOG_MAX;
    CHECK(sc_ipc_ring_pop(&o.from, &type, line, &n) == SC_IPC_OK && type == TL_MSG_LOG && n == 6 && !memcmp(line, "hello?", 6));

    // 6. A corrupted ring (a misaligned head) is ignored from then on, even once it looks fine.
    sc_ipc_ring* ring = reinterpret_cast<sc_ipc_ring*>(o.base + TL_OFF_TO_PEER);
    const uint64_t head = ring->head;
    ring->head = head + 3;
    CHECK(!link.NextEvent(e));
    ring->head = head;
    o.Push(TL_MSG_PILOT_OFF, nullptr, 0);
    CHECK(!link.NextEvent(e));

    // 7. The owner's heartbeat stops: attached, but not alive.
    t += tl::kOwnerTimeoutMs + 1;
    CHECK(link.Poll(t));
    CHECK(!link.OwnerAlive());

    // 8. The owner closes the channel: the link sees it once and drops.
    sc_ipc_close(&o.ch);
    CHECK(!link.Poll(t));
    CHECK(link.TakeClosed());
    CHECK(!link.TakeClosed());
    CHECK(!link.Attached());

    // 9. Re-created (a new epoch): attaches again as a new link, and START_MATCH is allowed again.
    t += 3000;
    CHECK(o.Init(TL_LAYOUT_VERSION, t));
    sc_ipc_owner_beat(&o.ch, t);
    CHECK(link.Poll(t));
    CHECK(link.TakeNewLink());
    o.Push(TL_MSG_START_MATCH, &m, sizeof(m));
    CHECK(link.NextEvent(e) && e.type == tl::EventType::StartMatch);

    // 10. The map and mode lists.
    CHECK(tl::KnownMap("mp_coliseum") && tl::KnownMap("mp_angel_city") && !tl::KnownMap("mp_lobby"));
    CHECK(tl::KnownMode("tdm") && tl::KnownMode("aitdm") && !tl::KnownMode("solo"));

    link.Drop();
    UnmapViewOfFile(o.base);
    CloseHandle(o.map);
    if (g_fail) printf("link_selftest: %d FAILED\n", g_fail);
    else printf("link_selftest: all passed\n");
    return g_fail ? 1 : 0;
}
