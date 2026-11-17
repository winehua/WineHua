// Exercise the production protocol/resource callbacks and event-loop transfers.
#include "input/data_device.cpp"
#include <cassert>
#include <iostream>
#include <sys/socket.h>

static void Dispatch() { assert(wl_event_loop_dispatch(wl_display_get_event_loop(display), 0) >= 0); }
static std::string Snapshot(uint64_t* v = nullptr) { char* text = WineClipboardSnapshot(v); std::string out(text); free(text); return out; }
static void ReadPipe(const std::string& text) {
    int p[2]; assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    assert(write(p[1], text.data(), text.size()) == static_cast<ssize_t>(text.size())); close(p[1]); Dispatch();
}
struct Wire { std::string bytes; std::vector<int> fds; };
// Inspect actual queued protocol events, including the source's SCM_RIGHTS fd.
static Wire Receive(wl_client* client, int peer) {
    wl_client_flush(client);
    char bytes[65536], control[CMSG_SPACE(8 * sizeof(int))];
    iovec io{bytes, sizeof(bytes)}; msghdr msg{};
    msg.msg_iov = &io; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(peer, &msg, MSG_DONTWAIT);
    if (n < 0) { assert(errno == EAGAIN); return {}; }
    assert(!(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
    Wire result{std::string(bytes, n), {}};
    for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        assert(c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS);
        auto* first = reinterpret_cast<int*>(CMSG_DATA(c));
        result.fds.insert(result.fds.end(), first, first + (c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    }
    return result;
}
static bool NullSelection(const Wire& wire, wl_resource* device) {
    for (size_t pos = 0; pos < wire.bytes.size();) {
        uint32_t header[2]; assert(pos + 8 <= wire.bytes.size()); memcpy(header, wire.bytes.data() + pos, 8);
        size_t size = header[1] >> 16; assert(size >= 8 && pos + size <= wire.bytes.size());
        if (header[0] == wl_resource_get_id(device) && (header[1] & 0xffff) == 5) {
            uint32_t id; assert(size == 12); memcpy(&id, wire.bytes.data() + pos + 8, 4);
            if (!id) return true;
        }
        pos += size;
    }
    return false;
}
int main() {
    wl_display* server = wl_display_create(); assert(server); WineClipboardRegister(server);
    int a[2], b[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, a));
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, b));
    auto* clientA = wl_client_create(server, a[0]); auto* clientB = wl_client_create(server, b[0]); assert(clientA && clientB);
    auto* surfaceA = wl_resource_create(clientA, &wl_surface_interface, 1, 0);
    auto* surfaceB = wl_resource_create(clientB, &wl_surface_interface, 1, 0);
    auto* seatA = wl_resource_create(clientA, &wl_seat_interface, 2, 0);
    auto* seatB = wl_resource_create(clientB, &wl_seat_interface, 2, 0);
    Bind(clientA, nullptr, 2, 0); auto* managerA = managers.back(); GetDevice(clientA, managerA, 0, seatA);
    Bind(clientB, nullptr, 2, 0); auto* managerB = managers.back(); GetDevice(clientB, managerB, 0, seatB);
    WineClipboardFocus(surfaceA); WineClipboardPost("host"); assert(Snapshot().empty()); Dispatch();
    assert(Snapshot() == "host");
    assert(!offers.empty()); for (auto* offer : offers) assert(wl_resource_get_client(offer->resource) == clientA);
    WineClipboardFocus(surfaceB); assert(wl_resource_get_client(offers.back()->resource) == clientB);
    // Host empty text remains a valid offer, and old offers retain their snapshot.
    auto* old = offers.front(); WineClipboardPost(""); Dispatch(); assert(*old->text == "host"); assert(offers.back()->text->empty());
    // Unsupported replacement must not expose an old offer to a newly focused client.
    CreateSource(clientB, managerB, 0); auto* unsupported = sources.rbegin()->first;
    SetSelection(clientB, devices.back(), unsupported, 1); assert(!hasSelection && !selectionText);
    wl_resource_destroy(unsupported);
    ReadPipe("guest"); assert(Snapshot() == "guest"); ReadPipe(""); assert(Snapshot().empty());
    assert(Receive(clientA, a[1]).fds.empty()); assert(Receive(clientB, b[1]).fds.empty());
    CreateSource(clientB, managerB, 0); auto* wineSource = sources.begin()->first;
    SourceOffer(clientB, wineSource, Utf8); SourceOffer(clientB, wineSource, WineTag);
    SetSelection(clientB, devices.back(), wineSource, 1);
    auto request = Receive(clientB, b[1]);
    assert(!NullSelection(request, devices.back())); // Never erase Wine's delayed-render owner before reading it.
    assert(request.fds.size() == 1 && wineSelection && !hasSelection);
    WineClipboardFocus(nullptr);
    assert(!NullSelection(Receive(clientB, b[1]), devices.back())); // Focus loss is not clipboard deletion.
    WineClipboardFocus(surfaceB);
    assert(!NullSelection(Receive(clientB, b[1]), devices.back())); // Pending read is not an empty selection.
    assert(write(request.fds[0], "wine guest", 10) == 10); close(request.fds[0]); Dispatch();
    assert(Snapshot() == "wine guest");
    auto exported = Receive(clientB, b[1]); assert(exported.bytes.find(WineTag) != std::string::npos);
    WineClipboardFocus(surfaceA);
    assert(Receive(clientA, a[1]).bytes.find(WineTag) != std::string::npos); // Other Wine process ignores its own export.
    WineClipboardPost("system"); Dispatch();
    assert(Receive(clientA, a[1]).bytes.find(WineTag) == std::string::npos && !wineSelection);
    wl_resource_destroy(wineSource); WineClipboardPost(""); Dispatch();
    int p[2]; assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    assert(write(p[1], "partial", 7) == 7); Dispatch(); assert(transfers.size() == 1);
    auto* expired = transfers.front(); Timeout(expired); close(p[1]); assert(transfers.empty()); assert(Snapshot().empty());
    assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    assert(write(p[1], "stale", 5) == 5); WineClipboardReset(); Dispatch(); close(p[1]); assert(transfers.empty()); assert(Snapshot().empty());
    // Four stalled writes occupy the shared read/write budget; fifth is closed.
    std::vector<int> readers;
    for (int i = 0; i < 4; ++i) { assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); readers.push_back(p[0]); assert(StartTransfer(p[1], true, std::string(MaxText, 'x'))); }
    assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(!StartTransfer(p[1], true, "fifth")); assert(fcntl(p[1], F_GETFD) == -1); close(p[0]);
    Invalidate(); assert(transfers.empty()); for (int fd : readers) close(fd);
    // A full text is allowed; one byte over the cap never replaces the cache.
    assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    transfers.back()->text.assign(MaxText, 'x'); assert(write(p[1], "x", 1) == 1); Dispatch(); close(p[1]); assert(transfers.empty()); assert(Snapshot().empty());
    WineClipboardFocus(surfaceA); wl_resource_destroy(surfaceA); assert(focusClient == nullptr);
    WineClipboardUnregister(); assert(offers.empty() && sources.empty() && devices.empty() && managers.empty() && transfers.empty());
    wl_client_destroy(clientA); wl_client_destroy(clientB); close(a[1]); close(b[1]); wl_display_destroy(server);
    server = wl_display_create(); WineClipboardRegister(server); assert(Snapshot().empty()); WineClipboardUnregister(); wl_display_destroy(server);
    std::cout << "PASS production Wayland clipboard: multi-process Wine tag, no destructive focus/read echo, SCM_RIGHTS, immutable/empty offers, EOF, timeout, cancel/reset, limits and restart\n";
}
