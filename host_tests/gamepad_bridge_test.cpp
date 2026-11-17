#include "input/controller/gamepad_bridge.h"
#include "input/controller/gamepad_ipc_protocol.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using winehua::controller::GamepadBridge;
using winehua::controller::LogicalGamepadState;

static void ReadExact(int fd, void* buffer, size_t length)
{
    auto* bytes = static_cast<char*>(buffer);
    while (length) {
        pollfd wait{fd, POLLIN, 0};
        assert(poll(&wait, 1, 1000) == 1);
        const auto got = recv(fd, bytes, length, 0);
        assert(got > 0);
        length -= got;
        bytes += got;
    }
}

static whgp_state_v2 ReadState(int fd, uint32_t slot = 0)
{
    whgp_header header{};
    ReadExact(fd, &header, sizeof(header));
    assert(header.magic == WHGP_MAGIC && header.version == WHGP_VERSION);
    assert(header.msg_type == WHGP_MSG_STATE && header.slot == slot);
    assert(header.payload_size == sizeof(whgp_state_v2));
    whgp_state_v2 state{};
    ReadExact(fd, &state, sizeof(state));
    return state;
}

static int Connect(const char* path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    for (uint32_t slot = 0; slot < WHGP_MAX_SLOTS; ++slot) ReadState(fd, slot);
    return fd;
}

static void WaitForClose(int fd)
{
    char pending[4096];
    for (;;) {
        pollfd wait{fd, POLLIN, 0};
        assert(poll(&wait, 1, 1000) == 1);
        const auto count = recv(fd, pending, sizeof(pending), 0);
        if (count == 0) return;
        assert(count > 0);
    }
}

static void Rumble(int fd)
{
    struct Packet { whgp_header header; whgp_rumble_v1 body; } packet{
        {WHGP_MAGIC, WHGP_VERSION, WHGP_MSG_RUMBLE, 0, sizeof(whgp_rumble_v1)},
        {12, 34, 56}};
    assert(send(fd, &packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet));
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    auto& bridge = GamepadBridge::Instance();
    std::atomic<unsigned> rumbles{0};
    bridge.SetRumbleListener([&](uint32_t slot, uint16_t low, uint16_t high, uint32_t ms) {
        assert(slot == 0);
        assert(low == 12 && high == 34 && ms == 56);
        ++rumbles;
    });
    assert(bridge.Start(argv[1]));
    assert(bridge.Start(argv[1]));
    const int first = Connect(argv[1]);
    int second = Connect(argv[1]);
    LogicalGamepadState state{};
    state.buttons = 1;
    bridge.PublishState(0, state);
    // This is the regression gate: the previous server disconnects first as
    // soon as second connects, before any state can be broadcast to both.
    assert(ReadState(first).buttons == 1);
    assert(ReadState(second).buttons == 1);
    state.buttons = 0;
    bridge.PublishState(0, state);
    assert(ReadState(first).buttons == 0);
    assert(ReadState(second).buttons == 0);
    Rumble(first);
    Rumble(second);
    for (unsigned i = 0; rumbles.load() != 2 && i < 100; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(rumbles.load() == 2);

    // Concurrent publishers must preserve whole WHGP packets for each peer.
    auto reader = [](int fd) {
        for (unsigned i = 0; i < 400; ++i) {
            const auto packet = ReadState(fd);
            assert(packet.buttons == 11 || packet.buttons == 22);
            assert(packet.lx == int16_t(packet.buttons * 10));
        }
    };
    std::thread readFirst(reader, first), readSecond(reader, second);
    auto writer = [&](uint32_t buttons) {
        LogicalGamepadState packet{};
        packet.buttons = buttons;
        packet.lx = int16_t(buttons * 10);
        for (unsigned i = 0; i < 200; ++i) bridge.PublishState(0, packet);
    };
    std::thread writeFirst(writer, 11), writeSecond(writer, 22);
    writeFirst.join(); writeSecond.join();
    readFirst.join(); readSecond.join();

    whgp_header bad{};
    assert(send(second, &bad, sizeof(bad), MSG_NOSIGNAL) == sizeof(bad));
    WaitForClose(second);
    close(second);
    state.buttons = 7;
    bridge.PublishState(0, state);
    assert(ReadState(first).buttons == 7);
    // Descriptor reuse and repeated connect/close must not evict a live peer
    // or deliver a stale sender's packet to the new connection.
    for (unsigned i = 0; i < 20; ++i) {
        second = Connect(argv[1]);
        close(second);
        bridge.PublishState(0, state);
        assert(ReadState(first).buttons == 7);
    }
    const int slow = Connect(argv[1]);
    const auto started = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 12000; ++i) {
        bridge.PublishState(0, state);
        assert(ReadState(first).buttons == 7);
    }
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    WaitForClose(slow);
    close(slow);

    // Stop must wake a receiver waiting halfway through a rumble header.
    second = Connect(argv[1]);
    const char partial = 'W';
    assert(send(second, &partial, 1, MSG_NOSIGNAL) == 1);
    std::atomic<bool> publishing{true};
    std::thread publisher([&] {
        while (publishing.load()) bridge.PublishState(0, state);
    });
    bridge.Stop();
    publishing = false;
    publisher.join();
    assert(!bridge.IsRunning());
    WaitForClose(first); WaitForClose(second);
    close(first); close(second);
    assert(bridge.Start(argv[1]));
    second = Connect(argv[1]);
    bridge.PublishState(0, state);
    assert(ReadState(second).buttons == 7);
    bridge.Stop();
    WaitForClose(second);
    close(second);
    bridge.SetRumbleListener(nullptr);
    puts("gamepad bridge: multiple peers, press/release, rumble, concurrent packets, disconnect, backpressure and restart passed");
}
