/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Align action, runs inside hooks.dll (WP-ALIGN fills this in). */

/* Perform the align sub action (1..11) on the given window.
 * hwnd is the only trusted target (ACT-7); invalid sub actions do nothing.
 */
static void ActionAlign(HWND hwnd, BYTE sub)
{
    ACT_TIMER_BEGIN(t);
    LOG("ActionAlign hwnd=%p sub=%d", hwnd, (int)sub);
    if (sub < 1 || sub > 11) {
        LOG("ActionAlign: invalid sub action %d", (int)sub);
        ACT_TIMER_END(t, "ActionAlign");
        return;
    }
    ACT_TIMER_END(t, "ActionAlign");
}
