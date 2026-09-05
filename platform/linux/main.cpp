// platform/linux/main.cpp — Linux 平台入口。
// 本阶段仅 GUI 薄入口；后续如需 CLI 子命令可参考姊妹项目 Clash-Flux 的分流写法。
#include <huxerui/app.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return huxerui::RunApplication();
}
