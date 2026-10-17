/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* EXE side dispatch of the forwarded actions (WP-G2). */

/* Compare the class name of hwnd with str (local helper, EXE side). */
static int IsClassNameTxt(HWND hwnd, const TCHAR *str)
{
    TCHAR classname[256];
    return GetClassName(hwnd, classname, ARR_SZ(classname))
        && !lstrcmp(classname, str);
}

/* Show a short user notification. Uses a tray balloon so that nothing is
 * added to the main thread beyond one Shell_NotifyIcon call.
 */
static void ActionNotify(const TCHAR *text)
{
    if (!text || !*text) return;
    if (!tray_added || tray_hidden) {
        LOG("ActionNotify: tray unavailable, message dropped");
        return;
    }
    NOTIFYICONDATAW nid;
    mem00(&nid, sizeof(nid));
    // V2 size (C8): sizeof(NOTIFYICONDATAW) is the Vista layout with
    // _WIN32_WINNT=0x0600 and would fail on XP.
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hwnd;
    nid.uID = 0;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpy_s(nid.szInfoTitle, ARR_SZ(nid.szInfoTitle), TEXT(APP_NAMEA));
    lstrcpy_s(nid.szInfo, ARR_SZ(nid.szInfo), text);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
        LOG("ActionNotify: Shell_NotifyIcon failed");
}

/* Normalize the target window of a forwarded action (TGT-1/TGT-2/TGT-3).
 * Returns NULL when there is nothing valid to act on.
 */
static HWND NormalizeTarget(HWND hwnd, int needproc)
{
    if (!hwnd) return NULL;
    HWND root = GetAncestorL(hwnd, GA_ROOT);
    if (root) hwnd = root;
    if (needproc && IsClassNameTxt(hwnd, TEXT("Ghost"))) {
        HWND real = HungWindowFromGhostWindowL(hwnd);
        if (real) hwnd = real;
    }
    if (!IsWindow(hwnd)) {
        LOG("NormalizeTarget: window %p is gone", hwnd);
        ActionNotify(l10n->ActMsgNoTarget);
        return NULL;
    }
    return hwnd;
}

/* Dispatch one forwarded action, runs on the main thread. */
static void ActionFwdDispatch(HWND hwnd, action_t action)
{
    ACT_TIMER_BEGIN(t);
    HWND target = hwnd;
    switch (action.ac) {
    case AC_WINDOWDETAILS:
        target = NormalizeTarget(hwnd, 1);
        if (target) WindowDetailsShow(target);
        break;
    case AC_REVEALEXE:
        target = NormalizeTarget(hwnd, 1);
        if (target) RevealExeShow(target);
        break;
    case AC_DIMMERFIXED:
    case AC_DIMMERFOLLOW:
        target = NormalizeTarget(hwnd, 0);
        if (target) DimmerToggle(target, action.ac == AC_DIMMERFOLLOW);
        break;
    case AC_SCREENSHOT:
        target = NormalizeTarget(hwnd, 0);
        if (target) ScreenshotStart(target, 0);
        break;
    case AC_CLIPBOARD:
        if (action.fl < 1 || action.fl > 4) {
            LOG("ActFwd: invalid Clipboard sub action %d", (int)action.fl);
            break;
        }
        if (action.fl == 4) {
            // Clearing the clipboard needs no target window.
            ClipboardRequest(NULL, action.fl);
            break;
        }
        target = NormalizeTarget(hwnd, action.fl == 3);
        if (target) ClipboardRequest(target, action.fl);
        break;
    default:
        LOG("ActFwd: unexpected action %d", (int)action.ac);
        break;
    }
    LOG("ActFwd ac=%d fl=%d hwnd=%p -> %p", (int)action.ac, (int)action.fl, hwnd, target);
    ACT_TIMER_END(t, "ActionFwdDispatch");
}

/* Report the outcome of a worker task, runs on the main thread. */
static void ActionResult(WPARAM res)
{
    ACT_TIMER_BEGIN(t);
    UINT kind  = (UINT)((res >> 8) & 0xFF);
    UINT code  = (UINT)(res & 0xFF);
    UINT flags = (UINT)(res & ACRF_PROTECTED);
    const TCHAR *msg = NULL;
    LOG("ActRes kind=%u code=%u flags=%X", kind, code, flags);
    switch (code) {
    case ACR_OK:
    case ACR_CANCEL:     msg = NULL; break;
    case ACR_BUSY:       msg = l10n->ActMsgBusy; break;
    case ACR_CLIPBUSY:   msg = l10n->ActMsgClipBusy; break;
    case ACR_STALE:      msg = l10n->ActMsgStale; break;
    case ACR_NOTARGET:   msg = l10n->ActMsgNoTarget; break;
    case ACR_CAPFAIL:    msg = l10n->ActMsgCapFail; break;
    case ACR_SAVEFAIL:   msg = l10n->ActMsgSaveFail; break;
    case ACR_NOWIC:      msg = l10n->ActMsgNoWIC; break;
    case ACR_NOMEM:      msg = l10n->ActMsgNoMem; break;
    default:             msg = l10n->ActMsgFail; break;
    }
    if (msg) {
        if (flags & ACRF_PROTECTED) {
            // One single balloon: a second call would replace the first one.
            TCHAR buf[256];
            buf[0] = TEXT('\0');
            sb_t sb = { buf, 0, ARR_SZ(buf) };
            SbAppendStr(&sb, msg);
            SbAppendStr(&sb, TEXT(" "));
            SbAppendStr(&sb, l10n->ActMsgProtected);
            ActionNotify(buf);
        } else {
            ActionNotify(msg);
        }
    } else if (flags & ACRF_PROTECTED) {
        ActionNotify(l10n->ActMsgProtected);
    }
    ACT_TIMER_END(t, "ActionResult");
}
