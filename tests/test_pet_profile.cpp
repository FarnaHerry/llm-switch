#include <cstdio>

import std;
import llmswitch.pet_profile;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

struct TempDirectory {
    std::filesystem::path path;
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void WriteFile(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

template <typename F>
bool Throws(F&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    TempDirectory temp{fs::temp_directory_path() /
                       std::format("llmswitch-test-pet-profile-{}", std::chrono::steady_clock::now()
                           .time_since_epoch().count())};
    const fs::path root = temp.path / "live2d";
    WriteFile(root / "alpha" / "alpha.model3.json", "{}");
    WriteFile(root / "beta" / "beta.model3.json", "{}");
    WriteFile(root / "beta" / "pet.json", R"json({
        "display_name":"高阶模型",
        "license":"fixture",
        "window":{"width":360,"height":480},
        "states":{
            "working":{"motion":"Work","expression":"Focus","priority":2,"loop":true,
                "parameters":[{"id":"ParamAngleX","value":12.5,"blend":"add","weight":0.5}]},
            "done":{"motion":"Celebrate","priority":4}
        },
        "hit_areas":{"Head":{"motion":"TapHead","priority":5}}
    })json");

    const auto models = pet_profile::ScanLive2DModels(root);
    CHECK(models.size() == 2);
    CHECK(models[0].id == "alpha");
    CHECK(models[1].id == "beta");
    CHECK(models[0].profile.states.contains("idle"));
    CHECK(models[1].profile.displayName == "高阶模型");
    CHECK(models[1].profile.width == 360.0F);
    CHECK(models[1].profile.height == 480.0F);

    const auto beta = pet_profile::ResolveLive2DModel(root, "beta");
    const pet_profile::MotionCue* working = pet_profile::FindMotionCue(beta.profile, "working");
    CHECK(working != nullptr);
    CHECK(working->motion == "Work");
    CHECK(working->expression == "Focus");
    CHECK(working->priority == 2);
    CHECK(working->loop);
    CHECK(working->parameters.size() == 1);
    CHECK(working->parameters[0].id == "ParamAngleX");
    CHECK(working->parameters[0].value == 12.5F);
    CHECK(working->parameters[0].blend == "add");
    CHECK(working->parameters[0].weight == 0.5F);
    CHECK(beta.profile.hitAreas.at("Head").motion == "TapHead");
    CHECK(pet_profile::FindMotionCue(beta.profile, "done")->motion == "Celebrate");
    CHECK(pet_profile::ResolveLive2DModel(root, "").id == "alpha");
    CHECK(Throws([&] { static_cast<void>(pet_profile::ResolveLive2DModel(root, "../outside")); }));

    WriteFile(root / "broken" / "broken.model3.json", "{}");
    WriteFile(root / "broken" / "pet.json",
              R"json({"states":{"idle":{"parameters":[{"id":"","value":0}]}}})json");
    CHECK(Throws([&] { static_cast<void>(pet_profile::ResolveLive2DModel(root, "broken")); }));

    if (g_failures == 0) std::println("test_pet_profile: all assertions passed");
    return g_failures == 0 ? 0 : 1;
}
