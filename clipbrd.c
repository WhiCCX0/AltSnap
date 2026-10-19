/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Clipboard text and clear requests (WP-PROC, A7 sub actions 2/3/4).
 * The main thread only copies the text into a request and starts a one shot
 * worker thread; the clipboard is opened, emptied and written by that thread
 * only (RES-2/RES-3). HGLOBAL never crosses a thread boundary (RES-2).
 */

/* Serializes the worker threads that write the clipboard. The main thread
 * never takes it (RES-2/RES-3); it lives for the whole process and is not
 * deleted, the workers die with ExitProcess (RES-5).
 */
static CRITICAL_SECTION g_clip_cs;
/* Request sequence, only modified by the main thread (RES-4). */
static volatile LONG g_clip_seq = 0;
/* Number of pending clipboard commit threads, only taken by the main
 * thread (RES-4): at most 2 at a time, released by the commit thread or by
 * the main thread when CreateThread fails (RES-2). */
static volatile LONG g_clip_pending = 0;

/* Retry budget of one commit: about 0.5 s in total (RES-3) */
#define CLIP_RETRY_COUNT 20
#define CLIP_RETRY_SLEEP 25 /* ms */

/* The request handed from the main thread to the commit thread.
 * It only holds a heap copy of the text, never an HGLOBAL (RES-2, CLIP-4).
 */
typedef struct clip_request {
    struct clip_request *next;
    LONG seq;         /* request sequence, checked for staleness (RES-4) */
    BOOL chkcbseq;    /* only Clipboard_1 checks the clipboard sequence */
    DWORD cbseq;      /* clipboard sequence at dispatch time */
    UINT fmt;         /* CF_UNICODETEXT, or 0 for a clear request (CLIP-3) */
    TCHAR *text;      /* heap copy of the text, NULL for a clear request */
} clip_request_t;

/* Called once from tWinMain before any worker thread exists (C6) */
static void ClipboardInit(void)
{
    InitializeCriticalSection(&g_clip_cs);
    LOG("ClipboardInit done");
}

/* Release the request structure (and its text copy) of the commit thread.
 * Every path of the thread ends here (CLIP-4).
 */
static void ClipboardFreeRequest(clip_request_t *req)
{
    if (!req) return;
    free(req->text);
    free(req);
}

/* Copy text into a GMEM_MOVEABLE block ending with a NUL (CLIP-4).
 * Returns NULL on failure, the caller releases the request and the slot.
 */
static HGLOBAL ClipboardAllocText(const TCHAR *text)
{
    size_t len = lstrlen(text) + 1;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(TCHAR));
    TCHAR *dst;
    if (!hg) return NULL;
    dst = (TCHAR *)GlobalLock(hg);
    if (!dst) {
        GlobalFree(hg);
        return NULL;
    }
    memcpy(dst, text, len * sizeof(TCHAR));
    GlobalUnlock(hg);
    return hg;
}

/* Write the clipboard from a worker thread (RES-3). Returns one of the
 * ACR_* codes. `hg` is owned by this function on every path: it either goes
 * to the system through SetClipboardData or is freed here (CLIP-5).
 */
static int ClipboardCommit(LONG reqseq, BOOL chkcbseq, DWORD cbseq, UINT fmt, HGLOBAL hg)
{
    int ret = ACR_CLIPBUSY;
    int attempt;

    for (attempt = 0; attempt < CLIP_RETRY_COUNT; attempt++) {
        if (!TryEnterCriticalSection(&g_clip_cs)) {
            /* Someone else is writing, wait a little (RES-3: bounded) */
            Sleep(CLIP_RETRY_SLEEP);
            continue;
        }
        if (!OpenClipboard(g_hwnd)) {
            LeaveCriticalSection(&g_clip_cs);
            Sleep(CLIP_RETRY_SLEEP);
            continue;
        }
        /* Expiry check (RES-4): a newer request or a clipboard change
         * discards this one, without touching the clipboard at all. */
        if (reqseq != g_clip_seq || (chkcbseq && cbseq != GetClipboardSequenceNumber())) {
            CloseClipboard();
            LeaveCriticalSection(&g_clip_cs);
            LOG("ClipboardCommit: stale seq=%d (now %d)", (int)reqseq, (int)g_clip_seq);
            ret = ACR_STALE;
            break;
        }
        if (!EmptyClipboard()) {
            CloseClipboard();
            LeaveCriticalSection(&g_clip_cs);
            LOG("ClipboardCommit: EmptyClipboard failed err=%u", (unsigned)GetLastError());
            ret = ACR_CLIPBUSY;
            break;
        }
        if (hg) {
            if (SetClipboardData(fmt, hg)) {
                hg = NULL; /* the system owns it now (CLIP-5) */
                ret = ACR_OK;
            } else {
                LOG("ClipboardCommit: SetClipboardData failed err=%u", (unsigned)GetLastError());
                ret = ACR_FAIL;
            }
        } else {
            ret = ACR_OK; /* Clipboard_4: only cleared (CLIP-3) */
        }
        CloseClipboard();
        LeaveCriticalSection(&g_clip_cs);
        break;
    }
    if (hg) GlobalFree(hg);
    if (attempt == CLIP_RETRY_COUNT)
        LOG("ClipboardCommit: clipboard busy after %d attempts", CLIP_RETRY_COUNT);
    return ret;
}

/* Body of the one shot commit thread: take the clipboard, then report the
 * result to the main thread and release everything (RES-4/RES-5).
 */
static DWORD WINAPI ClipboardThreadProc(LPVOID param)
{
    clip_request_t *req = (clip_request_t *)param;
    HGLOBAL hg = NULL;
    int code;

    if (req->text)
        hg = ClipboardAllocText(req->text);
    if (req->text && !hg)
        code = ACR_NOMEM;
    else
        code = ClipboardCommit(req->seq, req->chkcbseq, req->cbseq, req->fmt, hg);
    /* The result kind tells the user what was attempted (C6) */
    PostMessage(g_hwnd, WM_ACTIONRES
        , ACRES(req->fmt ? ACK_CLIPTEXT : ACK_CLIPCLEAR, code, 0), 0);
    LOG("ClipboardThreadProc: seq=%d code=%d", (int)req->seq, code);
    ClipboardFreeRequest(req);
    /* Last step of the thread: give the in-flight slot back (RES-4) */
    InterlockedDecrement(&g_clip_pending);
    return 0;
}

/* Take the request sequence of a clipboard request whose commit runs in a
 * worker that is not a clipboard commit thread, i.e. the screenshot worker of
 * Clipboard_1 (SHOT-3/NIT1). Only the main thread calls this. The clipboard
 * sequence number is read right after the increment and copied into the task,
 * so that a late screenshot does not overwrite content copied after the
 * request (CLIP-7/RES-4). Clipboard_1 takes no in-flight slot: the limit of
 * two only covers Clipboard_2/3/4 commit threads, and Clipboard_1 is already
 * serialised by the screenshot single flight flag (SHOT-2/RES-4).
 */
static LONG ClipboardTakeSeq(DWORD *cbseq)
{
    LONG seq = InterlockedIncrement(&g_clip_seq);
    *cbseq = GetClipboardSequenceNumber();
    return seq;
}

/* Put that sequence back when the dispatch of the request failed, so that the
 * failed request leaves no trace (RES-4/SHOT-4).
 */
static void ClipboardDropSeq(void)
{
    InterlockedDecrement(&g_clip_seq);
}

/* Copy text to the clipboard through a new commit thread (CLIP-1/CLIP-2).
 * Runs on the main thread: it only copies the text and creates the thread.
 * `text` is a private copy, it is released by the thread or here.
 * text==NULL and fmt==0 make a clear request (CLIP-3).
 */
static void ClipboardStartThread(const TCHAR *text, UINT fmt)
{
    clip_request_t *req;
    HANDLE thread;
    LONG pending;

    /* Take an in-flight slot, give it back right away if there are too
     * many (RES-4). The sequence is only taken once the slot is ours. */
    pending = InterlockedIncrement(&g_clip_pending);
    if (pending > 2) {
        InterlockedDecrement(&g_clip_pending);
        LOG("ClipboardStartThread: too many pending requests (%d)", (int)pending);
        ActionNotify(l10n->ActMsgClipBusy);
        return;
    }
    req = (clip_request_t *)calloc(1, sizeof(*req));
    if (!req) {
        InterlockedDecrement(&g_clip_pending);
        ActionNotify(l10n->ActMsgNoMem);
        return;
    }
    if (text) {
        req->text = lstrdup(text);
        if (!req->text) {
            ClipboardFreeRequest(req);
            InterlockedDecrement(&g_clip_pending);
            ActionNotify(l10n->ActMsgNoMem);
            return;
        }
    }
    req->seq = InterlockedIncrement(&g_clip_seq);
    req->fmt = fmt;
    req->chkcbseq = FALSE;

    thread = CreateThread(NULL, 0, ClipboardThreadProc, req, 0, NULL);
    if (!thread) {
        /* Dispatch failed: the request never existed. Release the slot and
         * put the sequence back, only the main thread changes it (RES-4). */
        LOG("ClipboardStartThread: CreateThread failed err=%u", (unsigned)GetLastError());
        InterlockedDecrement(&g_clip_seq);
        ClipboardFreeRequest(req);
        InterlockedDecrement(&g_clip_pending);
        ActionNotify(l10n->ActMsgFail);
        return;
    }
    /* The request belongs to the thread from now on (RES-2) */
    CloseHandle(thread);
}

/* Clipboard_2: copy the title of hwnd (CLIP-1: same read path as INFO-4) */
static void ClipboardCopyTitle(HWND hwnd)
{
    int empty = 0;
    TCHAR *title;

    if (!hwnd) {
        LOG("ClipboardCopyTitle: no target");
        ActionNotify(l10n->ActMsgNoTarget);
        return;
    }
    title = GetWindowTitleCopy(hwnd, &empty);
    if (!title) {
        ActionNotify(l10n->ActMsgNoMem);
        return;
    }
    if (empty) {
        LOG("ClipboardCopyTitle: empty or unavailable title");
        free(title);
        ActionNotify(l10n->ActMsgNoText);
        return;
    }
    ClipboardStartThread(title, CF_UNICODETEXT);
    free(title);
}

/* Clipboard_3: copy the full Win32 path of the program of hwnd (CLIP-2) */
static void ClipboardCopyPath(HWND hwnd)
{
    TCHAR *path = NULL;
    DWORD pid = 0, realpid = 0, err;

    if (!hwnd) {
        LOG("ClipboardCopyPath: no target");
        ActionNotify(l10n->ActMsgNoTarget);
        return;
    }
    err = GetWindowProcPath(hwnd, &pid, &realpid, &path);
    if (err || !path || !path[0]) {
        LOG("ClipboardCopyPath: no path err=%u", (unsigned)err);
        free(path);
        ActionNotify(l10n->ActMsgNoPath);
        return;
    }
    ClipboardStartThread(path, CF_UNICODETEXT);
    free(path);
}

/* Handle a Clipboard sub action (1..4) on the main thread (CLIP-1..CLIP-4) */
static void ClipboardRequest(HWND hwnd, BYTE sub)
{
    ACT_TIMER_BEGIN(t);
    LOG("ClipboardRequest: sub=%d hwnd=%p", (int)sub, hwnd);
    if (sub < 1 || sub > 4) {
        LOG("ClipboardRequest: invalid sub action %d", (int)sub);
        ACT_TIMER_END(t, "ClipboardRequest");
        return;
    }
    switch (sub) {
    case 1:
        /* Clipboard_1 runs in the screenshot worker (SHOT-12) */
        ScreenshotStart(hwnd, 1);
        break;
    case 2:
        ClipboardCopyTitle(hwnd);
        break;
    case 3:
        ClipboardCopyPath(hwnd);
        break;
    default:
        /* Clipboard_4: no data, the thread just clears the clipboard. It
         * takes a sequence too, so older requests are discarded (CLIP-3). */
        ClipboardStartThread(NULL, 0);
        break;
    }
    ACT_TIMER_END(t, "ClipboardRequest");
}
