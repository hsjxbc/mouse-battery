/*
 * MouseBattery — 迈从 A5/AX5 系列鼠标电量任务栏监视器
 *
 * 协议来源:逆向 MCHOSE DriverCore.exe v2.0.6.6(REA 静态分析)
 *   - HID 设备:VID 0x2023,PID 见 g_pids[],Feature 报告长度须为 65
 *     (官方 GetTargetDev 以 VID+PID+GetMaxFeatureReportLength==65 匹配)
 *   - 查询:65 字节 Feature 报告,buf[3]=0x02, buf[4]=0x02, buf[6]=0x83
 *     HidD_SetFeature 后等待,再 HidD_GetFeature
 *   - 响应:buf[1] = 0xA1 表示命令已执行(0xA0 为未就绪回显帧,电量字段为 0)
 *           buf[7] = 充电标志(1=充电中), buf[8] = 电量百分比 0..100
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

/* ---------------- 协议常量 ---------------- */
#define MOUSE_VID            0x2023
#define REPORT_LEN           65
#define CMD_BATTERY_HI_IDX   4
#define CMD_BATTERY_HI       0x02
#define CMD_BATTERY_CODE_IDX 6
#define CMD_BATTERY_CODE     0x83
#define CMD_PAGE_IDX         3
#define CMD_PAGE             0x02
#define RSP_SEQ_IDX          1
#define RSP_SEQ_READY        0xA1   /* 命令已执行;0xA0 = 未就绪/回显,该帧电量恒为 0 */
#define RSP_CHARGING_IDX     7
#define RSP_PERCENT_IDX      8

/* 查询重试:退避等待,避免把未就绪回显帧误判为 0% */
static const int g_retry_delays[] = { 50, 100, 200, 400 };

static const USHORT g_pids[] = {
    0xF017, /* A5 Pro 有线     */
    0xF019, /* A5 Pro Max 有线 */
    0xF013, /* A5 系接收器     */
    0xF015, /* A5 系通配接收器 */
    0xF026, /* AX5 Pro 有线    */
    0xF022, /* AX5 Pro Max 有线*/
    0xF024, /* AX5 接收器      */
};

static const struct { USHORT pid; const wchar_t *name; } g_names[] = {
    { 0xF017, L"MCHOSE A5 Pro (有线)" },
    { 0xF019, L"MCHOSE A5 Pro Max (有线)" },
    { 0xF013, L"MCHOSE A5 系列 (2.4G 接收器)" },   /* A5 Pro / A5 Pro Max 共用 */
    { 0xF015, L"MCHOSE 通配接收器" },
    { 0xF026, L"AX5 Pro (有线)" },
    { 0xF022, L"AX5 Pro Max (有线)" },
    { 0xF024, L"AX5 系列 (2.4G 接收器)" },
};

/* ---------------- 全局状态 ---------------- */
#define WM_TRAYICON      (WM_APP + 1)
#define IDT_REFRESH      1001
#define REFRESH_MS       60000
#define IDM_REFRESH      2001
#define IDM_AUTOSTART    2002
#define IDM_EXIT         2003

static HWND              g_hwnd;        /* 隐藏消息窗口 */
static HWND              g_popup;       /* 详情弹窗 */
static NOTIFYICONDATAW   g_nid;
static int               g_percent   = -1;   /* -1 = 未知 */
static int               g_charging  = 0;
static int               g_connected = 0;
static int               g_stale     = 0;   /* 本轮未取得有效响应 */
static wchar_t           g_devname[64] = L"未检测到设备";
static HICON             g_icon      = NULL;
static UINT              g_taskbarRestart;

/* ---------------- 电量查询 ---------------- */

static int pid_supported(USHORT pid)
{
    for (size_t i = 0; i < sizeof(g_pids) / sizeof(g_pids[0]); i++)
        if (g_pids[i] == pid) return 1;
    return 0;
}

static const wchar_t *pid_name(USHORT pid)
{
    for (size_t i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++)
        if (g_names[i].pid == pid) return g_names[i].name;
    return L"MCHOSE 鼠标";
}

/* 对单个已打开的句柄执行电量查询
 * 返回 1 = 拿到有效数据,0 = 设备在但未拿到有效响应 */
static int query_handle(HANDLE h, int *percent, int *charging)
{
    BYTE buf[REPORT_LEN];
    int n = (int)(sizeof(g_retry_delays) / sizeof(g_retry_delays[0]));

    for (int attempt = 0; attempt < n; attempt++) {
        ZeroMemory(buf, sizeof(buf));
        buf[CMD_PAGE_IDX]         = CMD_PAGE;
        buf[CMD_BATTERY_HI_IDX]   = CMD_BATTERY_HI;
        buf[CMD_BATTERY_CODE_IDX] = CMD_BATTERY_CODE;
        if (!HidD_SetFeature(h, buf, REPORT_LEN))
            continue;

        /* 接收器(尤其是鼠标休眠或与官方驱动同时访问时)需要更长的处理时间 */
        Sleep(g_retry_delays[attempt]);

        ZeroMemory(buf, sizeof(buf));
        buf[CMD_PAGE_IDX] = CMD_PAGE;   /* 与官方驱动一致:GetFeature 前置选择页 */
        if (!HidD_GetFeature(h, buf, REPORT_LEN))
            continue;

        /* 只有 seq=0xA1 才是命令已执行的响应;0xA0 是未就绪回显,其电量字段恒为 0 */
        if (buf[RSP_SEQ_IDX] == RSP_SEQ_READY && buf[RSP_PERCENT_IDX] <= 100) {
            *percent  = buf[RSP_PERCENT_IDX];
            *charging = (buf[RSP_CHARGING_IDX] == 1);
            return 1;
        }
    }
    return 0;
}

/* 枚举 HID 类设备,找到鼠标并查询电量
 * 返回 1 = 有效数据,0 = 有设备但无有效响应,-1 = 没有支持的设备 */
static int query_mouse(int *percent, int *charging, wchar_t *name, size_t name_len)
{
    GUID hid_guid;
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD idx;
    int device_seen = 0, got_data = 0;

    HidD_GetHidGuid(&hid_guid);
    devs = SetupDiGetClassDevsW(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return -1;

    ifd.cbSize = sizeof(ifd);
    for (idx = 0; !got_data && SetupDiEnumDeviceInterfaces(devs, NULL, &hid_guid, idx, &ifd); idx++) {
        DWORD need = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        HANDLE h;
        HIDD_ATTRIBUTES attr;
        HIDP_CAPS caps;
        PHIDP_PREPARSED_DATA ppd = NULL;
        int has_caps = 0;

        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, NULL, 0, &need, NULL);
        if (need == 0) continue;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, need, NULL, NULL)) {
            free(detail);
            continue;
        }

        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            /* 部分系统上只读也能完成 Feature 查询 */
            h = CreateFileW(detail->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
        if (h != INVALID_HANDLE_VALUE) {
            attr.Size = sizeof(attr);
            if (HidD_GetAttributes(h, &attr) &&
                attr.VendorID == MOUSE_VID && pid_supported(attr.ProductID)) {
                /* 与官方 GetTargetDev 一致:Feature 报告长度必须匹配命令长度 */
                ZeroMemory(&caps, sizeof(caps));
                if (HidD_GetPreparsedData(h, &ppd)) {
                    HidP_GetCaps(ppd, &caps);
                    HidD_FreePreparsedData(ppd);
                    has_caps = 1;
                }
                if (has_caps && caps.FeatureReportByteLength >= REPORT_LEN) {
                    device_seen = 1;
                    if (query_handle(h, percent, charging)) {
                        got_data = 1;
                        if (name) {
                            wcsncpy(name, pid_name(attr.ProductID), name_len - 1);
                            name[name_len - 1] = L'\0';
                        }
                    }
                }
            }
            CloseHandle(h);
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devs);

    if (got_data) return 1;
    return device_seen ? 0 : -1;
}

/* ---------------- 图标绘制 ---------------- */

static COLORREF level_color(int pct)
{
    if (pct < 0)  return RGB(120, 120, 128);            /* 未知:灰 */
    if (pct <= 20) return RGB(255, 69, 58);             /* 红 */
    if (pct <= 60) return RGB(255, 214, 10);            /* 黄 */
    return RGB(48, 209, 88);                            /* 绿 */
}

static void round_rect(HDC dc, int l, int t, int r, int b, int rad, COLORREF fill, COLORREF pen)
{
    HPEN hp = CreatePen(PS_SOLID, 1, pen);
    HBRUSH hb = CreateSolidBrush(fill);
    HPEN op = (HPEN)SelectObject(dc, hp);
    HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(hb);
    DeleteObject(hp);
}

/* 生成 size x size 的电量图标 */
static HICON make_icon(int size, int pct, int charging)
{
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HDC mask = CreateCompatibleDC(screen);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;   /* 自顶向下 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void *bits;
    HBITMAP color = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, NULL);
    HBITMAP oldC = (HBITMAP)SelectObject(dc, color);
    HBITMAP oldM = (HBITMAP)SelectObject(mask, maskBmp);

    /* 先整图填透明(alpha=0),用 mask 全白(显示)配合颜色位图 alpha 控制 */
    memset(bits, 0, size * size * 4);
    SetBkColor(dc, RGB(0, 0, 0));

    /* mask:黑 = 保留颜色,先全白→全透? 使用 alpha 通道即可,mask 全 0 */
    PatBlt(mask, 0, 0, size, size, WHITENESS);

    /* 电池参数(按 32 基准缩放) */
    int bx = size * 2 / 32, by = size * 9 / 32;
    int bw = size * 23 / 32, bh = size * 14 / 32;
    int capw = size * 3 / 32, caph = size * 6 / 32;
    COLORREF frame = (pct < 0) ? RGB(90, 90, 96) : RGB(235, 235, 240);
    COLORREF fill  = level_color(pct);

    /* 外框 */
    round_rect(dc, bx, by, bx + bw, by + bh, size / 8, RGB(24, 24, 26), frame);
    /* 正极帽 */
    round_rect(dc, bx + bw, by + (bh - caph) / 2, bx + bw + capw, by + (bh + caph) / 2,
               size / 16, frame, frame);

    /* 电量填充 */
    if (pct > 0) {
        int pad = size * 3 / 32;
        int fw = (bw - 2 * pad) * pct / 100;
        if (fw < size / 16) fw = size / 16;
        round_rect(dc, bx + pad, by + pad, bx + pad + fw, by + bh - pad, size / 12, fill, fill);
    }

    /* 充电闪电 */
    if (charging) {
        POINT bolt[6] = {
            { size * 18 / 32, size * 10 / 32 }, { size * 12 / 32, size * 22 / 32 },
            { size * 16 / 32, size * 22 / 32 }, { size * 14 / 32, size * 30 / 32 },
            { size * 21 / 32, size * 18 / 32 }, { size * 17 / 32, size * 18 / 32 },
        };
        HBRUSH bb = CreateSolidBrush(RGB(64, 156, 255));
        HPEN bp = CreatePen(PS_SOLID, 1, RGB(64, 156, 255));
        HBRUSH ob = (HBRUSH)SelectObject(dc, bb);
        HPEN op = (HPEN)SelectObject(dc, bp);
        Polygon(dc, bolt, 6);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(bb);
        DeleteObject(bp);
    }

    /* 未连接:斜杠提示 */
    if (pct < 0) {
        HPEN p = CreatePen(PS_SOLID, size / 16 > 0 ? size / 16 : 1, RGB(150, 150, 156));
        HPEN op = (HPEN)SelectObject(dc, p);
        MoveToEx(dc, bx + 2, by + bh - 2, NULL);
        LineTo(dc, bx + bw - 2, by + 2);
        SelectObject(dc, op);
        DeleteObject(p);
    }

    /* 把已绘制区域的 alpha 置为不透明 */
    DWORD *px = (DWORD *)bits;
    for (int i = 0; i < size * size; i++) {
        DWORD c = px[i] & 0x00FFFFFF;
        if (c != 0) px[i] |= 0xFF000000;
    }

    ICONINFO ii;
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = maskBmp;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    SelectObject(dc, oldC);
    SelectObject(mask, oldM);
    DeleteObject(color);
    DeleteObject(maskBmp);
    DeleteDC(dc);
    DeleteDC(mask);
    ReleaseDC(NULL, screen);
    return icon;
}

/* ---------------- 托盘与 UI ---------------- */

static void update_tray(void)
{
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    HICON icon = make_icon(size * 2, g_percent, g_charging);

    if (g_icon) DestroyIcon(g_icon);
    g_icon = icon;
    g_nid.hIcon = icon;

    if (g_connected && g_percent >= 0)
        _snwprintf(g_nid.szTip, 127, L"%s: %d%%%s", g_devname, g_percent,
                   g_charging ? L" (充电中)" : L"");
    else if (g_connected)
        wcsncpy(g_nid.szTip, L"正在读取鼠标电量...", 127);
    else
        wcsncpy(g_nid.szTip, L"未检测到鼠标", 127);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void refresh_battery(void)
{
    int pct = -1, chg = 0;
    wchar_t name[64];
    int r = query_mouse(&pct, &chg, name, 64);

    if (r == 1) {
        g_percent = pct;
        g_charging = chg;
        g_connected = 1;
        g_stale = 0;
        wcsncpy(g_devname, name, 63);
        g_devname[63] = L'\0';
    } else if (r == 0) {
        /* 设备在但没拿到有效响应:保留上次有效值,避免把未就绪帧显示成 0% */
        g_connected = 1;
        g_stale = 1;
    } else {
        g_percent = -1;
        g_charging = 0;
        g_connected = 0;
        g_stale = 1;
        wcsncpy(g_devname, L"未检测到设备", 63);
    }
    update_tray();
    if (g_popup && IsWindowVisible(g_popup))
        InvalidateRect(g_popup, NULL, TRUE);
}

/* ---------------- 开机启动 ---------------- */
#define REG_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VALUE   L"MouseBattery"

static int autostart_enabled(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, size = 0;
    int ok = (RegQueryValueExW(k, REG_VALUE, NULL, &type, NULL, &size) == ERROR_SUCCESS);
    RegCloseKey(k);
    return ok;
}

static void autostart_set(int enable)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        RegSetValueExW(k, REG_VALUE, 0, REG_SZ, (const BYTE *)path,
                       (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, REG_VALUE);
    }
    RegCloseKey(k);
}

/* ---------------- 详情弹窗 ---------------- */
#define POPUP_W 300
#define POPUP_H 150

static void popup_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    /* 深色卡片背景 */
    HBRUSH bg = CreateSolidBrush(RGB(28, 28, 32));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);

    /* 设备名 */
    HFONT fTitle = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT of = (HFONT)SelectObject(mem, fTitle);
    SetTextColor(mem, RGB(160, 160, 170));
    RECT r1 = { 20, 14, POPUP_W - 20, 36 };
    DrawTextW(mem, g_devname, -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 大字号百分比 */
    HFONT fBig = CreateFontW(-46, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SelectObject(mem, fBig);
    SetTextColor(mem, (g_connected && g_percent >= 0) ? level_color(g_percent) : RGB(110, 110, 118));
    wchar_t big[16];
    if (g_connected && g_percent >= 0) _snwprintf(big, 15, L"%d%%", g_percent);
    else wcsncpy(big, L"--", 15);
    RECT r2 = { 20, 34, POPUP_W - 20, 96 };
    DrawTextW(mem, big, -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 状态文本 */
    SelectObject(mem, fTitle);
    SetTextColor(mem, g_charging ? RGB(64, 156, 255) : RGB(160, 160, 170));
    const wchar_t *st;
    if (!g_connected)          st = L"请将鼠标连接或靠近接收器";
    else if (g_stale && g_percent < 0) st = L"正在读取电量...";
    else if (g_charging)       st = L"⚡ 充电中";
    else                       st = L"放电中";
    RECT r3 = { 20, 96, POPUP_W - 20, 120 };
    DrawTextW(mem, st, -1, &r3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 电量条 */
    int bl = 20, bt = 124, br_ = POPUP_W - 20, bb = 138;
    round_rect(mem, bl, bt, br_, bb, 8, RGB(52, 52, 58), RGB(52, 52, 58));
    if (g_connected && g_percent > 0) {
        int fw = (br_ - bl - 4) * g_percent / 100;
        if (fw < 6) fw = 6;
        round_rect(mem, bl + 2, bt + 2, bl + 2 + fw, bb - 2, 6, level_color(g_percent), level_color(g_percent));
    }

    SelectObject(mem, of);
    DeleteObject(fTitle);
    DeleteObject(fBig);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        popup_paint(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void popup_toggle(void)
{
    if (!g_popup) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = popup_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"MouseBatteryPopup";
        wc.hbrBackground = NULL;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);

        g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  L"MouseBatteryPopup", L"电量",
                                  WS_POPUP, 0, 0, POPUP_W, POPUP_H,
                                  g_hwnd, NULL, wc.hInstance, NULL);
        /* 圆角 */
        HRGN rgn = CreateRoundRectRgn(0, 0, POPUP_W + 1, POPUP_H + 1, 18, 18);
        SetWindowRgn(g_popup, rgn, TRUE);
    }

    if (IsWindowVisible(g_popup)) {
        ShowWindow(g_popup, SW_HIDE);
        return;
    }

    /* 打开前先取一次实时值,避免显示上一次的旧数据 */
    refresh_battery();

    /* 定位到托盘图标上方 */
    RECT tray = { 0 };
    APPBARDATA abd;
    ZeroMemory(&abd, sizeof(abd));
    abd.cbSize = sizeof(abd);
    int wa = ABM_GETTASKBARPOS ? 1 : 1;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) tray = abd.rc;
    (void)wa;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = sw - POPUP_W - 12, y;
    if (tray.top == 0 && tray.bottom < sh)       y = tray.bottom + 8;   /* 任务栏在顶部 */
    else if (tray.top > 0 && tray.bottom >= sh)  y = tray.top - POPUP_H - 8;
    else y = sh - POPUP_H - 48;

    SetWindowPos(g_popup, HWND_TOPMOST, x, y, POPUP_W, POPUP_H,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(g_popup, SW_SHOWNOACTIVATE);
    InvalidateRect(g_popup, NULL, TRUE);
}

/* ---------------- 主窗口 ---------------- */

static void show_menu(void)
{
    POINT pt;
    GetCursorPos(&pt);
    refresh_battery();   /* 先刷新,菜单里显示实时值 */
    HMENU m = CreatePopupMenu();
    wchar_t label[128];
    if (g_connected && g_percent >= 0)
        _snwprintf(label, 127, L"%s  %d%%%s", g_devname, g_percent,
                   g_charging ? L" 充电中" : L"");
    else if (g_connected)
        wcsncpy(label, L"正在读取鼠标电量...", 127);
    else
        wcsncpy(label, L"未检测到设备", 127);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, label);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"立即刷新");
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"开机自启动");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarRestart) {
        /* Explorer 重启后重建图标 */
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        update_tray();
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            popup_toggle();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu();
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == IDT_REFRESH) refresh_battery();
        return 0;
    case WM_DEVICECHANGE:
        refresh_battery();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_REFRESH: refresh_battery(); break;
        case IDM_AUTOSTART: autostart_set(!autostart_enabled()); break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) DestroyIcon(g_icon);
        if (g_popup) DestroyWindow(g_popup);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;

    /* 单实例 */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"MouseBatteryTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"MouseBattery 已在运行", L"提示", MB_ICONINFORMATION);
        return 0;
    }

    g_taskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MouseBatteryMain";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(L"MouseBatteryMain", L"MouseBattery", 0,
                           0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);

    /* 托盘图标 */
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = make_icon(32, -1, 0);
    g_icon = g_nid.hIcon;
    wcsncpy(g_nid.szTip, L"MouseBattery 加载中...", 127);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    /* 首次查询 + 定时器 */
    refresh_battery();
    SetTimer(g_hwnd, IDT_REFRESH, REFRESH_MS, NULL);

    /* 调试:带 -demo 启动时直接展示详情弹窗 */
    if (cmd && wcsstr(cmd, L"-demo"))
        popup_toggle();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return 0;
}
