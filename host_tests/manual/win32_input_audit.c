// Manual guest-Windows fixture. Compile with -municode -mwindows, then launch
// inside WineHua. Logs actual WM_KEY*/WM_*BUTTON events, independently of the
// host's injection log. Enter opens an owned modal for focus recovery testing.
// Repeated key-down and VK_PROCESSKEY are legitimate; inspect actual key codes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
static FILE *logfile;static unsigned downs,ups,mouseDowns,mouseUps,ticks;
static void record(const char *what,UINT code) {
    fprintf(logfile,"%lu %s code=%u foreground=%p\n",GetTickCount(),what,code,GetForegroundWindow());fflush(logfile);
}
static LRESULT CALLBACK window_proc(HWND w,UINT message,WPARAM a,LPARAM b) {
    switch(message) {
    case WM_CREATE:SetTimer(w,1,100,NULL);record("created",0);return 0;
    case WM_KEYDOWN:downs++;record("key-down",(UINT)a);InvalidateRect(w,NULL,FALSE);
        if(a==VK_RETURN)SetTimer(w,2,250,NULL);return 0;
    case WM_KEYUP:ups++;record("key-up",(UINT)a);InvalidateRect(w,NULL,FALSE);return 0;
    case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:mouseDowns++;record("mouse-down",message);InvalidateRect(w,NULL,FALSE);return 0;
    case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_MBUTTONUP:mouseUps++;record("mouse-up",message);InvalidateRect(w,NULL,FALSE);return 0;
    case WM_TIMER:
        if(a==2){KillTimer(w,2);record("modal-open",0);int result=MessageBoxW(w,L"Controlled error dialog. Press virtual Enter to dismiss.",L"WineHua modal input test",MB_OK|MB_ICONERROR);record("modal-closed",result);}
        else {ticks++;InvalidateRect(w,NULL,FALSE);}return 0;
    case WM_PAINT:{PAINTSTRUCT paint;HDC dc=BeginPaint(w,&paint);RECT rect;GetClientRect(w,&rect);
        HBRUSH brush=CreateSolidBrush(RGB(25,50,90));FillRect(dc,&rect,brush);DeleteObject(brush);
        SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);char text[512];
        snprintf(text,sizeof(text),"Native input audit\n\nKeyboard down=%u up=%u\nMouse down=%u up=%u\nPaint tick=%u\n\nTap virtual 1 / Esc. Enter opens a modal error dialog.",downs,ups,mouseDowns,mouseUps,ticks);
        rect.left+=20;rect.top+=20;DrawTextA(dc,text,-1,&rect,DT_LEFT|DT_TOP);EndPaint(w,&paint);return 0;}
    case WM_DESTROY:KillTimer(w,1);record("closed",0);PostQuitMessage(0);return 0;
    }return DefWindowProcW(w,message,a,b);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    (void)previous;(void)command;(void)show;
    logfile=_wfopen(L"C:\\windows\\temp\\winehua_input_audit.txt",L"wb");if(!logfile)return 2;
    WNDCLASSW type={0};type.hInstance=instance;type.lpfnWndProc=window_proc;type.lpszClassName=L"WineHuaInputAudit";type.hCursor=LoadCursorW(NULL,IDC_ARROW);RegisterClassW(&type);
    HWND window=CreateWindowExW(WS_EX_TOPMOST,type.lpszClassName,L"WineHua input audit",WS_OVERLAPPEDWINDOW|WS_VISIBLE,210,140,600,360,NULL,NULL,instance,NULL);
    SetForegroundWindow(window);UpdateWindow(window);MSG msg;while(GetMessageW(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}fclose(logfile);return 0;
}
