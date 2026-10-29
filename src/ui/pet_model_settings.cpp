#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pet.h"
#include "theme_colors.h"
#include "ui.h"

import llmswitch.config;
import llmswitch.pet_packages;
import llmswitch.pet_profile;
import llmswitch.shimeji;

namespace llmswitch::ui {
namespace {

namespace fs = std::filesystem;

struct ModelOption {
    std::string id;
    std::string name;
    std::string license;
    bool operator==(const ModelOption&) const = default;
};

struct ScanResult {
    std::vector<ModelOption> options;
    std::string warning;
};

ScanResult ScanModels(PetSkin kind) {
    ScanResult result;
    if (kind == PetSkin::shimeji) {
        const fs::path root = cfg::dataDir() / "shimeji";
        for (const fs::path& path : shimeji::ScanPacks(root)) {
            try {
                const shimeji::Pack pack = shimeji::LoadPack(path);
                result.options.push_back({
                    .id = fs::relative(path, root).generic_string(),
                    .name = pack.name,
                    .license = pack.license,
                });
                if (!pack.warnings.empty()) {
                    result.warning = "部分模型包含未支持的 Shimeji 动作，已按兼容子集运行";
                }
            } catch (const std::exception& e) {
                result.warning = e.what();
            }
        }
    } else if (kind == PetSkin::live2d) {
        for (const auto& model : pet_profile::ScanLive2DModels(cfg::dataDir() / "live2d")) {
            result.options.push_back({model.id, model.profile.displayName, model.profile.license});
        }
    }
    return result;
}

std::string SelectedModel(const std::shared_ptr<PetController>& pet, PetSkin kind) {
    return kind == PetSkin::shimeji ? pet->shimejiPack.Get() : pet->live2dModel.Get();
}

void SelectModel(const std::shared_ptr<PetController>& pet, PetSkin kind,
                 const std::string& id, const huxerui::ApplicationHandle& application) {
    if (kind == PetSkin::shimeji) {
        providerStore().setPetShimejiPack(id);
        pet->SetShimejiPack(id, application);
    } else {
        providerStore().setPetLive2DModel(id);
        pet->SetLive2DModel(id, application);
    }
}

} // namespace

[[huxerui::composable]]
huxerui::View PetModelSettings() {
    const auto pet = huxerui::UseService<PetController>();
    const auto application = huxerui::UseApplication();
    const auto picker = huxerui::UseService<huxerui::FilePicker>();
    const auto tasks = huxerui::UseTaskScope();
    const auto dialog = huxerui::UseDialog();
    const auto theme = huxerui::UseTheme();
    const PetSkin kind = pet->skin.Get();
    if (kind == PetSkin::lotus) {
        return huxerui::Text("莲花球是内建回退皮肤；Shimeji 与 Live2D 模型可分别在此管理。")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant});
    }

    auto options = huxerui::UseState<std::vector<ModelOption>>({});
    auto loading = huxerui::UseState(true);
    auto busy = huxerui::UseState(false);
    auto status = huxerui::UseState<std::string>({});
    auto revision = huxerui::UseState(0);
    auto generation = huxerui::UseState<std::uint64_t>(0);

    huxerui::Lifecycle([kind, revision, generation, options, loading, status, tasks] {
        const std::uint64_t request = generation.Get() + 1;
        generation = request;
        loading = true;
        tasks.Launch([kind, request, generation, options, loading, status]() -> huxerui::Task<void> {
            ScanResult result;
            try {
                result = co_await huxerui::RunWorker([kind] { return ScanModels(kind); });
            } catch (const std::exception& e) {
                result.warning = e.what();
            }
            if (generation.Get() != request) co_return;
            options = std::move(result.options);
            status = std::move(result.warning);
            loading = false;
        });
        return [generation, request] {
            if (generation.Get() == request) generation = request + 1;
        };
    }, kind, revision.Get());

    const std::string selectedId = SelectedModel(pet, kind);
    const auto currentOptions = options.Get();
    std::size_t selectedIndex = 0;
    bool selectedKnown = false;
    for (std::size_t index = 0; index < currentOptions.size(); ++index) {
        if (currentOptions[index].id == selectedId) {
            selectedIndex = index;
            selectedKnown = true;
            break;
        }
    }

    std::vector<huxerui::View> children;
    children.push_back(huxerui::Text(
        kind == PetSkin::shimeji ? "Shimeji 模型包" : "Live2D 模型包",
        huxerui::TextRole::Label));
    if (loading.Get()) {
        children.push_back(huxerui::Text("正在扫描模型…"));
    } else if (currentOptions.empty()) {
        children.push_back(huxerui::Text("尚未安装模型；请导入一个目录模型包。"));
    } else {
        children.push_back(
            huxerui::Select(currentOptions, selectedIndex,
                            [](const ModelOption& option) {
                                return huxerui::Text(option.name);
                            })
                .Label("选择模型包")
                .OnChanged([currentOptions, kind, pet, application, status](std::size_t index) {
                    if (index >= currentOptions.size()) return;
                    try {
                        SelectModel(pet, kind, currentOptions[index].id, application);
                        status = "";
                    } catch (const std::exception& e) {
                        status = e.what();
                    }
                }));
        if (selectedKnown && !currentOptions[selectedIndex].license.empty()) {
            children.push_back(huxerui::Text(
                "许可：" + currentOptions[selectedIndex].license));
        }
    }
    if (!selectedId.empty() && !selectedKnown) {
        children.push_back(huxerui::Text("当前选择的模型目录不存在或无法读取：" + selectedId)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption), theme.colors.error}));
    }

    children.push_back(huxerui::Row {
        huxerui::Button(busy.Get() ? "导入中…" : "导入目录")
            .OnClick([kind, pet, application, picker, tasks, revision, busy, status] {
                if (busy.Get()) return;
                busy = true;
                tasks.Launch([kind, pet, application, picker, revision, busy,
                              status]() -> huxerui::Task<void> {
                    auto directory = co_await picker->OpenDirectoryAsync(false);
                    if (!directory) {
                        busy = false;
                        co_return;
                    }
                    auto local = directory->AsFile();
                    if (!local) {
                        status = "当前平台未提供可导入的本地目录";
                        busy = false;
                        co_return;
                    }
                    try {
                        const fs::path root = cfg::dataDir() /
                            (kind == PetSkin::shimeji ? "shimeji" : "live2d");
                        const std::string id = co_await huxerui::RunWorker(
                            [kind, source = fs::path(local->Path()), root, directory] {
                                return pet_packages::Import(
                                    kind == PetSkin::shimeji
                                        ? pet_packages::Kind::shimeji
                                        : pet_packages::Kind::live2d,
                                    source, root);
                            });
                        if (pet->skin.Get() == kind) SelectModel(pet, kind, id, application);
                        status = "模型导入成功：" + id;
                        revision = revision.Get() + 1;
                    } catch (const std::exception& e) {
                        status = e.what();
                    }
                    busy = false;
                });
            })
            .With(huxerui::Enabled(!busy.Get() && picker->CanOpenDirectories(false))),
        huxerui::Button("刷新").OnClick([revision] {
            revision = revision.Get() + 1;
        }),
        huxerui::Button("删除当前模型")
            .OnClick([kind, pet, application, dialog, tasks, revision, status,
                      selectedId, selectedName = currentOptions.empty()
                          ? selectedId
                          : currentOptions[selectedIndex].name] {
                if (selectedId.empty()) return;
                dialog.Show(
                    "删除模型",
                    std::format("确定从应用数据目录删除「{}」？源目录中的模型不受影响。",
                                selectedName),
                    "删除", "取消",
                    [kind, pet, application, tasks, revision, status, selectedId] {
                        tasks.Launch([kind, pet, application, revision, status,
                                      selectedId]() -> huxerui::Task<void> {
                            try {
                                const fs::path root = cfg::dataDir() /
                                    (kind == PetSkin::shimeji ? "shimeji" : "live2d");
                                co_await huxerui::RunWorker([root, selectedId] {
                                    pet_packages::Delete(root, selectedId);
                                });
                                SelectModel(pet, kind, "", application);
                                status = "模型已删除";
                                revision = revision.Get() + 1;
                            } catch (const std::exception& e) {
                                status = e.what();
                            }
                        });
                    });
            })
            .With(huxerui::Enabled(!busy.Get() && !selectedId.empty())),
    }.With(huxerui::Spacing(8.0F)));
    if (!status.Get().empty()) {
        children.push_back(huxerui::Text(status.Get())
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}));
    }
    return huxerui::Column {children}.With(
        huxerui::Spacing(6.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
