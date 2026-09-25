/* ============================================================
   便携快速启动器 —— 原生 Windows 启动器（单文件 exe，无依赖）
   v1.1  新增：＋添加工具 / 编辑 / 删除 / 拖入文件添加 / 空白处双击不再误启动
   编译: i686-w64-mingw32-gcc -municode -mwindows -O2 -s -static \
            便携快速启动器.c app.rc -o 便携快速启动器.exe -lcomctl32 -lshlwapi -lshell32 -luser32 -lgdi32 -lcomdlg32
   配置: 与 exe 同目录的 tools_utf8.txt（UTF-8），行格式  面板|名称|相对路径|参数
         首行可写 #TITLE=标题；路径相对 exe 所在目录（工具箱根目录）
         界面里添加/编辑的工具会直接写回该文件（首次写前自动备份 .bak）
   ============================================================ */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0600
#define _WIN32_IE 0x0600

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define APP_NAME    L"便携快速启动器"
#define APP_VER     L"1.3"
#define MAX_ITEMS   6000
#define MAX_PANELS  128

#define IDC_SEARCH  1001
#define IDC_PANEL   1002
#define IDC_LIST    1003
#define IDC_STATUS  1004
#define IDC_ADDBTN  1005
#define IDM_RUN     2001
#define IDM_RUNAS   2002
#define IDM_FOLDER  2003
#define IDM_COPY    2004
#define IDM_RELOAD  2005
#define IDM_EXIT    2006
#define IDM_ABOUT   2007
#define IDM_README  2008
#define IDM_OPENDIR 2009
#define IDM_ADD     2010
#define IDM_EDIT    2011
#define IDM_DEL     2012
#define IDM_NEW     2020
#define IDM_IMPORT  2021
#define IDM_EXPORT  2022
#define IDM_SCAN    2023
#define IDM_VIEW_LIST 2100
#define IDM_VIEW_ICON 2101
#define WM_APP_LOAD      (WM_APP + 10)
#define WM_APP_STEP      (WM_APP + 11)   /* 后台小步：先查文件、再填图标，然后强制重绘 */
#define ICON_CHUNK 16
#define CHECK_CHUNK 24
#define PX16 (16 * 16 * 4)
#define PX32 (32 * 32 * 4)

/* 添加/编辑 对话框控件 */
#define IDC_DLG_L1    3101
#define IDC_DLG_L2    3102
#define IDC_DLG_L3    3103
#define IDC_DLG_L4    3104
#define IDC_DLG_HINT  3105
#define IDC_DLG_NAME  3001
#define IDC_DLG_PANEL 3002
#define IDC_DLG_PATH  3003
#define IDC_DLG_ARGS  3004
#define IDC_DLG_BROWSE 3005
#define IDC_DLG_OK    3006
#define IDC_DLG_CANCEL 3007

typedef struct {
    WCHAR panel[64];
    WCHAR title[256];
    WCHAR rel[1024];
    WCHAR args[512];
    WCHAR full[2048];
    int   missing;
} ITEM;

static ITEM  g_items[MAX_ITEMS];
static int   g_count = 0;
static int   g_view[MAX_ITEMS];
static int   g_views = 0;
static int   g_panelSel = 0;                 /* 0 = 全部 */
static WCHAR g_panels[MAX_PANELS][64];
static int   g_panelN = 0;
static WCHAR g_root[MAX_PATH] = L"";
static WCHAR g_appTitle[128] = APP_NAME;
static int   g_viewPref = 0;          /* 配置里 #VIEW=icon 时用大图标视图 */

/* 配置文件位置与原始注释头（保存时原样回写，便于手工编辑） */
static WCHAR g_cfgPath[MAX_PATH * 2] = L"";
static UINT  g_cfgCP = CP_UTF8;
static WCHAR *g_header = NULL;
static size_t g_headerCap = 0, g_headerLen = 0;

static HWND g_hMain, g_hSearch, g_hPanel, g_hList, g_hStatus, g_hAdd;
static HFONT g_hFont = NULL;
static HBRUSH g_hBg = NULL;
static WNDPROC g_oldEdit, g_oldList;

/* ---- 图标 ---- */
static HIMAGELIST g_himlSmall = NULL, g_himlLarge = NULL;
static int  g_iconPos = 0;          /* 异步填充进度 */
static int  g_viewMode = 0;         /* 0=列表 1=图标 */
typedef struct { WCHAR key[80]; int idx; BYTE *p16; BYTE *p32; } ICC;
static ICC  g_icc[1024];
static int  g_iccN = 0;
static int  g_noCache = 0;         /* 配置里写 #NOCACHE=1 可关掉磁盘图标缓存 */
static int  g_cacheLoaded = 0, g_cacheSaved = 0, g_cacheDirty = 0;
static int  g_cacheHit = 0, g_cacheMiss = 0;
static int  g_checkPos = 0, g_checking = 0;     /* 文件是否存在的分块检查进度 */
static const char CACHE_SIG[12] = { 'Q','L','I','C','O','N','C','A','C','H','E','1' };

/* ---------- 工具函数 ---------- */
static void join_path(WCHAR *out, size_t cap, const WCHAR *dir, const WCHAR *rel)
{
    WCHAR tmp[2048];
    if (rel[0] && rel[1] == L':' && (rel[2] == L'\\' || rel[2] == L'/')) {
        wcsncpy(out, rel, cap - 1); out[cap - 1] = 0; return;
    }
    if (rel[0] == L'\\' && rel[1] == L'\\') {                 /* UNC */
        wcsncpy(out, rel, cap - 1); out[cap - 1] = 0; return;
    }
    wcsncpy(tmp, rel, 2047); tmp[2047] = 0;
    for (WCHAR *p = tmp; *p; p++) if (*p == L'/') *p = L'\\';
    size_t n = wcslen(dir);
    if (n && dir[n - 1] == L'\\') _snwprintf(out, cap - 1, L"%s%s", dir, tmp);
    else                          _snwprintf(out, cap - 1, L"%s\\%s", dir, tmp);
    out[cap - 1] = 0;
}

static WCHAR *read_text_file(const WCHAR *path, UINT codepage)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 32u * 1024 * 1024) { CloseHandle(h); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD got = 0;
    ReadFile(h, buf, sz, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    DWORD off = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) off = 3;
    int need = MultiByteToWideChar(codepage, 0, buf + off, got - off, NULL, 0);
    WCHAR *w = (WCHAR *)malloc((need + 2) * sizeof(WCHAR));
    if (w) { MultiByteToWideChar(codepage, 0, buf + off, got - off, w, need); w[need] = 0; }
    free(buf);
    return w;
}

static void copy_field(WCHAR *dst, size_t cap, const WCHAR *src, size_t len)
{
    /* 去掉首尾空白与残留的 \r（兼容 CRLF 配置） */
    while (len > 0 && (*src == L' ' || *src == L'\t' || *src == L'\r')) { src++; len--; }
    while (len > 0 && (src[len - 1] == L' ' || src[len - 1] == L'\t' || src[len - 1] == L'\r')) len--;
    if (len >= cap) len = cap - 1;
    wcsncpy(dst, src, len); dst[len] = 0;
}

/* 把绝对路径尽量转成相对工具箱根目录的相对路径 */
static void to_rel(const WCHAR *full, WCHAR *out, size_t cap)
{
    size_t rl = wcslen(g_root);
    if (rl && !_wcsnicmp(full, g_root, rl) && (full[rl] == L'\\' || full[rl] == L'/')) {
        wcsncpy(out, full + rl + 1, cap - 1);
    } else {
        wcsncpy(out, full, cap - 1);
    }
    out[cap - 1] = 0;
}

/* 取文件名（不含路径） */
static const WCHAR *base_name(const WCHAR *p)
{
    const WCHAR *s = p, *q;
    for (q = p; *q; q++) if (*q == L'\\' || *q == L'/') s = q + 1;
    return s;
}

/* 文件名去扩展名，作为默认工具名 */
static void stem_name(const WCHAR *fullpath, WCHAR *out, size_t cap)
{
    const WCHAR *b = base_name(fullpath);
    const WCHAR *dot = wcsrchr(b, L'.');
    size_t n = dot ? (size_t)(dot - b) : wcslen(b);
    if (n == 0) { wcsncpy(out, b, cap - 1); out[cap - 1] = 0; return; }
    if (n >= cap) n = cap - 1;
    wcsncpy(out, b, n); out[n] = 0;
}

/* 字段里不能出现 | 或换行，否则配置文件行结构会被破坏 */
static void sanitize_field(WCHAR *s)
{
    for (; *s; s++)
        if (*s == L'|' || *s == L'\r' || *s == L'\n' || *s == L'\t') *s = L' ';
}

/* ---------- 图标（shell 取图标 + 缓存 + 异步分块填充） ---------- */
static int icc_get(const WCHAR *k)
{
    for (int i = 0; i < g_iccN; i++) if (!_wcsicmp(g_icc[i].key, k)) return g_icc[i].idx;
    return -1;
}
static void add_blank(HIMAGELIST h, int size);   /* 前向声明：缓存建图标时用来补齐索引 */
static void icc_put(const WCHAR *k, int idx, BYTE *p16, BYTE *p32)
{
    if (g_iccN >= 1024) {
        free(p16); free(p32);
        return;
    }
    copy_field(g_icc[g_iccN].key, 80, k, wcslen(k));
    g_icc[g_iccN].idx = idx;
    g_icc[g_iccN].p16 = p16;
    g_icc[g_iccN].p32 = p32;
    g_iccN++;
}

/* ---------- 图标 <-> 像素（磁盘缓存用） ---------- */
/* 取图标的 32 位 BGRA 像素（自上而下）。
   · 有 alpha 通道的图标直接用；
   · 老式图标没有 alpha：用 AND 掩码补（掩码位=1 → 透明），否则重建后背景会变黑。
   返回的 buffer 交给调用方 free()。 */
static BYTE *icon_to_pixels(HICON ic, int size, int *valid)
{
    ICONINFO ii;
    BITMAPINFO bi;
    HDC dc;
    BYTE *buf = NULL, *msk = NULL;
    int i, anyA = 0;
    *valid = 0;
    ZeroMemory(&ii, sizeof(ii));
    if (!GetIconInfo(ic, &ii)) return NULL;
    dc = CreateCompatibleDC(NULL);
    if (!dc) { if (ii.hbmColor) DeleteObject(ii.hbmColor); if (ii.hbmMask) DeleteObject(ii.hbmMask); return NULL; }
    buf = (BYTE *)malloc((size_t)size * size * 4);
    if (buf) {
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;           /* 自上而下 */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (!GetDIBits(dc, ii.hbmColor, 0, size, buf, &bi, DIB_RGB_COLORS)) { free(buf); buf = NULL; }
    }
    if (buf) {
        int stride = ((size + 31) / 32) * 4;
        msk = (BYTE *)malloc((size_t)stride * size);
        if (msk) {
            ZeroMemory(&bi, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = size;
            bi.bmiHeader.biHeight = size;        /* 1bpp 掩码按自下而上存放 */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 1;
            bi.bmiHeader.biCompression = BI_RGB;
            if (!GetDIBits(dc, ii.hbmMask, 0, size, msk, &bi, DIB_RGB_COLORS)) { free(msk); msk = NULL; }
        }
        for (i = 3; i < size * size * 4; i += 4) if (buf[i]) { anyA = 1; break; }
        if (!anyA) {                              /* 无 alpha → 用掩码合成 */
            for (i = 0; i < size * size; i++) {
                int y = i / size, x = i % size;   /* y: 自上而下 */
                int brow = msk ? (size - 1 - y) : -1;
                int transparent = 0;
                if (brow >= 0) {
                    const BYTE *row = msk + (size_t)brow * stride;
                    if (row[x >> 3] & (0x80 >> (x & 7))) transparent = 1;
                }
                buf[i * 4 + 3] = transparent ? 0 : 255;
            }
        }
        *valid = 1;
    }
    free(msk);
    DeleteDC(dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return buf;
}
static HICON pixels_to_icon(const BYTE *px, int size)
{
    BITMAPINFO bi;
    HDC dc;
    HBITMAP hbm, hmsk;
    ICONINFO ii;
    HICON ic = NULL;
    void *bits = NULL;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    if (!dc) return NULL;
    hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    DeleteDC(dc);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); return NULL; }
    memcpy(bits, px, (size_t)size * size * 4);
    hmsk = CreateBitmap(size, size, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = hbm;
    ii.hbmMask = hmsk;
    ic = CreateIconIndirect(&ii);
    DeleteObject(hbm);
    if (hmsk) DeleteObject(hmsk);
    return ic;
}
/* 从缓存像素建图标并进 ImageList（与 icon_add_both 的索引配对规则一致） */
static void icc_add_cached(const WCHAR *key, BYTE *p16, BYTE *p32)
{
    HICON hs = pixels_to_icon(p16, 16), hl = pixels_to_icon(p32, 32);
    int idx = -1;
    if (hs) { idx = ImageList_AddIcon(g_himlSmall, hs); DestroyIcon(hs); }
    if (hl) {
        if (idx >= 0) { int pad = 0; while (ImageList_GetImageCount(g_himlLarge) < idx && pad++ < 64) add_blank(g_himlLarge, 32); }
        if (ImageList_AddIcon(g_himlLarge, hl) < 0) add_blank(g_himlLarge, 32);
        DestroyIcon(hl);
    }
    if (idx >= 0) { icc_put(key, idx, p16, p32); return; }
    free(p16); free(p32);
}
static void add_blank(HIMAGELIST h, int size)
{
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateCompatibleBitmap(dc, size, size), old;
    RECT r; HBRUSH br;
    r.left = 0; r.top = 0; r.right = size; r.bottom = size;
    old = (HBITMAP)SelectObject(dc, bm);
    br = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &r, br);
    DeleteObject(br);
    SelectObject(dc, old);
    DeleteDC(dc);
    ImageList_Add(h, bm, NULL);
    DeleteObject(bm);
}
static int icon_add_both(const WCHAR *path, DWORD attr, int useAttr, const WCHAR *key)
{
    SHFILEINFOW sfi;
    HICON hs = NULL, hl = NULL;
    BYTE *px16 = NULL, *px32 = NULL;
    int v16 = 0, v32 = 0, idxLarge = -1;
    DWORD fl = SHGFI_ICON | (useAttr ? SHGFI_USEFILEATTRIBUTES : 0);
    int idx = -1;
    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW(path, attr, &sfi, sizeof(sfi), fl | SHGFI_SMALLICON) && sfi.hIcon) hs = sfi.hIcon;
    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW(path, attr, &sfi, sizeof(sfi), fl | SHGFI_LARGEICON) && sfi.hIcon) hl = sfi.hIcon;
    if (hs) {
        idx = ImageList_AddIcon(g_himlSmall, hs);
        DestroyIcon(hs);
    }
    if (hl) {
        if (idx >= 0) { int pad = 0; while (ImageList_GetImageCount(g_himlLarge) < idx && pad++ < 64) add_blank(g_himlLarge, 32); }
        idxLarge = ImageList_AddIcon(g_himlLarge, hl);
        if (idxLarge < 0) { add_blank(g_himlLarge, 32); idxLarge = ImageList_GetImageCount(g_himlLarge) - 1; }
        DestroyIcon(hl);
    }
    if (idx >= 0) {
        /* 像素从 ImageList 里的副本取（不碰 shell 给的 HICON，避免影响图标本身）；
           只缓存"按整个文件取"的真实图标——按扩展名的通用图标重取很便宜，不值得冒险 */
        if (!useAttr && idxLarge == idx) {
            HICON h16 = ImageList_GetIcon(g_himlSmall, idx, ILD_NORMAL);
            HICON h32 = ImageList_GetIcon(g_himlLarge, idx, ILD_NORMAL);
            if (h16) { px16 = icon_to_pixels(h16, 16, &v16); DestroyIcon(h16); }
            if (h32) { px32 = icon_to_pixels(h32, 32, &v32); DestroyIcon(h32); }
        }
        {
            int ok = (v16 && v32 && px16 && px32);
            icc_put(key, idx, ok ? px16 : NULL, ok ? px32 : NULL);
            if (ok) g_cacheDirty = 1; else { free(px16); free(px32); }
        }
    } else { free(px16); free(px32); }
    return idx;
}
static int icon_cached(const WCHAR *path, DWORD attr, int useAttr, const WCHAR *key)
{
    int idx = icc_get(key);
    if (idx >= 0) return idx;
    idx = icon_add_both(path, attr, useAttr, key);
    return idx < 0 ? 0 : idx;
}
/* ---------- 磁盘图标缓存（icon_cache.dat，放在 exe 同目录） ---------- */
static void cache_path(WCHAR *out, size_t cap)
{
    join_path(out, cap, g_root, L"icon_cache.dat");
}
static void cache_load(void)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD sz, got, n = 0, i;
    BYTE *buf, *p, *end;
    if (g_noCache || g_cacheLoaded) return;
    g_cacheLoaded = 1;
    cache_path(path, MAX_PATH * 2);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz < 16 || sz > 64u * 1024 * 1024) { CloseHandle(h); return; }
    buf = (BYTE *)malloc(sz);
    if (!buf) { CloseHandle(h); return; }
    if (!ReadFile(h, buf, sz, &got, NULL)) { free(buf); CloseHandle(h); return; }
    CloseHandle(h);
    p = buf; end = buf + got;
    if (memcmp(p, CACHE_SIG, 12)) { free(buf); return; }
    p += 12;
    memcpy(&n, p, 4); p += 4;
    for (i = 0; i < n; i++) {
        WORD kl = 0;
        WCHAR key[80];
        BYTE *p16, *p32;
        if (p + 2 > end) break;
        memcpy(&kl, p, 2); p += 2;
        if (kl == 0 || kl > 79 || p + (size_t)kl * 2 + PX16 + PX32 > end) break;
        memcpy(key, p, kl * 2); key[kl] = 0; p += (size_t)kl * 2;
        if (icc_get(key) >= 0) { p += PX16 + PX32; continue; }
        p16 = (BYTE *)malloc(PX16);
        p32 = (BYTE *)malloc(PX32);
        if (!p16 || !p32) { free(p16); free(p32); break; }
        memcpy(p16, p, PX16); p += PX16;
        memcpy(p32, p, PX32); p += PX32;
        icc_add_cached(key, p16, p32);
    }
    free(buf);
}
static void cache_save(void)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD wr, n = 0;
    int i;
    if (g_noCache || g_cacheSaved) return;
    g_cacheSaved = 1;
    if (!g_cacheDirty || !g_himlSmall || !g_iccN) return;
    cache_path(path, MAX_PATH * 2);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, CACHE_SIG, 12, &wr, NULL);
    WriteFile(h, &n, 4, &wr, NULL);                 /* 先占位，最后回填条数 */
    for (i = 0; i < g_iccN; i++) {
        WORD kl;
        if (!g_icc[i].p16 || !g_icc[i].p32) continue;
        kl = (WORD)wcslen(g_icc[i].key);
        if (!kl || kl > 79) continue;
        WriteFile(h, &kl, 2, &wr, NULL);
        WriteFile(h, g_icc[i].key, kl * 2, &wr, NULL);
        WriteFile(h, g_icc[i].p16, PX16, &wr, NULL);
        WriteFile(h, g_icc[i].p32, PX32, &wr, NULL);
        n++;
    }
    SetFilePointer(h, 12, NULL, FILE_BEGIN);
    WriteFile(h, &n, 4, &wr, NULL);
    CloseHandle(h);
}

/* 计算某条目图标的缓存键（不做提取，很快） */
static void icon_key_for(int i, WCHAR *key)
{
    ITEM *it = &g_items[i];
    const WCHAR *dot;
    DWORD attr = GetFileAttributesW(it->full);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dot = wcsrchr(it->rel, L'.');
        if (!dot || wcslen(dot) > 7) dot = L".file";
        _snwprintf(key, 79, L"E:%s", dot);
    } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        wcsncpy(key, L"D:dir", 79);
    } else {
        dot = wcsrchr(it->full, L'.');
        if (dot && (!_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".lnk") || !_wcsicmp(dot, L".ico") ||
                    !_wcsicmp(dot, L".msi") || !_wcsicmp(dot, L".scr") || !_wcsicmp(dot, L".com"))) {
            _snwprintf(key, 79, L"F:%s", it->full);
        } else {
            if (!dot || wcslen(dot) > 7) dot = L".file";
            _snwprintf(key, 79, L"E:%s", dot);
        }
    }
    key[79] = 0;
}
/* 取图标索引（缓存没有才真正提取；提取较慢，只在异步填充里调用） */
static int icon_for_item(int i)
{
    WCHAR key[80];
    int idx;
    icon_key_for(i, key);
    idx = icc_get(key);
    if (idx >= 0) return idx;
    if (!_wcsnicmp(key, L"F:", 2)) return icon_cached(g_items[i].full, 0, 0, key);
    if (!_wcsnicmp(key, L"D:", 2)) return icon_cached(L"dir", FILE_ATTRIBUTE_DIRECTORY, 1, key);
    return icon_cached(key + 2, FILE_ATTRIBUTE_NORMAL, 1, key);
}
static void init_image_lists(void)
{
    g_himlSmall = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 32, 128);
    g_himlLarge = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 32, 128);
    icon_add_both(L"file", FILE_ATTRIBUTE_NORMAL, 1, L"E:.file");   /* 索引 0 = 通用占位图标 */
}
/* 首屏先把缓存里已有的图标画上，其余交给异步填充 */
static int icon_index_fast(int i)
{
    WCHAR key[80];
    icon_key_for(i, key);
    int idx = icc_get(key);
    return idx < 0 ? 0 : idx;
}
static void fill_icons_chunk(HWND hwnd)
{
    int done = 0;
    while (g_iconPos < g_views && done < ICON_CHUNK) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = g_iconPos;
        if (!SendMessageW(g_hList, LVM_GETITEMW, 0, (LPARAM)&it)) break;
        int ii = (int)it.lParam;
        if (ii >= 0 && ii < g_count) {
            it.mask = LVIF_IMAGE;
            it.iImage = icon_for_item(ii);
            SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&it);
        }
        g_iconPos++; done++;
    }
}

/* ---------- 面板统计 ---------- */
static void build_panels(void)
{
    g_panelN = 0;
    for (int i = 0; i < g_count; i++) {
        int found = -1;
        for (int j = 0; j < g_panelN; j++)
            if (!wcscmp(g_panels[j], g_items[i].panel)) { found = j; break; }
        if (found < 0 && g_panelN < MAX_PANELS) {
            found = g_panelN++;
            copy_field(g_panels[found], 64, g_items[i].panel, wcslen(g_items[i].panel));
        }
    }
}

/* ---------- 载入配置 ---------- */
static void header_reset(void)
{
    if (!g_header) {
        g_headerCap = 65536;
        g_header = (WCHAR *)malloc(g_headerCap * sizeof(WCHAR));
    }
    if (g_header) { g_header[0] = 0; g_headerLen = 0; }
}
static void header_add(const WCHAR *line)
{
    size_t n;
    if (!g_header || !line) return;
    n = wcslen(line);
    if (g_headerLen + n + 3 >= g_headerCap) return;
    wmemcpy(g_header + g_headerLen, line, n);
    g_headerLen += n;
    g_header[g_headerLen++] = L'\r';
    g_header[g_headerLen++] = L'\n';
    g_header[g_headerLen] = 0;
}

static void load_config(void)
{
    WCHAR path[MAX_PATH * 2];
    WCHAR *text = NULL;
    g_count = 0;
    g_cfgPath[0] = 0;
    g_cfgCP = CP_UTF8;
    g_noCache = 0;
    g_checkPos = 0;
    g_checking = 0;
    header_reset();

    join_path(path, MAX_PATH * 2, g_root, L"tools_utf8.txt");
    text = read_text_file(path, CP_UTF8);
    if (text) { wcsncpy(g_cfgPath, path, MAX_PATH * 2 - 1); g_cfgPath[MAX_PATH * 2 - 1] = 0; g_cfgCP = CP_UTF8; }
    if (!text) {
        join_path(path, MAX_PATH * 2, g_root, L"tools_gbk.txt");
        text = read_text_file(path, CP_ACP);
        if (text) { wcsncpy(g_cfgPath, path, MAX_PATH * 2 - 1); g_cfgPath[MAX_PATH * 2 - 1] = 0; g_cfgCP = CP_ACP; }
    }
    if (!text) {
        /* 没有配置文件也不终止：让用户可以直接用界面添加工具，保存时自动创建 */
        join_path(g_cfgPath, MAX_PATH * 2, g_root, L"tools_utf8.txt");
        g_cfgCP = CP_UTF8;
        build_panels();
        return;
    }

    WCHAR *p = text;
    while (*p && g_count < MAX_ITEMS) {
        WCHAR *line = p;
        while (*p && *p != L'\n') p++;
        if (*p == L'\n') { *p = 0; p++; }
        while (*line == L'\r' || *line == L' ' || *line == L'\t') line++;
        if (!*line) continue;
        if (line[0] == L'#' || (line[0] == L'/' && line[1] == L'/')) {
            if (!_wcsnicmp(line, L"#TITLE=", 7)) {
                WCHAR *v = line + 7;
                while (*v == L' ' || *v == L'\t') v++;
                copy_field(g_appTitle, 128, v, wcslen(v));
            } else if (!_wcsnicmp(line, L"#NOCACHE=", 9)) {
                WCHAR *v = line + 9;
                while (*v == L' ' || *v == L'\t') v++;
                g_noCache = (*v == L'1' || *v == L'y' || *v == L'Y' || *v == L'是') ? 1 : 0;
            } else if (!_wcsnicmp(line, L"#VIEW=", 6)) {
                WCHAR *v = line + 6;
                while (*v == L' ' || *v == L'\t') v++;
                g_viewPref = (StrStrIW(v, L"icon") || StrStrIW(v, L"图标")) ? 1 : 0;
            } else if (_wcsnicmp(line, L"# 本文件由", 5) && _wcsnicmp(line, L"# 路径可写相对本程序目录", 12)) {
                size_t ln = wcslen(line);  /* 去掉行尾 \r 再存，避免回写时多出一个 ^M */
                while (ln && (line[ln - 1] == L'\r' || line[ln - 1] == L' ' || line[ln - 1] == L'\t')) line[--ln] = 0;
                header_add(line);          /* 其余注释行原样保留（自家那两行固定说明每次重新生成） */
            }
            continue;
        }
        WCHAR *f[4] = { line, NULL, NULL, NULL };
        int nf = 1;
        for (WCHAR *q = line; *q && nf < 4; q++) {
            if (*q == L'|' || *q == L'\t') { *q = 0; f[nf++] = q + 1; }
        }
        while (nf < 4) f[nf++] = L"";
        if (!*f[0] || !*f[1] || !*f[2]) continue;
        ITEM *it = &g_items[g_count];
        copy_field(it->panel, 64, f[0], wcslen(f[0]));
        copy_field(it->title, 256, f[1], wcslen(f[1]));
        copy_field(it->rel,   1024, f[2], wcslen(f[2]));
        copy_field(it->args,  512,  f[3], wcslen(f[3]));
        join_path(it->full, 2048, g_root, it->rel);
        it->missing = 0;     /* 文件是否缺失改到窗口显示之后分块检查，不再卡首屏 */
        g_count++;
    }
    free(text);

    build_panels();
}

/* ---------- 保存配置（写回原文件，首次写前备份 .bak） ---------- */
static int write_conv(HANDLE h, const WCHAR *s)
{
    char *buf;
    int need, len;
    DWORD wr;
    need = WideCharToMultiByte(g_cfgCP, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return 1;
    buf = (char *)malloc(need);
    if (!buf) return 0;
    len = WideCharToMultiByte(g_cfgCP, 0, s, -1, buf, need, NULL, NULL);
    if (len > 0) WriteFile(h, buf, len - 1, &wr, NULL);
    free(buf);
    return 1;
}
static int write_crlf(HANDLE h)
{
    DWORD wr;
    WriteFile(h, "\r\n", 2, &wr, NULL);
    return 1;
}
/* 把条目写进一个已打开的文件；onlyVisible=1 时只写当前列表里显示的那些 */
static void write_items(HANDLE h, int onlyVisible)
{
    WCHAR line[4096];
    int idx, n = onlyVisible ? g_views : g_count;
    for (idx = 0; idx < n; idx++) {
        ITEM *it = &g_items[onlyVisible ? g_view[idx] : idx];
        WCHAR panel[64], title[256], rel[1024], args[512];
        wcsncpy(panel, it->panel, 63); panel[63] = 0;
        wcsncpy(title, it->title, 255); title[255] = 0;
        wcsncpy(rel,   it->rel,  1023); rel[1023] = 0;
        wcsncpy(args,  it->args,  511); args[511] = 0;
        sanitize_field(panel); sanitize_field(title); sanitize_field(rel); sanitize_field(args);
        _snwprintf(line, 4095, L"%s|%s|%s|%s", panel, title, rel, args);
        line[4095] = 0;
        write_conv(h, line); write_crlf(h);
    }
}

/* 写配置；path/cp 可指定（导出用），onlyVisible 只写当前显示 */
static int save_config_ex(const WCHAR *path, UINT cp, int onlyVisible, int backup)
{
    WCHAR bak[MAX_PATH * 2 + 8];
    WCHAR line[4096];
    HANDLE h;
    DWORD wr;
    UINT oldCP = g_cfgCP;

    if (!path || !*path) return 0;
    if (backup) {
        /* 只备份一次（保留最初版本，防止误改无法回退） */
        wcsncpy(bak, path, MAX_PATH * 2 - 1); bak[MAX_PATH * 2 - 1] = 0;
        wcsncat(bak, L".bak", 8);
        if (GetFileAttributesW(bak) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(path, bak, TRUE);
    }
    g_cfgCP = cp;                       /* write_conv 按 g_cfgCP 转码 */
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_cfgCP = oldCP; return 0; }
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (g_cfgCP == CP_UTF8) {
        const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
        WriteFile(h, bom, 3, &wr, NULL);
    }
    _snwprintf(line, 4095, L"# 本文件由「%s」界面自动维护；每行格式：面板|名称|相对路径|参数", APP_NAME);
    line[4095] = 0;
    write_conv(h, line); write_crlf(h);
    _snwprintf(line, 4095, L"# 路径可写相对本程序目录的相对路径（如 工具\\xxx.exe）；工具箱目录外的文件会记为绝对路径");
    line[4095] = 0;
    write_conv(h, line); write_crlf(h);
    _snwprintf(line, 4095, L"#TITLE=%s", g_appTitle);
    line[4095] = 0;
    write_conv(h, line); write_crlf(h);
    if (g_viewPref) { write_conv(h, L"#VIEW=icon"); write_crlf(h); }
    if (g_header && g_headerLen) write_conv(h, g_header);
    write_items(h, onlyVisible);
    FlushFileBuffers(h);
    CloseHandle(h);
    g_cfgCP = oldCP;
    return 1;
}
static int save_config(void)
{
    return save_config_ex(g_cfgPath, g_cfgCP, 0, 1);
}

/* ---------- 过滤 + 刷新列表 ---------- */
static int item_match(int i, const WCHAR *kw)
{
    if (g_panelSel > 0 && wcscmp(g_items[i].panel, g_panels[g_panelSel - 1])) return 0;
    if (kw && *kw) {
        if (!StrStrIW(g_items[i].title, kw) && !StrStrIW(g_items[i].rel, kw) &&
            !StrStrIW(g_items[i].panel, kw)) return 0;
    }
    return 1;
}

static void set_status(void)
{
    int miss = 0;
    for (int i = 0; i < g_views; i++) if (g_items[g_view[i]].missing) miss++;
    WCHAR s[360];
    if (g_checking)
        _snwprintf(s, 359, L"显示 %d / 共 %d 项   |   正在后台检查文件是否存在…（%d / %d）",
                   g_views, g_count, g_checkPos, g_count);
    else
        _snwprintf(s, 359, L"显示 %d / 共 %d 项   缺失 %d 项   |   双击或回车启动；右键更多操作；「＋添加工具」或把文件拖进窗口即可新增",
                   g_views, g_count, miss);
    s[359] = 0;
    SetWindowTextW(g_hStatus, s);
}

/* 单行文本刷新（后台检查出结果后即时变红） */
static void update_row_text(int row)
{
    WCHAR nm[300];
    int i;
    LVITEMW lv;
    if (row < 0 || row >= g_views) return;
    i = g_view[row];
    if (g_items[i].missing) _snwprintf(nm, 299, L"%s   [文件缺失]", g_items[i].title);
    else                    _snwprintf(nm, 299, L"%s", g_items[i].title);
    nm[299] = 0;
    ZeroMemory(&lv, sizeof(lv));
    lv.mask = LVIF_TEXT;
    lv.iItem = row;
    lv.iSubItem = 0;
    lv.pszText = nm;
    SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&lv);
}

/* 分块检查文件是否存在：每帧只查 CHECK_CHUNK 条，窗口不会卡 */
static void check_files_chunk(HWND hwnd)
{
    int done = 0;
    if (!g_checking) { g_checking = 1; }
    while (g_checkPos < g_count && done < CHECK_CHUNK) {
        ITEM *it = &g_items[g_checkPos];
        it->missing = (GetFileAttributesW(it->full) == INVALID_FILE_ATTRIBUTES);
        for (int r = 0; r < g_views; r++)
            if (g_view[r] == g_checkPos) { update_row_text(r); break; }
        g_checkPos++;
        done++;
    }
    if (g_checkPos >= g_count) { g_checking = 0; }
}
/* CLI 模式用：一次性查完 */
static void check_all_files(void)
{
    for (int i = 0; i < g_count; i++)
        g_items[i].missing = (GetFileAttributesW(g_items[i].full) == INVALID_FILE_ATTRIBUTES);
    g_checkPos = g_count;
    g_checking = 0;
}

static void refresh_list(void)
{
    WCHAR kw[256];
    GetWindowTextW(g_hSearch, kw, 255);
    ListView_DeleteAllItems(g_hList);
    g_views = 0;
    for (int i = 0; i < g_count; i++) {
        if (!item_match(i, kw)) continue;
        int row = g_views;
        LVITEMW lv;
        ZeroMemory(&lv, sizeof(lv));
        lv.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        lv.iItem = row;
        lv.iSubItem = 0;
        lv.iImage = icon_index_fast(i);
        WCHAR nm[300];
        if (g_items[i].missing) _snwprintf(nm, 299, L"%s   [文件缺失]", g_items[i].title);
        else                    _snwprintf(nm, 299, L"%s", g_items[i].title);
        nm[299] = 0;
        lv.pszText = nm;
        lv.lParam = (LPARAM)i;
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&lv);
        LVITEMW sub;
        ZeroMemory(&sub, sizeof(sub));
        sub.mask = LVIF_TEXT;
        sub.iItem = row; sub.iSubItem = 1; sub.pszText = g_items[i].rel;
        SendMessageW(g_hList, LVM_SETITEMW, 0, (LPARAM)&sub);
        g_view[g_views++] = i;
    }
    set_status();
    g_iconPos = 0;
    if (g_himlSmall) PostMessageW(g_hMain, WM_APP_STEP, 0, 0);
}

static int selected_item(void)
{
    int row = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (row < 0 || row >= g_views) return -1;
    return g_view[row];
}

/* 选中某个相对路径对应的条目（添加/编辑后定位用） */
static void select_by_rel(const WCHAR *rel)
{
    for (int i = 0; i < g_views; i++) {
        if (!_wcsicmp(g_items[g_view[i]].rel, rel)) {
            LVITEMW it;
            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_STATE;
            it.state = LVIS_SELECTED | LVIS_FOCUSED;
            it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(g_hList, LVM_SETITEMSTATE, i, (LPARAM)&it);
            ListView_EnsureVisible(g_hList, i, FALSE);
            return;
        }
    }
}

/* ---------- 操作 ---------- */
static void open_folder_of(int idx)
{
    if (idx < 0) return;
    WCHAR dir[2048];
    wcsncpy(dir, g_items[idx].full, 2047); dir[2047] = 0;
    WCHAR *p = wcsrchr(dir, L'\\');
    if (p) *p = 0;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) wcsncpy(dir, g_root, 2047);
    ShellExecuteW(g_hMain, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

static void copy_path_of(int idx)
{
    if (idx < 0) return;
    size_t n = wcslen(g_items[idx].full);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(WCHAR));
    if (!h) return;
    void *dst = GlobalLock(h);
    memcpy(dst, g_items[idx].full, (n + 1) * sizeof(WCHAR));
    GlobalUnlock(h);
    if (OpenClipboard(g_hMain)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
    } else GlobalFree(h);
}

static void do_launch(int idx, int runas)
{
    if (idx < 0) return;
    ITEM *it = &g_items[idx];
    if (it->missing) {
        WCHAR msg[2400];
        _snwprintf(msg, 2399, L"文件不存在：\n%s\n\n是否打开所在文件夹？", it->full);
        msg[2399] = 0;
        if (MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONWARNING | MB_YESNO) == IDYES)
            open_folder_of(idx);
        return;
    }
    WCHAR dir[2048];
    wcsncpy(dir, it->full, 2047); dir[2047] = 0;
    WCHAR *q = wcsrchr(dir, L'\\');
    if (q) *q = 0;

    size_t rl = wcslen(it->full);
    HINSTANCE r;
    if (rl > 4 && !_wcsicmp(it->full + rl - 4, L".ps1")) {          /* PowerShell 脚本 */
        WCHAR cmd[2600];
        _snwprintf(cmd, 2599, L"-NoProfile -ExecutionPolicy Bypass -File \"%s\"", it->full);
        cmd[2599] = 0;
        r = ShellExecuteW(g_hMain, runas ? L"runas" : L"open", L"powershell.exe", cmd, dir, SW_SHOWNORMAL);
    } else {
        r = ShellExecuteW(g_hMain, runas ? L"runas" : L"open", it->full,
                          it->args[0] ? it->args : NULL, dir, SW_SHOWNORMAL);
    }
    if ((INT_PTR)r <= 32) {
        WCHAR msg[2400];
        _snwprintf(msg, 2399, L"启动失败（代码 %d）：\n%s\n\n若工具需要管理员权限，请用右键“以管理员身份运行”。",
                   (int)(INT_PTR)r, it->full);
        msg[2399] = 0;
        MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONERROR);
    }
}

/* ---------- 增删改 ---------- */
static void reload_all(void);
static void write_report(const WCHAR *outfile, const WCHAR *rep);
/* 已存在同名同路径返回 1 */
static int item_exists(const WCHAR *panel, const WCHAR *rel)
{
    for (int i = 0; i < g_count; i++)
        if (!_wcsicmp(g_items[i].panel, panel) && !_wcsicmp(g_items[i].rel, rel)) return 1;
    return 0;
}
static int add_item_raw(const WCHAR *panel, const WCHAR *title, const WCHAR *rel, const WCHAR *args)
{
    ITEM *it;
    if (g_count >= MAX_ITEMS) return 0;
    it = &g_items[g_count];
    copy_field(it->panel, 64,  panel, wcslen(panel));
    copy_field(it->title, 256, title, wcslen(title));
    copy_field(it->rel,   1024, rel,  wcslen(rel));
    copy_field(it->args,  512,  args, wcslen(args));
    join_path(it->full, 2048, g_root, it->rel);
    it->missing = (GetFileAttributesW(it->full) == INVALID_FILE_ATTRIBUTES);
    g_count++;
    return 1;
}
static void del_item_at(int idx)
{
    if (idx < 0 || idx >= g_count) return;
    for (int i = idx; i < g_count - 1; i++) g_items[i] = g_items[i + 1];
    g_count--;
}

static void show_save_error(void)
{
    MessageBoxW(g_hMain, L"写入配置文件失败（文件可能被占用或只读）。", APP_NAME, MB_ICONERROR);
}

/* ---------- 文件选择 ---------- */
static int pick_files(HWND owner, WCHAR out[][MAX_PATH * 2], int maxn, const WCHAR *title, const WCHAR *filter)
{
    static WCHAR buf[32768];
    OPENFILENAMEW ofn;
    WCHAR dir[MAX_PATH * 2];
    ZeroMemory(buf, sizeof(buf));
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner ? owner : g_hMain;
    ofn.lpstrFilter = filter ? filter :
        L"程序与脚本 (*.exe;*.bat;*.cmd;*.ps1;*.lnk;*.msi)\0*.exe;*.bat;*.cmd;*.ps1;*.lnk;*.msi;*.com;*.scr\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = title;
    join_path(dir, MAX_PATH * 2, g_root, L"");
    ofn.lpstrInitialDir = dir;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return 0;

    /* 多选时: 目录\0文件1\0文件2\0\0 ；单选时: 完整路径\0\0 */
    WCHAR *p = buf;
    WCHAR folder[MAX_PATH * 2];
    int n = 0;
    wcsncpy(folder, p, MAX_PATH * 2 - 1); folder[MAX_PATH * 2 - 1] = 0;
    p += wcslen(p) + 1;
    if (*p == 0) {                                     /* 单个文件 */
        wcsncpy(out[0], folder, MAX_PATH * 2 - 1); out[0][MAX_PATH * 2 - 1] = 0;
        return 1;
    }
    while (*p && n < maxn) {
        join_path(out[n], MAX_PATH * 2, folder, p);
        n++;
        p += wcslen(p) + 1;
    }
    return n;
}


/* ================= 导入 / 导出 / 新建 ================= */
/* base64 + swapcase 解密（Rolan 的 cfg/rcb 就是这个"假加密"） */
static int b64v(WCHAR c)
{
    if (c >= L'A' && c <= L'Z') return (int)(c - L'A');
    if (c >= L'a' && c <= L'z') return (int)(c - L'a') + 26;
    if (c >= L'0' && c <= L'9') return (int)(c - L'0') + 52;
    if (c == L'+') return 62;
    if (c == L'/') return 63;
    return -1;
}
static WCHAR *rolan_decrypt(const WCHAR *s)
{
    size_t n = wcslen(s), i;
    WCHAR *sw;
    char *bin;
    int bn = 0, acc = 0, bits = 0, need;
    WCHAR *out = NULL;
    if (n == 0) return NULL;
    sw = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    bin = (char *)malloc(n + 8);
    if (!sw || !bin) { free(sw); free(bin); return NULL; }
    for (i = 0; i < n; i++) {                       /* ① 大小写互换 */
        WCHAR c = s[i];
        if (c >= L'A' && c <= L'Z') c = (WCHAR)(c - L'A' + L'a');
        else if (c >= L'a' && c <= L'z') c = (WCHAR)(c - L'a' + L'A');
        sw[i] = c;
    }
    sw[n] = 0;
    for (i = 0; i < n; i++) {                       /* ② base64 解码 */
        int v = b64v(sw[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; bin[bn++] = (char)((acc >> bits) & 0xFF); }
    }
    free(sw);
    bin[bn] = 0;
    need = MultiByteToWideChar(936, 0, bin, bn, NULL, 0);   /* ③ GBK → 宽字符 */
    if (need <= 0) need = MultiByteToWideChar(CP_ACP, 0, bin, bn, NULL, 0);
    if (need > 0) {
        out = (WCHAR *)malloc((need + 2) * sizeof(WCHAR));
        if (out) {
            if (!MultiByteToWideChar(936, 0, bin, bn, out, need))
                MultiByteToWideChar(CP_ACP, 0, bin, bn, out, need);
            out[need] = 0;
        }
    }
    free(bin);
    return out;
}
/* 从 Title:"x",Path:"y",Parm:"z" 这种串里取字段 */
static void qfield(const WCHAR *s, const WCHAR *name, WCHAR *out, size_t cap)
{
    const WCHAR *p = StrStrIW(s, name);
    out[0] = 0;
    if (!p) return;
    p += wcslen(name);
    if (*p != L'"') return;
    p++;
    size_t i = 0;
    while (p[i] && p[i] != L'"' && i < cap - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    while (i > 0 && (out[i - 1] == L' ' || out[i - 1] == L'\r')) out[--i] = 0;
}
/* 把 Rolan 明文里的路径规范化：去掉 %rp%\ 前缀、斜杠转反斜杠 */
static void clean_rel(const WCHAR *in, WCHAR *out, size_t cap)
{
    const WCHAR *p = in;
    size_t i = 0;
    while (!_wcsnicmp(p, L"%rp%", 4)) { p += 4; if (*p == L'\\' || *p == L'/') p++; }
    while (*p == L'\\' || *p == L'/') p++;
    while (*p && i < cap - 1) { out[i++] = (*p == L'/') ? L'\\' : *p; p++; }
    out[i] = 0;
}
/* 解析本启动器格式的一行：面板|名称|路径|参数 */
static int parse_line(const WCHAR *line, WCHAR *panel, size_t pcap, WCHAR *title, size_t tcap,
                      WCHAR *rel, size_t rcap, WCHAR *args, size_t acap)
{
    WCHAR f0[1024], f1[1024], f2[2048], f3[1024];
    const WCHAR *f[4];
    int nf = 0;
    const WCHAR *seg = line;
    size_t len = 0;
    /* 用固定缓冲区切 4 段 */
    WCHAR *dst[4] = { f0, f1, f2, f3 };
    size_t dcap[4] = { 1024, 1024, 2048, 1024 };
    const WCHAR *q = line;
    for (;;) {
        if (*q == 0 || (*q == L'|' || *q == L'\t') || nf == 3 && *q == 0) {
            size_t j = 0;
            const WCHAR *s2 = seg;
            while (s2 < q && j < dcap[nf] - 1) dst[nf][j++] = *s2++;
            dst[nf][j] = 0;
            nf++;
            if (*q == 0 || nf >= 4) break;
            seg = q + 1;
        }
        if (*q == 0) break;
        q++;
    }
    while (nf < 3) { dst[nf][0] = 0; nf++; }
    f[0] = f0; f[1] = f1; f[2] = f2; f[3] = f3;
    (void)len;
    if (!f0[0] || !f1[0] || !f2[0]) return 0;
    copy_field(panel, pcap, f0, wcslen(f0));
    copy_field(title, tcap, f1, wcslen(f1));
    copy_field(rel,   rcap, f2, wcslen(f2));
    copy_field(args,  acap, f3, wcslen(f3));
    return 1;
}
/* 导入一个文件；返回新增条数。kind 里回填识别出的类型 */
static int import_file_core(const WCHAR *path, int replace, int *dup, const WCHAR **kind, WCHAR *newTitle, size_t tcap)
{
    WCHAR *text = NULL;
    int added = 0, isRolan = 0;
    const WCHAR *ext = wcsrchr(path, L'.');
    *dup = 0;
    *kind = L"启动器配置";
    newTitle[0] = 0;
    if (ext && (!_wcsicmp(ext, L".cfg") || !_wcsicmp(ext, L".rcb"))) {
        text = read_text_file(path, CP_ACP);         /* Rolan 的 cfg/rcb 是 GBK */
        if (!text) text = read_text_file(path, CP_UTF8);
    } else {
        text = read_text_file(path, CP_UTF8);
        if (!text) text = read_text_file(path, CP_ACP);
    }
    if (!text) { *kind = L"（读不出内容）"; return -1; }
    if ((ext && (!_wcsicmp(ext, L".cfg") || !_wcsicmp(ext, L".rcb"))) || StrStrIW(text, L"[Data]"))
        isRolan = 1;

    if (replace) g_count = 0;
    if (isRolan) {
        const WCHAR *p = StrStrIW(text, L"[Data]");
        *kind = L"Rolan 配置";
        if (p) {
            const WCHAR *nl = wcschr(p, L'\n');        /* 从 [Data] 的下一行开始 */
            p = nl ? nl + 1 : NULL;
        }
        if (p) {
            while (*p) {
                /* 取一行 */
                WCHAR line[8192];
                size_t n = 0;
                while (*p && *p != L'\n' && n < 8191) line[n++] = *p++;
                line[n] = 0;
                if (*p == L'\n') p++;
                while (n && (line[n - 1] == L'\r' || line[n - 1] == L' ')) line[--n] = 0;
                if (!line[0]) continue;
                if (line[0] == L'[') break;                 /* 下一个段 */
                {
                    WCHAR *eq = wcschr(line, L'=');
                    WCHAR *panelW, *items;
                    if (!eq) continue;
                    *eq = 0;
                    items = eq + 1;
                    panelW = rolan_decrypt(line);
                    if (!panelW || !panelW[0]) { free(panelW); continue; }
                    /* 逗号分隔的条目密文 */
                    {
                        WCHAR *tok = items, *q = items;
                        for (;;) {
                            if (*q == L',' || *q == 0) {
                                WCHAR save = *q;
                                WCHAR *dec;
                                *q = 0;
                                dec = rolan_decrypt(tok);
                                *q = save;
                                if (dec && dec[0]) {
                                    WCHAR t[512], rp[2048], pm[512], rel[2048];
                                    qfield(dec, L"Title:", t, 512);
                                    qfield(dec, L",Path:", rp, 2048);   /* 前面带逗号，避免匹到 IcoPath */
                                    qfield(dec, L"Parm:", pm, 512);
                                    clean_rel(rp, rel, 2048);
                                    if (t[0] && rel[0]) {
                                        if (item_exists(panelW, rel)) (*dup)++;
                                        else if (add_item_raw(panelW, t, rel, pm)) added++;
                                    }
                                    free(dec);
                                }
                                if (save == 0) break;
                                tok = q + 1;
                            }
                            q++;
                        }
                    }
                    free(panelW);
                }
            }
        }
    } else {
        const WCHAR *p = text;
        while (*p) {
            WCHAR line[4096];
            size_t n = 0;
            WCHAR panel[64], title[256], rel[1024], args[512];
            while (*p && *p != L'\n' && n < 4095) line[n++] = *p++;
            line[n] = 0;
            if (*p == L'\n') p++;
            {
                WCHAR *L2 = line;
                while (*L2 == L' ' || *L2 == L'\t' || *L2 == L'\r') L2++;
                size_t ln = wcslen(L2);
                while (ln && (L2[ln - 1] == L'\r' || L2[ln - 1] == L' ')) L2[--ln] = 0;
                if (!L2[0]) continue;
                if (L2[0] == L'#' || (L2[0] == L'/' && L2[1] == L'/')) {
                    if (!_wcsnicmp(L2, L"#TITLE=", 7)) copy_field(newTitle, tcap, L2 + 7, wcslen(L2 + 7));
                    continue;
                }
                if (parse_line(L2, panel, 64, title, 256, rel, 1024, args, 512)) {
                    WCHAR rel2[1024];
                    clean_rel(rel, rel2, 1024);
                    if (item_exists(panel, rel2)) (*dup)++;
                    else if (add_item_raw(panel, title, rel2, args)) added++;
                }
            }
        }
    }
    free(text);
    return added;
}
static void reload_all(void);

/* 图形界面：导入 / 导出 / 从文件夹新建 / 新建空配置 */
static void do_import(void)
{
    static WCHAR picked[2][MAX_PATH * 2];
    WCHAR msg[600], newTitle[128];
    const WCHAR *kind;
    int dup = 0, added, r;
    int n = pick_files(g_hMain, picked, 1, L"选择要导入的配置（启动器 txt 或 Rolan 的 cfg/rcb）",
        L"配置文件 (*.txt;*.cfg;*.rcb)\0*.txt;*.cfg;*.rcb\0所有文件 (*.*)\0*.*\0\0");
    if (n <= 0) return;
    load_config();                       /* 先保证内存里是磁盘上的最新内容 */
    _snwprintf(msg, 599, L"把「%s」导入到当前配置？\n\n【是】合并：保留现有 %d 条，重复的跳过\n【否】替换：先清空现有条目再导入\n【取消】放弃",
               base_name(picked[0]), g_count);
    msg[599] = 0;
    r = MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (r == IDCANCEL) return;
    added = import_file_core(picked[0], r == IDNO, &dup, &kind, newTitle, 128);
    if (added < 0) { MessageBoxW(g_hMain, L"这个文件读不出内容。", APP_NAME, MB_ICONERROR); return; }
    if (newTitle[0] && r == IDNO) copy_field(g_appTitle, 128, newTitle, wcslen(newTitle));
    if (!save_config()) { show_save_error(); return; }
    reload_all();
    _snwprintf(msg, 599, L"导入完成（%s）：新增 %d 条，跳过重复 %d 条，现在共 %d 条。", kind, added, dup, g_count);
    msg[599] = 0;
    MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONINFORMATION);
}
static void do_export(void)
{
    static WCHAR buf[MAX_PATH * 2];
    OPENFILENAMEW ofn;
    WCHAR def[MAX_PATH * 2];
    int onlyVisible, r;
    if (!g_count) { MessageBoxW(g_hMain, L"当前配置里还没有条目。", APP_NAME, MB_ICONINFORMATION); return; }
    r = MessageBoxW(g_hMain, L"导出哪些条目？\n\n【是】全部条目\n【否】只导出当前列表里显示的（搜索结果/面板过滤后）\n【取消】放弃",
                    APP_NAME, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (r == IDCANCEL) return;
    onlyVisible = (r == IDNO);
    join_path(def, MAX_PATH * 2, g_root, L"tools_utf8_导出.txt");
    wcsncpy(buf, def, MAX_PATH * 2 - 1); buf[MAX_PATH * 2 - 1] = 0;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = L"配置文本 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrTitle = L"导出配置（UTF-8 文本）";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!save_config_ex(buf, CP_UTF8, onlyVisible, 0)) {
        MessageBoxW(g_hMain, L"写文件失败（目标可能只读或被占用）。", APP_NAME, MB_ICONERROR);
        return;
    }
    _snwprintf(def, MAX_PATH * 2 - 1, L"已导出 %d 条到：\n%s\n\nUTF-8 编码，本启动器（或任何文本编辑器）都能再读回来。",
               onlyVisible ? g_views : g_count, buf);
    def[MAX_PATH * 2 - 1] = 0;
    MessageBoxW(g_hMain, def, APP_NAME, MB_ICONINFORMATION);
}
static int scan_folder_rec(const WCHAR *dir, const WCHAR *panel, int recurse, int *dup, int depth)
{
    static const WCHAR *exts[] = { L".exe", L".bat", L".cmd", L".ps1", L".lnk", L".msi", L".com", L".scr", L".vbs", L".reg" };
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pat[MAX_PATH * 2];
    int added = 0, i;
    if (depth > 6) return 0;
    join_path(pat, MAX_PATH * 2, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse) {
                WCHAR sub[MAX_PATH * 2];
                join_path(sub, MAX_PATH * 2, dir, fd.cFileName);
                added += scan_folder_rec(sub, panel, recurse, dup, depth + 1);
            }
            continue;
        }
        {
            const WCHAR *dot = wcsrchr(fd.cFileName, L'.');
            int ok = 0;
            if (!dot) continue;
            for (i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); i++) if (!_wcsicmp(dot, exts[i])) { ok = 1; break; }
            if (!ok) continue;
            {
                WCHAR full[MAX_PATH * 2], rel[MAX_PATH * 2], nm[256];
                join_path(full, MAX_PATH * 2, dir, fd.cFileName);
                to_rel(full, rel, MAX_PATH * 2);
                stem_name(full, nm, 256);
                if (item_exists(panel, rel)) { (*dup)++; continue; }
                if (!add_item_raw(panel, nm, rel, L"")) break;
                added++;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return added;
}
static void do_scan_folder(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST idl;
    WCHAR dir[MAX_PATH * 2], panel[64], msg[800], leaf[MAX_PATH * 2];
    const WCHAR *b;
    int recurse, dup = 0, added;
    int r;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g_hMain;
    bi.lpszTitle = L"选择要扫描的工具文件夹（里面的 exe/bat/ps1 会按文件夹名建一个面板）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);          /* BIF_NEWDIALOGSTYLE 需要 COM 初始化 */
    idl = SHBrowseForFolderW(&bi);
    if (idl) {
        if (!SHGetPathFromIDListW(idl, dir)) { CoTaskMemFree(idl); CoUninitialize(); return; }
        CoTaskMemFree(idl);
    } else {
        /* 文件夹选择框不可用（被组策略禁用等）时的退路：让用户点该文件夹里的任意一个文件 */
        static WCHAR picked[2][MAX_PATH * 2];
        WCHAR *sl;
        CoUninitialize();
        if (MessageBoxW(g_hMain, L"文件夹选择框没有打开（可能被系统策略禁用）。\n\n改成“进入该文件夹、点里面的任意一个文件”来确定目录？",
                        APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES) return;
        if (pick_files(g_hMain, picked, 1, L"点这个文件夹里的任意一个文件", NULL) <= 0) return;
        wcsncpy(dir, picked[0], MAX_PATH * 2 - 1); dir[MAX_PATH * 2 - 1] = 0;
        sl = wcsrchr(dir, L'\\');
        if (sl) *sl = 0;
        if (!dir[0]) return;
    }
    CoUninitialize();
    r = MessageBoxW(g_hMain, L"要连着子文件夹一起扫描吗？\n\n【是】包含子文件夹\n【否】只扫这一层", APP_NAME,
                    MB_ICONQUESTION | MB_YESNOCANCEL);
    if (r == IDCANCEL) return;
    recurse = (r == IDYES);
    load_config();
    b = base_name(dir);
    copy_field(panel, 64, b && *b ? b : L"新面板", wcslen(b && *b ? b : L"新面板"));
    added = scan_folder_rec(dir, panel, recurse, &dup, 0);
    if (added > 0) {
        if (!save_config()) { show_save_error(); return; }
        reload_all();
    }
    _snwprintf(msg, 799, L"扫描完成：加入面板【%s】%d 个工具（跳过重复 %d 个）。\n目录：%s%s",
               panel, added, dup, dir, added ? L"" : L"\n\n（没找到 exe/bat/ps1/lnk/msi 这类可执行文件）");
    msg[799] = 0;
    MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONINFORMATION);
}
static void do_new_config(void)
{
    WCHAR msg[600];
    if (!g_count) { MessageBoxW(g_hMain, L"当前配置本来就是空的。", APP_NAME, MB_ICONINFORMATION); return; }
    _snwprintf(msg, 599, L"新建空配置：清空列表里的 %d 条记录（磁盘上的工具文件不动，配置会立即写回并已自动备份 .bak）。\n\n确定要清空吗？",
               g_count);
    msg[599] = 0;
    if (MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONWARNING | MB_YESNO) != IDYES) return;
    g_count = 0;
    if (!save_config()) { show_save_error(); return; }
    reload_all();
    MessageBoxW(g_hMain, L"已清空。接下来可以用「添加工具」(Ctrl+N)、拖入文件、或「从文件夹批量新建」把工具加回来。",
                APP_NAME, MB_ICONINFORMATION);
}

/* ---------- 添加/编辑对话框（普通窗口 + 模态消息循环，不依赖资源） ---------- */
typedef struct {
    WCHAR name[256];
    WCHAR panel[64];
    WCHAR rel[1024];
    WCHAR args[512];
} DLGIN;

static DLGIN g_din;
static int   g_dlgOK = 0;
static HWND  g_hDlg = NULL, g_dName = NULL, g_dPanel = NULL, g_dPath = NULL, g_dArgs = NULL;
static int   g_dlgEdit = 0;

static void dlg_collect(void)
{
    GetWindowTextW(g_dName,  g_din.name, 255);   g_din.name[255] = 0;
    GetWindowTextW(g_dPanel, g_din.panel, 63);   g_din.panel[63] = 0;
    GetWindowTextW(g_dPath,  g_din.rel, 1023);   g_din.rel[1023] = 0;
    GetWindowTextW(g_dArgs,  g_din.args, 511);   g_din.args[511] = 0;
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(h, GWLP_HINSTANCE);
        const WCHAR *lab[4] = { L"名称:", L"面板:", L"程序:", L"参数:" };
        int i;
        for (i = 0; i < 4; i++) {
            HWND s = CreateWindowExW(0, L"STATIC", lab[i], WS_CHILD | WS_VISIBLE | SS_RIGHT,
                0, 0, 10, 10, h, (HMENU)(INT_PTR)(IDC_DLG_L1 + i), hi, NULL);
            SendMessageW(s, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
        g_dName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)IDC_DLG_NAME, hi, NULL);
        g_dPanel = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
            0, 0, 10, 240, h, (HMENU)IDC_DLG_PANEL, hi, NULL);
        g_dPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)IDC_DLG_PATH, hi, NULL);
        g_dArgs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)IDC_DLG_ARGS, hi, NULL);
        HWND br = CreateWindowExW(0, L"BUTTON", L"浏览…(&B)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h, (HMENU)IDC_DLG_BROWSE, hi, NULL);
        HWND hint = CreateWindowExW(0, L"STATIC",
            L"提示：程序路径可写相对本程序目录的相对路径（如 工具\\xxx.exe）；\r\n"
            L"面板可直接输入新名字，等于新建一个分类。",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, h, (HMENU)IDC_DLG_HINT, hi, NULL);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"确定(&O)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, h, (HMENU)IDC_DLG_OK, hi, NULL);
        HWND cc = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h, (HMENU)IDC_DLG_CANCEL, hi, NULL);
        HWND ctl[8]; int k;
        ctl[0] = g_dName; ctl[1] = g_dPanel; ctl[2] = g_dPath; ctl[3] = br;
        ctl[4] = g_dArgs; ctl[5] = ok; ctl[6] = cc; ctl[7] = hint;
        (void)ctl;
        for (k = 0; k < 4; k++) SendMessageW(GetDlgItem(h, IDC_DLG_L1 + k), WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(hint, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_dName,  WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_dPanel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_dPath,  WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_dArgs,  WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(br, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(ok, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(cc, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        for (k = 0; k < g_panelN; k++) SendMessageW(g_dPanel, CB_ADDSTRING, 0, (LPARAM)g_panels[k]);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == IDC_DLG_OK || (id == IDOK)) {
            dlg_collect();
            if (!g_din.name[0]) { MessageBoxW(h, L"请填写名称。", APP_NAME, MB_ICONWARNING); SetFocus(g_dName); return 0; }
            if (!g_din.rel[0])  { MessageBoxW(h, L"请选择或填写程序路径。", APP_NAME, MB_ICONWARNING); SetFocus(g_dPath); return 0; }
            if (!g_din.panel[0]) wcscpy(g_din.panel, L"常用");
            g_dlgOK = 1;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDC_DLG_CANCEL || id == IDCANCEL) { g_dlgOK = 0; DestroyWindow(h); return 0; }
        if (id == IDC_DLG_BROWSE && code == BN_CLICKED) {
            WCHAR picked[8][MAX_PATH * 2];
            int n = pick_files(h, picked, 8, L"选择要添加的工具", NULL);
            if (n == 1) {
                WCHAR rel[MAX_PATH * 2], nm[256];
                to_rel(picked[0], rel, MAX_PATH * 2);
                SetWindowTextW(g_dPath, rel);
                GetWindowTextW(g_dName, nm, 255);
                if (!nm[0]) { stem_name(picked[0], nm, 256); SetWindowTextW(g_dName, nm); }
            } else if (n > 1) {
                WCHAR msg[128];
                _snwprintf(msg, 127, L"这里一次只能编辑一个条目（已选 %d 个）。\n请回到主窗口用「＋添加工具」批量添加。", n);
                msg[127] = 0;
                MessageBoxW(h, msg, APP_NAME, MB_ICONINFORMATION);
            }
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)g_hBg;
    case WM_CLOSE:
        g_dlgOK = 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_hDlg = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void dlg_layout(HWND h)
{
    RECT rc;
    int w, y, pad = 14, lw = 52, hh = 24, fw, bw = 78, foot;
    GetClientRect(h, &rc);
    w = rc.right - rc.left;
    fw = w - pad * 2 - lw - 6;
    y = 16;
    MoveWindow(GetDlgItem(h, IDC_DLG_L1), pad, y + 4, lw, 18, TRUE);
    MoveWindow(g_dName,  pad + lw + 6, y, fw, hh, TRUE);
    y += hh + 10;
    MoveWindow(GetDlgItem(h, IDC_DLG_L2), pad, y + 4, lw, 18, TRUE);
    MoveWindow(g_dPanel, pad + lw + 6, y, fw, 240, TRUE);
    y += hh + 10;
    MoveWindow(GetDlgItem(h, IDC_DLG_L3), pad, y + 4, lw, 18, TRUE);
    MoveWindow(g_dPath,  pad + lw + 6, y, fw - bw - 6, hh, TRUE);
    MoveWindow(GetDlgItem(h, IDC_DLG_BROWSE), pad + lw + 6 + fw - bw, y, bw, hh, TRUE);
    y += hh + 10;
    MoveWindow(GetDlgItem(h, IDC_DLG_L4), pad, y + 4, lw, 18, TRUE);
    MoveWindow(g_dArgs,  pad + lw + 6, y, fw, hh, TRUE);
    y += hh + 12;
    MoveWindow(GetDlgItem(h, IDC_DLG_HINT), pad, y, w - pad * 2, 36, TRUE);
    foot = rc.bottom - 42;
    MoveWindow(GetDlgItem(h, IDC_DLG_OK),     w - pad - bw * 2 - 8, foot, bw, 26, TRUE);
    MoveWindow(GetDlgItem(h, IDC_DLG_CANCEL), w - pad - bw,         foot, bw, 26, TRUE);
}

/* 打开对话框；editIdx >= 0 表示编辑已有条目。返回 1 = 用户点了确定 */
static int run_item_dialog(int editIdx, const WCHAR *initPath)
{
    static int reg = 0;
    WNDCLASSEXW wc;
    MSG m;
    RECT rc;

    ZeroMemory(&g_din, sizeof(g_din));
    if (editIdx >= 0) {
        wcsncpy(g_din.name,  g_items[editIdx].title, 255);
        wcsncpy(g_din.panel, g_items[editIdx].panel, 63);
        wcsncpy(g_din.rel,   g_items[editIdx].rel, 1023);
        wcsncpy(g_din.args,  g_items[editIdx].args, 511);
    } else {
        if (initPath && *initPath) {
            to_rel(initPath, g_din.rel, 1024);
            stem_name(initPath, g_din.name, 256);
        }
        if (g_panelSel > 0 && g_panelSel - 1 < g_panelN) wcsncpy(g_din.panel, g_panels[g_panelSel - 1], 63);
        else wcscpy(g_din.panel, L"常用");
    }
    g_dlgOK = 0;
    g_dlgEdit = (editIdx >= 0);

    if (!reg) {
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = (HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE);
        wc.lpszClassName = L"ForensicToolboxItemDlg";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = g_hBg;
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        if (!RegisterClassExW(&wc)) return 0;
        reg = 1;
    }

    rc.left = 0; rc.top = 0; rc.right = 580; rc.bottom = 300;
    AdjustWindowRectEx(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);
    g_hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"ForensicToolboxItemDlg",
        editIdx >= 0 ? L"编辑工具" : L"添加工具",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        g_hMain, NULL, (HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE), NULL);
    if (!g_hDlg) return 0;
    dlg_layout(g_hDlg);

    EnableWindow(g_hMain, FALSE);
    ShowWindow(g_hDlg, SW_SHOW);
    UpdateWindow(g_hDlg);
    SetForegroundWindow(g_hDlg);
    /* 窗口可见之后才填内容：WM_CREATE 里填在部分环境（Wine/老系统）会不重绘，显示成空白框 */
    SetWindowTextW(g_dName,  g_din.name);
    SetWindowTextW(g_dPanel, g_din.panel);
    SetWindowTextW(g_dPath,  g_din.rel);
    SetWindowTextW(g_dArgs,  g_din.args);
    if (g_dlgEdit) { SetFocus(g_dName); SendMessageW(g_dName, EM_SETSEL, 0, -1); }   /* 编辑：名称全选，便于改名 */
    else { SetFocus(g_dName); SendMessageW(g_dName, EM_SETSEL, -1, -1); }            /* 新增：光标停在名称末尾 */
    SendMessageW(g_dPath, EM_SETSEL, 0, 0);                                          /* 路径框从头显示 */
    UpdateWindow(g_dName);
    /* 丢掉上一个对话框残留的按键消息（回车/退格可能落到刚打开的编辑框里） */
    {
        MSG dq;
        while (PeekMessageW(&dq, NULL, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) ;
    }

    while (IsWindow(g_hDlg) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { g_dlgOK = 0; DestroyWindow(g_hDlg); continue; }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && IsChild(g_hDlg, m.hwnd)) {
            PostMessageW(g_hDlg, WM_COMMAND, IDC_DLG_OK, 0);
            continue;
        }
        if (!IsDialogMessageW(g_hDlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetActiveWindow(g_hMain);
    return g_dlgOK;
}

/* ---------- 添加 / 批量添加 / 删除 ---------- */
static void after_change(const WCHAR *rel)
{
    reload_all();
    if (rel && *rel) select_by_rel(rel);
}

static void add_one_interactive(const WCHAR *absPath)
{
    WCHAR rel[MAX_PATH * 2];
    if (absPath) to_rel(absPath, rel, MAX_PATH * 2);
    else rel[0] = 0;
    if (!run_item_dialog(-1, absPath)) return;
    if (item_exists(g_din.panel, g_din.rel)) {
        if (MessageBoxW(g_hMain, L"该面板下已经存在同一个路径的条目，仍然要再添加一份吗？",
                        APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES) return;
    }
    if (!add_item_raw(g_din.panel, g_din.name, g_din.rel, g_din.args)) {
        MessageBoxW(g_hMain, L"工具条目已达上限（6000）。", APP_NAME, MB_ICONERROR);
        return;
    }
    if (!save_config()) { show_save_error(); return; }
    after_change(g_din.rel);
}

static void add_many(const WCHAR src[][MAX_PATH * 2], int n)
{
    WCHAR panel[64], msg[512];
    int ok = 0, dup = 0, i;
    if (g_panelSel > 0 && g_panelSel - 1 < g_panelN) wcsncpy(panel, g_panels[g_panelSel - 1], 63);
    else wcscpy(panel, L"常用");
    panel[63] = 0;

    for (i = 0; i < n; i++) {
        WCHAR rel[MAX_PATH * 2], nm[256];
        DWORD attr = GetFileAttributesW(src[i]);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        to_rel(src[i], rel, MAX_PATH * 2);
        stem_name(src[i], nm, 256);
        if (item_exists(panel, rel)) { dup++; continue; }
        if (!add_item_raw(panel, nm, rel, L"")) break;
        ok++;
    }
    if (ok > 0) {
        if (!save_config()) { show_save_error(); return; }
        after_change(NULL);
    }
    _snwprintf(msg, 511, L"已添加到面板【%s】：%d 个工具。", panel, ok);
    if (dup) {
        WCHAR t[64];
        _snwprintf(t, 63, L"\n跳过重复条目 %d 个。", dup);
        t[63] = 0;
        wcsncat(msg, t, 511 - wcslen(msg));
    }
    MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONINFORMATION);
}

static void do_add_tools(void)
{
    static WCHAR picked[64][MAX_PATH * 2];
    int n = pick_files(g_hMain, picked, 64, L"选择要添加的工具（可多选）", NULL);
    if (n <= 0) return;
    if (n == 1) add_one_interactive(picked[0]);
    else add_many((const WCHAR (*)[MAX_PATH * 2])picked, n);
}

static void do_edit_item(void)
{
    int idx = selected_item();
    if (idx < 0) { MessageBoxW(g_hMain, L"请先在列表里选中一个条目。", APP_NAME, MB_ICONINFORMATION); return; }
    if (!run_item_dialog(idx, NULL)) return;
    copy_field(g_items[idx].panel, 64,  g_din.panel, wcslen(g_din.panel));
    copy_field(g_items[idx].title, 256, g_din.name,  wcslen(g_din.name));
    copy_field(g_items[idx].rel,   1024, g_din.rel,  wcslen(g_din.rel));
    copy_field(g_items[idx].args,  512,  g_din.args, wcslen(g_din.args));
    join_path(g_items[idx].full, 2048, g_root, g_items[idx].rel);
    g_items[idx].missing = (GetFileAttributesW(g_items[idx].full) == INVALID_FILE_ATTRIBUTES);
    if (!save_config()) { show_save_error(); return; }
    after_change(g_items[idx].rel);
}

static void do_del_item(void)
{
    int idx = selected_item();
    WCHAR msg[600];
    if (idx < 0) { MessageBoxW(g_hMain, L"请先在列表里选中一个条目。", APP_NAME, MB_ICONINFORMATION); return; }
    _snwprintf(msg, 599, L"从工具列表中移除：\n\n%s\n（%s）\n\n只删除配置里的条目，不动磁盘上的文件。",
               g_items[idx].title, g_items[idx].rel);
    msg[599] = 0;
    if (MessageBoxW(g_hMain, msg, APP_NAME, MB_ICONWARNING | MB_YESNO) != IDYES) return;
    del_item_at(idx);
    if (!save_config()) { show_save_error(); return; }
    after_change(NULL);
}

/* ---------- 子类化：搜索框 / 列表 ---------- */
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN) {
            int idx = selected_item();
            if (idx < 0) idx = (g_views > 0) ? g_view[0] : -1;
            do_launch(idx, 0);
            return 0;
        }
        if (w == VK_ESCAPE) { SetWindowTextW(h, L""); refresh_list(); return 0; }
        if (w == VK_DOWN) {
            SetFocus(g_hList);
            if (ListView_GetNextItem(g_hList, -1, LVNI_SELECTED) < 0 && g_views > 0) {
                LVITEMW it; ZeroMemory(&it, sizeof(it));
                it.mask = LVIF_STATE; it.state = LVIS_SELECTED | LVIS_FOCUSED; it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageW(g_hList, LVM_SETITEMSTATE, 0, (LPARAM)&it);
                ListView_EnsureVisible(g_hList, 0, FALSE);
            }
            return 0;
        }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    return CallWindowProcW(g_oldEdit, h, m, w, l);
}

static LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        int idx = selected_item();
        if (idx < 0 && g_views > 0) idx = g_view[0];
        do_launch(idx, 0);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_DELETE) { do_del_item(); return 0; }
    if (m == WM_KEYDOWN && w == VK_UP) {
        int row = ListView_GetNextItem(h, -1, LVNI_SELECTED);
        if (row <= 0) { SetFocus(g_hSearch); return 0; }
    }
    return CallWindowProcW(g_oldList, h, m, w, l);
}

/* ---------- 界面 ---------- */
static void layout(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int pad = 8, top = 34, bottom = 26, leftw = 176, abw = 108;
    MoveWindow(g_hSearch, pad + 58, pad + 2, W - pad * 2 - 58 - 4 - abw - 8, 24, TRUE);
    HWND hLab = GetDlgItem(hwnd, 1999);
    if (hLab) MoveWindow(hLab, pad, pad + 5, 54, 18, TRUE);
    if (g_hAdd) MoveWindow(g_hAdd, W - pad - abw, pad - 1, abw, 27, TRUE);
    MoveWindow(g_hPanel,  pad, top, leftw, H - top - bottom - pad, TRUE);
    MoveWindow(g_hList,   pad + leftw + 8, top, W - (pad + leftw + 8) - pad, H - top - bottom - pad, TRUE);
    MoveWindow(g_hStatus, pad, H - bottom, W - pad * 2, 18, TRUE);
    int w2 = W - (pad + leftw + 8) - pad - 300;
    if (w2 < 120) w2 = 120;
    SendMessageW(g_hList, LVM_SETCOLUMNWIDTH, 1, MAKELPARAM(w2, 0));
}

static void set_view(int mode)
{
    LONG style;
    if (mode == g_viewMode || !g_hList) return;
    style = GetWindowLongW(g_hList, GWL_STYLE) & ~LVS_TYPEMASK;
    style |= (mode == 1) ? LVS_ICON : LVS_REPORT;
    SetWindowLongW(g_hList, GWL_STYLE, style);
    SendMessageW(g_hList, LVM_SETIMAGELIST, (mode == 1) ? LVSIL_NORMAL : LVSIL_SMALL,
                 (LPARAM)((mode == 1) ? g_himlLarge : g_himlSmall));
    ShowWindow(g_hList, SW_HIDE);
    ShowWindow(g_hList, SW_SHOW);
    g_viewMode = mode;
    if (mode == 0) {
        layout(g_hMain);                                  /* 恢复两列与列宽 */
    } else {
        SendMessageW(g_hList, LVM_SETICONSPACING, 0, MAKELPARAM(112, 84));   /* 单元格加宽，中文名少截断 */
    }
    InvalidateRect(g_hList, NULL, TRUE);
}

static void build_menu(HWND hwnd)
{
    HMENU mb = CreateMenu(), m1 = CreatePopupMenu(), m2 = CreatePopupMenu(), m3 = CreatePopupMenu();
    HMENU m4 = CreatePopupMenu();
    AppendMenuW(m1, MF_STRING, IDM_NEW,     L"新建空配置(&N)…");
    AppendMenuW(m1, MF_STRING, IDM_IMPORT,  L"导入配置(&I)…（支持 Rolan 的 cfg/rcb）");
    AppendMenuW(m1, MF_STRING, IDM_SCAN,    L"从文件夹批量新建(&F)…");
    AppendMenuW(m1, MF_STRING, IDM_EXPORT,  L"导出配置(&E)…");
    AppendMenuW(m1, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m1, MF_STRING, IDM_RELOAD,  L"重新载入配置(&R)\tF5");
    AppendMenuW(m1, MF_STRING, IDM_OPENDIR, L"打开工具箱目录(&O)");
    AppendMenuW(m1, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m1, MF_STRING, IDM_EXIT,    L"退出(&X)");
    AppendMenuW(m2, MF_STRING, IDM_RUN,     L"启动(&S)\tEnter");
    AppendMenuW(m2, MF_STRING, IDM_RUNAS,   L"以管理员身份运行(&A)");
    AppendMenuW(m2, MF_STRING, IDM_FOLDER,  L"打开所在文件夹(&F)");
    AppendMenuW(m2, MF_STRING, IDM_COPY,    L"复制完整路径(&C)");
    AppendMenuW(m2, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m2, MF_STRING, IDM_ADD,     L"添加工具(&N)…\tCtrl+N");
    AppendMenuW(m2, MF_STRING, IDM_EDIT,    L"编辑选中条目(&E)…");
    AppendMenuW(m2, MF_STRING, IDM_DEL,     L"删除选中条目(&D)\tDel");
    AppendMenuW(m3, MF_STRING, IDM_README,  L"使用说明(&H)");
    AppendMenuW(m3, MF_STRING, IDM_ABOUT,   L"关于(&A)");
    AppendMenuW(m4, MF_STRING, IDM_VIEW_LIST, L"列表视图（带小图标）\tCtrl+1");
    AppendMenuW(m4, MF_STRING, IDM_VIEW_ICON, L"图标视图（大图标）\tCtrl+2");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)m1, L"文件(&F)");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)m2, L"工具(&T)");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)m4, L"视图(&V)");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)m3, L"帮助(&H)");
    SetMenu(hwnd, mb);
}

static void make_font(void)
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g_hFont) g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void fill_panels(void)
{
    SendMessageW(g_hPanel, LB_RESETCONTENT, 0, 0);
    WCHAR s[128];
    _snwprintf(s, 127, L"全部 (%d)", g_count); s[127] = 0;
    SendMessageW(g_hPanel, LB_ADDSTRING, 0, (LPARAM)s);
    for (int j = 0; j < g_panelN; j++) {
        int c = 0;
        for (int i = 0; i < g_count; i++) if (!wcscmp(g_items[i].panel, g_panels[j])) c++;
        _snwprintf(s, 127, L"%s (%d)", g_panels[j], c); s[127] = 0;
        SendMessageW(g_hPanel, LB_ADDSTRING, 0, (LPARAM)s);
    }
    SendMessageW(g_hPanel, LB_SETCURSEL, 0, 0);
    g_panelSel = 0;
}

static void create_children(HWND hwnd)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    g_hSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, (HMENU)IDC_SEARCH, hi, NULL);
    SendMessageW(g_hSearch, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"搜索：工具名称 / 路径 / 面板，回车启动第一条");
    g_oldEdit = (WNDPROC)SetWindowLongPtrW(g_hSearch, GWLP_WNDPROC, (LONG_PTR)EditProc);

    CreateWindowExW(0, L"STATIC", L"搜索:", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
        hwnd, (HMENU)1999, hi, NULL);
    HWND hLabel = GetDlgItem(hwnd, 1999);
    SendMessageW(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    g_hAdd = CreateWindowExW(0, L"BUTTON", L"＋ 添加工具", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 10, 10, hwnd, (HMENU)IDC_ADDBTN, hi, NULL);
    SendMessageW(g_hAdd, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    g_hPanel = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_PANEL, hi, NULL);
    SendMessageW(g_hPanel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    init_image_lists();
    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_LIST, hi, NULL);
    SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    SendMessageW(g_hList, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)g_himlSmall);
    SendMessageW(g_hList, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)g_himlLarge);
    LVCOLUMNW col; ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = L"名称"; col.cx = 300; col.iSubItem = 0;
    SendMessageW(g_hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = L"路径（相对工具箱根目录）"; col.cx = 500; col.iSubItem = 1;
    SendMessageW(g_hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    g_oldList = (WNDPROC)SetWindowLongPtrW(g_hList, GWLP_WNDPROC, (LONG_PTR)ListProc);

    g_hStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_STATUS, hi, NULL);
    SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static void reload_all(void)
{
    load_config();
    cache_load();                      /* 磁盘图标缓存：命中项首屏直接就有图标 */
    fill_panels();
    SetWindowTextW(g_hSearch, L"");
    refresh_list();
    SetWindowTextW(g_hMain, g_appTitle);
}

/* 列表右键菜单：有选中条目时针对该条目，空白处也能用（新增/重载） */
static void show_list_menu(BOOL blank)
{
    POINT pt;
    HMENU m = CreatePopupMenu();
    int idx = blank ? -1 : selected_item();
    GetCursorPos(&pt);
    if (idx >= 0) {
        AppendMenuW(m, MF_STRING, IDM_RUN,    L"启动");
        AppendMenuW(m, MF_STRING, IDM_RUNAS,  L"以管理员身份运行");
        AppendMenuW(m, MF_STRING, IDM_FOLDER, L"打开所在文件夹");
        AppendMenuW(m, MF_STRING, IDM_COPY,   L"复制完整路径");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_EDIT,   L"编辑此条目…");
        AppendMenuW(m, MF_STRING, IDM_DEL,    L"删除此条目");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_ADD,     L"添加工具…");
    AppendMenuW(m, MF_STRING, IDM_IMPORT,  L"导入配置…");
    AppendMenuW(m, MF_STRING, IDM_SCAN,    L"从文件夹批量新建…");
    AppendMenuW(m, MF_STRING, IDM_EXPORT,  L"导出配置…");
    AppendMenuW(m, MF_STRING, IDM_RELOAD,  L"重新载入配置");
    AppendMenuW(m, MF_STRING, IDM_OPENDIR, L"打开工具箱目录");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hMain, NULL);
    DestroyMenu(m);
}

/* ---------- 主窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hwnd;
        make_font();
        build_menu(hwnd);
        create_children(hwnd);
        layout(hwnd);
        SetWindowTextW(hwnd, APP_NAME);
        DragAcceptFiles(hwnd, TRUE);
        /* 先把窗口显示出来：配置解析、图标、文件检查全部排在窗口出现之后（点开即见界面） */
        PostMessageW(hwnd, WM_APP_LOAD, 0, 0);
        return 0;

    case WM_SIZE: layout(hwnd); return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 820; mmi->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_DROPFILES: {
        static WCHAR dropped[64][MAX_PATH * 2];
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0), i, k = 0;
        for (i = 0; i < n && k < 64; i++) {
            if (DragQueryFileW(hd, i, dropped[k], MAX_PATH * 2)) k++;
        }
        DragFinish(hd);
        if (k) add_many((const WCHAR (*)[MAX_PATH * 2])dropped, (int)k);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_SEARCH && code == EN_CHANGE) { refresh_list(); return 0; }
        if (id == IDC_ADDBTN && code == BN_CLICKED) { do_add_tools(); return 0; }
        if (id == IDC_PANEL && code == LBN_SELCHANGE) {
            int s = (int)SendMessageW(g_hPanel, LB_GETCURSEL, 0, 0);
            g_panelSel = (s < 0) ? 0 : s;
            refresh_list(); return 0;
        }
        switch (id) {
        case IDM_RUN:     do_launch(selected_item(), 0); return 0;
        case IDM_RUNAS:   do_launch(selected_item(), 1); return 0;
        case IDM_FOLDER:  open_folder_of(selected_item()); return 0;
        case IDM_COPY:    copy_path_of(selected_item()); return 0;
        case IDM_ADD:     do_add_tools(); return 0;
        case IDM_EDIT:    do_edit_item(); return 0;
        case IDM_DEL:     do_del_item(); return 0;
        case IDM_NEW:     do_new_config(); return 0;
        case IDM_IMPORT:  do_import(); return 0;
        case IDM_EXPORT:  do_export(); return 0;
        case IDM_SCAN:    do_scan_folder(); return 0;
        case IDM_RELOAD:  reload_all(); return 0;
        case IDM_OPENDIR: ShellExecuteW(hwnd, L"open", g_root, NULL, NULL, SW_SHOWNORMAL); return 0;
        case IDM_README: {
            WCHAR p[MAX_PATH * 2];
            join_path(p, MAX_PATH * 2, g_root, L"使用说明.txt");
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
                ShellExecuteW(hwnd, L"open", p, NULL, g_root, SW_SHOWNORMAL);
            else
                MessageBoxW(hwnd, L"同目录下没有 使用说明.txt。", APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                APP_NAME L" " APP_VER L"\n\n"
                L"从 Rolan 配置迁移生成的便携启动器，单文件 exe、免安装、不写注册表。\n\n"
                L"· 左侧：面板分类　·　顶部：关键字搜索（名称/路径/面板），回车启动第一条\n"
                L"· 双击列表项或回车启动；右键可“以管理员身份运行/打开所在文件夹/复制完整路径”\n"
                L"· 【添加工具】右上角按钮 / 工具菜单 / 右键菜单，也可以把文件直接拖进窗口\n"
                L"    添加时可新建面板（面板框里直接输入新名字）；选中条目可编辑、删除（Del 键）\n"
                L"· 配置为同目录 tools_utf8.txt（UTF-8），界面里的增删改会直接写回该文件，\n"
                L"  首次写回前自动备份为 tools_utf8.txt.bak；路径相对本程序所在目录，\n"
                L"  所以整个工具箱换盘符、换电脑都不会失效\n\n"
                L"· 文件菜单里还能：新建空配置（清空）/ 导入配置（支持 Rolan 的 cfg、rcb）/\n"
                L"  从文件夹批量新建（扫描目录里的 exe、bat、ps1 建一个面板）/ 导出配置（另存为 txt）\n\n"
                L"Ctrl+1 列表视图(带小图标)　·　Ctrl+2 图标视图(大图标)　·　Ctrl+N 添加工具\n"
                L"F5 重载　·　Esc 清空搜索　·　在 tools_utf8.txt 里写一行 #VIEW=icon 可把大图标视图设为默认。",
                APP_NAME, MB_ICONINFORMATION);
            return 0;
        case IDM_VIEW_LIST: set_view(0); return 0;
        case IDM_VIEW_ICON: set_view(1); return 0;
        case IDM_EXIT: DestroyWindow(hwnd); return 0;
        }
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == IDC_LIST) {
            if (nh->code == NM_DBLCLK) {
                /* 只有真的双击在条目上才启动；空白处双击不再误启动第一条 */
                LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lp;
                if (ia->iItem < 0 || ia->iItem >= g_views) return 0;
                do_launch(g_view[ia->iItem], 0);
                return 0;
            }
            if (nh->code == LVN_KEYDOWN) {          /* 更稳的回车捕获（通知形式，不依赖子类化） */
                LPNMLVKEYDOWN kd = (LPNMLVKEYDOWN)lp;
                if (kd->wVKey == VK_RETURN) {
                    int idx = selected_item();
                    if (idx < 0 && g_views > 0) idx = g_view[0];
                    do_launch(idx, 0);
                    return 0;
                }
                if (kd->wVKey == VK_DELETE) { do_del_item(); return 0; }
            }
            if (nh->code == NM_RETURN || nh->code == LVN_ITEMACTIVATE) {
                LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lp;
                int idx = (nh->code == LVN_ITEMACTIVATE && ia && ia->iItem >= 0 && ia->iItem < g_views)
                          ? g_view[ia->iItem] : selected_item();
                if (idx >= 0) do_launch(idx, 0);
                return 0;
            }
            if (nh->code == NM_CUSTOMDRAW) {
                LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int i = (int)cd->nmcd.lItemlParam;
                    if (i >= 0 && i < g_count && g_items[i].missing) {
                        cd->clrText = RGB(200, 0, 0);
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (nh->code == NM_RCLICK) {
                LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lp;
                /* 先按右键落点决定选中项，空白处则给“添加工具”菜单 */
                if (ia && ia->iItem >= 0 && ia->iItem < g_views) {
                    LVITEMW it; ZeroMemory(&it, sizeof(it));
                    it.mask = LVIF_STATE; it.state = LVIS_SELECTED | LVIS_FOCUSED;
                    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                    SendMessageW(g_hList, LVM_SETITEMSTATE, ia->iItem, (LPARAM)&it);
                    show_list_menu(FALSE);
                } else show_list_menu(TRUE);
                return 0;
            }
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        if ((HWND)wp == g_hList) return 0;      /* 由 NM_RCLICK 处理，避免弹两次 */
        show_list_menu(selected_item() < 0);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)g_hBg;

    case WM_APP_LOAD:
        reload_all();
        if (g_viewPref && g_viewMode == 0) set_view(1);
        layout(hwnd);
        InvalidateRect(g_hList, NULL, FALSE);
        UpdateWindow(g_hList);              /* 先出一屏内容，再开始后台补图标 */
        UpdateWindow(g_hStatus);
        return 0;

    case WM_APP_STEP: {                 /* 一小步后台工作 + 强制重绘，保证界面不卡死 */
        int more = 0;
        if (g_checkPos < g_count) { check_files_chunk(hwnd); more = 1; }
        if (g_iconPos < g_views)  { fill_icons_chunk(hwnd);  more = 1; }
        UpdateWindow(g_hList);                       /* 关键：把列表画出来，别让重绘被消息洪水饿死 */
        set_status();
        UpdateWindow(g_hStatus);
        if (g_checkPos < g_count || g_iconPos < g_views) {
            PostMessageW(hwnd, WM_APP_STEP, 0, 0);
        } else {
            cache_save();                            /* 图标全就位 → 落盘缓存 */
            InvalidateRect(g_hList, NULL, FALSE);
            UpdateWindow(g_hList);
        }
        (void)more;
        return 0;
    }

    case WM_DESTROY:
        cache_save();                       /* 万一图标还没填完就退出，也把已有的存下来 */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- 自检模式（用于非图形环境验证） ---------- */
static void write_report(const WCHAR *outfile, const WCHAR *rep)
{
    HANDLE h;
    DWORD wr;
    char *utf8;
    int len;
    if (!outfile || !*outfile) return;
    h = CreateFileW(outfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    len = WideCharToMultiByte(CP_UTF8, 0, rep, -1, NULL, 0, NULL, NULL);
    utf8 = (char *)malloc(len + 1);
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, rep, -1, utf8, len, NULL, NULL);
        WriteFile(h, utf8, len - 1, &wr, NULL);
        free(utf8);
    }
    CloseHandle(h);
}

static void selftest(const WCHAR *outfile, const WCHAR *kw)
{
    static WCHAR rep[128 * 1024];
    int n = 0;
    int miss = 0, shown = 0;
    for (int i = 0; i < g_count; i++) if (g_items[i].missing) miss++;
    for (int i = 0; i < g_count; i++) {
        if (kw && *kw) {
            if (!StrStrIW(g_items[i].title, kw) && !StrStrIW(g_items[i].rel, kw) &&
                !StrStrIW(g_items[i].panel, kw)) continue;
        }
        shown++;
    }
    n += _snwprintf(rep, ARRAYSIZE(rep), L"标题=%s\r\n总条目=%d\r\n面板数=%d\r\n配置文件=%s\r\n",
                    g_appTitle, g_count, g_panelN, g_cfgPath);
    for (int j = 0; j < g_panelN; j++) {
        int c = 0;
        for (int i = 0; i < g_count; i++) if (!wcscmp(g_items[i].panel, g_panels[j])) c++;
        n += _snwprintf(rep + n, ARRAYSIZE(rep) - (size_t)n, L"面板[%s]=%d\r\n", g_panels[j], c);
    }
    n += _snwprintf(rep + n, ARRAYSIZE(rep) - (size_t)n, L"缺失条目=%d\r\n搜索'%s'命中=%d\r\n根目录=%s\r\n",
                    miss, kw ? kw : L"", shown, g_root);
    if (g_count > 0) {
        n += _snwprintf(rep + n, ARRAYSIZE(rep) - (size_t)n, L"示例-首条=%s | %s\r\n",
                        g_items[0].title, g_items[0].full);
        n += _snwprintf(rep + n, ARRAYSIZE(rep) - (size_t)n, L"示例-末条=%s | %s\r\n",
                        g_items[g_count - 1].title, g_items[g_count - 1].full);
    }
    write_report(outfile, rep);
}

/* 全量导出当前解析结果，便于自动化比对（面板|名称|相对路径|参数） */
static void dump_items(const WCHAR *outfile)
{
    static WCHAR rep[512 * 1024];
    int n = 0;
    n += _snwprintf(rep, ARRAYSIZE(rep), L"COUNT=%d\r\n", g_count);
    for (int i = 0; i < g_count && n < (int)ARRAYSIZE(rep) - 2048; i++) {
        n += _snwprintf(rep + n, ARRAYSIZE(rep) - (size_t)n, L"%d|%s|%s|%s|%s\r\n",
                        i, g_items[i].panel, g_items[i].title, g_items[i].rel, g_items[i].args);
    }
    write_report(outfile, rep);
}

/* ---------- 性能基准（--bench） ---------- */
static void bench(const WCHAR *outfile)
{
    LARGE_INTEGER fq, t0, t1, t2, t3, t4;
    static WCHAR rep[8192];
    int n = 0, ic = 0;
    double ms1, ms2, ms3, ms4, msCheck;
    LARGE_INTEGER tc;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    load_config();
    QueryPerformanceCounter(&t1);
    check_all_files();
    QueryPerformanceCounter(&tc);
    msCheck = (double)(tc.QuadPart - t1.QuadPart) * 1000.0 / fq.QuadPart;
    for (int k = 0; k < 1000; k++)
        for (int i = 0; i < g_count; i++) item_match(i, L"exe");
    QueryPerformanceCounter(&t2);
    for (int k = 0; k < 200; k++)
        for (int i = 0; i < g_count; i++) { WCHAR key[80]; icon_key_for(i, key); }
    QueryPerformanceCounter(&t3);
    init_image_lists();
    for (int i = 0; i < g_count; i++) {
        WCHAR key[80];
        icon_key_for(i, key);
        if (icc_get(key) < 0) { icon_for_item(i); ic++; }
    }
    QueryPerformanceCounter(&t4);
    ms1 = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    ms2 = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / fq.QuadPart;
    ms3 = (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / fq.QuadPart;
    ms4 = (double)(t4.QuadPart - t3.QuadPart) * 1000.0 / fq.QuadPart;
    n += _snwprintf(rep, ARRAYSIZE(rep), L"[--bench] 条目=%d 面板=%d 根目录=%s\r\n", g_count, g_panelN, g_root);
    n += _snwprintf(rep + n, ARRAYSIZE(rep) - (size_t)n,
        L"1) 读配置+拼路径（不含文件检查）  : %.2f ms\r\n"
        L"1b) 全量检查文件是否存在          : %.2f ms（界面上这两步都在窗口出现之后分块做）\r\n"
        L"2) 搜索过滤 1000 轮(共%d条)      : %.2f ms  → 单轮 %.3f ms\r\n"
        L"3) 图标缓存键 200 轮             : %.2f ms\r\n"
        L"4) 首次提取图标 %d 个             : %.2f ms  → 平均 %.2f ms/个(之后命中缓存)\r\n"
        L"5) 图标去重后实际种类             : %d\r\n",
        ms1, msCheck, g_count * 1000, ms2, ms2 / 1000.0, ms3, ic, ms4, ic ? ms4 / ic : 0.0, g_iccN);
    write_report(outfile, rep);
}

/* 图标缓存专项：--icons 报告文件（冷/热各跑一次对比） */
static void icon_bench(const WCHAR *outfile)
{
    LARGE_INTEGER fq, t0, t1, t2, t3;
    static WCHAR rep[4096];
    double msLoad, msFill;
    int i, itemHit = 0, itemMiss = 0, cacheN = 0, t0ms = GetTickCount();
    QueryPerformanceFrequency(&fq);
    load_config();
    check_all_files();
    init_image_lists();
    QueryPerformanceCounter(&t0);
    cache_load();
    QueryPerformanceCounter(&t1);
    cacheN = g_iccN;
    QueryPerformanceCounter(&t2);
    for (i = 0; i < g_count; i++) {
        WCHAR k[80];
        icon_key_for(i, k);
        if (icc_get(k) >= 0) { itemHit++; continue; }
        icon_for_item(i);
        itemMiss++;
    }
    QueryPerformanceCounter(&t3);
    msLoad = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    msFill = (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / fq.QuadPart;
    cache_save();
    _snwprintf(rep, ARRAYSIZE(rep) - 1,
        L"[--icons] 条目=%d  缓存文件里的图标数=%d\r\n"
        L"缓存命中（直接建图标，零磁盘/Shell 调用）= %d 条\r\n"
        L"需要重新提取                              = %d 条\r\n"
        L"读缓存+建图标耗时 = %.2f ms\r\n"
        L"提取未命中图标耗时 = %.2f ms%s\r\n"
        L"整体（含配置解析/文件检查）= %d ms，根目录=%s\r\n",
        g_count, cacheN > 0 ? cacheN - 1 : 0, itemHit, itemMiss, msLoad, msFill,
        itemMiss ? L"" : L"（全部命中，没有做任何图标提取）", (int)(GetTickCount() - t0ms), g_root);

    rep[ARRAYSIZE(rep) - 1] = 0;
    write_report(outfile, rep);
}

/* 隐藏测试用：导入 / 导出 / 扫描 / 新建空配置 */
static void cli_import(const WCHAR *file, const WCHAR *report, int replace)
{
    static WCHAR rep[4096];
    WCHAR newTitle[128];
    const WCHAR *kind;
    int dup = 0, added;
    load_config();
    added = import_file_core(file, replace, &dup, &kind, newTitle, 128);
    if (added < 0) { _snwprintf(rep, 4095, L"导入失败：读不出内容\r\n"); rep[4095] = 0; write_report(report, rep); return; }
    if (newTitle[0] && replace) copy_field(g_appTitle, 128, newTitle, wcslen(newTitle));
    save_config();
    _snwprintf(rep, 4095, L"类型=%s\r\n新增=%d 重复=%d 现有=%d\r\n标题=%s\r\n配置=%s\r\n",
               kind, added, dup, g_count, g_appTitle, g_cfgPath);
    rep[4095] = 0;
    write_report(report, rep);
}
static void cli_export(const WCHAR *file, const WCHAR *report, int onlyVisible)
{
    static WCHAR rep[4096];
    int ok;
    load_config();
    ok = save_config_ex(file, CP_UTF8, onlyVisible, 0);
    _snwprintf(rep, 4095, L"导出=%s 条数=%d 目标=%s\r\n", ok ? L"成功" : L"失败",
               onlyVisible ? g_views : g_count, file);
    rep[4095] = 0;
    write_report(report, rep);
}
static void cli_scan(const WCHAR *dir, const WCHAR *report, int recurse)
{
    static WCHAR rep[4096];
    WCHAR panel[64];
    const WCHAR *b = base_name(dir);
    int dup = 0, added;
    load_config();
    copy_field(panel, 64, b && *b ? b : L"新面板", wcslen(b && *b ? b : L"新面板"));
    added = scan_folder_rec(dir, panel, recurse, &dup, 0);
    if (added > 0) save_config();
    _snwprintf(rep, 4095, L"面板=%s 新增=%d 重复=%d 现有=%d 递归=%d\r\n", panel, added, dup, g_count, recurse);
    rep[4095] = 0;
    write_report(report, rep);
}
static void cli_newcfg(const WCHAR *report)
{
    static WCHAR rep[2048];
    int was;
    load_config();
    was = g_count;
    g_count = 0;
    save_config();
    _snwprintf(rep, 2047, L"清空前=%d 清空后=%d 配置=%s\r\n", was, g_count, g_cfgPath);
    rep[2047] = 0;
    write_report(report, rep);
}

/* 隐藏测试用：--additem "面板|名称|相对路径|参数" [报告文件]  走与界面相同的添加+保存逻辑 */
static void cli_additem(const WCHAR *spec, const WCHAR *outfile)
{
    WCHAR buf[2048], *f[4], *q;
    int nf = 1, dup, ok;
    static WCHAR rep[4096];
    wcsncpy(buf, spec, 2047); buf[2047] = 0;
    f[0] = buf; f[1] = f[2] = f[3] = (WCHAR *)L"";
    for (q = buf; *q && nf < 4; q++) if (*q == L'|') { *q = 0; f[nf++] = q + 1; }
    load_config();
    dup = item_exists(f[0], f[2]);
    ok = dup ? 0 : add_item_raw(f[0], f[1], f[2], f[3]);
    if (!dup) save_config();
    _snwprintf(rep, 4095, L"添加=%s 重复=%d 写入=%s 条目数=%d 配置=%s\r\n",
               ok ? L"是" : L"否", dup, (ok && !dup) ? L"是" : L"未写入", g_count, g_cfgPath);
    rep[4095] = 0;
    write_report(outfile, rep);
}

/* ---------- 入口 ---------- */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int show)
{
    (void)hp; (void)show;
    /* 兼容高 DPI */
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PSetDPIAware)(void);
        PSetDPIAware p = (PSetDPIAware)GetProcAddress(u32, "SetProcessDPIAware");
        if (p) p();
    }
    /* 自身目录 */
    GetModuleFileNameW(NULL, g_root, MAX_PATH);
    WCHAR *sl = wcsrchr(g_root, L'\\');
    if (sl) *sl = 0;

    g_hBg = CreateSolidBrush(RGB(244, 246, 249));

    /* 命令行开关: --selftest [报告文件] */
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR out[MAX_PATH * 2] = L"", kw[128] = L"";
    if (argc > 1 && (!_wcsicmp(argv[1], L"--icons") || !_wcsicmp(argv[1], L"-icons"))) {
        if (argc > 2) wcsncpy(out, argv[2], MAX_PATH * 2 - 1);
        icon_bench(out);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && !_wcsicmp(argv[1], L"--import")) {
        if (argc > 3) wcsncpy(out, argv[3], MAX_PATH * 2 - 1);
        cli_import(argc > 2 ? argv[2] : L"", out, (argc > 4 && !_wcsicmp(argv[4], L"replace")));
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && !_wcsicmp(argv[1], L"--export")) {
        if (argc > 3) wcsncpy(out, argv[3], MAX_PATH * 2 - 1);
        cli_export(argc > 2 ? argv[2] : L"", out, 0);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && !_wcsicmp(argv[1], L"--scan")) {
        if (argc > 3) wcsncpy(out, argv[3], MAX_PATH * 2 - 1);
        cli_scan(argc > 2 ? argv[2] : L"", out, (argc > 4 && !_wcsicmp(argv[4], L"r")));
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && !_wcsicmp(argv[1], L"--newcfg")) {
        if (argc > 2) wcsncpy(out, argv[2], MAX_PATH * 2 - 1);
        cli_newcfg(out);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && !_wcsicmp(argv[1], L"--additem")) {
        if (argc > 3) wcsncpy(out, argv[3], MAX_PATH * 2 - 1);
        cli_additem(argc > 2 ? argv[2] : L"", out);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && (!_wcsicmp(argv[1], L"--dump") || !_wcsicmp(argv[1], L"-dump"))) {
        if (argc > 2) wcsncpy(out, argv[2], MAX_PATH * 2 - 1);
        load_config();
        dump_items(out);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && (!_wcsicmp(argv[1], L"--run") || !_wcsicmp(argv[1], L"-run"))) {
        /* 排障用：不开窗口，直接走界面同样的启动逻辑启动第 N 条（0 开始）*/
        int n = (argc > 2) ? _wtoi(argv[2]) : 0;
        load_config();
        check_all_files();
        if (n >= 0 && n < g_count) do_launch(n, 0);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1 && (!_wcsicmp(argv[1], L"--bench") || !_wcsicmp(argv[1], L"-bench"))) {
        if (argc > 2) wcsncpy(out, argv[2], MAX_PATH * 2 - 1);
        bench(out);
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc > 1) {
        if (!_wcsicmp(argv[1], L"--selftest") || !_wcsicmp(argv[1], L"-selftest")) {
            if (argc > 2) wcsncpy(out, argv[2], MAX_PATH * 2 - 1);
            if (argc > 3) wcsncpy(kw, argv[3], 127);
            load_config();
            check_all_files();
            selftest(out, kw);
            if (argv) LocalFree(argv);
            return 0;
        }
    }
    if (argv) LocalFree(argv);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"ForensicToolboxLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hbrBackground = g_hBg;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 660,
        NULL, NULL, hi, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    ACCEL acc[4];
    HACCEL hac = NULL;
    acc[0].fVirt = FCONTROL | FVIRTKEY; acc[0].key = '1'; acc[0].cmd = IDM_VIEW_LIST;
    acc[1].fVirt = FCONTROL | FVIRTKEY; acc[1].key = '2'; acc[1].cmd = IDM_VIEW_ICON;
    acc[2].fVirt = FVIRTKEY;            acc[2].key = VK_F5; acc[2].cmd = IDM_RELOAD;
    acc[3].fVirt = FCONTROL | FVIRTKEY; acc[3].key = 'N';  acc[3].cmd = IDM_ADD;
    hac = CreateAcceleratorTableW(acc, 4);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (hac && TranslateAcceleratorW(hwnd, hac, &m)) continue;
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
