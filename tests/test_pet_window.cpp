#include "ui/pet.h"
#include "ui/pet_sprite_motion.h"

#include <array>
#include <cstdio>

int main() {
    using namespace llmswitch::ui;
    int failures = 0;
    struct Case {
        PetSkin skin;
        bool compact;
        huxerui::Size expected;
    };
    const std::array cases{
        Case{PetSkin::lotus, false, {180.0F, 220.0F}},
        Case{PetSkin::lotus, true, {150.0F, 44.0F}},
        Case{PetSkin::shimeji, false, {180.0F, 220.0F}},
        Case{PetSkin::shimeji, true, {180.0F, 220.0F}},
        Case{PetSkin::live2d, false, {180.0F, 220.0F}},
        Case{PetSkin::live2d, true, {180.0F, 220.0F}},
    };
    for (const auto& test : cases) {
        const PetSkinDescriptor descriptor{.kind = test.skin, .size = {180.0F, 220.0F}};
        const auto options = PetOptions(true, test.compact, descriptor);
        if (options.initial_size != test.expected || !options.transparent_surface ||
            !options.topmost || options.presentation != huxerui::WindowPresentation::Utility) {
            std::fprintf(stderr, "Unexpected pet window options: skin=%d compact=%d\n",
                         static_cast<int>(test.skin), test.compact);
            ++failures;
        }
    }
    const auto check = [&failures](bool condition, const char* message) {
        if (condition) return;
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    };
    for (int kind = 0; kind < 3; ++kind) {
        for (const float p : {-1.0F, 0.0F, 1.0F, 2.0F}) {
            const auto visual = SamplePetSpriteReaction(kind, p);
            check(visual.x == 0.0F && visual.y == 0.0F && visual.scale == 1.0F &&
                      visual.sparkle == 0.0F, "A completed reaction must settle without residual motion");
        }
        for (int frame = 0; frame <= 100; ++frame) {
            const auto visual = SamplePetSpriteReaction(kind, frame / 100.0F);
            check(std::abs(visual.x) <= 0.05F && visual.y >= -0.09F && visual.y <= 0.0F &&
                      visual.scale >= 1.0F && visual.scale <= 1.05F &&
                      visual.sparkle >= 0.0F && visual.sparkle <= 1.0F,
                  "A reaction must stay within the transparent sprite margins");
        }
    }
    check(SamplePetSpriteReaction(0, 0.5F).y < -0.08F, "Hop must visibly rise");
    check(SamplePetSpriteReaction(1, 0.08F).x > 0.02F &&
              SamplePetSpriteReaction(1, 0.25F).x < -0.02F, "Wiggle must change direction");
    check(SamplePetSpriteReaction(2, 0.5F).sparkle > 0.9F, "Celebration must show its stars");
    return failures == 0 ? 0 : 1;
}
