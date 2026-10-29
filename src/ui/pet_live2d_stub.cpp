// pet_live2d_stub.cpp — Live2D 皮肤禁用态（LLMSWITCH_LIVE2D=OFF 时编译）。
#include <huxerui/huxerui.h>

#include <memory>

#include "pet.h"

namespace llmswitch::ui {

bool PetLive2DAvailable() { return false; }

huxerui::View PetLive2DBody(const std::shared_ptr<PetController>& controller) {
    return PetSkinFallback(controller,
                           "此构建未启用 Live2D，已显示莲花球");
}

} // namespace llmswitch::ui
