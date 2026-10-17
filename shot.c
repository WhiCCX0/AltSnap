/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Screenshot task (WP-SHOT fills this in). */

/* Single flight flag shared by screenshots and Clipboard_1. */
static volatile LONG g_shot_busy = 0;

/* Start a screenshot task for hwnd; toclip=1 puts the image on the clipboard
 * instead of opening a save dialog.
 */
static void ScreenshotStart(HWND hwnd, int toclip)
{
    LOG("ScreenshotStart hwnd=%p toclip=%d", hwnd, toclip);
}
