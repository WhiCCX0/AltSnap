/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (C) 2026    AltSnap contributors                            *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 or later.              *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Window screenshot (A4) and Clipboard_1 (WP-SHOT).
 *
 * Main thread: quick pre checks, single flight, copy of every parameter into
 * a heap task, CreateThread, CloseHandle (SHOT-1..SHOT-4). From then on the
 * worker owns the whole task and is the only one that frees it (SHOT-16).
 * Results travel back as WM_ACTIONRES codes only, no pointer and no handle is
 * ever put in a message (MSG-3, SHOT-13).
 *
 * The worker captures the window into a top-down 24bpp BGR DIB section, then
 * either packs a CF_DIB and commits it from this very thread (SHOT-12), or
 * opens the save dialog and encodes PNG/JPEG (WIC v1) or BMP (SHOT-9..11)
 * into a temporary file that is moved over the target file (SHOT-10).
 */

#include <commdlg.h>  /* OPENFILENAMEW: types only, comdlg32 is loaded at runtime */
#include <wincodec.h> /* WIC v1 in-proc COM, no link dependency (API-3) */

/* Fixed JPEG quality, no new INI key (task order) */
#define SHOT_JPEG_QUALITY 0.90f
/* PrintWindow flags, local define so _WIN32_WINNT stays 0x0600 (API-5) */
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
/* Same reason: WDA_NONE is not in older SDK headers (API-5) */
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
/* Sanity limit on one dimension: every product then fits in 32 bits, so an
 * absurd window rect cannot ask for a huge allocation. */
#define SHOT_MAX_DIM 32768
/* Default file name buffer: "AltSnap_YYYYMMDD_HHMMSS.png" plus NUL */
#define SHOT_NAME_CHARS 40
/* Final path buffer, on the heap only (ABI-3) */
#define SHOT_PATH_CHARS 32768
/* GetTempFileNameW accepts at most MAX_PATH-14 characters of directory */
#define SHOT_TEMP_MAXDIR (MAX_PATH - 14)
/* Target probe budget (SHOT-5) */
#define SHOT_PROBE_MS 2000

/* Sentinel pattern that detects a PrintWindow() which reports success but
 * draws nothing (E11 / decision D-30). The bytes are the B, G, R of the
 * top-down DIB, so the pattern is 5A A5 3C over and over; that combination is
 * not a plausible image colour, and a real black image writes zeros, which
 * does not match it either.
 */
#define SHOT_SENT_B 0x5A
#define SHOT_SENT_G 0xA5
#define SHOT_SENT_R 0x3C

/* WIC v1 factory and containers (API-3), defined here as constants so that no
 * extra link library or INITGUID is needed. */
static const CLSID my_CLSID_WICImagingFactory =
    {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const IID my_IID_IWICImagingFactory =
    {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID my_GUID_ContainerFormatPng =
    {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
static const GUID my_GUID_ContainerFormatJpeg =
    {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
static const GUID my_GUID_WICPixelFormat24bppBGR =
    {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};

/* ole32 and comdlg32 (API-3): resolved at runtime, never static imports */
static HRESULT (WINAPI *ShotCoInitEx)(LPVOID pvReserved, DWORD dwCoInit);
static void (WINAPI *ShotCoUninit)(void);
static HRESULT (WINAPI *ShotCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
static BOOL (WINAPI *ShotGetSaveName)(LPOPENFILENAMEW);
static DWORD (WINAPI *ShotCommDlgErr)(void);

typedef struct shot_task {
    HWND hwnd;        /* target window, only touched by the worker */
    int toclip;       /* 1: Clipboard_1, 0: save to a file */
    float quality;    /* JPEG quality, copied from the constant */
    LONG seq;         /* clipboard request sequence (RES-4) */
    DWORD cbseq;      /* GetClipboardSequenceNumber() at dispatch (RES-4) */
    TCHAR defname[SHOT_NAME_CHARS];
} shot_task_t;

typedef struct shot_capture {
    HDC hdc;
    HBITMAP hbm;
    HGDIOBJ oldobj;   /* object that was selected in hdc before hbm */
    BYTE *bits;
    int width;
    int height;
    int stride;
    int offX;
    int offY;
    int cropW;
    int cropH;
    UINT flags;       /* ACRF_* for the result message */
} shot_capture_t;

/* Single flight flag shared by screenshots and Clipboard_1 (SHOT-2). Only the
 * main thread takes it and only the worker gives it back (SHOT-16); if the
 * worker never returns it stays set and later requests only report busy
 * (SHOT-15). */
static volatile LONG g_shot_busy = 0;

/* Post one result code to the main thread (SHOT-13) */
static void ShotReport(UINT kind, UINT code, UINT flags)
{
    PostMessage(g_hwnd, WM_ACTIONRES, ACRES(kind, code, flags), 0);
}

/* Row length of a 24bpp bitmap, 4 byte aligned (SHOT-8/SHOT-12) */
static unsigned int ShotStride(int width)
{
    return (unsigned int)(((unsigned long long)width * 3 + 3) & ~3ULL);
}

/* Fill the whole capture buffer, row padding included, with the sentinel
 * pattern. The padding bytes are never read: ShotPackDIB copies only cropW*3
 * bytes per row and zeroes the destination padding itself, and WritePixels is
 * called with cbStride = cbBufferSize = cropW*3, so filling them is harmless.
 */
static void ShotFillSentinel(BYTE *bits, int stride, int height)
{
    int y, x;
    for (y = 0; y < height; y++) {
        BYTE *row = bits + (size_t)y * (size_t)stride;
        for (x = 0; x + 3 <= stride; x += 3) {
            row[x]     = SHOT_SENT_B;
            row[x + 1] = SHOT_SENT_G;
            row[x + 2] = SHOT_SENT_R;
        }
        if (x < stride) row[x] = SHOT_SENT_B;
        if (x + 1 < stride) row[x + 1] = SHOT_SENT_G;
    }
}

/* 1 when any of the width*3 pixel bytes of any row is no longer the sentinel,
 * i.e. the previous PrintWindow() really drew something. Stops at the first
 * difference, so a drawn image costs a few bytes, not a full scan. Pixels are
 * compared as whole BGR triplets (width*3 is always a multiple of 3), and the
 * row padding is never looked at.
 */
static int ShotBufferChanged(const BYTE *bits, int stride, int width, int height)
{
    const size_t rowbytes = (size_t)width * 3;
    int y;
    size_t x;
    for (y = 0; y < height; y++) {
        const BYTE *row = bits + (size_t)y * (size_t)stride;
        for (x = 0; x < rowbytes; x += 3) {
            if (row[x] != SHOT_SENT_B || row[x + 1] != SHOT_SENT_G
            ||  row[x + 2] != SHOT_SENT_R)
                return 1;
        }
    }
    return 0;
}

/* Compare two GUIDs without memcmp (which would become a new import) */
static int ShotGuidEq(const GUID *a, const GUID *b)
{
    int i;
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return 0;
    for (i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) return 0;
    }
    return 1;
}

/* Load ole32 entry points on first use (API-3) */
static int ShotLoadOle32(void)
{
    if (ShotCoCreate) return 1;
    ShotCoInitEx = (HRESULT (WINAPI *)(LPVOID, DWORD))LoadDLLProc("OLE32.DLL", "CoInitializeEx");
    ShotCoUninit = (void (WINAPI *)(void))LoadDLLProc("OLE32.DLL", "CoUninitialize");
    ShotCoCreate = (HRESULT (WINAPI *)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *))
        LoadDLLProc("OLE32.DLL", "CoCreateInstance");
    if (!ShotCoInitEx || !ShotCoUninit || !ShotCoCreate) {
        LOG("Shot: ole32 unavailable");
        ShotCoInitEx = NULL;
        ShotCoUninit = NULL;
        ShotCoCreate = NULL;
        return 0;
    }
    return 1;
}

/* Load comdlg32 entry points on first use (API-3) */
static int ShotLoadComdlg32(void)
{
    if (ShotGetSaveName) return 1;
    ShotGetSaveName = (BOOL (WINAPI *)(LPOPENFILENAMEW))
        LoadDLLProc("COMDLG32.DLL", "GetSaveFileNameW");
    ShotCommDlgErr = (DWORD (WINAPI *)(void))
        LoadDLLProc("COMDLG32.DLL", "CommDlgExtendedError");
    if (!ShotGetSaveName) LOG("Shot: comdlg32 unavailable");
    return !!ShotGetSaveName;
}

/* GetWindowDisplayAffinity is Win7+ (API-3): runtime, diagnostic only */
static BOOL ShotGetDisplayAffinity(HWND hwnd, DWORD *affinity)
{
    typedef BOOL (WINAPI *funk_t)(HWND, DWORD *);
    static funk_t funk = (funk_t)IPTR;

    if (funk == (funk_t)IPTR)
        funk = (funk_t)LoadDLLProc("USER32.DLL", "GetWindowDisplayAffinity");
    if (!funk) return FALSE;
    return funk(hwnd, affinity);
}

/* Append a zero padded decimal number */
static void ShotPutNum(sb_t *sb, unsigned int val, int digits)
{
    TCHAR buf[12];
    int i = digits;
    while (i > 0) {
        i--;
        buf[i] = (TCHAR)(TEXT('0') + val % 10);
        val /= 10;
    }
    buf[digits] = TEXT('\0');
    SbAppendStr(sb, buf);
}

/* "AltSnap_YYYYMMDD_HHMMSS.png" from the local time (task order) */
static void ShotDefaultName(TCHAR *buf, size_t cch)
{
    SYSTEMTIME st;
    sb_t sb = {buf, 0, cch};

    buf[0] = TEXT('\0');
    GetLocalTime(&st);
    SbAppendStr(&sb, TEXT("AltSnap_"));
    ShotPutNum(&sb, st.wYear, 4);
    ShotPutNum(&sb, st.wMonth, 2);
    ShotPutNum(&sb, st.wDay, 2);
    SbAppendStr(&sb, TEXT("_"));
    ShotPutNum(&sb, st.wHour, 2);
    ShotPutNum(&sb, st.wMinute, 2);
    ShotPutNum(&sb, st.wSecond, 2);
    SbAppendStr(&sb, TEXT(".png"));
}

/* Free the capture resources. The pixels are only read until the last
 * WritePixels()/pack call, so this runs after the image was written.
 * DeleteDC() releases the bitmap selection, so the bitmap object can be
 * deleted right after it (SHOT-14).
 */
static void ShotCaptureFree(shot_capture_t *cap)
{
    if (cap->hdc) {
        if (cap->hbm && cap->oldobj) SelectObject(cap->hdc, cap->oldobj);
        DeleteDC(cap->hdc);
        cap->hdc = NULL;
    }
    if (cap->hbm) {
        DeleteObject(cap->hbm);
        cap->hbm = NULL;
    }
    cap->oldobj = NULL;
    cap->bits = NULL;
}

/* Capture the whole window rect into a top-down 24bpp BGR DIB section and
 * compute the visible crop rect (SHOT-5..SHOT-8). Returns an ACR_* code.
 */
static int ShotCapture(HWND hwnd, shot_capture_t *cap)
{
    RECT rcWin, rcVis, rcInter;
    BITMAPINFO bi;
    HDC hdc;
    HBITMAP hbm;
    HGDIOBJ old;
    void *bits = NULL;
    DWORD affinity = 0;
    DWORD_PTR probe = 0;
    unsigned int stride;
    unsigned long long total;
    int haveVis;

    mem00(cap, sizeof(*cap));
    if (!IsWindow(hwnd)) {
        LOG("Shot: target %p is gone", hwnd);
        return ACR_NOTARGET;
    }
    /* SHOT-5: probe the target, a hung window is reported as a capture
     * failure instead of blocking this thread forever. The two interesting
     * cases are logged apart: a real timeout (1460) and a denied request on a
     * higher integrity target (5); the latter still gets a capture attempt
     * (UIPI is never bypassed, UX-2). */
    if (!SendMessageTimeout(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG
                          , SHOT_PROBE_MS, &probe)) {
        DWORD err = GetLastError();
        if (err == ERROR_TIMEOUT) {
            LOG("Shot: probe of %p timed out (%u)", hwnd, (unsigned)err);
            return ACR_CAPFAIL;
        }
        LOG("Shot: probe of %p denied/failed err=%u, trying anyway"
          , hwnd, (unsigned)err);
    }
    /* SHOT-7: diagnostic only, never used to decide the outcome */
    if (ShotGetDisplayAffinity(hwnd, &affinity) && affinity != WDA_NONE) {
        LOG("Shot: display affinity %X, image may be blank", (unsigned)affinity);
        cap->flags |= ACRF_PROTECTED;
    }
    if (!GetWindowRect(hwnd, &rcWin)) {
        LOG("Shot: GetWindowRect failed err=%u", (unsigned)GetLastError());
        return ACR_CAPFAIL;
    }
    cap->width = (int)(rcWin.right - rcWin.left);
    cap->height = (int)(rcWin.bottom - rcWin.top);
    if (cap->width <= 0 || cap->height <= 0
    || cap->width > SHOT_MAX_DIM || cap->height > SHOT_MAX_DIM) {
        LOG("Shot: bad window rect %dx%d", cap->width, cap->height);
        return ACR_CAPFAIL;
    }
    stride = ShotStride(cap->width);
    total = (unsigned long long)stride * (unsigned long long)cap->height;
    if (total > 0xFFFFFFFFULL) {
        LOG("Shot: window too big (%dx%d)", cap->width, cap->height);
        return ACR_CAPFAIL;
    }

    mem00(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cap->width;
    bi.bmiHeader.biHeight = -cap->height; /* top down (SHOT-8) */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = (DWORD)total;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc) {
        LOG("Shot: CreateCompatibleDC failed");
        return ACR_NOMEM;
    }
    cap->hdc = hdc;
    hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) {
        LOG("Shot: CreateDIBSection failed err=%u", (unsigned)GetLastError());
        if (hbm) DeleteObject(hbm);
        ShotCaptureFree(cap);
        return ACR_NOMEM;
    }
    cap->hbm = hbm;
    cap->bits = (BYTE *)bits;
    old = SelectObject(hdc, hbm);
    if (!old || old == HGDI_ERROR) {
        LOG("Shot: SelectObject failed");
        ShotCaptureFree(cap);
        return ACR_NOMEM;
    }
    cap->oldobj = old;

    /* SHOT-6: full content first, plain flags only as a second attempt, and
     * no BitBlt fallback. TRUE does not promise a complete image.
     *
     * E11 / D-30: on some systems (measured here: Windows 11 26200 with an AMD
     * adapter in a console session) PrintWindow(hwnd, hdc, 2) returns TRUE and
     * draws not a single byte, while flags=0 draws the window correctly. A
     * TRUE return alone therefore cannot tell "drew the image" from "did
     * nothing", so the buffer is pre-filled with a sentinel pattern and the
     * pixels are scanned afterwards. A genuinely black image writes zeros and
     * does not match the sentinel, so it is not mistaken for "nothing drawn".
     * If neither flag touches the buffer, that is proof that nothing was
     * drawn (not "incomplete content"), so the task fails cleanly instead of
     * handing the sentinel pattern to the user (owner decision, r3b).
     */
    ShotFillSentinel(cap->bits, (int)stride, cap->height);
    if (!PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)) {
        LOG("Shot: PrintWindow flags=2 failed, retrying with 0");
        if (!PrintWindow(hwnd, hdc, 0)) {
            LOG("Shot: PrintWindow failed err=%u", (unsigned)GetLastError());
            ShotCaptureFree(cap);
            return ACR_CAPFAIL;
        }
    } else {
        /* Make sure the GDI output reached the DIB bits before reading them */
        GdiFlush();
        if (!ShotBufferChanged(cap->bits, (int)stride, cap->width, cap->height)) {
            /* SHOT-6 still applies here: flags=2 reported success, so this is
             * a second attempt with the plain flags, not a failure yet. The
             * sentinel is deliberately not refilled, so the check below also
             * covers "flags=0 drew nothing either". */
            LOG("Shot: PrintWindow flags=2 drew nothing, retrying with 0");
            if (!PrintWindow(hwnd, hdc, 0)) {
                LOG("Shot: PrintWindow flags=0 failed err=%u", (unsigned)GetLastError());
                ShotCaptureFree(cap);
                return ACR_CAPFAIL;
            }
        }
    }
    /* Make sure the GDI output reached the DIB bits before reading them. The
     * first flush above already happened when flags=2 was scanned. */
    GdiFlush();
    /* Both flags reported TRUE but the whole buffer is still the sentinel:
     * certain "not drawn", which is not the same as an incomplete image. */
    if (!ShotBufferChanged(cap->bits, (int)stride, cap->width, cap->height)) {
        LOG("Shot: PrintWindow drew nothing with either flag");
        ShotCaptureFree(cap);
        return ACR_CAPFAIL;
    }

    cap->stride = (int)stride;

    /* SHOT-8: visible rect = DWM extended frame bounds intersected with the
     * window rect. No DWM bounds at all (DWM off, old system) means no crop;
     * an empty intersection is a safe failure. */
    haveVis = S_OK == DwmGetWindowAttributeL(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS
                                           , &rcVis, sizeof(rcVis));
    if (!haveVis) {
        LOG("Shot: no DWM frame bounds, no crop");
        cap->offX = 0;
        cap->offY = 0;
        cap->cropW = cap->width;
        cap->cropH = cap->height;
    } else if (!IntersectRect(&rcInter, &rcVis, &rcWin)) {
        LOG("Shot: visible rect is empty");
        ShotCaptureFree(cap);
        return ACR_CAPFAIL;
    } else {
        cap->offX = (int)(rcInter.left - rcWin.left);
        cap->offY = (int)(rcInter.top - rcWin.top);
        cap->cropW = (int)(rcInter.right - rcInter.left);
        cap->cropH = (int)(rcInter.bottom - rcInter.top);
    }
    /* The crop has to stay inside the buffer we allocated (SHOT-8) */
    if (cap->cropW <= 0 || cap->cropH <= 0
    || cap->offX < 0 || cap->offY < 0
    || cap->offX + cap->cropW > cap->width
    || cap->offY + cap->cropH > cap->height) {
        LOG("Shot: crop %d,%d %dx%d outside %dx%d", cap->offX, cap->offY
          , cap->cropW, cap->cropH, cap->width, cap->height);
        ShotCaptureFree(cap);
        return ACR_CAPFAIL;
    }
    LOG("Shot: window %dx%d stride=%d crop %d,%d %dx%d flags=%X"
      , cap->width, cap->height, cap->stride, cap->offX, cap->offY
      , cap->cropW, cap->cropH, (unsigned)cap->flags);
    return ACR_OK;
}

/* Pack the crop of the top-down buffer into a bottom-up CF_DIB block: one
 * loop crops and flips at the same time (SHOT-12). Shared by Clipboard_1 and
 * the BMP writer (SHOT-11). Returns a GMEM_MOVEABLE block or NULL.
 */
static HGLOBAL ShotPackDIB(const shot_capture_t *cap)
{
    unsigned long long rowbytes = (unsigned long long)cap->cropW * 3;
    unsigned long long dstStride = (rowbytes + 3) & ~3ULL;
    unsigned long long pixsize = dstStride * (unsigned long long)cap->cropH;
    HGLOBAL hg;
    BITMAPINFOHEADER *bi;
    BYTE *dst;
    int y;

    if (!cap->bits || cap->cropW <= 0 || cap->cropH <= 0) return NULL;
    if (pixsize > 0xFFFFFFFFULL - sizeof(BITMAPINFOHEADER)) return NULL;
    hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(sizeof(BITMAPINFOHEADER) + pixsize));
    if (!hg) return NULL;
    dst = (BYTE *)GlobalLock(hg);
    if (!dst) {
        GlobalFree(hg);
        return NULL;
    }
    bi = (BITMAPINFOHEADER *)dst;
    mem00(bi, sizeof(*bi));
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = cap->cropW;
    bi->biHeight = cap->cropH; /* positive: bottom-up */
    bi->biPlanes = 1;
    bi->biBitCount = 24;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)pixsize;
    dst += sizeof(BITMAPINFOHEADER);
    for (y = 0; y < cap->cropH; y++) {
        const BYTE *src = cap->bits
            + (size_t)(cap->offY + cap->cropH - 1 - y) * (size_t)cap->stride
            + (size_t)cap->offX * 3;
        BYTE *row = dst + (size_t)y * (size_t)dstStride;
        unsigned long long i;
        for (i = 0; i < rowbytes; i++) row[i] = src[i];
        for (; i < dstStride; i++) row[i] = 0; /* padding bytes */
    }
    GlobalUnlock(hg);
    return hg;
}

/* Write one packed CF_DIB block as a .bmp file: the 14 byte file header plus
 * the CF_DIB bytes, no WIC (SHOT-11). The handle is closed before returning so
 * that the caller can replace the target file (SHOT-10).
 */
static int ShotWriteBMP(const TCHAR *path, HGLOBAL hg)
{
    BITMAPFILEHEADER bfh;
    const BYTE *dib;
    const BITMAPINFOHEADER *bi;
    SIZE_T allocsize = GlobalSize(hg);
    SIZE_T dibsize;
    HANDLE h;
    DWORD written = 0;
    int ok = 1;

    dib = (const BYTE *)GlobalLock(hg);
    if (!dib || allocsize < sizeof(BITMAPINFOHEADER)) {
        if (dib) GlobalUnlock(hg);
        return ACR_SAVEFAIL;
    }
    bi = (const BITMAPINFOHEADER *)dib;
    if (bi->biSize != sizeof(BITMAPINFOHEADER) || bi->biBitCount != 24) {
        GlobalUnlock(hg);
        return ACR_SAVEFAIL;
    }
    /* The size of the CF_DIB is what the header says, not the (possibly
     * larger) heap block size, otherwise the file would get trailing bytes */
    dibsize = sizeof(BITMAPINFOHEADER) + (SIZE_T)bi->biSizeImage;
    if (dibsize > allocsize) {
        GlobalUnlock(hg);
        return ACR_SAVEFAIL;
    }
    mem00(&bfh, sizeof(bfh));
    bfh.bfType = 0x4D42; /* "BM" */
    bfh.bfSize = (DWORD)(sizeof(bfh) + dibsize);
    bfh.bfOffBits = (DWORD)(sizeof(bfh) + sizeof(BITMAPINFOHEADER));
    /* CREATE_ALWAYS is safe on this name: it comes from GetTempFileNameW and
     * is unique to this task, the target file is never opened here (SHOT-10) */
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS
                  , FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG("Shot: CreateFileW failed err=%u", (unsigned)GetLastError());
        GlobalUnlock(hg);
        return ACR_SAVEFAIL;
    }
    if (!WriteFile(h, &bfh, (DWORD)sizeof(bfh), &written, NULL)
    || written != (DWORD)sizeof(bfh))
        ok = 0;
    if (ok) {
        written = 0;
        if (!WriteFile(h, dib, (DWORD)dibsize, &written, NULL)
        || written != (DWORD)dibsize)
            ok = 0;
    }
    if (!CloseHandle(h)) ok = 0;
    GlobalUnlock(hg);
    if (!ok) LOG("Shot: BMP write failed err=%u", (unsigned)GetLastError());
    return ok ? ACR_OK : ACR_SAVEFAIL;
}

/* The COM objects of one WIC encode. The pixel data is written into the
 * encoder first, then the GDI objects are freed, and only then are these
 * released (which also closes the file handle) (SHOT-14).
 */
typedef struct shot_wic {
    IWICImagingFactory *factory;
    IWICStream *stream;
    IWICBitmapEncoder *enc;
    IWICBitmapFrameEncode *frame;
    IPropertyBag2 *props;
} shot_wic_t;

/* Release every interface held by the context, in reverse order. Releasing
 * the stream also closes the file handle it opened (SHOT-10/SHOT-14).
 */
static void ShotWICFree(shot_wic_t *wic)
{
    if (wic->props) { wic->props->lpVtbl->Release(wic->props); wic->props = NULL; }
    if (wic->frame) { wic->frame->lpVtbl->Release(wic->frame); wic->frame = NULL; }
    if (wic->enc)   { wic->enc->lpVtbl->Release(wic->enc); wic->enc = NULL; }
    if (wic->stream){ wic->stream->lpVtbl->Release(wic->stream); wic->stream = NULL; }
    if (wic->factory){ wic->factory->lpVtbl->Release(wic->factory); wic->factory = NULL; }
}

/* Release the encoder side of the context, which flushes the image and closes
 * the file, and keep only the factory. Called right after the last commit so
 * that the file handle is gone before the GDI objects are freed (SHOT-14).
 */
static void ShotWICCloseOutput(shot_wic_t *wic)
{
    if (wic->props) { wic->props->lpVtbl->Release(wic->props); wic->props = NULL; }
    if (wic->frame) { wic->frame->lpVtbl->Release(wic->frame); wic->frame = NULL; }
    if (wic->enc)   { wic->enc->lpVtbl->Release(wic->enc); wic->enc = NULL; }
    if (wic->stream){ wic->stream->lpVtbl->Release(wic->stream); wic->stream = NULL; }
}

/* PNG/JPEG encoding through WIC v1 (API-3, SHOT-11). The frame is the crop
 * size and WritePixels reads the capture buffer one row at a time with
 * cbStride = cbBufferSize = cropW*3. On success the objects stay in *wic and
 * the caller releases them with ShotWICFree() after freeing the GDI objects;
 * on failure everything is released here and *wic stays empty.
 */
static int ShotWICEncode(shot_wic_t *wic, const GUID *container, const TCHAR *path
                       , const shot_capture_t *cap, float quality)
{
    IWICImagingFactory *factory = NULL;
    WICPixelFormatGUID pxfmt;
    UINT rowbytes = (UINT)cap->cropW * 3;
    HRESULT hr = E_FAIL;
    int y;

    mem00(wic, sizeof(*wic));
    if (!ShotCoCreate) return ACR_NOWIC;
    hr = ShotCoCreate(&my_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER
                    , &my_IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr) || !factory) {
        LOG("Shot: no WIC factory hr=%X", (unsigned)hr);
        return ACR_NOWIC;
    }
    wic->factory = factory;
    hr = factory->lpVtbl->CreateStream(factory, &wic->stream);
    if (FAILED(hr) || !wic->stream) goto fail;
    hr = wic->stream->lpVtbl->InitializeFromFilename(wic->stream, path, GENERIC_WRITE);
    if (FAILED(hr)) goto fail;
    hr = factory->lpVtbl->CreateEncoder(factory, container, NULL, &wic->enc);
    if (FAILED(hr) || !wic->enc) goto fail;
    /* WICBitmapEncoderNoCache: encode straight to the file */
    hr = wic->enc->lpVtbl->Initialize(wic->enc, (IStream *)wic->stream
                                    , WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto fail;
    hr = wic->enc->lpVtbl->CreateNewFrame(wic->enc, &wic->frame, &wic->props);
    if (FAILED(hr) || !wic->frame) goto fail;
    /* Only JPEG understands ImageQuality; PNG/BMP would just throw the
     * property away, so it is not written for them at all (no failed write
     * on every PNG save). A rejection for JPEG is still only logged. */
    if (wic->props && quality > 0.0f
    && ShotGuidEq(container, &my_GUID_ContainerFormatJpeg)) {
        PROPBAG2 pb;
        VARIANT var;
        mem00(&pb, sizeof(pb));
        pb.dwType = PROPBAG2_TYPE_DATA;
        pb.vt = VT_R4;
        pb.pstrName = (LPOLESTR)TEXT("ImageQuality");
        mem00(&var, sizeof(var));
        var.vt = VT_R4;
        var.fltVal = quality;
        /* A rejected option is not fatal: the encoder default is used */
        hr = wic->props->lpVtbl->Write(wic->props, 1, &pb, &var);
        if (FAILED(hr)) LOG("Shot: ImageQuality rejected hr=%X", (unsigned)hr);
    }
    hr = wic->frame->lpVtbl->Initialize(wic->frame, wic->props);
    if (FAILED(hr)) goto fail;
    hr = wic->frame->lpVtbl->SetSize(wic->frame, (UINT)cap->cropW, (UINT)cap->cropH);
    if (FAILED(hr)) goto fail;
    pxfmt = my_GUID_WICPixelFormat24bppBGR;
    hr = wic->frame->lpVtbl->SetPixelFormat(wic->frame, &pxfmt);
    if (FAILED(hr)) goto fail;
    /* The encoder may ask for another format; we can only feed 24bpp BGR */
    if (!ShotGuidEq(&pxfmt, &my_GUID_WICPixelFormat24bppBGR)) {
        LOG("Shot: encoder asked for another pixel format");
        hr = E_FAIL;
        goto fail;
    }
    for (y = 0; y < cap->cropH; y++) {
        BYTE *row = cap->bits
            + (size_t)(cap->offY + y) * (size_t)cap->stride
            + (size_t)cap->offX * 3;
        hr = wic->frame->lpVtbl->WritePixels(wic->frame, 1, rowbytes, rowbytes, row);
        if (FAILED(hr)) {
            LOG("Shot: WritePixels row=%d hr=%X", y, (unsigned)hr);
            goto fail;
        }
    }
    hr = wic->frame->lpVtbl->Commit(wic->frame);
    if (FAILED(hr)) {
        LOG("Shot: frame Commit hr=%X", (unsigned)hr);
        goto fail;
    }
    hr = wic->enc->lpVtbl->Commit(wic->enc);
    if (FAILED(hr)) {
        LOG("Shot: encoder Commit hr=%X", (unsigned)hr);
        goto fail;
    }
    /* Done with the pixels: close the output file here (SHOT-14 step 1) */
    ShotWICCloseOutput(wic);
    return ACR_OK;
fail:
    LOG("Shot: WIC encode failed hr=%X", (unsigned)hr);
    ShotWICFree(wic);
    return ACR_SAVEFAIL;
}

/* Write the image of cap into path (SHOT-11). The file is complete and closed
 * when this returns; container == NULL means BMP, which never uses WIC.
 * On success *wic holds the COM objects the caller has to release after the
 * GDI objects; for BMP it stays empty.
 */
static int ShotWriteImage(shot_wic_t *wic, const GUID *container
                        , const TCHAR *path, const shot_capture_t *cap, float quality)
{
    HGLOBAL hg;
    int code;

    mem00(wic, sizeof(*wic));
    if (container)
        return ShotWICEncode(wic, container, path, cap, quality);
    /* BMP never uses WIC nor the JPEG quality (SHOT-11) */
    hg = ShotPackDIB(cap);
    if (!hg) return ACR_NOMEM;
    code = ShotWriteBMP(path, hg);
    GlobalFree(hg);
    return code;
}

/* Extension of the final name, or NULL when there is none (SHOT-9) */
static const TCHAR *ShotExtension(const TCHAR *path)
{
    const TCHAR *dot = NULL;
    const TCHAR *p;

    if (!path) return NULL;
    for (p = path; *p; p++) {
        if (*p == TEXT('\\') || *p == TEXT('/') || *p == TEXT(':'))
            dot = NULL;
        else if (*p == TEXT('.'))
            dot = p;
    }
    return (dot && dot[1]) ? dot : NULL;
}

/* Pick the encoder from the extension, else from nFilterIndex (SHOT-9).
 * NULL means BMP.
 */
static const GUID *ShotContainerFor(const TCHAR *path, DWORD filterindex)
{
    const TCHAR *ext = ShotExtension(path);

    if (ext) {
        if (!lstrcmpi(ext, TEXT(".png")))  return &my_GUID_ContainerFormatPng;
        if (!lstrcmpi(ext, TEXT(".jpg")))  return &my_GUID_ContainerFormatJpeg;
        if (!lstrcmpi(ext, TEXT(".jpeg"))) return &my_GUID_ContainerFormatJpeg;
        if (!lstrcmpi(ext, TEXT(".bmp")))  return NULL;
    }
    /* Unknown or missing extension: the filter the user picked wins */
    if (filterindex == 2) return &my_GUID_ContainerFormatJpeg;
    if (filterindex == 3) return NULL;
    return &my_GUID_ContainerFormatPng;
}

/* Body of one screenshot task, runs on the worker thread only
 * (SHOT-5..SHOT-14). *flags receives the ACRF_* notes of the capture.
 */
static int ShotRunTask(shot_task_t *task, UINT *flags)
{
    static const TCHAR filter[] =
        TEXT("PNG image (*.png)\0*.png\0")
        TEXT("JPEG image (*.jpg; *.jpeg)\0*.jpg;*.jpeg\0")
        TEXT("Bitmap image (*.bmp)\0*.bmp\0\0");
    shot_capture_t cap;
    shot_wic_t wic;
    OPENFILENAMEW ofn;
    TCHAR temppath[MAX_PATH];
    TCHAR dirpath[MAX_PATH];
    TCHAR *path = NULL;
    const GUID *container;
    int coinit = 0;
    HRESULT hr;
    int code;

    *flags = 0;
    code = ShotCapture(task->hwnd, &cap);
    /* The protection note is reported even when the capture itself failed
     * (SHOT-7): it explains a blank or missing image. */
    *flags = cap.flags;
    if (code != ACR_OK) return code;

    if (task->toclip) {
        /* SHOT-12: pack, then commit from this very thread; the HGLOBAL
         * belongs to ClipboardCommit() from that call on (CLIP-5). */
        HGLOBAL hg = ShotPackDIB(&cap);
        ShotCaptureFree(&cap);
        if (!hg) return ACR_NOMEM;
        code = ClipboardCommit(task->seq, TRUE, task->cbseq, CF_DIB, hg);
        LOG("Shot: clipboard commit code=%d", code);
        return code;
    }

    /* SHOT-9: CoInitializeEx(STA) in this thread, before the dialog */
    if (ShotLoadOle32()) {
        hr = ShotCoInitEx(NULL, COINIT_APARTMENTTHREADED);
        coinit = SUCCEEDED(hr);
        if (!coinit) LOG("Shot: CoInitializeEx hr=%X", (unsigned)hr);
    }
    if (!ShotLoadComdlg32()) {
        ShotCaptureFree(&cap);
        if (coinit) ShotCoUninit();
        return ACR_SAVEFAIL;
    }
    path = (TCHAR *)calloc(SHOT_PATH_CHARS, sizeof(TCHAR));
    if (!path) {
        ShotCaptureFree(&cap);
        if (coinit) ShotCoUninit();
        return ACR_NOMEM;
    }
    lstrcpy_s(path, SHOT_PATH_CHARS, task->defname);
    mem00(&ofn, sizeof(ofn));
    /* The version 400 size (through lpTemplateName) is understood by every
     * system from Windows 2000 on, including the NT 5.2 floor of API-1; the
     * reserved fields of the Vista+ layout are not used here. */
    ofn.lStructSize = (UINT)OPENFILENAME_SIZE_VERSION_400W;
    ofn.hwndOwner = NULL; /* never g_hwnd or the target (SHOT-9) */
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = SHOT_PATH_CHARS;
    ofn.lpstrDefExt = TEXT("png");
    ofn.lpstrTitle = NULL; /* system localized title */
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!ShotGetSaveName(&ofn)) {
        LOG("Shot: save dialog cancelled err=%u"
          , (unsigned)(ShotCommDlgErr ? ShotCommDlgErr() : 0));
        ShotCaptureFree(&cap);
        free(path);
        if (coinit) ShotCoUninit();
        return ACR_CANCEL;
    }
    /* SHOT-9: the name is used as returned, no extension is appended here */
    container = ShotContainerFor(path, ofn.nFilterIndex);

    /* SHOT-10: unique temp file in the target directory, same volume. The
     * directory is taken from path without copying it through a MAX_PATH
     * buffer (a longer path simply cannot hold a temp file, so it fails). */
    {
        const TCHAR *p;
        size_t dirlen = 0;
        for (p = path; *p; p++) {
            if (*p == TEXT('\\') || *p == TEXT('/'))
                dirlen = (size_t)(p - path);
        }
        if (!dirlen || dirlen > SHOT_TEMP_MAXDIR) {
            LOG("Shot: unusable temp directory length %d", (int)dirlen);
            ShotCaptureFree(&cap);
            free(path);
            if (coinit) ShotCoUninit();
            return ACR_SAVEFAIL;
        }
        for (p = path; (size_t)(p - path) < dirlen; p++) dirpath[p - path] = *p;
        dirpath[dirlen] = TEXT('\0');
    }
    temppath[0] = TEXT('\0');
    if (!GetTempFileNameW(dirpath, TEXT("AltSnap"), 0, temppath)) {
        LOG("Shot: GetTempFileNameW failed err=%u", (unsigned)GetLastError());
        ShotCaptureFree(&cap);
        free(path);
        if (coinit) ShotCoUninit();
        return ACR_SAVEFAIL;
    }
    /* GetTempFileNameW leaves an empty file behind, so the encoders start from
     * length 0 and cannot overwrite or keep stale bytes of anything. */

    code = ShotWriteImage(&wic, container, temppath, &cap
                        , container ? task->quality : 0.0f);
    /* SHOT-14: the pixels are not needed any more, so the GDI objects go
     * before the COM interfaces. Releasing the WIC objects closes the file;
     * CoUninitialize comes after every COM release. */
    ShotCaptureFree(&cap);
    ShotWICFree(&wic);
    if (coinit) ShotCoUninit();
    if (code == ACR_OK) {
        if (!MoveFileExW(temppath, path, MOVEFILE_REPLACE_EXISTING)) {
            LOG("Shot: MoveFileExW failed err=%u", (unsigned)GetLastError());
            code = ACR_SAVEFAIL;
        }
    }
    if (code != ACR_OK) {
        /* Only the temporary file is removed, the target is never touched */
        DeleteFileW(temppath);
    }
    free(path);
    return code;
}

/* Body of the worker thread: own the task, report one code, free everything
 * and release the single flight flag as the very last step (SHOT-13..SHOT-16).
 */
static DWORD WINAPI ShotThreadProc(LPVOID param)
{
    shot_task_t *task = (shot_task_t *)param;
    UINT kind = task->toclip ? ACK_SHOTCLIP : ACK_SHOT;
    UINT flags = 0;
    int code;

    code = ShotRunTask(task, &flags);
    LOG("Shot: %s task done code=%d flags=%X", task->toclip ? "clipboard" : "save"
      , code, (unsigned)flags);
    free(task);
    ShotReport(kind, (UINT)code, flags);
    /* Last step of the task (SHOT-14) */
    InterlockedExchange(&g_shot_busy, 0);
    return 0;
}

/* Main thread entry, called from ActionFwdDispatch/ClipboardRequest
 * (SHOT-1..SHOT-4). It stays short: no dialog, no capture, no encoding and no
 * wait (HOOK-2).
 */
static void ScreenshotStart(HWND hwnd, int toclip)
{
    shot_task_t *task;
    HANDLE thread;
    UINT kind = toclip ? ACK_SHOTCLIP : ACK_SHOT;
    int reason = 0;

    ACT_TIMER_BEGIN(t);
    LOG("ScreenshotStart hwnd=%p toclip=%d", hwnd, toclip);
    /* SHOT-1: quick pre checks; a minimized or cloaked target is a clear
     * failure and is never restored (UX-2). */
    if (!hwnd || !IsWindow(hwnd)) reason = ACR_NOTARGET;
    else if (IsIconic(hwnd)) reason = ACR_CAPFAIL;
    else if (IsWindowCloaked(hwnd)) reason = ACR_CAPFAIL;
    else if (IsHungAppWindow(hwnd)) reason = ACR_CAPFAIL;
    if (reason) {
        LOG("ScreenshotStart: pre check failed code=%d", reason);
        ShotReport(kind, (UINT)reason, 0);
        ACT_TIMER_END(t, "ScreenshotStart");
        return;
    }
    /* SHOT-2: one screenshot or Clipboard_1 at a time */
    if (InterlockedCompareExchange(&g_shot_busy, 1, 0) != 0) {
        LOG("ScreenshotStart: busy");
        ShotReport(kind, ACR_BUSY, 0);
        ACT_TIMER_END(t, "ScreenshotStart");
        return;
    }
    /* SHOT-3: copy every parameter, the worker never reads l10n or config */
    task = (shot_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        InterlockedExchange(&g_shot_busy, 0);
        ShotReport(kind, ACR_NOMEM, 0);
        ACT_TIMER_END(t, "ScreenshotStart");
        return;
    }
    task->hwnd = hwnd;
    task->toclip = !!toclip;
    task->quality = SHOT_JPEG_QUALITY;
    ShotDefaultName(task->defname, ARR_SZ(task->defname));
    if (task->toclip) {
        /* RES-4: the request sequence is taken here, then the clipboard
         * sequence number, both copied into the task (NIT1, clipbrd.c). */
        task->seq = ClipboardTakeSeq(&task->cbseq);
    }
    /* SHOT-4: default stack, no reuse of the DLL WorkerThread */
    thread = CreateThread(NULL, 0, ShotThreadProc, task, 0, NULL);
    if (!thread) {
        LOG("ScreenshotStart: CreateThread failed err=%u", (unsigned)GetLastError());
        /* Same source as the sequence taken above, and read before the task
         * is released (free() also clears the pointer) */
        if (task->toclip) ClipboardDropSeq();
        free(task);
        InterlockedExchange(&g_shot_busy, 0);
        ShotReport(kind, ACR_FAIL, 0);
        ACT_TIMER_END(t, "ScreenshotStart");
        return;
    }
    /* The task belongs to the thread from now on (SHOT-4/SHOT-16) */
    CloseHandle(thread);
    ACT_TIMER_END(t, "ScreenshotStart");
}
