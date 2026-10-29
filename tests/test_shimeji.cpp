#include <cstdio>

import std;
import llmswitch.shimeji;

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
    if (ec) throw std::runtime_error("create fixture directory failed");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("write fixture failed");
}

std::string ActionXml(std::string_view name, std::string_view image,
                      std::string_view type = "Stay") {
    return std::format(
        "<Action Name=\"{}\" Type=\"{}\"><Animation>"
        "<Pose Image=\"/{}\" ImageAnchor=\"64,128\" Velocity=\"-2,0\" Duration=\"6\"/>"
        "</Animation></Action>", name, type, image);
}

void WritePack(const std::filesystem::path& root) {
    for (int index = 1; index <= 8; ++index) {
        WriteFile(root / "img" / "Shimeji" / std::format("shime{}.png", index), "png");
    }
    WriteFile(root / "conf" / "actions.xml", std::format(
        "<?xml version=\"1.0\"?><Mascot xmlns=\"http://www.group-finity.com/Mascot\">"
        "<ActionList>{}{}{}{}{}{}{}</ActionList></Mascot>",
        ActionXml("Stand", "shime1.png"), ActionXml("Walk", "shime2.png", "Move"),
        ActionXml("Run", "shime3.png", "Move"), ActionXml("Sit", "shime4.png"),
        ActionXml("Bouncing", "shime5.png", "Animate"),
        ActionXml("Tripping", "shime6.png", "Animate"),
        ActionXml("Pinched", "shime7.png", "Embedded")));
    WriteFile(root / "conf" / "behaviors.xml",
              "<Mascot><BehaviorList>"
              "<Behavior Name=\"Stand\" Frequency=\"10\" Hidden=\"true\">"
              "<NextBehavior Add=\"false\"><BehaviorReference Name=\"Walk\" Frequency=\"3\"/>"
              "<BehaviorReference Name=\"Sit\" Frequency=\"1\"/></NextBehavior></Behavior>"
              "<Behavior Name=\"Walk\" Frequency=\"5\"/>"
              "<Behavior Name=\"Sit\" Frequency=\"2\"/>"
              "</BehaviorList></Mascot>");
    WriteFile(root / "pet.json",
              R"json({"display_name":"Q 版测试","window":{"width":180,"height":170},"license":"fixture","actions":{"working":"Run"}})json");
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
                       std::format("llmswitch-test-shimeji-{}", std::chrono::steady_clock::now()
                           .time_since_epoch().count())};
    const fs::path packRoot = temp.path / "packs" / "chibi";
    WritePack(packRoot);

    const auto scanned = shimeji::ScanPacks(temp.path / "packs");
    CHECK(scanned.size() == 1);
    CHECK(scanned.front() == packRoot);

    const shimeji::Pack pack = shimeji::LoadPack(packRoot);
    CHECK(pack.id == "chibi");
    CHECK(pack.name == "Q 版测试");
    CHECK(pack.width == 180.0F);
    CHECK(pack.height == 170.0F);
    CHECK(pack.license == "fixture");
    CHECK(pack.actions.size() == 7);
    CHECK(pack.behaviors.size() == 3);

    const shimeji::Action* stand = shimeji::FindAction(pack, "Stand");
    CHECK(stand != nullptr);
    CHECK(stand->poses.size() == 1);
    CHECK(stand->poses.front().anchor.x == 64.0F);
    CHECK(stand->poses.front().anchor.y == 128.0F);
    CHECK(stand->poses.front().velocity.x == -2.0F);
    CHECK(stand->poses.front().durationTicks == 6);
    CHECK(!shimeji::ImagePath(pack, stand->poses.front()).empty());

    CHECK(shimeji::ActionForActivity(pack, shimeji::Activity::unknown) == stand);
    CHECK(shimeji::ActionForActivity(pack, shimeji::Activity::idle) == stand);
    CHECK(shimeji::ActionForActivity(pack, shimeji::Activity::working)->name == "Run");
    CHECK(shimeji::ActionForActivity(pack, shimeji::Activity::waiting)->name == "Sit");
    CHECK(shimeji::ActionForActivity(pack, shimeji::Activity::completed)->name == "Bouncing");
    CHECK(shimeji::ActionForActivity(pack, shimeji::Activity::failed)->name == "Tripping");
    CHECK(shimeji::NextIdleAction(pack, "Stand", 0) == "Walk");
    CHECK(shimeji::NextIdleAction(pack, "Stand", 3) == "Sit");
    CHECK(shimeji::NextIdleAction(pack, "missing", 0) == "Stand");
    CHECK(shimeji::NextIdleAction(pack, "missing", 6) == "Stand");
    CHECK(shimeji::NextIdleAction(pack, "missing", 16) == "Sit");

    const fs::path noProfile = temp.path / "packs" / "plain";
    WritePack(noProfile);
    std::error_code ec;
    fs::remove(noProfile / "pet.json", ec);
    fs::remove(noProfile / "conf" / "behaviors.xml", ec);
    const shimeji::Pack plain = shimeji::LoadPack(noProfile);
    CHECK(plain.stateActions.working == "Walk");
    CHECK(plain.warnings.size() == 1);
    CHECK(shimeji::NextIdleAction(plain, "Walk", 42) == "Stand");

    const fs::path invalid = temp.path / "invalid";
    WritePack(invalid);
    WriteFile(invalid / "conf" / "actions.xml",
              "<Mascot><ActionList><Action Name=\"Bad\" Type=\"Stay\"><Animation>"
              "<Pose Image=\"/../secret.png\" Duration=\"1\"/>"
              "</Animation></Action></ActionList></Mascot>");
    CHECK(Throws([&] { static_cast<void>(shimeji::LoadPack(invalid)); }));

    const fs::path malformed = temp.path / "malformed";
    WritePack(malformed);
    WriteFile(malformed / "conf" / "actions.xml", "<Mascot>");
    CHECK(Throws([&] { static_cast<void>(shimeji::LoadPack(malformed)); }));

    shimeji::LoadLimits limits;
    limits.actions = 2;
    CHECK(Throws([&] { static_cast<void>(shimeji::LoadPack(packRoot, limits)); }));

    if (g_failures == 0) std::println("test_shimeji: all assertions passed");
    return g_failures == 0 ? 0 : 1;
}
