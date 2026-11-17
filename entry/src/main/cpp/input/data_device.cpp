#include "data_device.h"
#include <wayland-server-protocol.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t MaxText = 1024 * 1024, MaxTransfers = 4;
const char* Utf8 = "text/plain;charset=utf-8";
const char* Plain = "text/plain";
// Wine processes share one Windows clipboard. Preserve this marker so another
// Wine client does not import its own export and replace the delayed-render owner.
const char* WineTag = "application/x.winewayland.tag";
struct Source { wl_resource* resource; std::vector<std::string> mimes; };
struct Offer { wl_resource* resource; std::shared_ptr<const std::string> text; };
struct Transfer {
    int fd; bool writing; uint64_t generation; size_t offset = 0;
    std::string text; wl_event_source *event = nullptr, *timer = nullptr;
};
wl_display* display;
wl_global* global;
wl_resource* selection;
wl_resource* focusSurface;
wl_listener focusDestroyed;
wl_client* focusClient;
std::vector<wl_resource*> devices;
std::map<wl_resource*, Source*> sources;
std::vector<Offer*> offers;
std::vector<Transfer*> transfers;
std::mutex bridgeMutex;
int wake[2] = {-1, -1};
wl_event_source* wakeEvent;
bool pending = false, reset = false, hasSelection = false;
bool wineSelection = false;
std::string pendingText, snapshot;
std::shared_ptr<const std::string> selectionText;
uint64_t version = 0, generation = 0;

bool TextMime(const char* mime) { return mime && (!strcmp(mime, Utf8) || !strcmp(mime, Plain)); }
void Finish(Transfer* t) {
    if (t->event) wl_event_source_remove(t->event);
    if (t->timer) wl_event_source_remove(t->timer);
    close(t->fd);
    transfers.erase(std::remove(transfers.begin(), transfers.end(), t), transfers.end());
    delete t;
}
void Publish(const std::string& text) {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    // A reset may be queued before its event-loop turn.
    if (reset) return;
    snapshot = text; ++version;
}
void Invalidate() {
    ++generation;
    for (auto* t : std::vector<Transfer*>(transfers)) Finish(t);
}
int Timeout(void* data) { Finish(static_cast<Transfer*>(data)); return 0; }
void SendSelections();
int TransferReady(int fd, uint32_t, void* data) {
    auto* t = static_cast<Transfer*>(data);
    if (t->generation != generation) { Finish(t); return 0; }
    if (t->writing) {
        // Block SIGPIPE only around this write. Never alter process disposition.
        sigset_t blocked, previous, waiting; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, &previous); sigpending(&waiting);
        ssize_t n = write(fd, t->text.data() + t->offset, t->text.size() - t->offset);
        int error = errno;
        if (n < 0 && error == EPIPE && !sigismember(&waiting, SIGPIPE)) {
            timespec zero{}; sigtimedwait(&blocked, nullptr, &zero);
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (n > 0) t->offset += static_cast<size_t>(n);
        if (t->offset == t->text.size() || (n < 0 && error != EAGAIN && error != EINTR)) Finish(t);
        return 0;
    }
    // Bound work per dispatch so a large selection cannot starve rendering.
    char bytes[16384];
    for (unsigned i = 0; i < 64; ++i) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        if (n > 0) {
            if (t->text.size() + static_cast<size_t>(n) > MaxText) { Finish(t); return 0; }
            t->text.append(bytes, static_cast<size_t>(n));
        } else if (!n) {
            selectionText = std::make_shared<const std::string>(t->text); hasSelection = true;
            Publish(t->text); Finish(t); SendSelections(); return 0;
        } else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        else { Finish(t); return 0; }
    }
    return 0;
}

bool StartTransfer(int fd, bool writing, const std::string& text = {}) {
    if (transfers.size() >= MaxTransfers) { close(fd); return false; }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); return false; }
    auto* t = new Transfer{fd, writing, generation, 0, {}, nullptr, nullptr}; t->text = text;
    auto* loop = wl_display_get_event_loop(display);
    t->event = wl_event_loop_add_fd(loop, fd, writing ? WL_EVENT_WRITABLE : WL_EVENT_READABLE, TransferReady, t);
    t->timer = wl_event_loop_add_timer(loop, Timeout, t);
    transfers.push_back(t);
    if (!t->event || !t->timer) { Finish(t); return false; }
    wl_event_source_timer_update(t->timer, 1000);
    return true;
}
void DestroyResource(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void OfferAccept(wl_client*, wl_resource*, uint32_t, const char*) {}
void OfferReceive(wl_client*, wl_resource* r, const char* mime, int32_t fd) {
    auto* offer = static_cast<Offer*>(wl_resource_get_user_data(r));
    if (!offer || !TextMime(mime)) { close(fd); return; }
    StartTransfer(fd, true, *offer->text);
}
void OfferFinish(wl_client*, wl_resource*) {}
void OfferActions(wl_client*, wl_resource*, uint32_t, uint32_t) {}
const struct wl_data_offer_interface OfferImpl = {OfferAccept, OfferReceive, DestroyResource, OfferFinish, OfferActions};
void OfferDestroyed(wl_resource* r) {
    auto* offer = static_cast<Offer*>(wl_resource_get_user_data(r));
    offers.erase(std::remove(offers.begin(), offers.end(), offer), offers.end()); delete offer;
}
void SendTo(wl_resource* device) {
    if (wl_resource_get_client(device) != focusClient) return;
    if (!hasSelection) {
        // The new owner may still be rendering its data. A transient NULL offer
        // would empty Wine's shared clipboard before it services source.send.
        if (!selection) wl_data_device_send_selection(device, nullptr);
        return;
    }
    if (offers.size() >= 256) { wl_client_post_no_memory(focusClient); return; }
    auto* resource = wl_resource_create(focusClient, &wl_data_offer_interface, wl_resource_get_version(device), 0);
    if (!resource) { wl_client_post_no_memory(focusClient); return; }
    auto* offer = new Offer{resource, selectionText}; offers.push_back(offer);
    wl_resource_set_implementation(resource, &OfferImpl, offer, OfferDestroyed);
    wl_data_device_send_data_offer(device, resource);
    wl_data_offer_send_offer(resource, Utf8); wl_data_offer_send_offer(resource, Plain);
    if (wineSelection) wl_data_offer_send_offer(resource, WineTag);
    wl_data_device_send_selection(device, resource);
}
void SendSelections() { for (auto* d : devices) SendTo(d); }
void SourceOffer(wl_client*, wl_resource* r, const char* mime) {
    auto* source = static_cast<Source*>(wl_resource_get_user_data(r));
    if (source && (TextMime(mime) || (mime && !strcmp(mime, WineTag))) && source->mimes.size() < 3 &&
        std::find(source->mimes.begin(), source->mimes.end(), mime) == source->mimes.end()) source->mimes.emplace_back(mime);
}
void SourceActions(wl_client*, wl_resource*, uint32_t) {}
const struct wl_data_source_interface SourceImpl = {SourceOffer, DestroyResource, SourceActions};
void SourceDestroyed(wl_resource* r) {
    if (selection == r) { selection = nullptr; Invalidate(); }
    auto it = sources.find(r);
    if (it != sources.end()) { delete it->second; sources.erase(it); }
}
void CreateSource(wl_client* client, wl_resource* manager, uint32_t id) {
    auto* resource = wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(manager), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    auto* source = new Source{resource, {}}; sources[resource] = source;
    wl_resource_set_implementation(resource, &SourceImpl, source, SourceDestroyed);
}
void StartDrag(wl_client*, wl_resource*, wl_resource*, wl_resource*, wl_resource*, uint32_t) {}
void SetSelection(wl_client* client, wl_resource*, wl_resource* source, uint32_t) {
    if (client != focusClient || (source && (wl_resource_get_client(source) != client || !sources.count(source)))) return;
    if (selection && selection != source) wl_data_source_send_cancelled(selection);
    Invalidate(); selection = source;
    // Never offer the previous owner's text while the new source is being read.
    selectionText.reset(); hasSelection = false; wineSelection = false;
    if (!source) { Publish(""); SendSelections(); return; }
    const auto& mimes = sources[source]->mimes;
    wineSelection = std::find(mimes.begin(), mimes.end(), WineTag) != mimes.end();
    auto utf8 = std::find(mimes.begin(), mimes.end(), Utf8);
    if (utf8 == mimes.end() && std::find(mimes.begin(), mimes.end(), Plain) == mimes.end()) return;
    int pipefd[2]; if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK)) return;
    if (!StartTransfer(pipefd[0], false)) { close(pipefd[1]); return; }
    // wl_data_source.send duplicates the descriptor over SCM_RIGHTS.
    wl_data_source_send_send(source, utf8 != mimes.end() ? Utf8 : Plain, pipefd[1]); close(pipefd[1]);
}
const struct wl_data_device_interface DeviceImpl = {StartDrag, SetSelection, DestroyResource};
void DeviceDestroyed(wl_resource* r) { devices.erase(std::remove(devices.begin(), devices.end(), r), devices.end()); }
void GetDevice(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* seat) {
    if (!seat || wl_resource_get_client(seat) != client || strcmp(wl_resource_get_class(seat), "wl_seat")) return;
    auto* resource = wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(manager), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &DeviceImpl, nullptr, DeviceDestroyed); devices.push_back(resource); SendTo(resource);
}
const struct wl_data_device_manager_interface ManagerImpl = {CreateSource, GetDevice};
std::vector<wl_resource*> managers;
void ManagerDestroyed(wl_resource* r) { managers.erase(std::remove(managers.begin(), managers.end(), r), managers.end()); }
void Bind(wl_client* client, void*, uint32_t v, uint32_t id) {
    auto* resource = wl_resource_create(client, &wl_data_device_manager_interface, std::min(v, 2u), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &ManagerImpl, nullptr, ManagerDestroyed); managers.push_back(resource);
}
void FocusDestroyed(wl_listener*, void*) { WineClipboardFocus(nullptr); }
void ClearLoopState() {
    Invalidate(); if (selection) wl_data_source_send_cancelled(selection);
    selection = nullptr; hasSelection = false; wineSelection = false; selectionText.reset(); WineClipboardFocus(nullptr);
}
int Posted(int fd, uint32_t, void*) {
    char bytes[64]; while (read(fd, bytes, sizeof(bytes)) > 0) {}
    std::string text; bool clear, apply;
    { std::lock_guard<std::mutex> lock(bridgeMutex);
      clear = reset; reset = false; apply = pending; pending = false; text.swap(pendingText); }
    if (clear) ClearLoopState();
    if (apply) {
        Invalidate(); if (selection) wl_data_source_send_cancelled(selection);
        selection = nullptr; wineSelection = false;
        selectionText = std::make_shared<const std::string>(std::move(text)); hasSelection = true; SendSelections();
        Publish(*selectionText); // Acknowledge only after the loop applied the host selection.
    }
    return 0;
}
}
void WineClipboardFocus(wl_resource* surface) {
    if (surface == focusSurface) return;
    if (focusSurface) { wl_list_remove(&focusDestroyed.link); wl_list_init(&focusDestroyed.link); }
    auto* old = focusClient;
    focusSurface = surface; focusClient = surface ? wl_resource_get_client(surface) : nullptr;
    if (surface) { focusDestroyed.notify = FocusDestroyed; wl_resource_add_destroy_listener(surface, &focusDestroyed); }
    if (old != focusClient) {
        // Losing keyboard focus does not clear the selection. In particular,
        // Wine clients share ownership even when their Wayland clients differ.
        SendSelections();
    }
}
void WineClipboardRegister(wl_display* d) {
    display = d;
    std::lock_guard<std::mutex> lock(bridgeMutex);
    reset = false; pending = false; pendingText.clear(); snapshot.clear(); ++version;
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK)) return;
    wakeEvent = wl_event_loop_add_fd(wl_display_get_event_loop(d), wake[0], WL_EVENT_READABLE, Posted, nullptr);
    if (wakeEvent) global = wl_global_create(d, &wl_data_device_manager_interface, 2, nullptr, Bind);
}
void WineClipboardUnregister() {
    ClearLoopState();
    while (!offers.empty()) wl_resource_destroy(offers.back()->resource);
    while (!sources.empty()) wl_resource_destroy(sources.begin()->first);
    while (!devices.empty()) wl_resource_destroy(devices.back());
    while (!managers.empty()) wl_resource_destroy(managers.back());
    if (global) wl_global_destroy(global);
    global = nullptr;
    std::lock_guard<std::mutex> lock(bridgeMutex);
    if (wakeEvent) wl_event_source_remove(wakeEvent);
    wakeEvent = nullptr;
    for (int& fd : wake) { if (fd >= 0) close(fd); fd = -1; }
    pending = reset = false; pendingText.clear(); snapshot.clear(); ++version; display = nullptr;
}
extern "C" void WineClipboardPost(const char* text) {
    if (!text || strnlen(text, MaxText + 1) > MaxText) return;
    std::lock_guard<std::mutex> lock(bridgeMutex);
    if (wake[1] < 0) return;
    pendingText = text; pending = true; char byte = 1; (void)write(wake[1], &byte, 1);
}
extern "C" char* WineClipboardSnapshot(uint64_t* v) {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    if (v) *v = version;
    return strdup(snapshot.c_str());
}
void WineClipboardReset() {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    pending = false; pendingText.clear(); snapshot.clear(); ++version; reset = true;
    if (wake[1] >= 0) { char byte = 1; (void)write(wake[1], &byte, 1); }
}
