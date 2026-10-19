/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Dimmer (A2/A3, WP-DIM).
 *
 * One single owner=NULL top level layered window (DIM-5) that covers the whole
 * virtual screen and is kept right below the anchor window (DIM-7/DIM-10), so
 * that everything below the anchor looks dimmed and nothing above it does
 * (DIM-2). It is click through, never activated and never in the task bar
 * (DIM-6). The colour is painted by the window itself in WM_ERASEBKGND and the
 * opacity is set with LWA_ALPHA only (DIM-6).
 *
 * Everything here runs on the main thread: the WinEvent callbacks (DIM-13..
 * DIM-15) only count and set dirty bits and arm one merge timer (DIM-16,
 * HOOK-6); the timers do a bounded amount of GetWindow/SetWindowPos work
 * (HOOK-1). Every resource exists only while the dimmer is open (RES-7) and is
 * released by DimmerCleanup(), reached from the three RES-6 places only.
 */

/* Overlay window class, registered once, in this EXE (DIM-5/DIM-6). The name
 * is filtered in the DLL next to APP_NAMEA"-Pin" (DIM-6 last sentence). */
#define DIM_CLASS       TEXT(APP_NAMEA) TEXT("-Dim")

/* DIM-15 range: EVENT_OBJECT_DESTROY (0x8001), EVENT_OBJECT_SHOW (0x8002) and
 * EVENT_OBJECT_HIDE (0x8003). EVENT_OBJECT_LOCATIONCHANGE (0x800B) is outside
 * it on purpose (DIM-17). */
#define DIM_OBJ_MIN     EVENT_OBJECT_DESTROY
#define DIM_OBJ_MAX     EVENT_OBJECT_HIDE

/* One shot merge timer armed by the WinEvent callbacks (DIM-16). */
#define DIM_TIMER_FLUSH 0x4D01
#define DIM_FLUSH_MS    30
/* Backstop re-check period, kept inside the 0.5-1 s window (DIM-16, HOOK-6). */
#define DIM_TIMER_CHECK 0x4D02
#define DIM_CHECK_MS    700
/* DIM-23 owner chain depth limit. */
#define DIM_MAX_OWNER   32
/* DIM-10 Z order search limit above the overlay, and the three outcomes of
 * DimZSearch(): found / budget exhausted / reached the top without finding. */
#define DIM_MAX_ZSEARCH 1024
#define DIM_ZWALK_TOP   0
#define DIM_ZWALK_LIMIT 1
#define DIM_ZWALK_FOUND 2

/* INI keys, section [Advanced] (DIM-4). DimmerColor is the readable RRGGBB
 * hex string, DimmerAlpha an integer 0-255 clamped to 16-255. */
#define DIM_INI_COLOR   TEXT("DimmerColor")
#define DIM_INI_ALPHA   TEXT("DimmerAlpha")
#define DIM_COLOR_DEF   0x000000u
#define DIM_ALPHA_DEF   160
#define DIM_ALPHA_MIN   16

/* Dirty bits, set by the event callbacks only (HOOK-6). */
#define DIM_DIRTY_FG    0x01  /* foreground changed */
#define DIM_DIRTY_VIS   0x02  /* target shown/hidden/minimized/restored */
#define DIM_DIRTY_GONE  0x04  /* target destroy event */
#define DIM_DIRTY_GEOM  0x08  /* display configuration changed */

static struct {
    HWND overlay;        /* the single overlay window, NULL when closed */
    HWND target;         /* current anchor (DIM-23), NULL when there is none */
    int follow;          /* 1 = follow mode, 0 = fixed mode */
    int visible;         /* overlay currently shown */
    int placed;          /* geometry already applied for this target */
    int zblocked;        /* hidden by the DIM-10 check, wait for an event */
    COLORREF color;
    BYTE alpha;
    HBRUSH brush;        /* overlay background, released with the overlay */
    RECT rect;           /* geometry currently set on the overlay */
    HWINEVENTHOOK hook_fg;
    HWINEVENTHOOK hook_min;
    HWINEVENTHOOK hook_obj;
    DWORD obj_pid;       /* PID the per process hook was created for */
    UINT_PTR flush_timer;/* merge timer, 0 when not armed */
    UINT_PTR check_timer;/* backstop timer, 0 when not armed */
    unsigned dirty;
    /* HOOK-5 counters, one summary line when the dimmer closes. */
    unsigned events;
    unsigned flushes;
    unsigned checks;
    unsigned long long max_us;
} dimmer;

#ifdef LOG_STUFF
static LARGE_INTEGER dim_freq;
static void DimStatAdd(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1;
    unsigned long long us;
    QueryPerformanceCounter(&t1);
    if (!dim_freq.QuadPart) QueryPerformanceFrequency(&dim_freq);
    if (!dim_freq.QuadPart) return;
    us = (unsigned long long)(t1.QuadPart - t0.QuadPart) * 1000000ULL
       / (unsigned long long)dim_freq.QuadPart;
    if (us > dimmer.max_us) dimmer.max_us = us;
}
#define DIMSTAT_BEGIN(t) LARGE_INTEGER t; QueryPerformanceCounter(&t)
#define DIMSTAT_END(t) DimStatAdd(t)
#else
#define DIMSTAT_BEGIN(t) do {} while (0)
#define DIMSTAT_END(t) do {} while (0)
#endif

static void DimSchedule(void);
static void DimApply(int from_event);
static void DimClose(const char *why);
static void DimZCheck(int accepted);
static void CALLBACK DimTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
static void CALLBACK DimWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD thread, DWORD ms);

/* Class name test (same shape as the helpers already used on this side). */
static int DimIsClass(HWND hwnd, const TCHAR *str)
{
    TCHAR cls[256];
    return GetClassName(hwnd, cls, ARR_SZ(cls)) && !lstrcmp(cls, str);
}

/* Windows that never are a dimming target nor an anchor: the desktop, the task
 * bar, the system switching UI and our own overlay (DIM-1, DIM-23). */
static int DimIgnored(HWND hwnd)
{
    static const TCHAR *const names[] = {
        TEXT("Progman"), TEXT("WorkerW"),
        TEXT("Shell_TrayWnd"), TEXT("Shell_SecondaryTrayWnd"),
        TEXT("TaskSwitcherWnd"), TEXT("MultitaskingViewFrame"),
        TEXT("XamlExplorerHostIslandWindow"), TEXT("ForegroundStaging"),
        TEXT("DV2ControlHost"),
        DIM_CLASS,
    };
    unsigned i;
    if (!hwnd) return 1;
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) return 1;
    for (i = 0; i < ARR_SZ(names); i++)
        if (DimIsClass(hwnd, names[i])) return 1;
    return 0;
}

/* DIM-23: walk the owner chain from start and stop at the first owner that is
 * not acceptable; the last accepted window is the anchor. */
static HWND DimAnchor(HWND start)
{
    HWND w = start;
    int depth = 0;
    if (!w || (dimmer.overlay && w == dimmer.overlay)) return NULL;
    while (depth++ < DIM_MAX_OWNER) {
        HWND o = GetWindow(w, GW_OWNER);
        DWORD pidw = 0, pido = 0;
        RECT rc;
        if (!o || o == w) break;                        /* no owner left */
        if (DimIgnored(o)) break;                       /* desktop, task bar */
        if (!IsWindowVisible(o) || IsWindowCloaked(o)) break;
        if (IsIconic(o)) break;                         /* still "visible" */
        if (!GetWindowRect(o, &rc)) break;
        if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) break;
        GetWindowThreadProcessId(w, &pidw);
        GetWindowThreadProcessId(o, &pido);
        if (!pidw || pidw != pido) break;               /* other process */
        w = o;
    }
    return w;
}

/* Window that can be dimmed right now (DIM-20: IsWindow, visible, cloak). */
static int DimShowable(HWND hwnd)
{
    RECT rc;
    if (!hwnd || !IsWindow(hwnd)) return 0;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return 0;
    if (IsWindowCloaked(hwnd)) return 0;
    if (!GetWindowRect(hwnd, &rc)) return 0;
    return rc.right > rc.left && rc.bottom > rc.top;
}

/* Usable foreground window for follow mode (DIM-1 ignore list). */
static int DimForegroundOk(HWND fg)
{
    if (!fg || dimmer.overlay == fg || DimIgnored(fg)) return 0;
    return DimShowable(fg);
}

/* Virtual screen rect in physical pixels (DIM-6, DIM-11). */
static void DimVirtualRect(RECT *rc)
{
    rc->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rc->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rc->right  = rc->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rc->bottom = rc->top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* Parse "RRGGBB" (human order, red first). Returns -1 when the string is not
 * exactly six hexadecimal digits (DIM-4: range checked, no guessing). */
static int DimParseColor(const TCHAR *txt)
{
    unsigned val = 0;
    int i;
    for (i = 0; i < 6; i++) {
        TCHAR c = txt[i];
        unsigned d;
        if (c >= TEXT('0') && c <= TEXT('9')) d = (unsigned)(c - TEXT('0'));
        else if (c >= TEXT('a') && c <= TEXT('f')) d = (unsigned)(c - TEXT('a')) + 10;
        else if (c >= TEXT('A') && c <= TEXT('F')) d = (unsigned)(c - TEXT('A')) + 10;
        else return -1;
        val = (val << 4) | d;
    }
    if (txt[6]) return -1;   /* trailing characters are not a valid colour */
    return (int)val;
}

/* Parse a decimal string into 0..255. Returns -1 for anything else, so that
 * an invalid value falls back to the default (DIM-4). */
static int DimParseByte(const TCHAR *txt)
{
    int i, ndig = 0;
    unsigned val = 0;
    while (txt[ndig] == TEXT(' ')) ndig++;
    for (i = ndig; txt[i]; i++) {
        if (txt[i] < TEXT('0') || txt[i] > TEXT('9')) return -1;
        val = val * 10 + (unsigned)(txt[i] - TEXT('0'));
        if (val > 255) return -1;   /* bail out before it can wrap */
    }
    if (i == ndig) return -1;       /* empty string */
    return (int)val;
}

/* Read the colour and the opacity, once per opening (DIM-4). */
static void DimReadConfig(void)
{
    TCHAR txt[32];
    int val = DIM_COLOR_DEF;
    int alpha = -1;

    txt[0] = TEXT('\0');
    if (GetPrivateProfileString(TEXT("Advanced"), DIM_INI_COLOR, TEXT("000000"),
                                txt, ARR_SZ(txt), inipath)) {
        int parsed = DimParseColor(txt);
        if (parsed < 0) {
            LOG("Dimmer: invalid DimmerColor \"%S\", using default", txt);
        } else {
            val = parsed;
        }
    }
    /* "RRGGBB" is the human order, COLORREF is 0x00BBGGRR. */
    dimmer.color = RGB((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);

    txt[0] = TEXT('\0');
    if (GetPrivateProfileString(TEXT("Advanced"), DIM_INI_ALPHA, TEXT("160"),
                                txt, ARR_SZ(txt), inipath))
        alpha = DimParseByte(txt);
    if (alpha < 0) {
        if (txt[0]) LOG("Dimmer: invalid DimmerAlpha \"%S\", using default", txt);
        alpha = DIM_ALPHA_DEF;
    }
    /* Below 16 the overlay would be invisible, so clamp it up (DIM-4). */
    if (alpha < DIM_ALPHA_MIN) {
        LOG("Dimmer: DimmerAlpha %d is below the minimum, using %d",
            alpha, DIM_ALPHA_MIN);
        alpha = DIM_ALPHA_MIN;
    }
    dimmer.alpha = (BYTE)alpha;
    LOG("Dimmer: RRGGBB=%06X alpha=%u", (unsigned)val, (unsigned)dimmer.alpha);
}

/* Overlay window proc: paints the colour, notices display changes. */
static LRESULT CALLBACK DimWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        /* DIM-6: fill the whole client area with the configured colour. */
        if (dimmer.brush) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, dimmer.brush);
            return 1;
        }
        break;
    case WM_PAINT: {
        /* DIM-6: BeginPaint/EndPaint only, the erase handler did the work. */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        /* DIM-11: never destroy or recreate, only recompute and resize. */
        dimmer.dirty |= DIM_DIRTY_GEOM;
        dimmer.placed = 0;
        DimSchedule();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* Register the class once and create the single overlay (DIM-5, DIM-6). */
static int DimCreateOverlay(void)
{
    WNDCLASS wnd;

    dimmer.brush = CreateSolidBrush(dimmer.color);
    if (!dimmer.brush) {
        LOG("Dimmer: CreateSolidBrush failed, err=%u", (unsigned)GetLastError());
        return 0;
    }
    if (!GetClassInfo(g_hinst, DIM_CLASS, &wnd)) {
        mem00(&wnd, sizeof(wnd));
        wnd.style = CS_NOCLOSE | CS_HREDRAW | CS_VREDRAW;
        wnd.lpfnWndProc = DimWindowProc;
        wnd.hInstance = g_hinst;
        wnd.hCursor = LoadCursor(NULL, IDC_ARROW);
        wnd.hbrBackground = NULL;   /* DIM-6: it paints its own colour */
        wnd.lpszClassName = DIM_CLASS;
        if (!RegisterClass(&wnd)) {
            LOG("Dimmer: RegisterClass failed, err=%u", (unsigned)GetLastError());
            DeleteObject(dimmer.brush);
            dimmer.brush = NULL;
            return 0;
        }
    }
    /* Owner NULL and no WS_EX_TOPMOST (DIM-5); click through, never activated,
     * no task bar entry (DIM-6). */
    dimmer.overlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        DIM_CLASS, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_hinst, NULL);
    if (!dimmer.overlay) {
        LOG("Dimmer: CreateWindowEx failed, err=%u", (unsigned)GetLastError());
        DeleteObject(dimmer.brush);
        dimmer.brush = NULL;
        return 0;
    }
    /* DIM-6: LWA_ALPHA only, no LWA_COLORKEY, and before the first show. */
    if (!SetLayeredWindowAttributesL(dimmer.overlay, 0, dimmer.alpha, LWA_ALPHA))
        LOG("Dimmer: SetLayeredWindowAttributes failed, err=%u",
            (unsigned)GetLastError());
    return 1;
}

/* DIM-13/DIM-14: the two global hooks, registered only while open. */
static void DimHookEvents(void)
{
    dimmer.hook_fg = SetWinEventHookL(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, DimWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    dimmer.hook_min = SetWinEventHookL(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        NULL, DimWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!dimmer.hook_fg || !dimmer.hook_min)
        LOG("Dimmer: SetWinEventHook failed fg=%p min=%p",
            dimmer.hook_fg, dimmer.hook_min);
}

static void DimUnhookEvents(void)
{
    if (dimmer.hook_fg) {
        UnhookWinEventL(dimmer.hook_fg);
        dimmer.hook_fg = NULL;
    }
    if (dimmer.hook_min) {
        UnhookWinEventL(dimmer.hook_min);
        dimmer.hook_min = NULL;
    }
}

static void DimUnhookObject(void)
{
    if (dimmer.hook_obj) {
        UnhookWinEventL(dimmer.hook_obj);
        dimmer.hook_obj = NULL;
    }
    dimmer.obj_pid = 0;
}

/* DIM-15: per target PID destroy/hide/show events. The old hook is always
 * released before another process gets watched. */
static void DimHookObject(HWND hwnd)
{
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    if (pid && pid == dimmer.obj_pid && dimmer.hook_obj) return;
    DimUnhookObject();
    if (!pid) return;
    dimmer.hook_obj = SetWinEventHookL(DIM_OBJ_MIN, DIM_OBJ_MAX,
        NULL, DimWinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
    if (dimmer.hook_obj) dimmer.obj_pid = pid;
    else LOG("Dimmer: per PID hook failed, pid=%u", (unsigned)pid);
}

/* Arm the one shot merge timer if it is not armed yet (DIM-16). If SetTimer
 * fails nothing is retried here: the dirty bits stay set and are consumed by the
 * next merge attempt or by the backstop timer, so a failing SetTimer cannot spin.
 * (While the overlay is hidden the backstop skips from_event=0 runs, so in that
 * state the bits wait for the next event.) */
static void DimSchedule(void)
{
    if (!dimmer.overlay || dimmer.flush_timer) return;
    dimmer.flush_timer = SetTimer(NULL, DIM_TIMER_FLUSH, DIM_FLUSH_MS, DimTimerProc);
    if (!dimmer.flush_timer) LOG("Dimmer: SetTimer(merge) failed");
}

/* WinEvent callback: counters and dirty bits only, then one merge timer
 * (DIM-13..DIM-16, HOOK-5/6). */
static void CALLBACK DimWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD thread, DWORD ms)
{
    DIMSTAT_BEGIN(st);
    (void)hook; (void)idChild; (void)thread; (void)ms;
    dimmer.events++;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        dimmer.dirty |= DIM_DIRTY_FG;
    } else if (event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND) {
        /* DIM-14: only events about the target matter. */
        if (hwnd && hwnd == dimmer.target) dimmer.dirty |= DIM_DIRTY_VIS;
    } else if (idObject == OBJID_WINDOW && hwnd && hwnd == dimmer.target) {
        /* DIM-15: the per PID hook also covers other windows of the PID. */
        if (event == EVENT_OBJECT_DESTROY) dimmer.dirty |= DIM_DIRTY_GONE;
        else dimmer.dirty |= DIM_DIRTY_VIS;
    }
    DimSchedule();
    DIMSTAT_END(st);
}

static void DimShowOverlay(void)
{
    if (!dimmer.overlay || dimmer.visible) return;
    /* SW_SHOWNA: show without activating (DIM-6). */
    ShowWindow(dimmer.overlay, SW_SHOWNA);
    dimmer.visible = 1;
}

static void DimHideOverlay(void)
{
    if (!dimmer.overlay || !dimmer.visible) return;
    ShowWindow(dimmer.overlay, SW_HIDE);
    dimmer.visible = 0;
}

/* Walk the Z order upwards from 'from' looking for 'want' (DIM-10).
 * Three results so that "the walk gave up" is never confused with "the target is
 * not anywhere above the overlay":
 *   DIM_ZWALK_FOUND - 'want' was reached, so the overlay is below it;
 *   DIM_ZWALK_LIMIT - the DIM_MAX_ZSEARCH budget ran out, result unknown;
 *   DIM_ZWALK_TOP   - the walk reached the top of the Z order, 'want' is not above.
 */
static int DimZSearch(HWND from, HWND want)
{
    HWND h = from;
    int i = 0;
    while ((h = GetWindow(h, GW_HWNDPREV)) != NULL) {
        if (h == want) return DIM_ZWALK_FOUND;
        if (++i > DIM_MAX_ZSEARCH) return DIM_ZWALK_LIMIT;
    }
    return DIM_ZWALK_TOP;
}

/* DIM-7, exactly this form, and never more than once per check. */
static void DimReanchor(void)
{
    SetWindowPos(dimmer.overlay, dimmer.target, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* DIM-10 level check. 'accepted' marks a placement that an earlier check already
 * accepted. After the round 2 order fix both cases verify the current level the
 * same way, so the flag no longer selects a code path (the call sites still pass
 * it to document their intent). Every call does at most one DIM-7, there is no
 * loop, and the topmost state of neither window is ever touched. The routine
 * paths stay silent because this also runs from the backstop timer (HOOK-5/6);
 * only the give-up path logs one line.
 */
static void DimZCheck(int accepted)
{
    int walk;

    (void)accepted;
    if (!dimmer.overlay || !dimmer.target || !dimmer.visible) return;
    if (GetWindow(dimmer.target, GW_HWNDNEXT) == dimmer.overlay) return;

    /* The overlay can be below the target without being its immediate neighbour:
     * the system keeps other windows of the same band in between (IME and CTF
     * windows do it). Searching first avoids spending the DIM-7 of this check on
     * a level that is already healthy. */
    walk = DimZSearch(dimmer.overlay, dimmer.target);
    if (walk == DIM_ZWALK_FOUND) return;

    /* Not provably below: spend the one DIM-7 of this check, then re-check it the
     * same way, neighbour first and upward walk second. */
    DimReanchor();
    if (GetWindow(dimmer.target, GW_HWNDNEXT) == dimmer.overlay) return;
    walk = DimZSearch(dimmer.overlay, dimmer.target);
    if (walk == DIM_ZWALK_FOUND) return;
    /* The overlay is not provably below the anchor, so it may be above it and
     * would dim it: hide and wait for the next relevant event, never retry from
     * the timer (DIM-10). This is a conservative fallback, not a proof, hence the
     * walk result in the log: DIM_ZWALK_TOP means the walk reached the top of the
     * Z order without meeting the target, DIM_ZWALK_LIMIT that the step budget
     * ran out first (inconclusive, the chain may just be long). */
    LOG("Dimmer: overlay not below anchor %p (walk=%d), hiding", dimmer.target, walk);
    dimmer.zblocked = 1;
    DimHideOverlay();
}

/* DIM-8: size/position the overlay and keep it right below the target.
 * 'recheck_only' is set by the periodic backstop: it must not do a full DIM-7
 * cycle, only repeat the DIM-10 search (never spin, see DIM-10/DIM-16). */
static void DimPlaceOverlay(int recheck_only)
{
    RECT vs;
    if (!dimmer.overlay || !dimmer.target || !dimmer.visible) return;
    DimVirtualRect(&vs);
    if (vs.right <= vs.left || vs.bottom <= vs.top) {
        LOG("Dimmer: empty virtual screen %ld,%ld %ldx%ld", (long)vs.left,
            (long)vs.top, (long)(vs.right - vs.left), (long)(vs.bottom - vs.top));
        return;
    }
    if (!dimmer.placed
    ||  vs.left != dimmer.rect.left || vs.top != dimmer.rect.top
    ||  vs.right != dimmer.rect.right || vs.bottom != dimmer.rect.bottom) {
        /* DIM-8 second form: resize and re-anchor in one call. Never HWND_TOP
         * (DIM-9) and never SWP_NOMOVE/SWP_NOSIZE while changing geometry. */
        SetWindowPos(dimmer.overlay, dimmer.target, vs.left, vs.top,
                     vs.right - vs.left, vs.bottom - vs.top, SWP_NOACTIVATE);
        dimmer.rect = vs;
        dimmer.placed = 1;
        DimZCheck(0);
        return;
    }
    DimZCheck(recheck_only);
}

/* Reconcile the overlay with the current target (DIM-3, DIM-14, DIM-18..20). */
static void DimApply(int from_event)
{
    ACT_TIMER_BEGIN(t);
    if (!dimmer.overlay) {
        ACT_TIMER_END(t, "DimApply");
        return;
    }
    if (!dimmer.target || !IsWindow(dimmer.target)) {
        int had = (dimmer.target != NULL);
        if (!dimmer.follow) {
            /* DIM-18: a destroyed fixed target closes the dimmer. */
            ACT_TIMER_END(t, "DimApply");
            DimClose("fixed target destroyed");
            return;
        }
        if (!had) {
            /* DIM-19: already waiting for the next usable foreground window,
             * nothing to do and nothing to log on every backstop tick. */
            ACT_TIMER_END(t, "DimApply");
            return;
        }
        /* DIM-19: stay in follow mode, forget the dead target. */
        LOG("Dimmer: follow target is gone");
        dimmer.target = NULL;
        dimmer.placed = 0;
        DimUnhookObject();
        DimHideOverlay();
        ACT_TIMER_END(t, "DimApply");
        return;
    }
    if (!DimShowable(dimmer.target)) {
        /* DIM-18: a hidden/minimized fixed target keeps its binding.
         * DIM-19/DIM-20: a follow target that stopped being usable is dropped. */
        if (dimmer.follow) {
            if (dimmer.visible) {
                LOG("Dimmer: follow target %p not usable, dropping it", dimmer.target);
            }
            dimmer.target = NULL;
            dimmer.placed = 0;
            DimUnhookObject();
        }
        DimHideOverlay();
        ACT_TIMER_END(t, "DimApply");
        return;
    }
    if (!dimmer.visible) {
        /* DIM-10: after the Z check hid the overlay only a fresh event may
         * try again, the periodic backstop must not spin. */
        if (dimmer.zblocked && !from_event) {
            ACT_TIMER_END(t, "DimApply");
            return;
        }
        dimmer.zblocked = 0;
        DimShowOverlay();
    }
    DimPlaceOverlay(!from_event);
    ACT_TIMER_END(t, "DimApply");
}

/* Follow mode: recompute the anchor from the current foreground (DIM-13). */
static void DimFollowForeground(void)
{
    HWND fg = GetForegroundWindow();
    HWND anchor;
    if (!DimForegroundOk(fg)) {
        /* DIM-19: desktop/task bar/foreign switcher, keep the current target. */
        return;
    }
    anchor = DimAnchor(fg);
    if (!anchor || anchor == dimmer.target) return;
    LOG("Dimmer: follow anchor %p -> %p", dimmer.target, anchor);
    dimmer.target = anchor;
    dimmer.placed = 0;
    dimmer.zblocked = 0;
    DimHookObject(anchor);
}

/* DIM-18/DIM-19: the target window got an EVENT_OBJECT_DESTROY. The handle may
 * already be recycled by the time the merge timer runs, so the event itself is
 * the evidence: a fixed target closes the dimmer, a follow target is dropped
 * while follow mode stays on.
 */
static void DimTargetGone(void)
{
    if (!dimmer.overlay) return;
    if (!dimmer.follow) {
        LOG("Dimmer: fixed target destroyed, closing");
        DimClose("fixed target destroyed");
        return;
    }
    LOG("Dimmer: follow target destroyed, waiting for the next one");
    dimmer.target = NULL;
    dimmer.placed = 0;
    dimmer.zblocked = 0;
    DimUnhookObject();
    DimHideOverlay();
}

/* Merge and backstop timer (DIM-16, HOOK-5/6). */
static void CALLBACK DimTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    DIMSTAT_BEGIN(st);
    (void)hwnd; (void)msg; (void)time;
    if (dimmer.flush_timer && id == dimmer.flush_timer) {
        unsigned dirty;
        KillTimer(NULL, dimmer.flush_timer);
        dimmer.flush_timer = 0;
        dimmer.flushes++;
        if (!dimmer.overlay) {
            DIMSTAT_END(st);
            return;
        }
        dirty = dimmer.dirty;
        dimmer.dirty = 0;
        if (dirty & DIM_DIRTY_GONE) {
            /* DIM-18/DIM-19: handle the destroy event before touching the
             * (possibly recycled) handle in DimApply. */
            DimTargetGone();
            dirty &= ~DIM_DIRTY_GONE;
        }
        if (!dimmer.overlay) {
            DIMSTAT_END(st);
            return;
        }
        if (dirty & DIM_DIRTY_FG) {
            if (dimmer.follow) {
                DimFollowForeground();
                DimApply(1);
            } else if (dimmer.visible) {
                /* DIM-13: fixed mode only re-checks the Z order. */
                DimZCheck(1);
            } else if (dimmer.zblocked) {
                /* DIM-10: a relevant event is the only thing that may retry
                 * after the overlay was hidden by the level check. */
                DimApply(1);
            }
        }
        if (dirty & (DIM_DIRTY_VIS | DIM_DIRTY_GEOM)) DimApply(1);
    } else if (dimmer.check_timer && id == dimmer.check_timer) {
        dimmer.checks++;
        if (dimmer.overlay) DimApply(0);
    }
    DIMSTAT_END(st);
}

/* Release every resource and log one HOOK-5 summary line.
 * Invariant this early return relies on: the two timers and the three WinEvent
 * hooks only ever exist while the overlay exists. They are all created after a
 * successful DimCreateOverlay() and are all released here, so "no overlay and no
 * brush" means there is nothing left to release.
 */
static void DimClose(const char *why)
{
    if (!dimmer.overlay && !dimmer.brush) return;
    LOG("Dimmer: closing (%s)", why);
    if (dimmer.flush_timer) {
        KillTimer(NULL, dimmer.flush_timer);
        dimmer.flush_timer = 0;
    }
    if (dimmer.check_timer) {
        KillTimer(NULL, dimmer.check_timer);
        dimmer.check_timer = 0;
    }
    DimUnhookEvents();
    DimUnhookObject();
    if (dimmer.overlay) {
        DestroyWindow(dimmer.overlay);
        dimmer.overlay = NULL;
    }
    if (dimmer.brush) {
        DeleteObject(dimmer.brush);
        dimmer.brush = NULL;
    }
    LOG("Dimmer summary: events=%u flushes=%u checks=%u max_us=%u",
        dimmer.events, dimmer.flushes, dimmer.checks, (unsigned)dimmer.max_us);
    mem00(&dimmer, sizeof(dimmer));
}

/* Toggle the dimmer for hwnd; follow=1 keeps following the foreground window.
 * DIM-3: the same mode again closes, the other mode switches. */
static void DimmerToggle(HWND hwnd, int follow)
{
    ACT_TIMER_BEGIN(t);
    HWND anchor = hwnd ? DimAnchor(hwnd) : NULL;
    LOG("DimmerToggle hwnd=%p follow=%d anchor=%p open=%d mode=%d",
        hwnd, follow, anchor, !!dimmer.overlay, dimmer.follow);

    if (!dimmer.overlay) {
        /* Closed: open in the requested mode. */
        if (!anchor) {
            /* UX-3: the user asked for the dimmer, tell them why nothing
             * happened (only this user triggered path notifies). */
            LOG("Dimmer: no usable anchor for %p", hwnd);
            ActionNotify(l10n->ActMsgNoTarget);
            ACT_TIMER_END(t, "DimmerToggle");
            return;
        }
        DimReadConfig();
        if (!DimCreateOverlay()) {
            ActionNotify(l10n->ActMsgFail);
            ACT_TIMER_END(t, "DimmerToggle");
            return;
        }
        dimmer.follow = follow;
        DimHookEvents();
        if (follow) {
            HWND fg = GetForegroundWindow();
            /* DIM-3: follow mode takes the current usable foreground window.
             * When there is none (for example the action came from the tray),
             * the window the action was invoked on is a fair starting point. */
            if (DimForegroundOk(fg)) {
                HWND fanchor = DimAnchor(fg);
                if (fanchor) anchor = fanchor;
            }
        }
        dimmer.target = anchor;
        dimmer.placed = 0;
        DimHookObject(anchor);
        dimmer.check_timer = SetTimer(NULL, DIM_TIMER_CHECK, DIM_CHECK_MS, DimTimerProc);
        if (!dimmer.check_timer) LOG("Dimmer: backstop timer failed");
        DimApply(1);
    } else if (dimmer.follow == follow) {
        /* Same mode again: close (DIM-3). */
        DimClose("same mode toggled off");
    } else {
        /* Other mode: switch, keep the single overlay (DIM-3). */
        dimmer.follow = follow;
        LOG("Dimmer: mode switch, follow=%d", follow);
        if (follow) {
            DimFollowForeground();
        } else {
            /* DIM-3: bind the target that is dimmed right now. When follow mode
             * had dropped its target (DIM-19), fall back to the window the
             * action was invoked on, which is the only clear target left. */
            HWND fixed = dimmer.target ? dimmer.target : anchor;
            if (!fixed) {
                /* No target to pin: close instead of leaving the dimmer in
                 * fixed mode with nothing to dim (DIM-3/UX-3). */
                DimClose("no target for fixed mode");
                ActionNotify(l10n->ActMsgNoTarget);
                ACT_TIMER_END(t, "DimmerToggle");
                return;
            }
            dimmer.target = fixed;
            dimmer.placed = 0;
            dimmer.zblocked = 0;
            DimHookObject(fixed);
        }
        DimApply(1);
    }
    ACT_TIMER_END(t, "DimmerToggle");
}

/* Close the dimmer. Called only from the three RES-6 places. */
static void DimmerCleanup(void)
{
    DimClose("cleanup");
}
