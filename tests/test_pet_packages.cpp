#include <cstdio>

import std;
import llmswitch.pet_packages;

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

void WriteShimeji(const std::filesystem::path& root) {
    WriteFile(root / "img" / "Shimeji" / "shime1.png", "png");
    WriteFile(root / "conf" / "actions.xml",
              "<Mascot><ActionList><Action Name=\"Stand\" Type=\"Stay\"><Animation>"
              "<Pose Image=\"/shime1.png\" ImageAnchor=\"1,1\" Duration=\"1\"/>"
              "</Animation></Action></ActionList></Mascot>");
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
                       std::format("llmswitch-test-pet-packages-{}", std::chrono::steady_clock::now()
                           .time_since_epoch().count())};
    const fs::path shimejiSource = temp.path / "source" / "chibi";
    const fs::path live2dSource = temp.path / "source" / "hero";
    WriteShimeji(shimejiSource);
    WriteFile(live2dSource / "hero.model3.json", "{}");
    WriteFile(live2dSource / "pet.json",
              R"json({"states":{"working":{"motion":"Work","loop":true}}})json");

    const fs::path shimejiRoot = temp.path / "data" / "shimeji";
    const std::string shimejiId = pet_packages::Import(
        pet_packages::Kind::shimeji, shimejiSource, shimejiRoot);
    CHECK(shimejiId == "chibi");
    CHECK(fs::is_regular_file(shimejiRoot / "chibi" / "conf" / "actions.xml"));
    CHECK(Throws([&] {
        static_cast<void>(pet_packages::Import(
            pet_packages::Kind::shimeji, shimejiSource, shimejiRoot));
    }));
    CHECK(Throws([&] { pet_packages::Delete(shimejiRoot, "../chibi"); }));
    pet_packages::Delete(shimejiRoot, shimejiId);
    CHECK(!fs::exists(shimejiRoot / "chibi"));

    const fs::path live2dRoot = temp.path / "data" / "live2d";
    CHECK(pet_packages::Import(pet_packages::Kind::live2d,
                               live2dSource, live2dRoot) == "hero");
    CHECK(fs::is_regular_file(live2dRoot / "hero" / "pet.json"));
    pet_packages::Delete(live2dRoot, "hero");
    CHECK(!fs::exists(live2dRoot / "hero"));

    const fs::path invalid = temp.path / "source" / "invalid";
    WriteFile(invalid / "readme.txt", "not a model");
    CHECK(Throws([&] {
        static_cast<void>(pet_packages::Import(
            pet_packages::Kind::live2d, invalid, live2dRoot));
    }));
    CHECK(!fs::exists(live2dRoot / "invalid"));

    if (g_failures == 0) std::println("test_pet_packages: all assertions passed");
    return g_failures == 0 ? 0 : 1;
}
