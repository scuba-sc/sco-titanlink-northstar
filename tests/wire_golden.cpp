// SPDX-License-Identifier: MIT
// Independent little-endian wire fixtures plus gaps not covered by link_selftest.cpp.
// Expected words/offsets below are literals, not sizeof/offsetof or values read from the SDK.
// Runs the real link.cpp with Win32 mappings on Windows; see portable/windows.h for POSIX limits.
#include "link.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) throw std::runtime_error(#c); } while (0)
std::vector<std::string> logs;
void Log(const char* text) { logs.emplace_back(text); }
size_t LogCount(const char* text) {
    size_t count = 0;
    for (const auto& line : logs) if (line.find(text) != std::string::npos) ++count;
    return count;
}

// Encode independently specified 32-bit words; zero-fill the rest of the fixed golden size.
// These are IEEE-754 binary32 bit patterns, not native float serialization or SDK constants.
template <size_t N> std::array<uint8_t, N> Words(std::initializer_list<uint32_t> words) {
    std::array<uint8_t, N> bytes{};
    CHECK(words.size() * 4 <= N);
    size_t at = 0;
    for (uint32_t word : words) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes[at++] = static_cast<uint8_t>(word >> shift);
    }
    return bytes;
}
template <size_t N> void Equal(const void* actual, size_t size, const std::array<uint8_t, N>& expected) {
    CHECK(size == N);
    CHECK(std::memcmp(actual, expected.data(), N) == 0);
}

wchar_t kName[96]{};
struct Owner {
    HANDLE map = nullptr;
    uint8_t* base = nullptr;
    sc_ipc_chan ch{};
    sc_ipc_ring_view to{}, from{};
    Owner() {
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, kName);
        if (!map) throw std::runtime_error("CreateFileMappingW");
        base = static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 65536));
        if (!base) { CloseHandle(map); throw std::runtime_error("MapViewOfFile"); }
        // A deterministic initial epoch and padding, even if a previous test mapping existed.
        std::memset(base, 0, 65536);
        Init();
    }
    ~Owner() { UnmapViewOfFile(base); CloseHandle(map); }
    void Init(uint64_t now = 10000) {
        CHECK(sc_ipc_init(base, 65536, 0x11223344, 0x314b4c54, 1, now, &ch) == SC_IPC_OK);
        CHECK(sc_ipc_ring_init(&ch, 0x1000, 0x4000, 1, &to) == SC_IPC_OK);
        CHECK(sc_ipc_ring_init(&ch, 0x5100, 0x4000, 2, &from) == SC_IPC_OK);
    }
    sc_ipc_hdr* Header() { return reinterpret_cast<sc_ipc_hdr*>(base); }
};

void HeaderAndRingBytes() {
    Owner owner;
    const auto header = Words<64>({
        0x5343494f, 1, 0x11223344, 0, 1, 0, 10000, 0,
        65536, 0, 0, 0, 0x314b4c54, 1, 1, 0
    });
    Equal(owner.base, sizeof(sc_ipc_hdr), header);
    auto ring = Words<192>({0x53435247, 1, 0x4000, 0, 1, 0});
    Equal(owner.base + 0x1000, sizeof(sc_ipc_ring), ring);
    ring[4] = 2;
    Equal(owner.base + 0x5100, sizeof(sc_ipc_ring), ring);
    static_assert(SC_IPC_VERSION == 1 && TL_LAYOUT_ID == 0x314b4c54 && TL_LAYOUT_VERSION == 1);
    static_assert(TL_CHANNEL_BYTES == 65536 && TL_OFF_SC_STATE == 0x100 && TL_OFF_TF_STATE == 0x400);
    static_assert(TL_OFF_FRAME == 0x600 && TL_OFF_TO_PEER == 0x1000 && TL_OFF_FROM_PEER == 0x5100);
    static_assert(TL_RING_TO_PEER == 0x4000 && TL_RING_FROM_PEER == 0x4000);
}

void StateAndBlockBytes() {
    Owner owner;
    const tl_sc_state state = {
        0x0102030405060708ull, 7, 0x3ff,
        {1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, 1, 1920, 1080, 0
    };
    const auto bytes = Words<80>({
        0x05060708, 0x01020304, 7, 0x3ff,
        0x3f800000, 0x40000000, 0x40400000, 0x40800000, 0x40a00000, 0x40c00000,
        0x40e00000, 0x41000000, 0x41100000, 0x41200000, 0x41300000, 0x41400000,
        0x3f800000, 1920, 1080, 0
    });
    Equal(&state, sizeof(state), bytes);
    CHECK(sc_ipc_block_write(&owner.ch, TL_OFF_SC_STATE, &state, sizeof(state)) == SC_IPC_OK);
    const auto block = Words<16>({2, 0, 80, 0});
    Equal(owner.base + 0x100, sizeof(sc_ipc_block), block);
    Equal(owner.base + 0x110, sizeof(state), bytes);
    // A peer sees the same independent bytes, not merely a write/read round trip.
    tl::Link link(kName, Log);
    CHECK(link.Poll(10000));
    CHECK(link.Sc().fresh && link.Sc().pose_ok && link.Sc().eye[1] == 2 && link.Sc().vel[2] == 12);
}

void PublishedBytes() {
    Owner owner;
    tl::Link link(kName, Log);
    CHECK(link.Poll(10000));
    const tl_tf_state state = {
        0xf, 0x12345678, {1, 2, 3}, 4, {5, 6, 7}, 8, -1, 0x10203040,
        9, 1, {10, 11, 12}, 1, 70, "fixture"
    };
    auto bytes = Words<124>({
        0xf, 0x12345678, 0x3f800000, 0x40000000, 0x40400000, 0x40800000,
        0x40a00000, 0x40c00000, 0x40e00000, 0x41000000, 0xffffffff, 0x10203040,
        0x41100000, 0x3f800000, 0x41200000, 0x41300000, 0x41400000, 0x3f800000, 0x428c0000
    });
    std::memcpy(bytes.data() + 76, "fixture", 7);
    Equal(&state, sizeof(state), bytes);
    link.PublishTf(state);
    Equal(owner.base + 0x400, sizeof(sc_ipc_block), Words<16>({2, 0, 124, 0}));
    Equal(owner.base + 0x410, sizeof(state), bytes);

    const tl_frame frame = {{0x0102030405060708ull, 0x1112131415161718ull},
        {0x21222324, 0x31323334}, 1920, 1080, 3, 0x41424344, "frame"};
    auto frameBytes = Words<136>({0x05060708, 0x01020304, 0x15161718, 0x11121314,
        0x21222324, 0x31323334, 1920, 1080, 3, 0x41424344});
    std::memcpy(frameBytes.data() + 40, "frame", 5);
    Equal(&frame, sizeof(frame), frameBytes);
    link.PublishFrame("wire\n");
    auto published = Words<136>({});
    std::memcpy(published.data() + 40, "wire?", 5);
    Equal(owner.base + 0x600, sizeof(sc_ipc_block), Words<16>({2, 0, 136, 0}));
    Equal(owner.base + 0x610, sizeof(tl_frame), published);

    link.Send("ok");
    Equal(owner.from.data, 16, Words<16>({2, 1, 0x00006b6f, 0}));
}

void CommandBytes() {
    Owner owner;
    tl_msg_start_match start{};
    std::memcpy(start.map, "mp_coliseum", 11);
    std::memcpy(start.mode, "tdm", 3);
    std::array<uint8_t, 96> payload{};
    std::memcpy(payload.data(), "mp_coliseum", 11);
    std::memcpy(payload.data() + 64, "tdm", 3);
    Equal(&start, sizeof(start), payload);
    CHECK(sc_ipc_ring_push(&owner.to, TL_MSG_START_MATCH, &start, sizeof(start)) == SC_IPC_OK);
    auto startBytes = Words<104>({96, 1});
    std::memcpy(startBytes.data() + 8, payload.data(), 96);
    Equal(owner.to.data, 104, startBytes);
    const tl_msg_call_titan call = {{1, 2, 3}, 90};
    Equal(&call, sizeof(call), Words<16>({0x3f800000, 0x40000000, 0x40400000, 0x42b40000}));
    CHECK(sc_ipc_ring_push(&owner.to, TL_MSG_CALL_TITAN, &call, sizeof(call)) == SC_IPC_OK);
    Equal(owner.to.data + 104, 24, Words<24>({16, 2, 0x3f800000, 0x40000000, 0x40400000, 0x42b40000}));
    auto ringBytes = Words<192>({0x53435247, 1, 0x4000, 0, 1, 0});
    ringBytes[64] = 128; // head: 104-byte START_MATCH plus 24-byte CALL_TITAN
    ringBytes[72] = 2;   // pushed
    Equal(owner.base + 0x1000, sizeof(sc_ipc_ring), ringBytes);
    tl::Link link(kName, Log);
    CHECK(link.Poll(10000));
    tl::Event event;
    CHECK(link.NextEvent(event) && event.type == tl::EventType::StartMatch);
    CHECK(!std::strcmp(event.map, "mp_coliseum") && !std::strcmp(event.mode, "tdm"));
    CHECK(link.NextEvent(event) && event.type == tl::EventType::CallTitan && event.drop[1] == 2 && event.yaw == 90);
    CHECK(!link.NextEvent(event));
    ringBytes[128] = 128; // tail
    ringBytes[136] = 2;   // popped
    Equal(owner.base + 0x1000, sizeof(sc_ipc_ring), ringBytes);
}

void VersionRefusal() {
    // Literal header byte offsets also test wire version and layout id, missing in link_selftest.
    for (size_t offset : {4u, 48u, 52u}) {
        logs.clear();
        Owner owner;
        owner.base[offset] ^= 0x80;
        tl::Link link(kName, Log);
        CHECK(!link.Poll(10000) && link.Refused() && !link.Attached() && !link.OwnerAlive());
        CHECK(owner.Header()->peer_pid == 0 && owner.Header()->peer_heartbeat_ms == 0);
        CHECK(!link.TakeNewLink());
        CHECK(!link.Poll(10001)); // attach throttle
        CHECK(!link.Poll(12000)); // same incompatible tuple: no duplicate diagnostic
        CHECK(LogCount("REFUSING to attach") == 1);
        CHECK(owner.Header()->peer_pid == 0 && owner.Header()->peer_heartbeat_ms == 0);
        owner.Init(14000);
        CHECK(link.Poll(14000) && !link.Refused() && link.OwnerAlive());
        CHECK(link.TakeNewLink() && !link.TakeNewLink());
    }
}

void HeartbeatBoundaryAndRestart() {
    Owner owner;
    tl::Link link(kName, Log);
    CHECK(link.Poll(10000) && link.OwnerAlive());
    CHECK(link.TakeNewLink());
    CHECK(link.Poll(13999) && link.OwnerAlive());
    CHECK(link.Poll(14000) && !link.OwnerAlive()); // exactly 4000 ms, not only beyond it
    CHECK(link.Attached() && !link.TakeClosed() && !link.TakeNewLink());
    sc_ipc_owner_beat(&owner.ch, 14001);
    CHECK(link.Poll(14001) && link.OwnerAlive() && !link.TakeNewLink());
    sc_ipc_owner_beat(&owner.ch, 0);
    CHECK(link.Poll(14002) && !link.OwnerAlive()); // never beaten is not alive
    owner.Init(14003); // live epoch change, no close and no two-second retry wait
    CHECK(link.Poll(14003) && link.OwnerAlive());
    CHECK(link.TakeNewLink() && !link.TakeNewLink() && !link.TakeClosed());
    CHECK(link.Sc().flags == 0 && !link.Sc().fresh);
}

void TornAndWrongSizeBlock() {
    logs.clear();
    Owner owner;
    tl::Link link(kName, Log);
    tl_sc_state state{};
    state.flags = 7;
    state.buttons = 1;
    state.eye[0] = 42;
    state.fov_scale = 1;
    CHECK(sc_ipc_block_write(&owner.ch, 0x100, &state, sizeof(state)) == SC_IPC_OK);
    CHECK(link.Poll(10000) && link.Sc().pose_ok && link.Sc().buttons == 1);
    auto* block = reinterpret_cast<sc_ipc_block*>(owner.base + 0x100);
    block->seq = 3; // interrupted writer; no usable new snapshot
    state.eye[0] = 99;
    std::memcpy(owner.base + 0x110, &state, sizeof(state));
    CHECK(link.Poll(10001) && !link.Sc().fresh && link.Sc().eye[0] == 42);
    block->size = 79;
    block->seq = 4;
    CHECK(link.Poll(10002) && !link.Sc().fresh && !link.Sc().pose_ok && link.Sc().buttons == 0);
    CHECK(link.Sc().flags == 0 && link.Sc().eye[0] == 0);
    CHECK(link.Poll(10003) && LogCount("state block has another size") == 1);
    CHECK(sc_ipc_block_write(&owner.ch, 0x100, &state, sizeof(state)) == SC_IPC_OK);
    CHECK(link.Poll(10004) && link.Sc().fresh && link.Sc().eye[0] == 99);
}

void CloseClearsAndStopsWrites() {
    Owner owner;
    tl::Link link(kName, Log);
    tl_sc_state state{};
    state.flags = 7;
    state.buttons = 1;
    CHECK(sc_ipc_block_write(&owner.ch, 0x100, &state, sizeof(state)) == SC_IPC_OK);
    CHECK(link.Poll(10000) && link.Sc().buttons == 1);
    CHECK(sc_ipc_ring_push(&owner.to, 3, nullptr, 0) == SC_IPC_OK);
    sc_ipc_close(&owner.ch);
    CHECK(!link.Poll(10001) && !link.Attached() && !link.OwnerAlive());
    CHECK(link.Sc().buttons == 0 && link.Sc().flags == 0 && !link.Sc().pose_ok && !link.Sc().fresh);
    CHECK(link.TakeClosed() && !link.TakeClosed());
    const std::vector<uint8_t> before(owner.base, owner.base + 65536);
    tl::Event event;
    CHECK(!link.NextEvent(event));
    link.PublishTf(tl_tf_state{});
    link.PublishFrame("must not write");
    link.Send("must not write");
    CHECK(!link.Poll(12000) && !link.TakeClosed());
    CHECK(std::memcmp(owner.base, before.data(), before.size()) == 0);
}

void MalformedFrames() {
    // Raw frame bytes bypass ring_push, which cannot manufacture impossible frames.
    // Each damaged ring is latched dead even after repair, and recovers only under a new epoch.
    for (int kind = 0; kind < 7; ++kind) {
        logs.clear();
        Owner owner;
        tl::Link link(kName, Log);
        CHECK(link.Poll(10000));
        const auto good = Words<8>({0, 3});
        std::memcpy(owner.to.data, good.data(), 8);
        auto* ring = owner.to.ring;
        ring->head = 8;
        switch (kind) {
        case 0: { // complete record header, incomplete 16-byte payload
            const auto raw = Words<16>({16, 2, 0, 0});
            std::memcpy(owner.to.data, raw.data(), 16);
            ring->head = 16;
            break;
        }
        case 1: { // huge advertised payload must not wrap its required length
            const auto raw = Words<8>({0xffffffff, 2});
            std::memcpy(owner.to.data, raw.data(), 8);
            break;
        }
        case 2: { // impossible padding size, not the remainder of the ring
            const auto raw = Words<8>({0, 0xffffffff});
            std::memcpy(owner.to.data, raw.data(), 8);
            break;
        }
        case 3: // two consecutive PAD frames
            ring->tail = 0x3ff8;
            ring->head = 0x4008;
            std::memcpy(owner.to.data + 0x3ff8, Words<8>({0, 0xffffffff}).data(), 8);
            std::memcpy(owner.to.data, Words<8>({0x3ff8, 0xffffffff}).data(), 8);
            break;
        case 4: ring->head = 0x4008; break; // used bytes exceed capacity
        case 5: ring->tail = 16; break;     // tail ahead of head: unsigned underflow
        case 6: ring->tail = 1; break;      // unaligned consumer counter
        }
        tl::Event event;
        event.type = tl::EventType::CallTitan;
        event.yaw = 77;
        CHECK(!link.NextEvent(event));
        CHECK(event.type == tl::EventType::CallTitan && event.yaw == 77);
        CHECK(LogCount("message ring failed validation") == 1);
        ring->head = 8;
        ring->tail = 0;
        std::memcpy(owner.to.data, good.data(), 8);
        CHECK(!link.NextEvent(event) && LogCount("message ring failed validation") == 1);
        owner.Init(10001);
        CHECK(link.Poll(10001) && link.TakeNewLink());
        CHECK(sc_ipc_ring_push(&owner.to, 3, nullptr, 0) == SC_IPC_OK);
        CHECK(link.NextEvent(event) && event.type == tl::EventType::Embark);
    }
}

void WrappedFrameBytes() {
    Owner owner;
    tl::Link link(kName, Log);
    CHECK(link.Poll(10000));
    // Only eight bytes remain at the end: CALL_TITAN writes a PAD then its literal 24 bytes.
    owner.to.ring->head = 0x3ff8;
    owner.to.ring->tail = 0x3ff8;
    const tl_msg_call_titan call = {{1, 2, 3}, 90};
    CHECK(sc_ipc_ring_push(&owner.to, 2, &call, sizeof(call)) == SC_IPC_OK);
    Equal(owner.to.data + 0x3ff8, 8, Words<8>({0, 0xffffffff}));
    Equal(owner.to.data, 24, Words<24>({16, 2, 0x3f800000, 0x40000000, 0x40400000, 0x42b40000}));
    tl::Event event;
    CHECK(link.NextEvent(event) && event.type == tl::EventType::CallTitan && event.yaw == 90);
    CHECK(!link.NextEvent(event) && owner.to.ring->tail == 0x4018);
}
} // namespace

int main() {
    std::swprintf(kName, sizeof(kName) / sizeof(kName[0]), L"Local\\SCO_titanlink_wire_golden_%lu", GetCurrentProcessId());
    struct Case { const char* name; void (*run)(); };
    const Case cases[] = {
        {"header and ring golden bytes", HeaderAndRingBytes},
        {"state and block golden bytes", StateAndBlockBytes},
        {"published TF, frame and log golden bytes", PublishedBytes},
        {"command golden bytes", CommandBytes},
        {"wire/layout refusal and recovery", VersionRefusal},
        {"heartbeat boundary and live restart", HeartbeatBoundaryAndRestart},
        {"torn and wrong-size state blocks", TornAndWrongSizeBlock},
        {"close clears state and prevents writes", CloseClearsAndStopsWrites},
        {"malformed raw frames and epoch recovery", MalformedFrames},
        {"wrapped frame golden bytes", WrappedFrameBytes},
    };
    int passed = 0;
    for (const auto& test : cases) {
        try { test.run(); ++passed; std::printf("PASS %s\n", test.name); }
        catch (const std::exception& error) { std::printf("FAIL %s: %s\n", test.name, error.what()); }
    }
    std::printf("wire_golden: %d/%zu cases passed, %d checks\n", passed, sizeof(cases) / sizeof(cases[0]), checks);
    return passed == static_cast<int>(sizeof(cases) / sizeof(cases[0])) ? 0 : 1;
}
