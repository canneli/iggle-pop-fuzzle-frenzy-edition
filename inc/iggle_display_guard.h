#ifndef IGGLE_DISPLAY_GUARD_H
#define IGGLE_DISPLAY_GUARD_H
#include <windows.h>
/* This fork never switches the Windows display mode, including on shutdown. */
static inline LONG iggle_display_guard(DEVMODEA *mode, DWORD flags)
{
    if (!mode || (flags & CDS_TEST)) return DISP_CHANGE_SUCCESSFUL;
    return DISP_CHANGE_BADMODE;
}
#undef ChangeDisplaySettings
#define ChangeDisplaySettings iggle_display_guard
#endif
