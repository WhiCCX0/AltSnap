/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Align action (A6), runs inside hooks.dll.
 * The align area is the work area of the monitor the target window is on
 * (ALIGN-3, T4), the placement restore rect is used as the geometry of a
 * zoomed window (ALIGN-6). The path follows CenterWindow(): geometry ->
 * monitor info -> invisible borders -> MoveWindowAsync() (ALIGN-7).
 */

/* Placement of one axis inside the align area, used for X and for Y */
#define AL_START  1
#define AL_CENTER 2
#define AL_END    3

/* Keypad layout of the sub actions (ALIGN-5, ACT-5):
 * 7/8/9 = top left/top middle/top right, 4/5/6 = left/middle/right,
 * 1/2/3 = bottom left/bottom middle/bottom right,
 * 10 = horizontal center (keep Y), 11 = vertical center (keep X).
 */
static void AlignGrid(BYTE sub, int *xalign, int *yalign)
{
    static const BYTE grid[9][2] = {
        { AL_START,  AL_END    },
        { AL_CENTER, AL_END    },
        { AL_END,    AL_END    },
        { AL_START,  AL_CENTER },
        { AL_CENTER, AL_CENTER },
        { AL_END,    AL_CENTER },
        { AL_START,  AL_START  },
        { AL_CENTER, AL_START  },
        { AL_END,    AL_START  },
    };
    if (sub >= 1 && sub <= 9) {
        *xalign = grid[sub-1][0];
        *yalign = grid[sub-1][1];
    } else if (sub == 10) {
        *xalign = AL_CENTER;
        *yalign = 0; // keep the current Y
    } else if (sub == 11) {
        *xalign = 0; // keep the current X
        *yalign = AL_CENTER;
    }
}

/* MDI client the target belongs to, or NULL for a normal window.
 * The MDI case follows the original path and uses state.mdiclient (ALIGN-3);
 * the cache is only used when the target really is that MDI client or one of
 * its children, so a stale value can not move the wrong window (ACT-7).
 */
static HWND AlignMdiClient(HWND hwnd)
{
    HWND mdiclient = state.mdiclient;
    if (mdiclient && IsWindow(mdiclient)
    && (hwnd == mdiclient || GetParent(hwnd) == mdiclient)) {
        return mdiclient;
    }
    // The cache is not about this window: detect the MDI client directly.
    if (conf.MDI && (GetWindowLongPtr(hwnd, GWL_EXSTYLE)&WS_EX_MDICHILD)) {
        HWND parent = GetParent(hwnd);
        if (parent && isClassName(parent, TEXT("MDIClient"))) return parent;
    }
    return NULL;
}

/* Area the window is aligned in: the client area of the MDI client, or the
 * work area of the monitor the target window is on (ALIGN-3, T4). Same base
 * as GetMonitorRect(), but taken from the target window and not from the
 * mouse position. Returns FALSE if the area is unknown or empty.
 */
static BOOL AlignBaseRect(HWND hwnd, HWND mdiclient, RECT *base)
{
    if (mdiclient && GetClientRect(mdiclient, base)) {
        // MDI: the MDI client area, in the same client coordinates as the
        // MDI child geometry, so no offset is needed (ALIGN-3).
        return !IsRectEmpty(base);
    }
    // GetMonitorInfoFromWin() does not check GetMonitorInfo(), so the struct
    // is cleared first: an empty work area means the area is unknown.
    MONITORINFO mi;
    mem00(&mi, sizeof(mi));
    GetMonitorInfoFromWin(hwnd, &mi);
    if (IsRectEmpty(&mi.rcWork)) return FALSE;
    CopyRect(base, &mi.rcWork);
    return TRUE;
}

/* Geometry the align is computed from, in the coordinate space that
 * MoveWindowAsync() uses (client coordinates for MDI children).
 * A normal window keeps its current geometry (ALIGN-2). A zoomed window uses
 * the normal rect from its window placement, never the zoomed rect (ALIGN-6).
 * The restore data is not used: it holds the size of the last window AltSnap
 * moved and is guarded by state.origin.maximized, so it can not tell the
 * normal size of this window (B2).
 * rcNormalPosition is used as is, without any workspace offset: upstream
 * writes screen coordinates into it everywhere (hooks.c:1309, 2288-2291,
 * RestoreWindowToRect at 1329), so both sides stay in the same space.
 */
static BOOL AlignWindowRect(HWND hwnd, HWND mdiclient, RECT *rc)
{
    if (!IsZoomed(hwnd)) {
        if (!GetWindowRect(hwnd, rc)) return FALSE;
        if (mdiclient) {
            // MDI child: screen coordinates -> MDI client coordinates (ALIGN-3)
            POINT pt = { 0, 0 };
            if (!ClientToScreen(mdiclient, &pt)) return FALSE;
            OffsetRect(rc, -pt.x, -pt.y);
        }
    } else {
        // Zoomed window: the normal rect is the geometry it is restored to
        WINDOWPLACEMENT wndpl;
        wndpl.length = sizeof(wndpl);
        if (!GetWindowPlacement(hwnd, &wndpl)) return FALSE;
        CopyRect(rc, &wndpl.rcNormalPosition);
    }
    return rc->right > rc->left && rc->bottom > rc->top;
}

/* Position of one axis inside [start, end] for a visible size of vis.
 * The three placements are start, centered and end (ALIGN-5). A window
 * bigger than the area keeps its size and is aligned on the left/top
 * border point (ALIGN-6).
 */
static int AlignAxis(int start, int end, int vis, int align)
{
    int space = (end - start) - vis;
    if (space < 0) return start;
    if (align == AL_CENTER) return start + space / 2;
    if (align == AL_END)    return start + space;
    return start;
}

/* Perform the align sub action (1..11) on the given window.
 * hwnd is the only trusted target (ACT-7); invalid sub actions do nothing.
 * Only the position changes (ALIGN-2), the window is never activated and
 * the Z order is left alone (ALIGN-8).
 */
static void ActionAlign(HWND hwnd, BYTE sub)
{
    ACT_TIMER_BEGIN(t);
    int xalign = 0, yalign = 0;
    RECT base, rc, bd;
    int width, height, x, y;

    LOG("ActionAlign hwnd=%p sub=%d", hwnd, (int)sub);
    if (sub < 1 || sub > 11) {
        LOG("ActionAlign: invalid sub action %d", (int)sub);
        ACT_TIMER_END(t, "ActionAlign");
        return;
    }
    if (!IsWindow(hwnd)) {
        LOG("ActionAlign: invalid window %p", hwnd);
        ACT_TIMER_END(t, "ActionAlign");
        return;
    }
    if (IsIconic(hwnd)) {
        // A minimized window is not restored here (UX-2)
        LOG("ActionAlign: minimized window %p, nothing done", hwnd);
        ACT_TIMER_END(t, "ActionAlign");
        return;
    }

    AlignGrid(sub, &xalign, &yalign);
    HWND mdiclient = AlignMdiClient(hwnd);
    if (!AlignWindowRect(hwnd, mdiclient, &rc)) {
        LOG("ActionAlign: no usable geometry for %p", hwnd);
        ACT_TIMER_END(t, "ActionAlign");
        return;
    }
    width  = rc.right - rc.left;
    height = rc.bottom - rc.top;
    if (!AlignBaseRect(hwnd, mdiclient, &base)) {
        LOG("ActionAlign: no align area for %p", hwnd);
        ACT_TIMER_END(t, "ActionAlign");
        return;
    }

    // Invisible borders, so the visible part of the window is aligned
    // (ALIGN-5). bd is the offset from the window rect to the visible rect
    // (positive on Win10+), the same convention CenterWindow() uses.
    FixDWMRectLL(hwnd, &bd, 0);

    // The axis that is not aligned keeps the rect from above (ACT-5, ALIGN-6)
    x = rc.left;
    if (xalign)
        x = AlignAxis(base.left, base.right, width - bd.left - bd.right, xalign) - bd.left;
    y = rc.top;
    if (yalign)
        y = AlignAxis(base.top, base.bottom, height - bd.top - bd.bottom, yalign) - bd.top;

    LOG("ActionAlign: mdi=%p base=(%d,%d)-(%d,%d) rc=(%d,%d) size=%dx%d bd=(%d,%d)-(%d,%d) -> (%d,%d)"
       , mdiclient, (int)base.left, (int)base.top, (int)base.right, (int)base.bottom
       , (int)rc.left, (int)rc.top, width, height
       , (int)bd.left, (int)bd.top, (int)bd.right, (int)bd.bottom, x, y);
    // MoveWindowAsync() restores the window if needed (ALIGN-7)
    MoveWindowAsync(hwnd, x, y, width, height);
    ACT_TIMER_END(t, "ActionAlign");
}
