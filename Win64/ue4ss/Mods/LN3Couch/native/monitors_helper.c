#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

#define PREFIX L"ue4ss\\Mods\\LN3Couch\\native\\monitors_"
#define MAX_DISPLAYS 32

typedef struct {
    RECT area;
    unsigned id;
    int primary;
} Display;

typedef struct {
    RECT game, projection, crop;
    int horizontal, player2First, project;
} Layout;

static Display displays[MAX_DISPLAYS];
static unsigned count;
static HINSTANCE instance;
typedef struct {
    HWND window;
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previousBitmap;
    RECT area;
    ULONGLONG fadeAt;
} Snapshot;

typedef struct {
    HWND window;
    HTHUMBNAIL thumbnail;
    RECT display, crop, content;
    int dark;
} Projection;

static HWND game;
static Projection outputs[2];
static Snapshot masks[2];
static LONG_PTR oldStyle, oldExStyle;
static WINDOWPLACEMENT oldPlacement;
static Layout applied;
static unsigned focusLosses;
static BOOL restoreTopmost;
static HRESULT (WINAPI *registerThumbnail)(HWND, HWND, PHTHUMBNAIL);
static HRESULT (WINAPI *updateThumbnail)(HTHUMBNAIL, const DWM_THUMBNAIL_PROPERTIES *);
static HRESULT (WINAPI *unregisterThumbnail)(HTHUMBNAIL);

static BOOL CALLBACK addDisplay(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data) {
    (void)dc; (void)rect; (void)data;
    MONITORINFOEXW info = {0};
    unsigned id;
    info.cbSize = sizeof(info);
    if (count < MAX_DISPLAYS && GetMonitorInfoW(monitor, (MONITORINFO *)&info)
        && swscanf_s(info.szDevice, L"\\\\.\\DISPLAY%u", &id) == 1) {
        displays[count++] = (Display){info.rcMonitor, id, !!(info.dwFlags & MONITORINFOF_PRIMARY)};
    }
    return TRUE;
}

static void scanDisplays(void) {
    count = 0;
    EnumDisplayMonitors(NULL, NULL, addDisplay, 0);
}

static int chooseLayout(RECT p1, RECT p2, RECT desktop, Layout *layout) {
    LONG width = p1.right - p1.left, height = p1.bottom - p1.top;
    *layout = (Layout){0};
    if (EqualRect(&p1, &p2) || width <= 0 || height <= 0 ||
        p2.right <= p2.left || p2.bottom <= p2.top) return 0;

    if (width == p2.right-p2.left && height == p2.bottom-p2.top) {
        if (p1.top == p2.top && (p1.right == p2.left || p2.right == p1.left) && width <= 8192) {
            layout->horizontal = 1;
            layout->player2First = p2.left < p1.left;
            UnionRect(&layout->game, &p1, &p2);
            return 1;
        }
        if (p1.left == p2.left && (p1.bottom == p2.top || p2.bottom == p1.top) && height <= 8192) {
            layout->player2First = p2.top < p1.top;
            UnionRect(&layout->game, &p1, &p2);
            return 1;
        }
    }

    // Render the projected half beyond the desktop edge so other monitors stay free.
    for (int anchorIsP2 = 0; anchorIsP2 <= 1; ++anchorIsP2) {
        RECT anchor = anchorIsP2 ? p2 : p1;
        LONG w = anchor.right-anchor.left, h = anchor.bottom-anchor.top;
        int projectFirst;
        layout->game = anchor;
        if (w <= 8192 && (anchor.left == desktop.left || anchor.right == desktop.right)) {
            layout->horizontal = 1;
            projectFirst = anchor.left == desktop.left;
            if (projectFirst) layout->game.left -= w;
            else layout->game.right += w;
        } else if (h <= 8192 && (anchor.top == desktop.top || anchor.bottom == desktop.bottom)) {
            layout->horizontal = 0;
            projectFirst = anchor.top == desktop.top;
            if (projectFirst) layout->game.top -= h;
            else layout->game.bottom += h;
        } else continue;

        layout->project = 1;
        layout->projection = anchorIsP2 ? p1 : p2;
        layout->player2First = anchorIsP2 ? !projectFirst : projectFirst;
        layout->crop = (RECT){0, 0, w, h};
        if (!projectFirst) OffsetRect(&layout->crop, layout->horizontal ? w : 0, layout->horizontal ? 0 : h);
        return 1;
    }
    return 0;
}

static RECT virtualDesktop(void) {
    RECT rect;
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect;
}

static BOOL CALLBACK findGame(HWND window, LPARAM data) {
    DWORD pid;
    wchar_t cls[64];
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(window) &&
        GetClassNameW(window, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"UnrealWindow") == 0) {
        *(HWND *)data = window;
        return FALSE;
    }
    return TRUE;
}

static LRESULT CALLBACK monitorFocus(HWND window, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if ((msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)) ++focusLosses;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, monitorFocus, id);
    return DefSubclassProc(window, msg, wp, lp);
}

static void freeSnapshot(Snapshot *frame) {
    if (frame->dc && frame->previousBitmap) SelectObject(frame->dc, frame->previousBitmap);
    if (frame->bitmap) DeleteObject(frame->bitmap);
    if (frame->dc) DeleteDC(frame->dc);
    *frame = (Snapshot){0};
}

static LRESULT CALLBACK projectionProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lp;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    }
    Snapshot *frame = (Snapshot *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (frame) {
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            BitBlt(dc, 0, 0, frame->area.right-frame->area.left, frame->area.bottom-frame->area.top,
                   frame->dc, 0, 0, SRCCOPY);
            EndPaint(window, &paint);
            return 0;
        }
        if (msg == WM_TIMER && wp == 1) {
            ULONGLONG elapsed = GetTickCount64() - frame->fadeAt;
            if (elapsed >= 120) DestroyWindow(window);
            else SetLayeredWindowAttributes(window, 0, (BYTE)(255 - elapsed*255/120), LWA_ALPHA);
            return 0;
        }
        if (msg == WM_NCDESTROY) {
            KillTimer(window, 1);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            freeSnapshot(frame);
        }
    }
    Projection *output = (Projection *)GetPropW(window, L"LN3Couch.Output");
    if (msg == WM_MOUSEACTIVATE) {
        SetForegroundWindow(GetWindow(window, GW_OWNER));
        return output && !output->dark ? MA_NOACTIVATE : MA_NOACTIVATEANDEAT;
    }
    if (output && !output->dark && ((msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK) || msg == WM_MOUSEWHEEL)) {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (msg == WM_MOUSEWHEEL) ScreenToClient(window, &point);
        RECT dst = output->content, source = output->crop;
        if (!PtInRect(&dst, point)) return 0;
        point.x = source.left + MulDiv(point.x-dst.left, source.right-source.left, dst.right-dst.left);
        point.y = source.top + MulDiv(point.y-dst.top, source.bottom-source.top, dst.bottom-dst.top);
        if (msg == WM_MOUSEWHEEL) ClientToScreen(game, &point);
        if (GetForegroundWindow() == game) PostMessageW(game, msg, wp, MAKELPARAM(point.x, point.y));
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

static void destroyProjection(Projection *output) {
    if (output->thumbnail) unregisterThumbnail(output->thumbnail);
    if (output->window) { RemovePropW(output->window, L"LN3Couch.Output"); DestroyWindow(output->window); }
    *output = (Projection){0};
}

static void clearMasks(void) {
    for (int i = 0; i < 2; ++i) {
        if (masks[i].window) DestroyWindow(masks[i].window);
        else if (masks[i].dc) freeSnapshot(&masks[i]);
    }
}

static void restoreWindow(void) {
    clearMasks();
    for (int i = 0; i < 2; ++i) destroyProjection(&outputs[i]);
    if (IsWindow(game)) {
        RemoveWindowSubclass(game, monitorFocus, 1);
        SetWindowLongPtrW(game, GWL_STYLE, oldStyle);
        SetWindowLongPtrW(game, GWL_EXSTYLE, oldExStyle);
        ShowWindow(game, SW_SHOWNOACTIVATE);
        SetWindowPlacement(game, &oldPlacement);
        SetWindowPos(game, restoreTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    game = NULL;
}

static HWND openOwnedWindow(RECT rect, Snapshot *frame) {
    static ATOM projectionClass;
    if (!projectionClass) {
        WNDCLASSW cls = {0};
        cls.hInstance = instance;
        cls.lpfnWndProc = projectionProc;
        cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        cls.lpszClassName = L"LN3CouchProjection";
        projectionClass = RegisterClassW(&cls);
        if (!projectionClass) return NULL;
    }
    DWORD flags = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (frame) flags |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    return CreateWindowExW(flags, MAKEINTATOM(projectionClass), L"LN3Couch", WS_POPUP,
        rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top, game, NULL, instance, frame);
}

static void captureFrame(Snapshot *frame, HDC screen, RECT r) {
    int width = r.right-r.left, height = r.bottom-r.top;
    frame->area = r;
    frame->dc = CreateCompatibleDC(screen);
    frame->bitmap = CreateCompatibleBitmap(screen, width, height);
    if (frame->dc && frame->bitmap) {
        frame->previousBitmap = SelectObject(frame->dc, frame->bitmap);
        if (frame->previousBitmap && BitBlt(frame->dc, 0, 0, width, height, screen, r.left, r.top,
                                           SRCCOPY | CAPTUREBLT)) return;
    }
    freeSnapshot(frame);
}

static void showMasks(RECT p1, RECT p2) {
    if (GetForegroundWindow() != game) return;
    RECT rects[2] = {p1, p2};
    if ((!masks[0].window && !masks[1].window) || masks[0].fadeAt || masks[1].fadeAt ||
        (masks[0].window && !EqualRect(&masks[0].area, &p1)) ||
        (masks[1].window && !EqualRect(&masks[1].area, &p2))) {
        clearMasks();
        HDC screen = GetDC(NULL);
        if (!screen) return;
        for (int i = 0; i < 2; ++i) captureFrame(&masks[i], screen, rects[i]);
        ReleaseDC(NULL, screen);
        for (int i = 0; i < 2; ++i) {
            Snapshot *frame = &masks[i];
            if (!frame->dc) continue;
            frame->window = openOwnedWindow(rects[i], frame);
            if (!frame->window || !SetLayeredWindowAttributes(frame->window, 0, 255, LWA_ALPHA)) {
                if (frame->window) DestroyWindow(frame->window);
                else freeSnapshot(frame);
            }
        }
    }
    for (int i = 0; i < 2; ++i) {
        HWND window = masks[i].window;
        if (!window) continue;
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(window);
    }
}

static void fadeMasks(void) {
    for (int i = 0; i < 2; ++i) {
        Snapshot *frame = &masks[i];
        if (frame->window && !frame->fadeAt) {
            frame->fadeAt = GetTickCount64();
            if (!SetTimer(frame->window, 1, 15, NULL)) DestroyWindow(frame->window);
        }
    }
}

static int showProjection(Projection *output, RECT r, RECT crop, int dark) {
    if (output->window && output->dark == dark && EqualRect(&output->display, &r) &&
        EqualRect(&output->crop, &crop)) return 1;
    destroyProjection(output);
    if (!dark && !registerThumbnail) {
        wchar_t system[MAX_PATH];
        UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
        if (!length || length >= ARRAYSIZE(system) - 12) return 0;
        if (wcscat_s(system, ARRAYSIZE(system), L"\\dwmapi.dll")) return 0;
        HMODULE dwm = LoadLibraryW(system);
        if (!dwm) return 0;
        *(FARPROC *)&registerThumbnail = GetProcAddress(dwm, "DwmRegisterThumbnail");
        *(FARPROC *)&updateThumbnail = GetProcAddress(dwm, "DwmUpdateThumbnailProperties");
        *(FARPROC *)&unregisterThumbnail = GetProcAddress(dwm, "DwmUnregisterThumbnail");
        if (!registerThumbnail || !updateThumbnail || !unregisterThumbnail) {
            registerThumbnail = NULL;
            FreeLibrary(dwm);
            return 0;
        }
    }
    output->window = openOwnedWindow(r, NULL);
    if (!output->window) return 0;
    output->display = r;
    output->crop = crop;
    output->dark = dark;
    if (!SetPropW(output->window, L"LN3Couch.Output", output)) return 0;
    if (!dark) {
        if (FAILED(registerThumbnail(output->window, game, &output->thumbnail))) return 0;
        DWM_THUMBNAIL_PROPERTIES props = {0};
        props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE |
                        DWM_TNP_SOURCECLIENTAREAONLY;
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = TRUE;
        LONG width = r.right-r.left, height = r.bottom-r.top;
        LONG sourceW = crop.right-crop.left, sourceH = crop.bottom-crop.top;
        LONG fitW = min(width, MulDiv(sourceW, height, sourceH));
        LONG fitH = min(height, MulDiv(sourceH, width, sourceW));
        LONG x = (width-fitW)/2, y = (height-fitH)/2;
        output->content = (RECT){x, y, x+fitW, y+fitH};
        props.rcDestination = output->content;
        props.rcSource = crop;
        if (FAILED(updateThumbnail(output->thumbnail, &props))) return 0;
    }
    ShowWindow(output->window, SW_SHOWNOACTIVATE);
    return 1;
}

static void writeStatus(const char *status, const Layout *layout) {
    FILE *f = NULL;
    if (_wfopen_s(&f, PREFIX L"status.txt", L"w") != 0) return;
    fprintf(f, "%s", status);
    if (layout) {
        HWND foreground = GetForegroundWindow();
        fprintf(f, " %d %d %d %ld %ld %d %u\n", layout->horizontal, layout->player2First, layout->project,
                layout->game.right-layout->game.left, layout->game.bottom-layout->game.top,
                (foreground == game || foreground == outputs[0].window || foreground == outputs[1].window) && foreground != NULL, focusLosses);
    }
    fclose(f);
}

__declspec(dllexport) int monitor_scan(void *lua) {
    (void)lua;
    DPI_AWARENESS_CONTEXT dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    scanDisplays();
    FILE *f = NULL;
    if (_wfopen_s(&f, PREFIX L"list.txt", L"w") == 0) {
        RECT desktop = virtualDesktop();
        for (unsigned i = 0; i < count; ++i) {
            RECT r = displays[i].area;
            fprintf(f, "display %u %ld %ld %ld %ld %d\n", displays[i].id, r.left, r.top,
                    r.right-r.left, r.bottom-r.top, displays[i].primary);
            for (unsigned j = i + 1; j < count; ++j) {
                Layout layout;
                if (chooseLayout(r, displays[j].area, desktop, &layout))
                    fprintf(f, "pair %u %u %d\n", displays[i].id, displays[j].id, layout.project);
            }
        }
        fclose(f);
    }
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    return 0;
}

static const char *applySelected(unsigned p1, unsigned p2, int shared, int mask) {
    scanDisplays();
    RECT *a = NULL, *b = NULL;
    for (unsigned i = 0; i < count; ++i) {
        if (displays[i].id == p1) a = &displays[i].area;
        if (displays[i].id == p2) b = &displays[i].area;
    }
    Layout layout;
    if (!a || !b || !chooseLayout(*a, *b, virtualDesktop(), &layout)) {
        restoreWindow();
        return "pair";
    }

    RECT sharedCrop = {0};
    if (shared) {
        if (shared <= 2) {
            layout.game = *a;
            sharedCrop = (RECT){0, 0, a->right-a->left, a->bottom-a->top};
        } else {
            LONG w = layout.game.right-layout.game.left, h = layout.game.bottom-layout.game.top;
            sharedCrop = layout.horizontal ? (RECT){w/4, 0, 3*w/4, h} : (RECT){0, h/4, w, 3*h/4};
        }
        layout.project = 1;
    }

    HWND hwnd = NULL;
    EnumWindows(findGame, (LPARAM)&hwnd);
    if (!hwnd) return "window";
    if (IsIconic(hwnd)) return "minimized";
    if (game != hwnd) {
        restoreWindow();
        oldPlacement.length = sizeof(oldPlacement);
        if (!GetWindowPlacement(hwnd, &oldPlacement)) return "window";
        oldStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        oldExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        restoreTopmost = (oldExStyle & WS_EX_TOPMOST) != 0;
        game = hwnd;
        focusLosses = 0;
        if (GetWindowThreadProcessId(hwnd, NULL) == GetCurrentThreadId())
            SetWindowSubclass(hwnd, monitorFocus, 1, 0);
        if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    }
    if (mask) showMasks(*a, *b);
    RECT current, client;
    RECT r = layout.game;
    if (!GetWindowRect(game, &current) || !GetClientRect(game, &client)) {
        restoreWindow();
        return "window";
    }
    if (!EqualRect(&current, &r) || client.right != r.right-r.left || client.bottom != r.bottom-r.top) {
        LONG_PTR style = GetWindowLongPtrW(game, GWL_STYLE);
        LONG_PTR ex = GetWindowLongPtrW(game, GWL_EXSTYLE);
        SetWindowLongPtrW(game, GWL_STYLE, (style & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP);
        SetWindowLongPtrW(game, GWL_EXSTYLE, ex & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        if (!SetWindowPos(game, NULL, r.left, r.top, r.right-r.left, r.bottom-r.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
            restoreWindow();
            return "position";
        }
        if (!GetWindowRect(game, &current) || !GetClientRect(game, &client) ||
            !EqualRect(&current, &r) || client.right != r.right-r.left || client.bottom != r.bottom-r.top) {
            restoreWindow();
            return "position";
        }
    }
    int ready = 1;
    if (shared) {
        if (shared >= 3) ready = showProjection(&outputs[0], *a, sharedCrop, 0);
        else destroyProjection(&outputs[0]);
        ready = ready && showProjection(&outputs[1], *b, sharedCrop, shared == 1 || shared == 3);
    } else {
        destroyProjection(&outputs[0]);
        if (layout.project) ready = showProjection(&outputs[1], layout.projection, layout.crop, 0);
        else destroyProjection(&outputs[1]);
    }
    if (!ready) {
        restoreWindow();
        return "projection";
    }
    if (mask) showMasks(*a, *b);
    else fadeMasks();
    applied = layout;
    return "ok";
}

__declspec(dllexport) int monitor_apply(void *lua) {
    (void)lua;
    FILE *f = NULL;
    unsigned p1, p2;
    int shared, mask;
    if (_wfopen_s(&f, PREFIX L"request.txt", L"r") != 0) {
        writeStatus("request", NULL);
        return 0;
    }
    int fields = fscanf_s(f, "%u %u %d %d", &p1, &p2, &shared, &mask);
    fclose(f);
    if (fields != 4 || p1 == p2 || shared < 0 || shared > 4 || (mask != 0 && mask != 1)) {
        writeStatus("request", NULL);
        return 0;
    }
    DPI_AWARENESS_CONTEXT dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const char *result = applySelected(p1, p2, shared, mask);
    writeStatus(result, result[0] == 'o' ? &applied : NULL);
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    return 0;
}

__declspec(dllexport) int monitor_resume(void *lua) {
    (void)lua;
    if (!IsWindow(game) || GetForegroundWindow() != game || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        return 0;
    LPARAM key = 1 | ((LPARAM)MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC) << 16);
    PostMessageW(game, WM_KEYDOWN, VK_ESCAPE, key);
    PostMessageW(game, WM_KEYUP, VK_ESCAPE, key | (LPARAM)0xc0000000u);
    return 0;
}

__declspec(dllexport) int monitor_restore(void *lua) {
    (void)lua;
    DPI_AWARENESS_CONTEXT dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    restoreWindow();
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) instance = module;
    return TRUE;
}
