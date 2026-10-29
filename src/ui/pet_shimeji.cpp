#include <huxerui/huxerui.h>

#include <memory>

#include "pet.h"

namespace llmswitch::ui {

[[huxerui::composable]]
huxerui::View PetShimejiBody(const std::shared_ptr<PetController>& controller) {
    return PetSkinFallback(controller, "未找到可用的 Shimeji 模型包，已显示莲花球");
}

} // namespace llmswitch::ui
