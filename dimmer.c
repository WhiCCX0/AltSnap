/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Dim others (WP-DIM fills this in). */

/* Toggle the dimmer for hwnd; follow=1 keeps following the foreground window. */
static void DimmerToggle(HWND hwnd, int follow)
{
    LOG("DimmerToggle hwnd=%p follow=%d", hwnd, follow);
}

/* Close the dimmer. Called only from the three RES-6 places. */
static void DimmerCleanup(void)
{
    LOG("DimmerCleanup");
}
