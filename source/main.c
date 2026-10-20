/*
 * nxhomebrew - 环境自检程序 (Switch / Atmosphère homebrew baseline)
 *
 * 用途：验证 devkitPro 工具链、hbmenu、SD 卡访问、手柄输入、nxlink 调试
 * 是否全部打通。跑通这一个，后面写什么都只是加代码的事。
 *
 * 退出：按 + 键
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <switch.h>

/* ---------------------------------------------------------------
 * 调试开关
 *   0 = printf 输出到 Switch 屏幕（默认）
 *   1 = printf 经 nxlink 回传到 PC 终端（屏幕不再显示文字）
 * 改完重新 make 即可。用 1 之前先在 PC 上跑：
 *   nxlink -s nxhomebrew.nro
 * --------------------------------------------------------------- */
#define USE_NXLINK 0

static void banner(const char *title)
{
    printf("\n\x1b[36m== %s ==\x1b[0m\n", title);
}

static const char *appletTypeName(AppletType t)
{
    switch (t) {
        case AppletType_None:              return "None";
        case AppletType_Program:           return "Program (applet 模式 / 约 440MB)";
        case AppletType_LibraryApplet:     return "LibraryApplet";
        case AppletType_OverlayApplet:     return "OverlayApplet";
        case AppletType_SystemApplet:      return "SystemApplet";
        case AppletType_Application:       return "Application (full memory / 约 3.2GB)";
        case AppletType_SystemApplication: return "SystemApplication";
        default:                           return "Unknown";
    }
}

/* 列出 sdmc:/switch 下前几条内容，验证 SD 卡读写通道 */
static void listSwitchDir(void)
{
    Result rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        printf("  fsdevMountSdmc 失败: 0x%x\n", rc);
        return;
    }

    DIR *d = opendir("sdmc:/switch");
    if (d == NULL) {
        printf("  sdmc:/switch 打不开（目录不存在？）\n");
        return;
    }

    struct dirent *e;
    int shown = 0, total = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        total++;
        if (shown < 6) {
            printf("  %s\n", e->d_name);
            shown++;
        }
    }
    closedir(d);

    if (total == 0) printf("  （空目录）\n");
    else if (total > shown) printf("  ... 共 %d 项\n", total);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    consoleInit(NULL);

#if USE_NXLINK
    socketInitializeDefault();
    int nxfd = nxlinkStdio();
    if (nxfd < 0) {
        /* 连不上，stdout 仍然是屏幕，照常往下跑 */
        printf("[nxlink] 未连接（fd=%d），输出留在屏幕\n", nxfd);
    } else {
        printf("[nxlink] 已连接 PC，fd=%d，以下输出在 PC 终端\n", nxfd);
    }
#endif

    banner("构建信息");
    printf("  编译时间 : %s %s\n", __DATE__, __TIME__);
    printf("  目标     : %s\n", "aarch64-none-elf (Nintendo Switch)");

    banner("系统环境");
    u32 v = hosversionGet();
    printf("  HOS 版本 : %u.%u.%u\n", (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    printf("  CFW      : %s\n", hosversionIsAtmosphere() ? "Atmosphère" : "官方系统");

    banner("运行形态");
    AppletType at = appletGetAppletType();
    printf("  类型     : %s\n", appletTypeName(at));
    printf("  可用堆   : %llu MB\n",
           (unsigned long long)(envGetHeapSize() / 1024 / 1024));
    printf("  说明     : applet 模式约 440MB / full memory 约 3.2GB\n");
    printf("             full memory 进法 = 按住 R 启动任一游戏\n");

    banner("存储");
    listSwitchDir();

    printf("\n\x1b[33m按 + 退出，按其他键会打印键值\x1b[0m\n");

    while (appletMainLoop()) {
        hidScanInput();
        u64 k = hidKeysDown(CONTROLLER_P1_AUTO);

        if (k & KEY_PLUS) break;

        if (k != 0) {
            printf("  按下: 0x%016llx\n", (unsigned long long)k);
        }

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
#if USE_NXLINK
    socketExit();
#endif
    return 0;
}
