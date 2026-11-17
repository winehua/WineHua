#pragma once
#include "compositor/input/input_resolver.h"
// Platform window movement and raising are outside the focus test. Input
// resolution, queuing, dispatch and injection use production implementations.
class WaylandServer {
public:
    using InputTarget = ::InputTarget;
    static WaylandServer* GetInstance() { static WaylandServer server; return &server; }
    void RaiseToplevel(uint32_t, bool) {}
    void SetToplevelRestored(uint32_t) {}
    // Protocol close delivery is covered by wayland_close_lifecycle_test.py.
    void DispatchToplevelClose(uint32_t, wl_resource*) {}
    uint32_t resizedHost=0;
    int32_t resizeW=0,resizeH=0; bool resizing=false;
    void NotifyToplevelResize(uint32_t id,int32_t w,int32_t h,bool active) {
        resizedHost=id;resizeW=w;resizeH=h;resizing=active;
    }
    int32_t moveX=0,moveY=0;
    bool ProcessMoveGrabMotion(int32_t x, int32_t y) {
        if (!movedHost) return false;
        moveX=x;moveY=y;return true;
    }
    void EndMoveGrab() { movedHost=0; }
    uint32_t movedHost = 0;
    void StartMoveGrabAt(uint32_t host, uint32_t, int32_t x,int32_t y) {
        movedHost=host;moveX=x;moveY=y;
    }
    uint32_t resizeEdges = 0;
    void StartExternalResizeGrabAt(uint32_t host, uint32_t serial, int32_t x, int32_t y, uint32_t edges) {
        StartMoveGrabAt(host,serial,x,y);resizeEdges=edges;
    }
};
