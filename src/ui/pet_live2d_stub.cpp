// pet_live2d_stub.cpp — Live2D 皮肤禁用态（LLMSWITCH_LIVE2D=OFF 时编译）。
// PetLive2DAvailable() 恒 false，PetContent 不会走 PetLive2DBody 分支，
// 配置里的 petLive2D 不生效（桌宠保持莲花球/胶囊）。
#include <huxerui/huxerui.h>

#include <memory>

#include "pet.h"

namespace llmswitch::ui {

bool PetLive2DAvailable() { return false; }

huxerui::View PetLive2DBody(const std::shared_ptr<PetController>&) {
    return {};
}

} // namespace llmswitch::ui
