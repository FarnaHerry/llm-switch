// provider_form_dsh.cpp — dsh（DeepSeek Harness）供应商表单的 agent 专属区块：
// 模型条目的官方能力声明。dsh 的 llm-pi-ai 把「模型能力」放在模型条目上
// （@deepseek-ai/dsh-llm-pi-ai 的 PiAiModelProfile，dsh 官方设置页编辑的就是
// 这一组字段）：
//   * input:            请求模态。手工声明的路由只声明了 text 就收不了图片附件，
//                       dsh 会在附件进上下文之前按模型名直接拒绝。
//   * contextWindow:    上下文容量；maxTokens: 输出上限。不声明时 dsh 退回已装
//                       catalog 的值、再退回路由默认（262144 / 32768）。
//   * reasoningEfforts: 可选推理档位。不声明时 dsh 把手写模型当成不支持思考，
//                       模型菜单里连「推理等级」都不出现。
// 三者都是 per-model 能力声明，主模型为空时整块不生效，所以这里只写进
// llmswitch-* 条目的首个模型条目（见 store 的 dsh 写入端）。
#include <huxerui/huxerui.h>

#include <string>
#include <utility>
#include <vector>

#include "providers_internal.h"

import llmswitch.models;

namespace llmswitch::ui {

const AgentFormPolicy& DshFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = "模型（必填）",
        .showMappings = false,
        .showApiFormat = true,
        .showToml = false,
        .showDshModelFields = true,
    };
    return policy;
}

// 表单里的容量文本 → token 数（0 = 空 = 不声明，负数 = 无法解析）。
std::int64_t DshCapacity(const huxerui::State<huxerui::TextEditingValue>& field) {
    return models::parseTokenCount(field.Get().text);
}

bool DshCapacitiesValid(const FormStates& fs, huxerui::ToastHandle toast) {
    if (DshCapacity(fs.contextWindow) < 0) {
        toast.Show("上下文窗口必须是正数，例如 131072、256K 或 1M");
        return false;
    }
    if (DshCapacity(fs.maxTokens) < 0) {
        toast.Show("最大输出 token 数必须是正数，例如 8192、64K 或 1M");
        return false;
    }
    return true;
}

std::vector<std::string> DshInputModalities(const FormStates& fs) {
    return models::inputModalitiesFromMask(fs.inputMask.Get());
}

// 输入模态 chips：文本 / 图像。两个都不选 = 不声明（dsh 退回 catalog 值或路由
// 默认，即只收文本），所以这里不强制至少选一个。
[[huxerui::composable]] huxerui::View DshInputFields(const FormStates& fs) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> chips;
    const auto& modalities = models::inputModalities();
    const int textBit =
        models::inputModalityMask(std::vector<std::string>{"text"});
    for (std::size_t i = 0; i < modalities.size(); ++i) {
        const int bit = 1 << static_cast<int>(i);
        const bool isText = modalities[i] == "text";
        const bool selected = (fs.inputMask.Get() & bit) != 0;
        chips.push_back(
            huxerui::Chip(models::inputModalityLabel(modalities[i]), selected)
                .OnChanged([fs, bit, textBit, isText](bool on) {
                    // 文本是底座：勾「图像」一并带上「文本」（只声明 image
                    // 等于声明这个模型收不了文字）；取消「文本」等于撤掉整份
                    // 声明——「有图像没文字」不可表达，两个一起灭。
                    int mask = fs.inputMask.Get();
                    if (on) {
                        mask |= isText ? bit : (bit | textBit);
                    } else {
                        mask = isText ? 0 : (mask & ~bit);
                    }
                    fs.inputMask = mask;
                }));
    }
    return huxerui::Column {
        huxerui::Text("输入模态（写入 dsh 的 input）：只有声明了 image，"
                      "dsh 才会把图片附件发给这条路由；不选 = 不声明。"
                      "文本是底座，勾图像会一并声明文本")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::Flow(std::move(chips)).With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 容量两栏：上下文窗口 / 最大输出 token 数。接受十进制数或 K/M 后缀
// （256K、1M），与 dsh 官方设置页同一套拼写。
[[huxerui::composable]] huxerui::View DshCapacityFields(const FormStates& fs) {
    return huxerui::Row {
        huxerui::TextField(fs.contextWindow.Get())
            .Label("上下文窗口（可选）")
            .Placeholder("131072 / 256K / 1M")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) {
                fs.contextWindow = v;
            })
            .With(huxerui::Grow(1.0F)),
        huxerui::TextField(fs.maxTokens.Get())
            .Label("最大输出 token 数（可选）")
            .Placeholder("8192 / 64K / 1M")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) {
                fs.maxTokens = v;
            })
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 推理档位 chips。
[[huxerui::composable]] huxerui::View DshReasoningChips(const FormStates& fs) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> chips;
    const auto& levels = models::reasoningLevels();
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const int bit = 1 << static_cast<int>(i);
        const bool selected = (fs.reasoningMask.Get() & bit) != 0;
        chips.push_back(
            huxerui::Chip(models::reasoningLevelLabel(levels[i]), selected)
                .OnChanged([fs, bit](bool on) {
                    fs.reasoningMask =
                        on ? (fs.reasoningMask.Get() | bit)
                           : (fs.reasoningMask.Get() & ~bit);
                }));
    }
    return huxerui::Column {
        huxerui::Text("推理档位（写入 dsh 的 reasoningEfforts；不选 = 不声明，"
                      "dsh 视为不支持思考）")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        // 档位较多，用 Flow 自动换行避免单行 Row 横向溢出。
        huxerui::Flow(std::move(chips)).With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DshModelFields(const FormStates& fs) {
    return huxerui::Column {
        DshInputFields(fs),
        DshCapacityFields(fs),
        DshReasoningChips(fs),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
