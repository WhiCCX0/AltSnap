#ifndef ALTDRAG_RPC_H
#define ALTDRAG_RPC_H

//#define UNICODE
//#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#define WINVER 0x0600
#endif
#include <windows.h>
#include "unfuck.h"

#ifdef UNICODE
#  define tWinMain wWinMain
#else
#  define tWinMain WinMain
#endif

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600 \
  || defined(__MINGW32__) && ( !defined(__MINGW64_VERSION_MAJOR) || __MINGW64_VERSION_MAJOR < 2 )
// Old version of MINGW64 header or MINGW32 only!
// Unable to use endpointvolume.h !
#define NO_OLEAPI
#endif

// MainWindow commands
#define CMD_TOGGLE        (401)
#define CMD_HIDE          (402)
#define CMD_ELEVATE       (403)
#define CMD_CONFIG        (404)
#define CMD_ABOUT         (405)
#define CMD_EXIT          (406)
#define CMD_FIND          (407)
#define CMD_HELP          (408)
#define CMD_SAVEZONES     (409)
#define CMD_TESTWIN       (410)
#define CMD_OPENINIFILE   (411)
#define CMD_SNAPLAYOUT    (412)
#define CMD_SNAPLAYOUTEND (422)
#define CMD_EDITLAYOUT    (430)
#define CMD_CLOSEZONES    (431)


#ifndef LOW_LEVEL_KB_PROC
	#ifndef DECORATED_HOOKS_DLL_PROCS
		#define LOW_LEVEL_KB_PROC "LowLevelKeyboardProc"
		#define LOAD_PROC "Load"
		#define UNLOAD_PROC "Unload"
	#else
		#define LOW_LEVEL_KB_PROC "_LowLevelKeyboardProc@12"
		#define LOAD_PROC "_Load@8"
		#define UNLOAD_PROC "_Unload@0"
	#endif
#endif

// Extra messages for Action Menu
#define LP_CURSORPOS   (1<<0)
#define LP_TOPMOST     (1<<1)
#define LP_BORDERLESS  (1<<2)
#define LP_MAXIMIZED   (1<<3)
#define LP_ROLLED      (1<<4)
#define LP_MOVEONOFF   (1<<5)
#define LP_NOALTACTION (1<<6)

// App
#define APP_NAME       TEXT("AltSnap")
#define APP_NAMEA      "AltSnap"
#define APP_PROPPT     APP_NAME TEXT("-RDim")
#define APP_PROPFL     APP_NAME TEXT("-RFlag")
#define APP_PROPOFFSET APP_NAME TEXT("-ROffset")
#define APP_PRBDLESS   APP_NAME TEXT("-RStyle")
#define APP_ROLLED     APP_NAME TEXT("-Rolled")
#define APP_ASONOFF    APP_NAME TEXT("-ASOnOff")
#define APP_MOVEONOFF  APP_NAME TEXT("-MoveOnOff")
#define APP_PRODPI     APP_NAME TEXT("-ODPI")
#define APP_OWDMCP     APP_NAME TEXT("-OWDMCP")
#define FZ_PROPPT      TEXT("FancyZones_RestoreSize")
#define FZ_PROPZONES   TEXT("FancyZones_zones")

// User Messages
#define WM_TRAY           (WM_USER+2)
#define WM_SCLICK         (WM_USER+3)
#define WM_UPDCFRACTION   (WM_USER+4)
#define WM_UPDATETRAY     (WM_USER+5)
#define WM_OPENCONFIG     (WM_USER+6)
#define WM_CLOSECONFIG    (WM_USER+7)
#define WM_UPDATESETTINGS (WM_USER+8)
#define WM_ADDTRAY        (WM_USER+9)
#define WM_HIDETRAY       (WM_USER+10)
#define WM_UNIKEYMENU     (WM_USER+11)
#define WM_GETCLICKHWND   (WM_USER+12)
#define WM_STACKLIST      (WM_USER+13)
#define WM_FINISHMOVEMENT (WM_USER+14)
#define WM_CLOSEMODE      (WM_USER+15)
#define WM_SETLAYOUTNUM   (WM_USER+16)
#define WM_GETLAYOUTREZ   (WM_USER+17)
#define WM_GETBESTLAYOUT  (WM_USER+18)
//#define WM_GETZONESLEN    (WM_USER+19)
#define WM_GETZONES       (WM_USER+20)
#define WM_ACTIONFWD      (WM_USER+21) // hooks.dll -> AltSnap.exe: new action forward
#define WM_ACTIONRES      (WM_USER+22) // worker/main thread -> AltSnap.exe: action result

// List of possible actions
// ACVALUE(AC_ENUM, IniString, Info)
#define ACTION_MAP \
    ACVALUE(AC_NONE=0,       Nothing,     00) \
    ACVALUE(AC_MOVE,         Move,        MR) \
    ACVALUE(AC_RESIZE,       Resize,      MR) \
    ACVALUE(AC_RESTORE,      Restore,     MR) \
    ACVALUE(AC_MENU,         Menu,        CL) \
    ACVALUE(AC_MINIMIZE,     Minimize,    MR) \
    ACVALUE(AC_MAXIMIZE,     Maximize,    MR) \
    ACVALUE(AC_CENTER,       Center,      MR) \
    ACVALUE(AC_CENTER2,      Center2,     MR) \
    ACVALUE(AC_ALWAYSONTOP,  AlwaysOnTop, ZO) \
    ACVALUE(AC_CLOSE,        Close,       CL) \
    ACVALUE(AC_LOWER,        Lower,       ZO) \
    ACVALUE(AC_FOCUS,        Focus,       ZO) \
    ACVALUE(AC_BORDERLESS,   Borderless,  00) \
    ACVALUE(AC_KILL,         Kill,        CL) \
    ACVALUE(AC_PAUSE,        Pause,       CL) \
    ACVALUE(AC_RESUME,       Resume,      CL) \
    ACVALUE(AC_MAXHV,        MaximizeHV,  MR) \
    ACVALUE(AC_MINALL,       MinAllOther, 00) \
    ACVALUE(AC_MUTE,         Mute,        00) \
    ACVALUE(AC_SIDESNAP,     SideSnap,    MR) \
    ACVALUE(AC_EXTENDSNAP,   ExtendSnap,  MR) \
    ACVALUE(AC_EXTENDTNEDGE, ExtendTNEdge,MR) \
    ACVALUE(AC_MOVETNEDGE,   MoveTNEdge,  MV) \
    ACVALUE(AC_NSTACKED,     NStacked,    ZO) \
    ACVALUE(AC_NSTACKED2,    NStacked2,   ZO) \
    ACVALUE(AC_PSTACKED,     PStacked,    ZO) \
    ACVALUE(AC_PSTACKED2,    PStacked2,   ZO) \
    ACVALUE(AC_STACKLIST,    StackList,   CL) \
    ACVALUE(AC_STACKLIST2,   StackList2,  CL) \
    ACVALUE(AC_ALTTABLIST,   AltTabList,  CL) \
    ACVALUE(AC_ALTTABFULLLIST, AltTabFullList,  CL) \
    ACVALUE(AC_ASONOFF,      ASOnOff,     CL) \
    ACVALUE(AC_MOVEONOFF,    MoveOnOff,   CL) \
    \
    ACVALUE(AC_MZONE, MZone, MR) \
    ACVALUE(AC_XZONE, XZone, MR) \
    \
    ACVALUE(AC_STEP,  Step,  MR) \
    \
    ACVALUE(AC_NLAYOUT,  NLayout,  00) \
    ACVALUE(AC_PLAYOUT,  PLayout,  00) \
    ACVALUE(AC_SHRT,     Shrt,     00) \
    \
    ACVALUE(AC_NPLAYOUT,     NPLayout,     00) \
    ACVALUE(AC_ROLL,         Roll,         MR) \
    ACVALUE(AC_ALTTAB,       AltTab,       ZO) \
    ACVALUE(AC_VOLUME,       Volume,       00) \
 /* ACVALUE(AC_BRIGHTNESS,   Brightness,   00) */ \
    ACVALUE(AC_TRANSPARENCY, Transparency, 00) \
    ACVALUE(AC_HSCROLL,      HScroll,      00) \
    ACVALUE(AC_ZOOM,         Zoom,         MR) \
    ACVALUE(AC_ZOOM2,        Zoom2,        MR) \
    ACVALUE(AC_NPSTACKED,    NPStacked,    ZO) \
    ACVALUE(AC_NPSTACKED2,   NPStacked2,   ZO) \
    ACVALUE(AC_WINDOWDETAILS, WindowDetails, 00) \
    ACVALUE(AC_DIMMERFIXED,  DimmerFixed,  00) \
    ACVALUE(AC_DIMMERFOLLOW, DimmerFollow, 00) \
    ACVALUE(AC_SCREENSHOT,   Screenshot,   00) \
    ACVALUE(AC_REVEALEXE,    RevealExe,    00) \
    ACVALUE(AC_ALIGN,        Align,        MR) \
    ACVALUE(AC_CLIPBOARD,    Clipboard,    00)

#define ACVALUE(a, b, c) a,
enum action { ACTION_MAP AC_MAXVALUE, AC_ORICLICK };
#undef ACVALUE

typedef struct rgTagAction {
    BYTE  ac;
    BYTE  fl;
    short wp;
} action_t;

static const action_t k_action_none = { AC_NONE, 0, 0 };
static const action_t k_action_move = { AC_MOVE, 0, 0 };

#define PACK_ACTION(x) ( (LPARAM)(x.ac << 24) | (LPARAM)(x.fl << 16) | (LPARAM)(x.wp << 0) )
#define UNPACK_ACTION(dst, lp) do { dst.ac = (lp>>24) & 0xFF; dst.fl = (lp>>16) & 0xFF; dst.wp = lp&0xFFFF; } while(0)

// List of extra info options
enum {
    ACINFO_MOVE   =  1,
    ACINFO_RESIZE =  2,
    ACINFO_ZORDER =  4,
    ACINFO_CLOSE  =  8,
};

// Helper function to get extra action info
static xpure UCHAR ActionInfo(action_t action)
{
    enum {
        MV = ACINFO_MOVE,
        RZ = ACINFO_RESIZE,
        ZO = ACINFO_ZORDER,
        CL = ACINFO_CLOSE,
        MR = ACINFO_MOVE|ACINFO_RESIZE,
    };
    #define ACVALUE(a, b, c) (c),
    static const UCHAR action_info[] = { ACTION_MAP };

    #undef ACVALUE
    return action_info[action.ac];
}

enum {
    ACFL_ZERO  = 0,
    ACFL_LEFT  = 1,
    ACFL_UP    = 2,
    ACFL_RIGHT = 3,
    ACFL_DOWN  = 4,
    ACFL_SET   = 5,
    ACFL_TOGGLE= 6,
};

#define MOUVEMENT(action) (action.ac <= AC_RESIZE)

///////////////////////////////////////////////////////////////////////////
// Check if key is assigned in the HKlist
static int pure IsHotkeyy(unsigned char key, const unsigned char *HKlist)
{
    const UCHAR *pos=&HKlist[0];
    while (*pos) {
        if (key == *pos) {
            return 1;
        }
        pos++;
    }
    return 0;
}
static int pure IsActionInList(const enum action ac, const enum action *aclst)
{
    do {
        if(ac == *aclst)
            return 1;
    } while(*aclst++ != AC_NONE);

    return 0;
}
// Convert zone number to ini name entry
static TCHAR *ZidxToZonestr(int laynum, int idx, TCHAR zname[AT_LEAST 32])
{
    if (laynum > 9 ) return NULL;
    TCHAR txt[UINT_DIGITS+1];
    zname[0] = !laynum?TEXT('\0'): TEXT('A')+laynum-1 ;
    zname[1] = '\0';
    lstrcat_s(zname, 32, TEXT("Zone"));
    lstrcat_s(zname, 32, Uint2lStr(txt, idx)); // Zone Name from zone number

    return zname;
}
static char *ZidxToZonestrA(int laynum, int idx, char zname[AT_LEAST 32])
{
    if (laynum > 9 ) return NULL;
    char txt[16];
    zname[0] = !laynum?'\0': 'A'+laynum-1 ;
    zname[1] = '\0';
    lstrcat_sA(zname, 32, "Zone");
    lstrcat_sA(zname, 32, Uint2lStrA(txt, idx)); // Zone Name from zone number

    return zname;
}


/* Timer helper for main thread entries (HOOK-5): only active with LOG_STUFF.
 * The counter frequency is cached on first use, it never changes at runtime.
 */
#ifdef LOG_STUFF
#define ACT_TIMER_BEGIN(t) LARGE_INTEGER t; QueryPerformanceCounter(&t)
#define ACT_TIMER_END(t, name) do { \
        static LARGE_INTEGER freq; \
        LARGE_INTEGER t2; \
        QueryPerformanceCounter(&t2); \
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq); \
        if (freq.QuadPart > 0) { \
            unsigned long long us = (unsigned long long)(t2.QuadPart - t.QuadPart) * 1000000ULL / (unsigned long long)freq.QuadPart; \
            if (us >= 10000ULL) LOG("SLOW %s %u us", (name), (unsigned)us); \
        } \
    } while (0)
#else
#define ACT_TIMER_BEGIN(t) do {} while (0)
#define ACT_TIMER_END(t, name) do {} while (0)
#endif

/* Strict sub-action parsing for Align/Clipboard (ACT-4, T3).
 * p points at the character returned by is_base_action(), i.e. '_', '=' or NUL.
 * Only '_' followed by 1-3 ASCII digits and then NUL, '=', ' ', Tab or '"' is
 * accepted; the value has to be in 1..127. Anything else gives 0.
 */
static pure BYTE ParseActionSub(const TCHAR *p)
{
    unsigned val = 0;
    int ndigits = 0;
    if (!p || *p != TEXT('_')) return 0;
    p++;
    while (TEXT('0') <= *p && *p <= TEXT('9')) {
        val = val * 10 + (unsigned)(*p - TEXT('0'));
        p++;
        if (++ndigits > 3) return 0;
    }
    if (!ndigits) return 0;
    if (*p != TEXT('\0') && *p != TEXT('=') && *p != TEXT(' ')
    &&  *p != TEXT('\t') && *p != TEXT('"')) return 0;
    if (val < 1 || val > 127) return 0;
    return (BYTE)val;
}

static pure TCHAR* is_base_action(const TCHAR *a, const char *b)
{
    while (*a && *a != '=' && *a != '_' && *a == *b) {
        b++; a++;
    }
    return (*a == '_' || *a == '=' || *a == *b) ? (TCHAR*)a : NULL;
}

/* Map action string to actual action_t struct
 * Possible formats:
 * MyActionName[_XXX][_XXX_YYY]\0 ; ending with a NULL
 * MyActionName[_XXX][_XXX_YYY]=  ; Or with an equal
 */
static pure action_t MapActionW(const TCHAR *txt)
{
    action_t action = { AC_NONE };
    if (!txt || !*txt) return action;
    #define ACVALUE(a, b, c) (#b),
    static const char *action_map[] = { ACTION_MAP };
    #undef ACVALUE
    UCHAR ac;

    // ShrtX X = 0 to Z (for backward compat...)
    // It should be done with Shrt_XX.
    if (txt[0] == 'S' && txt[1] == 'h' && txt[2] == 'r' && txt[3] == 't'
    && '0' <= txt[4] && txt[4] <= 'Z'
    && (txt[5] == '\0' || txt[5] == '=') ) {
        TCHAR c = txt[4];
        UCHAR num = c<='9' ? c - '0' : c-'A'+10;
        action.ac = AC_SHRT;
        action.fl = min(num, 35); // Safety clamp
        return action;
    }

    for (ac=0; ac < ARR_SZ(action_map); ac++) {
        TCHAR *params;
        if ((params = is_base_action(txt, action_map[ac]))) {
            BYTE flagparam = 0;
            action.ac = ac;
            if (ac == AC_ALIGN || ac == AC_CLIPBOARD) {
                // Strict sub action parsing, no wp allowed (MSG-2).
                action.fl = ParseActionSub(params);
                action.wp = 0;
                return action;
            }
            if (*params == TEXT('_') && *++params) {
                // We got 1 parameter.
                TCHAR cc = *params++;
                switch(cc) {
                // Direction flags
                case 'L': flagparam = ACFL_LEFT;   break;
                case 'U': flagparam = ACFL_UP;     break;
                case 'R': flagparam = ACFL_RIGHT;  break;
                case 'D': flagparam = ACFL_DOWN;   break;
                case 'S': flagparam = ACFL_SET;    break;
                case 'T': flagparam = ACFL_TOGGLE; break;
                default: // Parse an int
                    --params;
                    for (; cc != '=' && cc != '_' && cc; cc = *params) {
                        flagparam = flagparam * 10 + (cc - TEXT('0'));
                        ++params;
                    }
                }
                action.fl = flagparam;

                if (*params == ('_') && *++params) {
                    // We got 2 parameters.
                    action.wp = strtoi(params); // strtoi stops on any non digit
                }
                //LOGA("Read action %s_%d_%d",  action_map[ac], (int)action.fl, (int)action.wp);
            }
            // We got the action!
            return action;
        }
    }
    return action;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void assertions(void)
{
    //static_assert(sizeof(action_t) == 4, "sizeof(action_t) should be 4!");
    static_assert(AC_ORICLICK <= 127, "Maximal action number should be 127 or less!");
}

#endif /* ALTDRAG_RPC_H */
