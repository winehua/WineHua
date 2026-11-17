#pragma once
#include <wayland-server-core.h>
#include <stdint.h>
// Protocol resources are event-loop owned. Post/snapshot/reset are thread safe.
void WineClipboardRegister(wl_display* display);
void WineClipboardUnregister();
void WineClipboardFocus(wl_resource* surface);
extern "C" void WineClipboardPost(const char* text);
extern "C" char* WineClipboardSnapshot(uint64_t* version);
void WineClipboardReset();
