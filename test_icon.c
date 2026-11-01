/* 调试图标渲染:把多种状态的图标画到一张 BMP 上输出 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define level_color level_color_impl
static COLORREF level_color_impl(int pct)
{
    if (pct < 0)  return RGB(120, 120, 128);
    if (pct <= 20) return RGB(255, 69, 58);
    if (pct <= 60) return RGB(255, 214, 10);
    return RGB(48, 209, 88);
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

/* 与主程序相同的图标内容,直接画到目标 DC(便于检查) */
static void draw_icon(HDC dc, int ox, int oy, int size, int pct, int charging)
{
    int bx = ox + size * 2 / 32, by = oy + size * 9 / 32;
    int bw = size * 23 / 32, bh = size * 14 / 32;
    int capw = size * 3 / 32, caph = size * 6 / 32;
    COLORREF frame = (pct < 0) ? RGB(90, 90, 96) : RGB(235, 235, 240);
    COLORREF fill  = level_color_impl(pct);

    round_rect(dc, bx, by, bx + bw, by + bh, size / 8, RGB(24, 24, 26), frame);
    round_rect(dc, bx + bw, by + (bh - caph) / 2, bx + bw + capw, by + (bh + caph) / 2,
               size / 16, frame, frame);
    if (pct > 0) {
        int pad = size * 3 / 32;
        int fw = (bw - 2 * pad) * pct / 100;
        if (fw < size / 16) fw = size / 16;
        round_rect(dc, bx + pad, by + pad, bx + pad + fw, by + bh - pad, size / 12, fill, fill);
    }
    if (charging) {
        POINT bolt[6] = {
            { ox + size * 18 / 32, oy + size * 10 / 32 }, { ox + size * 12 / 32, oy + size * 22 / 32 },
            { ox + size * 16 / 32, oy + size * 22 / 32 }, { ox + size * 14 / 32, oy + size * 30 / 32 },
            { ox + size * 21 / 32, oy + size * 18 / 32 }, { ox + size * 17 / 32, oy + size * 18 / 32 },
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
    if (pct < 0) {
        HPEN p = CreatePen(PS_SOLID, size / 16 > 0 ? size / 16 : 1, RGB(150, 150, 156));
        HPEN op = (HPEN)SelectObject(dc, p);
        MoveToEx(dc, bx + 2, by + bh - 2, NULL);
        LineTo(dc, bx + bw - 2, by + 2);
        SelectObject(dc, op);
        DeleteObject(p);
    }
}

int main(void)
{
    int states[5][2] = { { 85, 0 }, { 55, 0 }, { 15, 0 }, { 40, 1 }, { -1, 0 } };
    int sizes[3] = { 16, 32, 64 };
    int W = 5 * 100, H = 3 * 110 + 40;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, W, H);
    SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 36));   /* 深色任务栏底色 */
    RECT rc = { 0, 0, W, H };
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    for (int s = 0; s < 3; s++)
        for (int i = 0; i < 5; i++)
            draw_icon(dc, i * 100 + 10, s * 110 + 10, sizes[s], states[i][0], states[i][1]);

    /* 标注 */
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(200, 200, 200));
    const char *labels[5] = { "85%", "55%", "15%", "40%+charge", "offline" };
    for (int i = 0; i < 5; i++)
        TextOutA(dc, i * 100 + 10, 3 * 110 + 15, labels[i], (int)strlen(labels[i]));

    /* 保存 BMP */
    BITMAPINFOHEADER bih = { sizeof(bih), W, -H, 1, 24, BI_RGB, 0, 0, 0, 0, 0 };
    BITMAPINFO b24;
    ZeroMemory(&b24, sizeof(b24));
    b24.bmiHeader = bih;
    HDC dc24 = CreateCompatibleDC(screen);
    void *bits;
    HBITMAP b24m = CreateDIBSection(dc24, &b24, DIB_RGB_COLORS, &bits, NULL, 0);
    HBRUSH bg2 = CreateSolidBrush(RGB(32, 32, 36));
    SelectObject(dc24, b24m);
    FillRect(dc24, &rc, bg2);
    BitBlt(dc24, 0, 0, W, H, dc, 0, 0, SRCCOPY);

    BITMAPFILEHEADER bfh = { 0x4D42, sizeof(bfh) + sizeof(bih) + (DWORD)(W * H * 3), 0, 0, sizeof(bfh) + sizeof(bih) };
    FILE *f = fopen("icon_preview.bmp", "wb");
    fwrite(&bfh, 1, sizeof(bfh), f);
    fwrite(&bih, 1, sizeof(bih), f);
    fwrite(bits, 1, (size_t)W * H * 3, f);
    fclose(f);
    printf("saved icon_preview.bmp\n");
    return 0;
}
