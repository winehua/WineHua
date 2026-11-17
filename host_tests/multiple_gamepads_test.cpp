#include "input/controller/controller_hub.h"
#include "input/controller/gamepad_bridge.h"
#include "input/controller/gamepad_ipc_protocol.h"
#include "input/controller/physical_gamepad.h"

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

using namespace winehua::controller;

static void ReadExact(int fd, void* output, size_t length)
{
    auto* bytes = static_cast<char*>(output);
    while (length) {
        pollfd wait{fd, POLLIN, 0};
        assert(poll(&wait, 1, 1000) == 1);
        const auto got = recv(fd, bytes, length, 0);
        assert(got > 0);
        length -= got;
        bytes += got;
    }
}

static whgp_state_v2 ReadState(int fd, uint32_t slot)
{
    whgp_header header{};
    whgp_state_v2 state{};
    ReadExact(fd, &header, sizeof(header));
    assert(header.magic == WHGP_MAGIC && header.version == WHGP_VERSION);
    assert(header.slot == slot && header.msg_type == WHGP_MSG_STATE);
    assert(header.payload_size == sizeof(state));
    ReadExact(fd, &state, sizeof(state));
    assert(state.reserved[0] & WHGP_STATE_TOPOLOGY_VALID);
    return state;
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    auto& hub = ControllerHub::Instance();
    auto& bridge = GamepadBridge::Instance();
    hub.SetEnabled(true);
    PhysicalResetDevices();
    // Devices already paired before app/Wine startup must all be enumerated.
    PhysicalSyncDevices({"101", "202", "303", "404"});
    assert(PhysicalDeviceForSlot(0) == "101");
    assert(PhysicalDeviceForSlot(1) == "202");
    assert(PhysicalDeviceForSlot(2) == "303");
    assert(PhysicalDeviceForSlot(3) == "404");
    PhysicalFeedButton("101", 2301, true);
    PhysicalFeedButton("202", 2302, true);
    PhysicalFeedAxis("303", 0, -1, -1);
    PhysicalFeedAxis("404", 4, 1, 0);
    PhysicalFeedButton("overflow", 2305, true);
    assert(hub.GetState(0).buttons == ButtonBit(LogicalButton::A));
    assert(hub.GetState(1).buttons == ButtonBit(LogicalButton::B));
    assert(hub.GetState(2).lx < 0 && hub.GetState(2).ly > 0);
    assert(hub.GetState(3).rt == 32767);

    bridge.AttachToHub();
    assert(bridge.Start(argv[1]));
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(strlen(argv[1]) < sizeof(address.sun_path));
    strcpy(address.sun_path, argv[1]);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    for (uint32_t slot = 0; slot < WHGP_MAX_SLOTS; ++slot)
        assert(ReadState(fd, slot).reserved[0] & WHGP_STATE_CONNECTED);

    PhysicalFeedButton("202", 2302, false);
    assert(ReadState(fd, 1).buttons == 0);
    assert(hub.GetState(0).buttons == ButtonBit(LogicalButton::A));
    PhysicalFeedButton("202", 2304, true);
    assert(ReadState(fd, 1).buttons == ButtonBit(LogicalButton::X));
    PhysicalFeedDevice("202", false);
    assert(ReadState(fd, 1).buttons == 0);
    assert(!(ReadState(fd, 1).reserved[0] & WHGP_STATE_CONNECTED));
    assert(hub.GetState(0).buttons == ButtonBit(LogicalButton::A));
    assert(hub.GetState(2).lx < 0 && hub.GetState(3).rt == 32767);
    // A late callback from a disconnected pad must not take a new slot.
    PhysicalFeedButton("202", 2305, true);
    assert(PhysicalDeviceForSlot(1).empty());
    PhysicalFeedDevice("replacement", true);
    assert(ReadState(fd, 1).reserved[0] & WHGP_STATE_CONNECTED);
    assert(PhysicalDeviceForSlot(2) == "303");
    assert(PhysicalDeviceForSlot(3) == "404");

    // Slot-zero touch and physical ownership still coexist after unplugging P1.
    hub.SetButton(ControllerSourceId::Touch, 0, LogicalButton::A, true);
    PhysicalFeedDevice("101", false);
    assert(hub.GetState(0).buttons == ButtonBit(LogicalButton::A));
    assert(hub.GetState(0).connected);
    hub.ResetSource(ControllerSourceId::Touch);
    assert(ReadState(fd, 0).buttons == 0);

    std::atomic<bool> rumble{false};
    bridge.SetRumbleListener([&](uint32_t slot, uint16_t low, uint16_t high, uint32_t duration) {
        assert(slot == 3 && low == 123 && high == 456 && duration == 250);
        assert(PhysicalDeviceForSlot(slot) == "404");
        rumble = true;
    });
    struct Packet { whgp_header header; whgp_rumble_v1 body; } packet{
        {WHGP_MAGIC, WHGP_VERSION, WHGP_MSG_RUMBLE, 3, sizeof(whgp_rumble_v1)},
        {123, 456, 250}};
    assert(send(fd, &packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet));
    for (unsigned i = 0; !rumble.load() && i < 100; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(rumble.load());
    bridge.Stop();
    close(fd);
    bridge.SetRumbleListener(nullptr);
    hub.SetStateListener(nullptr);
    PhysicalSyncDevices({});
    for (uint32_t slot = 0; slot < WHGP_MAX_SLOTS; ++slot) {
        assert(hub.GetState(slot).buttons == 0);
        assert(hub.GetState(slot).connected == (slot == 0));
    }
    puts("PASS: four pads, initial snapshots, independent input, overflow, unplug, late events, slot reuse, touch and targeted rumble");
}
