/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Result kinds and codes for WM_ACTIONRES */
enum { ACK_SHOT = 1, ACK_SHOTCLIP = 2, ACK_CLIPTEXT = 3, ACK_CLIPCLEAR = 4 };
enum { ACR_OK = 0, ACR_CANCEL, ACR_BUSY, ACR_CLIPBUSY, ACR_STALE, ACR_NOTARGET,
       ACR_CAPFAIL, ACR_SAVEFAIL, ACR_NOWIC, ACR_NOMEM, ACR_FAIL };
#define ACRF_PROTECTED 0x8000 /* SHOT-7: target may block capture */
#define ACRES(kind, code, flags) ((WPARAM)((flags) | ((kind) << 8) | (code)))
/* actexe.c */
static void ActionFwdDispatch(HWND hwnd, action_t action);
static void ActionResult(WPARAM res);
static HWND NormalizeTarget(HWND hwnd, int needproc);
static void ActionNotify(const TCHAR *text);
/* procinfo.c */
static DWORD GetWindowProcPath(HWND hwnd, DWORD *pid, DWORD *realpid, TCHAR **path);
static void WindowDetailsShow(HWND hwnd);
static void RevealExeShow(HWND hwnd);
/* clipbrd.c */
static void ClipboardInit(void);
static void ClipboardRequest(HWND hwnd, BYTE sub);
static int ClipboardCommit(LONG reqseq, BOOL chkcbseq, DWORD cbseq, UINT fmt, HGLOBAL hg);
/* shot.c */
static void ScreenshotStart(HWND hwnd, int toclip);
/* dimmer.c */
static void DimmerToggle(HWND hwnd, int follow);
static void DimmerCleanup(void);
