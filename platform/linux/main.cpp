// platform/linux/main.cpp — Linux 平台入口。
// 本阶段仅 GUI 薄入口；后续如需 CLI 子命令可参考姊妹项目 Clash-Flux 的分流写法。
#include <huxerui/app.h>

#include "single_instance.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (!llmswitch::single_instance::StartOrActivate()) return 0;

    struct SingleInstanceCleanup {
        ~SingleInstanceCleanup() { llmswitch::single_instance::Stop(); }
    } cleanup;
    return huxerui::RunApplication();
}
