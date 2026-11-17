#include "data_device.h"

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Clip"
#include <hilog/log.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// ArkTS 侧剪贴板回调入口 (napi_init.cpp 的 napi threadsafe function, 两条
// 路线共享同一入口; 未注册回调时内部判空返回 -1, 直接丢文本)
extern "C" int winehua_host_clipboard_text(const char* utf8);

namespace {

constexpr uint32_t kManagerVersion = 2;   // wine 绑定 min(version, 2), 与其对齐
constexpr size_t kMaxTextBytes = 8u * 1024u * 1024u;
const char* kMimeUtf8 = "text/plain;charset=utf-8";
const char* kMimePlain = "text/plain";

// 全量写 (EINTR/短写重试; 其余错误放弃)
bool WriteAllFd(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

// ============================================================================
//  接口实现表 (noop 槽位补齐: 客户端按 resource 版本发请求, 缺表项的请求
//  会落到 nullptr 直接崩; 全部显式占位。前置定义: 成员函数体在中部引用)
// ============================================================================

// manager.release (since 4) 不实现: global 钉 v2, 客户端不会发
const struct wl_data_device_manager_interface kManagerImpl = {
    .create_data_source = ClipboardDevice::manager_create_data_source,
    .get_data_device = ClipboardDevice::manager_get_data_device,
};

const struct wl_data_source_interface kSourceImpl = {
    .offer = ClipboardDevice::source_offer,
    .destroy = ClipboardDevice::source_destroy,
    .set_actions = ClipboardDevice::source_set_actions,
};

const struct wl_data_device_interface kDeviceImpl = {
    .start_drag = ClipboardDevice::device_start_drag,
    .set_selection = ClipboardDevice::device_set_selection,
    .release = ClipboardDevice::device_release,
};

const struct wl_data_offer_interface kOfferImpl = {
    .accept = ClipboardDevice::offer_accept,
    .receive = ClipboardDevice::offer_receive,
    .destroy = ClipboardDevice::offer_destroy,
    .finish = ClipboardDevice::offer_finish,
    .set_actions = ClipboardDevice::offer_set_actions,
};

ClipboardDevice* ClipboardDevice::GetInstance() {
    static ClipboardDevice s;
    return &s;
}

// ============================================================================
//  生命周期
// ============================================================================

void ClipboardDevice::Register(wl_display* display) {
    if (global_) {
        OH_LOG_WARN(LOG_APP, "[Clip] already registered");
        return;
    }
    display_ = display;
    wl_event_loop* loop = wl_display_get_event_loop(display);
    if (pipe(postPipe_) == 0) {
        for (int& fd : postPipe_) fcntl(fd, F_SETFL, O_NONBLOCK);
        pipeSource_ = wl_event_loop_add_fd(loop, postPipe_[0], WL_EVENT_READABLE,
                                           OnPostPipeReadable, this);
    } else {
        OH_LOG_ERROR(LOG_APP, "[Clip] post pipe failed errno=%{public}d", errno);
    }
    global_ = wl_global_create(display, &wl_data_device_manager_interface,
                               kManagerVersion, this, manager_bind);
    // Register 前积压的 host 文本 (ArkTS onCreate 剪贴板同步早于引擎启动):
    // 此时 EventLoop 线程尚未启动, 直接同步收口, 供 wine get_data_device 补发
    {
        std::lock_guard<std::mutex> lk(postMutex_);
        for (char* text : postQueue_) {
            hostText_ = text;
            free(text);
        }
        postQueue_.clear();
    }
    OH_LOG_INFO(LOG_APP, "[Clip] wl_data_device_manager global v%{public}u OK, pending=%{public}zu",
                kManagerVersion, hostText_.size());
}

void ClipboardDevice::Unregister() {
    if (global_) {
        wl_global_destroy(global_);
        global_ = nullptr;
    }
    if (pipeSource_) {
        wl_event_source_remove(pipeSource_);
        pipeSource_ = nullptr;
    }
    for (int fd : postPipe_) {
        if (fd >= 0) close(fd);
    }
    postPipe_[0] = postPipe_[1] = -1;
    {
        std::lock_guard<std::mutex> lk(postMutex_);
        for (char* t : postQueue_) free(t);
        postQueue_.clear();
    }
    for (GuestRead* r : guestReads_) {
        if (r->ev) wl_event_source_remove(r->ev);
        close(r->fd);
        delete r;
    }
    guestReads_.clear();
    devices_.clear();
    guestSource_ = nullptr;
    sourceMimes_.clear();
    selectionIsHost_ = false;
    display_ = nullptr;
    // hostText_ 保留: 会话重启后 wine 重新 get_data_device 时补发 selection,
    // 宿主剪贴板跨会话生效
    OH_LOG_INFO(LOG_APP, "[Clip] unregistered OK");
}

// ============================================================================
//  host -> guest (PostHostClipboard 队列消费 + selection 广播)
// ============================================================================

void ClipboardDevice::PostHostClipboard(const char* utf8) {
    if (!utf8 || !*utf8) return;
    char* copy = strdup(utf8);
    if (!copy) return;
    {
        std::lock_guard<std::mutex> lk(postMutex_);
        postQueue_.push_back(copy);
    }
    // pipe 未建 (Register 前) 只入队: Register 后 drain 会消费
    if (postPipe_[1] >= 0) {
        char wake = 1;
        ssize_t rc = write(postPipe_[1], &wake, 1);
        (void)rc;  // 唤醒失败无碍: Register drain 与下一次 wake 兜底
    }
}

int ClipboardDevice::OnPostPipeReadable(int fd, uint32_t mask, void* data) {
    auto* self = static_cast<ClipboardDevice*>(data);
    (void)mask;
    char drain[64];
    while (read(fd, drain, sizeof(drain)) > 0) {
    }
    std::vector<char*> batch;
    {
        std::lock_guard<std::mutex> lk(self->postMutex_);
        batch.swap(self->postQueue_);
    }
    for (char* text : batch) {
        self->hostText_ = text;
        free(text);
    }
    if (!batch.empty()) {
        OH_LOG_INFO(LOG_APP, "[Clip] host -> guest %{public}zu bytes, devices=%{public}zu",
                    self->hostText_.size(), self->devices_.size());
        // host 接管 selection: wine 旧 source 失效, 通知其回收; 在途的 guest
        // 读全部作废 (迟到 EOF 不得翻转所有权)
        if (self->guestSource_) {
            wl_data_source_send_cancelled(self->guestSource_);
            self->guestSource_ = nullptr;
        }
        for (GuestRead* r : self->guestReads_) r->discard = true;
        self->selectionIsHost_ = true;
        // 协议投递: 发给持键盘焦点的客户端 (wayland.xml wl_data_device.selection);
        // 暂无焦点 (引擎刚起) 则挂起, 键盘聚焦时补发 (OnKeyboardFocus)
        if (self->focusClient_) {
            wl_resource* dev = self->FindDeviceForClient(self->focusClient_);
            if (dev) self->SendHostSelectionTo(static_cast<DeviceData*>(
                                                 wl_resource_get_user_data(dev)));
        }
    }
    return 0;
}

void ClipboardDevice::OnKeyboardFocus(wl_client* client) {
    focusClient_ = client;
    // 协议: 焦点切换时向新聚焦客户端补发当前 selection。仅宿主 selection
    // 需要 wayland 投递 —— wine 进程间的剪贴板走 wineserver 全局通道, 与
    // 本协议面无关; wine 自身的 selection 不回投 (其 offer 带 winewayland
    // tag, wine 侧本来就会忽略)。
    if (selectionIsHost_ && !hostText_.empty()) {
        wl_resource* dev = FindDeviceForClient(client);
        if (dev) {
            SendHostSelectionTo(static_cast<DeviceData*>(wl_resource_get_user_data(dev)));
        }
    }
}

wl_resource* ClipboardDevice::FindDeviceForClient(wl_client* client) {
    for (DeviceData* dd : devices_) {
        if (wl_resource_get_client(dd->device) == client) return dd->device;
    }
    return nullptr;
}

void ClipboardDevice::SendHostSelectionTo(DeviceData* dd) {
    // 旧 offer 先收回 (置空再销毁: destructor 会扫描 devices_ 置空, 先置空防回写)
    if (dd->offer) {
        wl_resource* old = dd->offer;
        dd->offer = nullptr;
        wl_resource_destroy(old);
    }
    if (hostText_.empty()) {
        return;
    }
    wl_client* client = wl_resource_get_client(dd->device);
    wl_resource* offer = wl_resource_create(client, &wl_data_offer_interface,
                                            wl_resource_get_version(dd->device), 0);
    if (!offer) return;
    wl_resource_set_implementation(offer, &kOfferImpl, new OfferData{this},
                                   offer_resource_destroy);
    dd->offer = offer;
    wl_data_device_send_data_offer(dd->device, offer);
    wl_data_offer_send_offer(offer, kMimeUtf8);
    wl_data_offer_send_offer(offer, kMimePlain);
    wl_data_device_send_selection(dd->device, offer);
}

// ============================================================================
//  guest -> host (set_selection 请求 + source fd 异步读)
// ============================================================================

void ClipboardDevice::device_set_selection(wl_client*, wl_resource*, wl_resource* source,
                                           uint32_t serial) {
    auto* self = GetInstance();
    (void)serial;
    // 读侧失配保护: 上一轮未读完的 read 全部 discard (迟到 EOF 不得翻转所有权)
    for (GuestRead* r : self->guestReads_) r->discard = true;
    if (self->guestSource_ && self->guestSource_ != source) {
        wl_data_source_send_cancelled(self->guestSource_);
    }

    if (!source) {
        // wine 清空剪贴板: 撤宿主 offer (选择权交还), 不动宿主文本
        self->guestSource_ = nullptr;
        if (self->selectionIsHost_) {
            self->selectionIsHost_ = false;
            for (DeviceData* dd : self->devices_) {
                if (dd->offer) {
                    wl_resource* old = dd->offer;
                    dd->offer = nullptr;
                    wl_resource_destroy(old);
                }
            }
        }
        OH_LOG_INFO(LOG_APP, "[Clip] guest selection cleared");
        return;
    }
    self->guestSource_ = source;
    // 选 mime: utf-8 优先, plain 兜底; 都没有则 guest 无文本
    auto it = self->sourceMimes_.find(source);
    const char* mime = nullptr;
    if (it != self->sourceMimes_.end()) {
        for (const std::string& m : it->second) {
            if (m == kMimeUtf8) { mime = kMimeUtf8; break; }
        }
        if (!mime) {
            for (const std::string& m : it->second) {
                if (m == kMimePlain) { mime = kMimePlain; break; }
            }
        }
    }
    if (!mime) {
        // 无文本 mime (如 wine 启动期的空剪贴板导出): 读不到内容, 不撤宿主
        // offer 也不翻所有权 —— 所有权裁决统一推迟到读完成 (OnGuestReadReadable)
        OH_LOG_INFO(LOG_APP, "[Clip] guest selection has no text mime");
        return;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Clip] pipe failed errno=%{public}d", errno);
        return;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    GuestRead* r = new GuestRead{fds[0], nullptr, {}, false, source};
    r->ev = wl_event_loop_add_fd(wl_display_get_event_loop(self->display_), fds[0],
                                 WL_EVENT_READABLE, OnGuestReadReadable, r);
    if (!r->ev) {
        close(fds[0]);
        close(fds[1]);
        delete r;
        return;
    }
    self->guestReads_.push_back(r);
    OH_LOG_INFO(LOG_APP, "[Clip] guest selection read mime=%{public}s", mime);
    wl_data_source_send_send(source, mime, fds[1]);
    // wl_data_device 协议来源: fd 已 dup 进 client socket, 宿主写端必须 close
    // — 客户端写完关它那份后引用归零, 读端才见 EOF; 不关则 clip read 逐次泄漏
    close(fds[1]);
}

int ClipboardDevice::OnGuestReadReadable(int fd, uint32_t mask, void* data) {
    auto* r = static_cast<GuestRead*>(data);
    auto* self = GetInstance();
    (void)mask;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (r->discard) continue;
            if (r->buf.size() + static_cast<size_t>(n) > kMaxTextBytes) {
                OH_LOG_WARN(LOG_APP, "[Clip] guest text exceeds %{public}zu bytes, dropped",
                            kMaxTextBytes);
                r->discard = true;
                r->buf.clear();
                continue;
            }
            r->buf.insert(r->buf.end(), tmp, tmp + n);
        } else if (n == 0) {
            // EOF: 客户端写完关写端。所有权裁决在此 (内容读到才判):
            // 与宿主文本相同 = wine 进程间导入回声 (一个进程导入触发其它
            // 进程的 WM_CLIPBOARDUPDATE 反向导出), 桥做镜像 no-op, 不翻
            // 所有权不撤宿主 offer; 不同 = 真实 guest 复制, 中继宿主并接管。
            if (!r->discard && !r->buf.empty()) {
                r->buf.push_back('\0');
                bool echo = !self->hostText_.empty() &&
                            self->hostText_.compare(r->buf.data()) == 0;
                if (echo) {
                    OH_LOG_INFO(LOG_APP, "[Clip] guest echo suppressed (%{public}zu bytes)",
                                r->buf.size() - 1);
                } else {
                    OH_LOG_INFO(LOG_APP, "[Clip] guest -> host %{public}zu bytes",
                                r->buf.size() - 1);
                    winehua_host_clipboard_text(r->buf.data());
                    if (self->selectionIsHost_) {
                        self->selectionIsHost_ = false;
                        for (DeviceData* dd : self->devices_) {
                            if (dd->offer) {
                                wl_resource* old = dd->offer;
                                dd->offer = nullptr;
                                wl_resource_destroy(old);
                            }
                        }
                    }
                    if (self->sourceMimes_.count(r->source)) {
                        self->guestSource_ = r->source;
                    }
                }
            }
            GetInstance()->GuestReadDestroy(r, true);
            return 0;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            OH_LOG_WARN(LOG_APP, "[Clip] guest read failed errno=%{public}d", errno);
            GetInstance()->GuestReadDestroy(r, true);
            return 0;
        }
    }
}

void ClipboardDevice::GuestReadDestroy(GuestRead* r, bool removeLink) {
    if (r->ev) {
        wl_event_source_remove(r->ev);
        r->ev = nullptr;
    }
    close(r->fd);
    if (removeLink) {
        auto& v = guestReads_;
        v.erase(std::remove(v.begin(), v.end(), r), v.end());
    }
    delete r;
}

// ============================================================================
//  wl_data_device_manager / wl_data_source / wl_data_device / wl_data_offer
//  接口实现
// ============================================================================

void ClipboardDevice::manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<ClipboardDevice*>(data);
    uint32_t v = std::min(version, kManagerVersion);
    wl_resource* res = wl_resource_create(client, &wl_data_device_manager_interface, v, id);
    wl_resource_set_implementation(res, &kManagerImpl, self, nullptr);
    OH_LOG_INFO(LOG_APP, "[Clip] manager bound v%{public}u", v);
}

void ClipboardDevice::manager_create_data_source(wl_client* client, wl_resource* resource,
                                                 uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_data_source_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &kSourceImpl, GetInstance(), source_resource_destroy);
}

void ClipboardDevice::manager_get_data_device(wl_client*, wl_resource* resource, uint32_t id,
                                              wl_resource* seat) {
    auto* self = static_cast<ClipboardDevice*>(wl_resource_get_user_data(resource));
    (void)seat;
    wl_resource* res = wl_resource_create(wl_resource_get_client(resource),
                                          &wl_data_device_interface,
                                          wl_resource_get_version(resource), id);
    auto* dd = new DeviceData{self, res, nullptr};
    wl_resource_set_implementation(res, &kDeviceImpl, dd, device_destroy);
    self->devices_.push_back(dd);
    // selection 不在 bind 时投递: 协议规定 selection 跟随键盘焦点
    // (聚焦时补发, 见 OnKeyboardFocus / InputInjector::InjectKeyboardEnter
    // 钩子)。bind 即推会让多个 wine 进程各自导入全局剪贴板, 触发导入级联
    // 与反向回声 (2026-11-16 真机实锤的粘贴失效根因)。
    OH_LOG_INFO(LOG_APP, "[Clip] data_device created (total=%{public}zu)", self->devices_.size());
}

void ClipboardDevice::source_offer(wl_client*, wl_resource* resource, const char* mime) {
    GetInstance()->sourceMimes_[resource].push_back(mime);
}

void ClipboardDevice::source_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ClipboardDevice::source_resource_destroy(wl_resource* resource) {
    auto* self = GetInstance();
    self->sourceMimes_.erase(resource);
    if (self->guestSource_ == resource) {
        self->guestSource_ = nullptr;
    }
}

void ClipboardDevice::device_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*,
                                        wl_resource*, uint32_t) {
    OH_LOG_WARN(LOG_APP, "[Clip] start_drag unsupported (dnd 不在剪贴板桥范围)");
}

void ClipboardDevice::device_release(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ClipboardDevice::device_destroy(wl_resource* resource) {
    auto* self = GetInstance();
    auto* dd = static_cast<DeviceData*>(wl_resource_get_user_data(resource));
    if (dd->offer) {
        wl_resource* old = dd->offer;
        dd->offer = nullptr;
        wl_resource_destroy(old);
    }
    auto& v = self->devices_;
    v.erase(std::remove(v.begin(), v.end(), dd), v.end());
    delete dd;
    OH_LOG_INFO(LOG_APP, "[Clip] data_device destroyed (remaining=%{public}zu)", v.size());
}

void ClipboardDevice::offer_accept(wl_client*, wl_resource*, uint32_t, const char*) {}
void ClipboardDevice::offer_finish(wl_client*, wl_resource*) {}

void ClipboardDevice::offer_receive(wl_client*, wl_resource*, const char* mime, int32_t fd) {
    auto* self = GetInstance();
    // receive 只可能来自我们发的 host offer (guest source 的数据由 source.send
    // 送出); 非宿主文本期收到一律关 fd 拒答
    if (!self->selectionIsHost_ ||
        (strcmp(mime, kMimeUtf8) != 0 && strcmp(mime, kMimePlain) != 0)) {
        close(fd);
        return;
    }
    WriteAllFd(fd, self->hostText_.data(), self->hostText_.size());
    close(fd);
}

void ClipboardDevice::offer_set_actions(wl_client*, wl_resource*, uint32_t, uint32_t) {}
void ClipboardDevice::source_set_actions(wl_client*, wl_resource*, uint32_t) {}

void ClipboardDevice::offer_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void ClipboardDevice::offer_resource_destroy(wl_resource* resource) {
    auto* self = GetInstance();
    for (DeviceData* dd : self->devices_) {
        if (dd->offer == resource) dd->offer = nullptr;
    }
    delete static_cast<OfferData*>(wl_resource_get_user_data(resource));
}
