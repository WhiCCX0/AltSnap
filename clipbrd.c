/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Clipboard request handling (WP-PROC fills this in). */

/* Serializes the worker threads that write the clipboard. The main thread
 * never takes it (RES-2/RES-3); it lives for the whole process.
 */
static CRITICAL_SECTION g_clip_cs;
/* Request sequence, only modified by the main thread (RES-4). */
static volatile LONG g_clip_seq = 0;
/* Number of pending clipboard commit threads, only taken by main thread. */
static volatile LONG g_clip_pending = 0;

/* Called once from tWinMain before any worker thread exists. */
static void ClipboardInit(void)
{
    InitializeCriticalSection(&g_clip_cs);
    LOG("ClipboardInit done");
}

/* Handle a Clipboard sub action (1..4) on the main thread. */
static void ClipboardRequest(HWND hwnd, BYTE sub)
{
    LOG("ClipboardRequest hwnd=%p sub=%d", hwnd, (int)sub);
    if (sub < 1 || sub > 4) {
        LOG("ClipboardRequest: invalid sub action %d", (int)sub);
        return;
    }
    if (sub == 1) {
        ScreenshotStart(hwnd, 1);
        return;
    }
}

/* Write the clipboard from a worker thread (RES-3).
 * Returns one of the ACR_* codes.
 */
static int ClipboardCommit(LONG reqseq, BOOL chkcbseq, DWORD cbseq, UINT fmt, HGLOBAL hg)
{
    LOG("ClipboardCommit seq=%d chk=%d fmt=%u hg=%p", (int)reqseq, (int)chkcbseq, fmt, hg);
    (void)cbseq;
    return ACR_FAIL;
}
