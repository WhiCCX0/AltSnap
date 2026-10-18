/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Process identity, window details dialog and reveal-in-Explorer (WP-PROC).
 * A: GetWindowProcPath()  - full Win32 path of the process of a window
 * B: WindowDetailsShow()  - modal read-only window details dialog
 * C: RevealExeShow()      - explorer.exe /select,"<exe>"
 * Everything here runs on the main thread and must stay bounded (HOOK-2),
 * all big buffers are on the heap (ABI-3).
 */

#include <psapi.h> /* only PROCESS_MEMORY_COUNTERS, the API is loaded at runtime */

#define PATH_CHARS   32768 /* PATH-5: 32768 TCHAR path buffer, heap only */
#define DETAILS_CHARS (PATH_CHARS + 4096) /* one path plus all the other fields */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * A. Process identity and full path (PATH-1..PATH-5)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* QueryFullProcessImageNameW is Vista+ (API-1/API-3): resolved at runtime,
 * it never appears in the import table. */
static BOOL QueryFullProcessImageNameWL(HANDLE proc, DWORD flags, TCHAR *name, DWORD *size)
{
    typedef BOOL (WINAPI *funk_t)(HANDLE, DWORD, TCHAR *, DWORD *);
    static funk_t funk = (funk_t)IPTR;

    if (funk == (funk_t)IPTR) {
        funk = (funk_t)LoadDLLProc("KERNEL32.DLL", "QueryFullProcessImageNameW");
    }
    if (!funk) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    return funk(proc, flags, name, size);
}

/* AFH support (TGT-2, PATH-1): the real child process of
 * ApplicationFrameHost.exe is looked for exactly like
 * GetWindowProgNameSubWindow() does (same enumeration, unchanged behaviour,
 * PATH-4), but only its PID is kept here: the full path is queried by the
 * caller afterwards. Returns 0 when there is no such child.
 */
static DWORD GetWindowRealPid(DWORD pid, HWND hwnd)
{
    TCHAR *tmp;
    DWORD child = 0;
    if (!pid) return 0;
    /* The child name is of no use here, but the function needs a buffer */
    tmp = (TCHAR *)calloc(PATH_CHARS, sizeof(TCHAR));
    if (!tmp) return 0;
    child = GetWindowProgNameSubWindow(pid, hwnd, tmp, PATH_CHARS);
    if (child) LOG("GetWindowRealPid: AFH %u -> child %u", (unsigned)pid, (unsigned)child);
    free(tmp);
    return child;
}

/* Name of the file in a full path, without modifying the path.
 * Returns an empty string for NULL or for a path that ends with a separator,
 * so that callers can test `*PathFileName(p)` directly.
 */
static const TCHAR *PathFileName(const TCHAR *path)
{
    const TCHAR *name = path ? path : TEXT("");
    for (const TCHAR *p = path; p && *p; p++) {
        if (*p == TEXT('\\') || *p == TEXT('/'))
            name = p + 1;
    }
    return name;
}

/* Query the full Win32 path of one process (PATH-2 steps 1 and 2).
 * Returns 0 on success and *path is a heap buffer of PATH_CHARS characters
 * that the caller frees. On failure *path stays NULL.
 */
static DWORD QueryProcPath(DWORD target, TCHAR **path)
{
    DWORD err, err2, size;
    TCHAR *buf;
    HANDLE proc;

    *path = NULL;
    /* 1) Vista+: limited information rights are enough for the path (API-3) */
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target);
    if (proc) {
        buf = (TCHAR *)malloc(PATH_CHARS * sizeof(TCHAR));
        if (!buf) {
            CloseHandle(proc);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        size = PATH_CHARS;
        if (QueryFullProcessImageNameWL(proc, 0, buf, &size) && size && size < PATH_CHARS) {
            buf[PATH_CHARS-1] = TEXT('\0');
            CloseHandle(proc);
            *path = buf;
            LOG("QueryProcPath: QFPIN ok pid=%u len=%u", (unsigned)target, (unsigned)size);
            return 0;
        }
        err = GetLastError();
        if (!err) err = ERROR_BUFFER_OVERFLOW;
        free(buf);
        CloseHandle(proc);
    } else {
        err = GetLastError();
    }
    LOG("QueryProcPath: QueryFullProcessImageNameW failed err=%u", (unsigned)err);

    /* 2) Old systems, or the limited rights were refused */
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, target);
    if (!proc)
        proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, target);
    if (!proc) {
        err2 = GetLastError();
        LOG("QueryProcPath: OpenProcess failed err=%u", (unsigned)err2);
        return err2 ? err2 : err;
    }
    buf = (TCHAR *)malloc(PATH_CHARS * sizeof(TCHAR));
    if (!buf) {
        CloseHandle(proc);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    buf[0] = TEXT('\0');
    size = GetModuleFileNameExL(proc, NULL, buf, PATH_CHARS);
    CloseHandle(proc);
    if (!size || size >= PATH_CHARS) {
        err2 = GetLastError();
        if (!err2) err2 = ERROR_BUFFER_OVERFLOW;
        LOG("QueryProcPath: GetModuleFileNameExL failed err=%u", (unsigned)err2);
        free(buf);
        return err2;
    }
    buf[PATH_CHARS-1] = TEXT('\0');
    *path = buf;
    LOG("QueryProcPath: GetModuleFileNameExL ok pid=%u len=%u", (unsigned)target, (unsigned)size);
    return 0;
}

/* PATH-2 order: QueryFullProcessImageNameW with limited rights, then
 * GetModuleFileNameExL with a full query handle, then fail with a reason.
 * GetProcessImageFileNameL is never used (PATH-3), it returns a \Device path
 * that Explorer /select and the clipboard can not use.
 * Returns 0 on success and *path is a heap buffer of PATH_CHARS characters
 * (the caller frees it). On failure *path stays NULL.
 */
static DWORD GetWindowProcPath(HWND hwnd, DWORD *pid, DWORD *realpid, TCHAR **path)
{
    DWORD winpid = 0, target, err;
    TCHAR *buf = NULL;

    if (pid) *pid = 0;
    if (realpid) *realpid = 0;
    if (path) *path = NULL;
    if (!hwnd || !path) return ERROR_INVALID_PARAMETER;

    if (!GetWindowThreadProcessId(hwnd, &winpid) || !winpid) {
        LOG("GetWindowProcPath: no pid for hwnd=%p", hwnd);
        return ERROR_INVALID_WINDOW_HANDLE;
    }
    if (pid) *pid = winpid;
    if (realpid) *realpid = winpid;

    err = QueryProcPath(winpid, &buf);
    if (!err && buf && !lstrcmpi(PathFileName(buf), TEXT("ApplicationFrameHost.exe"))) {
        /* UWP host: the program the user cares about is the hosted one. If
         * its path can not be read, report that instead of the host path
         * (UX-1: never show a value that is not the one asked for). */
        target = GetWindowRealPid(winpid, hwnd);
        if (target && target != winpid) {
            TCHAR *childpath = NULL;
            DWORD err2 = QueryProcPath(target, &childpath);
            free(buf);
            buf = childpath;
            if (realpid) *realpid = target;
            if (err2 || !buf) {
                LOG("GetWindowProcPath: AFH child %u path unavailable err=%u",
                    (unsigned)target, (unsigned)err2);
                free(buf);
                return err2 ? err2 : ERROR_FILE_NOT_FOUND;
            }
            LOG("GetWindowProcPath: AFH child %u", (unsigned)target);
        }
    }
    if (err || !buf) {
        free(buf);
        return err ? err : ERROR_FILE_NOT_FOUND;
    }
    *path = buf;
    return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * B. Window details dialog (INFO-1..INFO-7)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Control id of the text box, only used by the in-memory template */
enum { IDD_INFO_EDIT = 100 };

/* INFO-2: one details dialog at a time, the target can not change while it
 * is open (a second request is dropped, not queued). */
static int g_info_open = 0;

/* Dialog template, built in memory (the .rc files are not touched).
 * Layout: DLGTEMPLATE (18 bytes), 3 zero words (menu, class, title),
 * DS_SETFONT (word point size + face name), then two DLGITEMTEMPLATE
 * blocks, each DWORD aligned, each ending with its creation data size word.
 * All of it lives in a DWORD array on the heap so the alignment is
 * guaranteed without casts on the stack (ABI-3).
 */
#define INFO_TPL_FONT   TEXT("MS Shell Dlg 2")
#define INFO_TPL_PT     8  /* same 8 pt as the dialogs of window.rc */
#define INFO_DLG_W      420 /* dialog units */
#define INFO_DLG_H      260
#define INFO_BTN_W      50
#define INFO_BTN_H      14
/* 18 (header) + 6 (3 words) + 2 (font size) + 30 (face name) + 2 pad
 * + 2 * (18 + 6 + 2) = 110 bytes, keep a margin. */
#define INFO_TPL_BYTES  128

/* Append one character, O(1), keeps the string NUL terminated */
static void SbPutChar(sb_t *sb, TCHAR ch)
{
    if (sb->len + 1 < sb->cap) {
        sb->it[sb->len] = ch;
        sb->len++;
        sb->it[sb->len] = TEXT('\0');
    }
}

/* Append an unsigned 64 bit number */
static void SbPutULL(sb_t *sb, unsigned long long v)
{
    TCHAR buf[24];
    int i = ARR_SZ(buf) - 1;
    buf[i] = TEXT('\0');
    do {
        buf[--i] = (TCHAR)(TEXT('0') + (TCHAR)(v % 10));
        v /= 10;
    } while (v);
    SbAppendStr(sb, &buf[i]);
}

/* Fill a modal dialog template with one read-only multiline edit control
 * and one Close button. Returns NULL when out of memory. */
static DLGTEMPLATE *InfoBuildTemplate(void)
{
    static const TCHAR face[] = INFO_TPL_FONT;
    DLGTEMPLATE *dt;
    DLGITEMTEMPLATE *it;
    WORD *w;
    size_t i;
    DWORD *tpl = (DWORD *)calloc(INFO_TPL_BYTES / 4, sizeof(DWORD));
    if (!tpl) return NULL;

    dt = (DLGTEMPLATE *)tpl;
    dt->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER
              | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->dwExtendedStyle = 0;
    dt->cdit = 2;
    dt->x = 0; dt->y = 0;
    dt->cx = INFO_DLG_W; dt->cy = INFO_DLG_H;

    /* No menu, no class, no title (the title is set in WM_INITDIALOG).
     * The header is 18 bytes: sizeof(DLGTEMPLATE) is 20 only because of the
     * padding after cy, so the header is skipped by its documented size. */
    w = (WORD *)((BYTE *)dt + 18);
    *w++ = 0;
    *w++ = 0;
    *w++ = 0;
    /* DS_SETFONT: point size then the face name, both WORD aligned */
    *w++ = INFO_TPL_PT;
    for (i = 0; i < ARR_SZ(face); i++)
        *w++ = (WORD)face[i];

    /* INFO-1: read-only, scrollable, multiline edit control */
    it = (DLGITEMTEMPLATE *)(((UINT_PTR)w + 3) & ~(UINT_PTR)3);
    it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL
              | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
    it->dwExtendedStyle = 0;
    it->x = 8; it->y = 8;
    it->cx = INFO_DLG_W - 16; it->cy = INFO_DLG_H - 32;
    it->id = IDD_INFO_EDIT;
    w = (WORD *)((BYTE *)it + 18); /* sizeof(DLGITEMTEMPLATE) is 18, no padding */
    *w++ = 0xFFFF; *w++ = 0x0081; /* class "EDIT", ordinal */
    *w++ = 0; /* no title, the text is set before the dialog is shown */
    *w++ = 0; /* creation data size, always present */

    /* Close button, no copy button (INFO-1) */
    it = (DLGITEMTEMPLATE *)(((UINT_PTR)w + 3) & ~(UINT_PTR)3);
    it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    it->dwExtendedStyle = 0;
    it->x = (INFO_DLG_W - INFO_BTN_W) / 2;
    it->y = INFO_DLG_H - INFO_BTN_H - 6;
    it->cx = INFO_BTN_W; it->cy = INFO_BTN_H;
    it->id = IDCANCEL;
    w = (WORD *)((BYTE *)it + 18); /* sizeof(DLGITEMTEMPLATE) is 18, no padding */
    *w++ = 0xFFFF; *w++ = 0x0080; /* class "BUTTON", ordinal */
    *w++ = 0; /* no title, set in WM_INITDIALOG */
    *w++ = 0; /* creation data size, always present */

    return dt;
}

/* Append "label: value" plus a line break */
static void InfoLine(sb_t *sb, const TCHAR *label, const TCHAR *value)
{
    SbAppendStr(sb, label);
    SbAppendStr(sb, TEXT(": "));
    SbAppendStr(sb, value);
    SbAppendStr(sb, TEXT("\r\n"));
}

/* Append a decimal int */
static void InfoLineInt(sb_t *sb, const TCHAR *label, int value)
{
    TCHAR num[INT_DIGITS+1];
    InfoLine(sb, label, Int2lStr(num, value));
}

/* Append an unsigned int */
static void InfoLineUint(sb_t *sb, const TCHAR *label, unsigned value)
{
    TCHAR num[UINT_DIGITS+1];
    InfoLine(sb, label, Uint2lStr(num, value));
}

/* Append a pointer sized value in hexadecimal */
static void InfoLineHex(sb_t *sb, const TCHAR *label, UINT_PTR value)
{
    TCHAR num[LPTR_HDIGITS+8];
    sb_t b = { num, 0, ARR_SZ(num) };
    SbAppendStr(&b, TEXT("0x"));
    SbAppendHex(&b, value);
    InfoLine(sb, label, num);
}

/* Append a FILETIME as local date and time */
static void InfoLineFileTime(sb_t *sb, const TCHAR *label, const FILETIME *ft)
{
    FILETIME local;
    SYSTEMTIME st;
    TCHAR buf[64];
    sb_t b = { buf, 0, ARR_SZ(buf) };
    if (!FileTimeToLocalFileTime(ft, &local) || !FileTimeToSystemTime(&local, &st)) {
        InfoLine(sb, label, l10n->InfoUnavailable);
        return;
    }
    SbAppendInt(&b, (int)st.wYear);
    SbPutChar(&b, TEXT('-'));
    if (st.wMonth < 10) SbPutChar(&b, TEXT('0'));
    SbAppendInt(&b, (int)st.wMonth);
    SbPutChar(&b, TEXT('-'));
    if (st.wDay < 10) SbPutChar(&b, TEXT('0'));
    SbAppendInt(&b, (int)st.wDay);
    SbPutChar(&b, TEXT(' '));
    if (st.wHour < 10) SbPutChar(&b, TEXT('0'));
    SbAppendInt(&b, (int)st.wHour);
    SbPutChar(&b, TEXT(':'));
    if (st.wMinute < 10) SbPutChar(&b, TEXT('0'));
    SbAppendInt(&b, (int)st.wMinute);
    SbPutChar(&b, TEXT(':'));
    if (st.wSecond < 10) SbPutChar(&b, TEXT('0'));
    SbAppendInt(&b, (int)st.wSecond);
    InfoLine(sb, label, buf);
}

/* Append a size as "12.3 MB (12902345 B)" */
static void InfoLineBytes(sb_t *sb, const TCHAR *label, unsigned long long bytes)
{
    TCHAR buf[80];
    sb_t b = { buf, 0, ARR_SZ(buf) };
    unsigned long long mb = bytes / (1024ULL * 1024ULL);
    unsigned frac = (unsigned)(((bytes % (1024ULL * 1024ULL)) * 10ULL) / (1024ULL * 1024ULL));
    SbPutULL(&b, mb);
    SbPutChar(&b, TEXT('.'));
    SbPutChar(&b, (TCHAR)(TEXT('0') + (TCHAR)frac));
    SbAppendStr(&b, TEXT(" MB ("));
    SbPutULL(&b, bytes);
    SbAppendStr(&b, TEXT(" B)"));
    InfoLine(sb, label, buf);
}

/* Process memory. GetProcessMemoryInfo comes from PSAPI.DLL at runtime
 * (API-3), it must not become a static K32 import (Win7+ only).
 * INFO-5: the memory query opens its own handle, a failure only makes the
 * two memory lines unavailable.
 */
static void InfoAppendMemory(sb_t *sb, DWORD pid)
{
    typedef BOOL (WINAPI *funk_t)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
    static funk_t funk = (funk_t)IPTR;
    PROCESS_MEMORY_COUNTERS pmc;
    HANDLE proc;

    if (funk == (funk_t)IPTR)
        funk = (funk_t)LoadDLLProc("PSAPI.DLL", "GetProcessMemoryInfo");
    if (!funk) {
        LOG("WindowDetails: no GetProcessMemoryInfo");
        InfoLine(sb, l10n->InfoWorkingSet, l10n->InfoUnavailable);
        InfoLine(sb, l10n->InfoPrivateMem, l10n->InfoUnavailable);
        return;
    }
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        LOG("WindowDetails: memory OpenProcess failed err=%u", (unsigned)GetLastError());
        InfoLine(sb, l10n->InfoWorkingSet, l10n->InfoUnavailable);
        InfoLine(sb, l10n->InfoPrivateMem, l10n->InfoUnavailable);
        return;
    }
    mem00(&pmc, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (funk(proc, &pmc, sizeof(pmc))) {
        InfoLineBytes(sb, l10n->InfoWorkingSet, (unsigned long long)pmc.WorkingSetSize);
        InfoLineBytes(sb, l10n->InfoPrivateMem, (unsigned long long)pmc.PagefileUsage);
    } else {
        LOG("WindowDetails: GetProcessMemoryInfo failed err=%u", (unsigned)GetLastError());
        InfoLine(sb, l10n->InfoWorkingSet, l10n->InfoUnavailable);
        InfoLine(sb, l10n->InfoPrivateMem, l10n->InfoUnavailable);
    }
    CloseHandle(proc);
}

/* Process start time and total CPU time (kernel + user), no live sampling */
static void InfoAppendTimes(sb_t *sb, DWORD pid)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    FILETIME create, exit, kern, user;
    unsigned long long cpu;

    if (!proc)
        proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) {
        LOG("WindowDetails: times OpenProcess failed err=%u", (unsigned)GetLastError());
        InfoLine(sb, l10n->InfoStartTime, l10n->InfoUnavailable);
        InfoLine(sb, l10n->InfoCpuTime, l10n->InfoUnavailable);
        return;
    }
    if (GetProcessTimes(proc, &create, &exit, &kern, &user)) {
        TCHAR buf[64];
        sb_t b = { buf, 0, ARR_SZ(buf) };
        InfoLineFileTime(sb, l10n->InfoStartTime, &create);
        cpu = (((unsigned long long)kern.dwHighDateTime << 32) | kern.dwLowDateTime)
            + (((unsigned long long)user.dwHighDateTime << 32) | user.dwLowDateTime);
        /* FILETIME counts 100 ns units */
        SbPutULL(&b, cpu / 10000000ULL);
        SbPutChar(&b, TEXT('.'));
        {
            unsigned cs = (unsigned)((cpu % 10000000ULL) / 100000ULL);
            if (cs < 10) SbPutChar(&b, TEXT('0'));
            SbAppendInt(&b, (int)cs);
        }
        SbAppendStr(&b, TEXT(" s"));
        InfoLine(sb, l10n->InfoCpuTime, buf);
    } else {
        LOG("WindowDetails: GetProcessTimes failed err=%u", (unsigned)GetLastError());
        InfoLine(sb, l10n->InfoStartTime, l10n->InfoUnavailable);
        InfoLine(sb, l10n->InfoCpuTime, l10n->InfoUnavailable);
    }
    CloseHandle(proc);
}

/* INFO-7: ES_READONLY edit controls do not implement Ctrl+A (measured: the
 * control ignores WM_CHAR 0x01), this minimal subclass adds it. Ctrl+C is
 * handled by the control itself, everything else goes to the original
 * procedure. Only one details dialog can exist at a time (g_info_open), so
 * the previous procedure can be kept in a single static variable.
 */
static WNDPROC g_info_edit_proc = NULL;

static LRESULT CALLBACK InfoEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CHAR && wParam == 1) { /* Ctrl+A arrives as WM_CHAR 0x01 */
        SendMessage(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (g_info_edit_proc)
        return CallWindowProc(g_info_edit_proc, hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* Dialog procedure of the details dialog (HOOK-4: nothing blocks here) */
static INT_PTR CALLBACK InfoDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INITDIALOG) {
        HWND edit = GetDlgItem(hdlg, IDD_INFO_EDIT);
        SetWindowText(hdlg, l10n->InfoTitle);
        SetWindowText(GetDlgItem(hdlg, IDCANCEL), l10n->InfoClose);
        if (edit) {
            SetWindowText(edit, (const TCHAR *)lParam);
            g_info_edit_proc = (WNDPROC)SetWindowLongPtr(edit, GWLP_WNDPROC, (LONG_PTR)InfoEditProc);
            SendMessage(edit, EM_SETSEL, 0, 0);
            SetFocus(edit);
        }
        return FALSE; /* the focus is set explicitly */
    }
    if (msg == WM_COMMAND && (LOWORD(wParam) == IDCANCEL || LOWORD(wParam) == IDOK)) {
        EndDialog(hdlg, 0);
        return TRUE;
    }
    if (msg == WM_CLOSE) {
        EndDialog(hdlg, 0);
        return TRUE;
    }
    return FALSE;
}

/* Read the caption of hwnd into a heap copy (INFO-4): GetWindowTextW reads
 * the caption the system keeps for other processes, it never sends
 * WM_GETTEXT, so a hung target can not block the main thread. A window that
 * reports no length is still read with a small buffer, because
 * GetWindowTextLengthW is documented to return 0 for some other process
 * windows that do have a caption.
 * Returns NULL when out of memory. *empty is 1 when there is no caption.
 */
static TCHAR *GetWindowTitleCopy(HWND hwnd, int *empty)
{
    int len = GetWindowTextLength(hwnd);
    DWORD cap = (len > 0 && len < 32768) ? (DWORD)len + 2 : 1024;
    TCHAR *title = (TCHAR *)calloc(cap, sizeof(TCHAR));

    *empty = 1;
    if (!title) return NULL;
    if (GetWindowText(hwnd, title, (int)cap) > 0 && title[0])
        *empty = 0;
    else
        title[0] = TEXT('\0');
    return title;
}

/* Append the whole window identity/state/style/process/file/statistics
 * snapshot of hwnd to text (INFO-2: taken once, before the dialog opens).
 */
static void InfoBuildText(sb_t *sb, HWND hwnd)
{
    DWORD pid = 0, realpid = 0, tid, err;
    TCHAR *path = NULL;

    /* PID, real (AFH) PID and path first: later fields only need pid */
    err = GetWindowProcPath(hwnd, &pid, &realpid, &path);

    /* Window identity */
    InfoLineHex(sb, l10n->InfoWindow, (UINT_PTR)hwnd);
    {
        int empty = 0;
        TCHAR *title = GetWindowTitleCopy(hwnd, &empty);
        if (!title)
            InfoLine(sb, l10n->InfoWindowTitle, l10n->InfoUnavailable);
        else if (empty)
            InfoLine(sb, l10n->InfoWindowTitle, l10n->InfoEmpty);
        else
            InfoLine(sb, l10n->InfoWindowTitle, title);
        free(title);
    }
    {
        TCHAR cls[256];
        cls[0] = TEXT('\0');
        if (GetClassName(hwnd, cls, ARR_SZ(cls)) && cls[0])
            InfoLine(sb, l10n->InfoClassName, cls);
        else
            InfoLine(sb, l10n->InfoClassName, l10n->InfoUnavailable);
    }

    /* Window state */
    {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            InfoLineInt(sb, l10n->InfoPositionX, (int)rc.left);
            InfoLineInt(sb, l10n->InfoPositionY, (int)rc.top);
            InfoLineInt(sb, l10n->InfoWidth, (int)(rc.right - rc.left));
            InfoLineInt(sb, l10n->InfoHeight, (int)(rc.bottom - rc.top));
        } else {
            InfoLine(sb, l10n->InfoPositionX, l10n->InfoUnavailable);
            InfoLine(sb, l10n->InfoPositionY, l10n->InfoUnavailable);
            InfoLine(sb, l10n->InfoWidth, l10n->InfoUnavailable);
            InfoLine(sb, l10n->InfoHeight, l10n->InfoUnavailable);
        }
    }
    {
        WINDOWPLACEMENT wpl;
        mem00(&wpl, sizeof(wpl));
        wpl.length = sizeof(wpl);
        if (GetWindowPlacement(hwnd, &wpl)) {
            const TCHAR *st = l10n->InfoStateNormal;
            if (wpl.showCmd == SW_SHOWMAXIMIZED)
                st = l10n->InfoStateMaximized;
            else if (wpl.showCmd == SW_SHOWMINIMIZED || wpl.showCmd == SW_SHOWMINNOACTIVE)
                st = l10n->InfoStateMinimized;
            InfoLine(sb, l10n->InfoState, st);
        } else {
            InfoLine(sb, l10n->InfoState, l10n->InfoUnavailable);
        }
    }
    InfoLine(sb, l10n->InfoVisible, IsWindowVisible(hwnd) ? l10n->InfoYes : l10n->InfoNo);
    {
        int cloaked = 0;
        if (S_OK == DwmGetWindowAttributeL(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
            InfoLine(sb, l10n->InfoCloaked, cloaked ? l10n->InfoYes : l10n->InfoNo);
        else
            InfoLine(sb, l10n->InfoCloaked, l10n->InfoUnknown);
    }

    /* Styles, hexadecimal values */
    InfoLineHex(sb, l10n->InfoStyle, (UINT_PTR)GetWindowLongPtr(hwnd, GWL_STYLE));
    InfoLineHex(sb, l10n->InfoExStyle, (UINT_PTR)GetWindowLongPtr(hwnd, GWL_EXSTYLE));

    /* Process identity, the AFH child PID is only shown when there is one */
    tid = GetWindowThreadProcessId(hwnd, NULL);
    InfoLineUint(sb, l10n->InfoPid, (unsigned)pid);
    InfoLineUint(sb, l10n->InfoTid, (unsigned)tid);
    {
        /* The name comes from the path when it is known (it also works for
         * elevated targets). The fallback helper returns a full path and may
         * even return a \Device\... path (GetProcessImageFileNameL is its last
         * resort), so only the file name part is kept: a device path is never
         * shown as a program name (PATH-3, UX-1).
         */
        if (path && path[0] && PathFileName(path)[0])
            InfoLine(sb, l10n->InfoProcessName, PathFileName(path));
        else {
            TCHAR *name = (TCHAR *)calloc(PATH_CHARS, sizeof(TCHAR));
            const TCHAR *fname = NULL;
            if (name && pid && GetWindowProgNameFromPid(pid, name, PATH_CHARS)) {
                name[PATH_CHARS-1] = TEXT('\0');
                fname = PathFileName(name);
            }
            if (fname && fname[0])
                InfoLine(sb, l10n->InfoProcessName, fname);
            else
                InfoLine(sb, l10n->InfoProcessName, l10n->InfoUnavailable);
            free(name);
        }
    }
    if (realpid && realpid != pid)
        InfoLineUint(sb, l10n->InfoRealPid, (unsigned)realpid);

    /* File: full Win32 path, or unavailable with the same reason as above */
    if (!err && path && path[0])
        InfoLine(sb, l10n->InfoPath, path);
    else
        InfoLine(sb, l10n->InfoPath, l10n->InfoUnavailable);
    if (path) free(path);

    /* Run statistics, each group degrades on its own (INFO-5, UX-1) */
    InfoAppendTimes(sb, pid);
    InfoAppendMemory(sb, pid);
}

/* Show the details of hwnd in a modal dialog (INFO-1, C10).
 * The system dialog manager runs its own message loop, so the main thread
 * keeps pumping messages while the dialog is open; a WM_QUIT (exit or tray
 * disable) is reposted by that loop and unwinds the whole stack in order.
 */
static void WindowDetailsShow(HWND hwnd)
{
    ACT_TIMER_BEGIN(t);
    TCHAR *text;
    DLGTEMPLATE *tpl;
    sb_t sb;

    if (g_info_open) {
        LOG("WindowDetailsShow: dialog already open");
        return;
    }
    if (!hwnd || !IsWindow(hwnd)) {
        LOG("WindowDetailsShow: invalid window %p", hwnd);
        ActionNotify(l10n->ActMsgNoTarget);
        return;
    }
    text = (TCHAR *)calloc(DETAILS_CHARS, sizeof(TCHAR));
    tpl = InfoBuildTemplate();
    if (!text || !tpl) {
        free(text);
        free(tpl);
        ActionNotify(l10n->ActMsgNoMem);
        return;
    }
    sb.it = text;
    sb.len = 0;
    sb.cap = DETAILS_CHARS;
    InfoBuildText(&sb, hwnd);

    g_info_open = 1;
    /* The dialog lifetime is user time, the timer only covers the snapshot */
    ACT_TIMER_END(t, "WindowDetailsShow");
    LOG("WindowDetailsShow: opening dialog for %p", hwnd);
    {
        INT_PTR dlgret = DialogBoxIndirectParam(g_hinst, tpl, g_hwnd, InfoDlgProc, (LPARAM)text);
        /* Braces are required: LOG is an if() in release builds */
        if (dlgret == -1) {
            LOG("WindowDetailsShow: DialogBoxIndirectParam failed err=%u", (unsigned)GetLastError());
        } else {
            LOG("WindowDetailsShow: dialog closed ret=%d", (int)dlgret);
        }
    }
    g_info_open = 0;
    free(tpl);
    free(text);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * C. Reveal the EXE in Explorer (REVEAL-1..REVEAL-4)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Append one command line argument, quoted the way CommandLineToArgvW
 * parses it: the argument is always quoted, backslashes before a quote are
 * doubled and the quote is escaped. */
static void RevealQuoteArg(sb_t *sb, const TCHAR *arg)
{
    SbPutChar(sb, TEXT('"'));
    while (*arg) {
        int backslashes = 0;
        while (*arg == TEXT('\\')) {
            backslashes++;
            arg++;
        }
        if (!*arg) {
            /* Trailing backslashes precede the closing quote */
            for (int i = 0; i < backslashes * 2; i++) SbPutChar(sb, TEXT('\\'));
            break;
        }
        if (*arg == TEXT('"')) {
            for (int i = 0; i < backslashes * 2 + 1; i++) SbPutChar(sb, TEXT('\\'));
        } else {
            for (int i = 0; i < backslashes; i++) SbPutChar(sb, TEXT('\\'));
        }
        SbPutChar(sb, *arg);
        arg++;
    }
    SbPutChar(sb, TEXT('"'));
}

/* REVEAL-2: CreateProcessW with the full path of explorer.exe taken from
 * GetSystemWindowsDirectoryW (never a hardcoded C:\Windows, never PATH).
 * The command line is "<explorer.exe>" /select,"<exe>": the first token is
 * the program itself, otherwise Explorer may treat /select as the program.
 */
static void RevealExeShow(HWND hwnd)
{
    ACT_TIMER_BEGIN(t);
    TCHAR *path = NULL, *cmdline = NULL;
    TCHAR windir[MAX_PATH], explorer[MAX_PATH];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD pid = 0, realpid = 0, err;
    sb_t sb;
    BOOL ok;

    if (!hwnd || !IsWindow(hwnd)) {
        LOG("RevealExeShow: invalid window %p", hwnd);
        ActionNotify(l10n->ActMsgNoTarget);
        return;
    }
    err = GetWindowProcPath(hwnd, &pid, &realpid, &path);
    if (err || !path || !path[0]) {
        LOG("RevealExeShow: no path err=%u", (unsigned)err);
        free(path);
        ActionNotify(l10n->ActMsgNoPath);
        return;
    }
    windir[0] = TEXT('\0');
    if (GetSystemWindowsDirectory(windir, ARR_SZ(windir)) >= ARR_SZ(windir) || !windir[0]) {
        LOG("RevealExeShow: GetSystemWindowsDirectory failed err=%u", (unsigned)GetLastError());
        free(path);
        ActionNotify(l10n->ActMsgFail);
        return;
    }
    lstrcpy_s(explorer, ARR_SZ(explorer), windir);
    lstrcat_s(explorer, ARR_SZ(explorer), TEXT("\\explorer.exe"));

    cmdline = (TCHAR *)malloc(2 * PATH_CHARS * sizeof(TCHAR));
    if (!cmdline) {
        free(path);
        ActionNotify(l10n->ActMsgNoMem);
        return;
    }
    sb.it = cmdline;
    sb.len = 0;
    sb.cap = 2 * PATH_CHARS;
    RevealQuoteArg(&sb, explorer);
    SbAppendStr(&sb, TEXT(" /select,"));
    RevealQuoteArg(&sb, path);
    if (sb.len + 1 > 32767) {
        /* A command line can not hold more than 32767 characters */
        LOG("RevealExeShow: command line too long (%u)", (unsigned)sb.len);
        free(cmdline);
        free(path);
        ActionNotify(l10n->ActMsgFail);
        return;
    }
    mem00(&si, sizeof(si));
    si.cb = sizeof(si);
    ok = CreateProcess(explorer, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) {
        /* Both handles are closed right away (REVEAL-2) */
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOG("RevealExeShow: explorer started pid=%u", (unsigned)pi.dwProcessId);
    } else {
        LOG("RevealExeShow: CreateProcess failed err=%u", (unsigned)GetLastError());
        ActionNotify(l10n->ActMsgFail);
    }
    free(cmdline);
    free(path);
    ACT_TIMER_END(t, "RevealExeShow");
}
