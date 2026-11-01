/* 电量读取验证工具:与 MouseBattery.exe 使用相同的匹配规则与响应校验
 * 输出每一步的原始报文,便于与官方驱动显示的电量对比
 * 编译: gcc -O2 -mconsole -municode -o test_query.exe test_query.c -lsetupapi -lhid
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOUSE_VID 0x2023
#define REPORT_LEN 65
#define SEQ_IDX 1
#define SEQ_READY 0xA1

static const USHORT g_pids[] = { 0xF017, 0xF019, 0xF013, 0xF015, 0xF026, 0xF022, 0xF024 };
static const int g_delays[] = { 50, 100, 200, 400 };

static int pid_supported(USHORT pid)
{
    for (size_t i = 0; i < sizeof(g_pids) / sizeof(g_pids[0]); i++)
        if (g_pids[i] == pid) return 1;
    return 0;
}

int wmain(void)
{
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO devs = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) { printf("枚举失败 %lu\n", GetLastError()); return 1; }

    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    int found = 0, reported = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = malloc(need);
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, d, need, NULL, NULL)) { free(d); continue; }

        HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileW(d->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            HIDD_ATTRIBUTES a; a.Size = sizeof(a);
            HIDP_CAPS caps; ZeroMemory(&caps, sizeof(caps));
            PHIDP_PREPARSED_DATA ppd = NULL;
            int has_caps = 0;
            if (HidD_GetAttributes(h, &a) && a.VendorID == MOUSE_VID && pid_supported(a.ProductID)) {
                if (HidD_GetPreparsedData(h, &ppd)) {
                    HidP_GetCaps(ppd, &caps);
                    HidD_FreePreparsedData(ppd);
                    has_caps = 1;
                }
                if (has_caps && caps.FeatureReportByteLength >= REPORT_LEN) {
                    found++;
                    printf("设备: VID=%04X PID=%04X  FeatureLen=%u\n", a.VendorID, a.ProductID,
                           caps.FeatureReportByteLength);
                    for (int k = 0; k < (int)(sizeof(g_delays) / sizeof(g_delays[0])); k++) {
                        BYTE c[REPORT_LEN], r[REPORT_LEN];
                        ZeroMemory(c, sizeof(c));
                        c[3] = 0x02; c[4] = 0x02; c[6] = 0x83;
                        if (!HidD_SetFeature(h, c, REPORT_LEN)) {
                            printf("  第%d次: SetFeature 失败 err=%lu\n", k + 1, GetLastError());
                            continue;
                        }
                        Sleep(g_delays[k]);
                        ZeroMemory(r, sizeof(r));
                        r[3] = 0x02;
                        if (!HidD_GetFeature(h, r, REPORT_LEN)) {
                            printf("  第%d次: GetFeature 失败 err=%lu\n", k + 1, GetLastError());
                            continue;
                        }
                        printf("  第%d次(等%3dms) seq=%02X 电量=%3d%% 充电=%d\n",
                               k + 1, g_delays[k], r[SEQ_IDX], r[8], r[7]);
                        if (r[SEQ_IDX] == SEQ_READY && r[8] <= 100) {
                            printf("  => 有效读数: %d%%%s\n", r[8], r[7] == 1 ? " (充电中)" : "");
                            reported = 1;
                            break;
                        }
                        printf("  => seq=%02X 为未就绪回显帧,丢弃重试\n", r[SEQ_IDX]);
                    }
                }
            }
            CloseHandle(h);
        }
        free(d);
    }
    SetupDiDestroyDeviceInfoList(devs);
    if (!found) printf("未找到 MCHOSE 鼠标(VID 2023)\n");
    return reported ? 0 : 2;
}
