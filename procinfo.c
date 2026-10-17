/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Process information helpers (WP-PROC fills this in). */

/* Query the process path of hwnd.
 * Returns 0 on success, otherwise a Win32 error code.
 * On success *path is a heap buffer of 32768 TCHAR that the caller frees,
 * *pid is the process id and *realpid the real (AFH child) process id.
 */
static DWORD GetWindowProcPath(HWND hwnd, DWORD *pid, DWORD *realpid, TCHAR **path)
{
    LOG("GetWindowProcPath hwnd=%p", hwnd);
    if (pid) *pid = 0;
    if (realpid) *realpid = 0;
    if (path) *path = NULL;
    return ERROR_CALL_NOT_IMPLEMENTED;
}
static void WindowDetailsShow(HWND hwnd)
{
    LOG("WindowDetailsShow hwnd=%p", hwnd);
}
static void RevealExeShow(HWND hwnd)
{
    LOG("RevealExeShow hwnd=%p", hwnd);
}
