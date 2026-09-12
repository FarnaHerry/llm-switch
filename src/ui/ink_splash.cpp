// ink_splash.cpp — Canvas + Path 程序化泼墨伪元素（环境装饰层，不承载内容）。
//
// 「伪元素」定位：类似 CSS ::before 的纯装饰——组件输出一张铺满宿主的
// 半透明墨渍画布，按 anchor 落在窗口角落（右上/左下），叠进 Stack 环境层；
// 尺度与窗口高度解耦限幅，不随窗口无限放大。
//
// 形态 = 副墨团 + 主墨团（Catmull-Rom 平滑闭合 Blob）+ DrawPathShadow 洇边
// + 浓墨内斑 + 飞白细枝（Round 帽曲线）+ 飞溅圆点与旋转椭圆墨点。
// 随机数用固定种子的 splitmix32：同一 seed 形态恒定，重组/重绘不抖动。
// 颜色只取主题语义层 on_surface（深色主题=宣纸反白淡墨，浅色=浓墨），
// 深浅自适应，不写死明暗色。
// 注意：不要给这张画布再包 Align 修饰符——Align 节点会把内容按父约束
// 撑满再对齐，与画布自身铺满宿主的语义重复。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "ui.h"

import std;

namespace llmswitch::ui {
namespace {

// splitmix32：小巧、统计性足够画墨点的确定性随机。
std::uint32_t NextRandom(std::uint32_t& state) {
    state += 0x9E3779B9;
    std::uint32_t z = state;
    z = (z ^ (z >> 16)) * 0x21F0AAAD;
    z = (z ^ (z >> 15)) * 0x735A2D97;
    return z ^ (z >> 15);
}

float Rand01(std::uint32_t& state) {
    return static_cast<float>(NextRandom(state)) / 4294967295.0F;
}

float RandRange(std::uint32_t& state, float low, float high) {
    return low + (high - low) * Rand01(state);
}

// 把一圈锚点用 Catmull-Rom（转三次 Bézier）平滑闭合：墨团边缘圆润又带起伏。
huxerui::Path SmoothClosedBlob(const std::vector<huxerui::Point>& anchors) {
    const std::size_t n = anchors.size();
    huxerui::Path path;
    path.MoveTo(anchors[0]);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p0 = anchors[(i + n - 1) % n];
        const auto& p1 = anchors[i];
        const auto& p2 = anchors[(i + 1) % n];
        const auto& p3 = anchors[(i + 2) % n];
        const huxerui::Point c1{p1.x + (p2.x - p0.x) / 6.0F,
                                p1.y + (p2.y - p0.y) / 6.0F};
        const huxerui::Point c2{p2.x - (p3.x - p1.x) / 6.0F,
                                p2.y - (p3.y - p1.y) / 6.0F};
        path.CubicTo(c1, c2, p2);
    }
    path.Close();
    return path;
}

// 以 (cx,cy) 为心、rx/ry 为轴、theta 为旋转角的椭圆锚点环。
std::vector<huxerui::Point> EllipseAnchors(float cx, float cy, float rx, float ry,
                                           float theta, std::uint32_t& rng,
                                           float jitter) {
    const float cos_t = std::cos(theta);
    const float sin_t = std::sin(theta);
    const int count = 8;
    std::vector<huxerui::Point> anchors;
    anchors.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float angle = (static_cast<float>(i) / count) * 6.2831853F;
        const float rx_j = rx * (1.0F + RandRange(rng, -jitter, jitter));
        const float ry_j = ry * (1.0F + RandRange(rng, -jitter, jitter));
        const float local_x = rx_j * std::cos(angle);
        const float local_y = ry_j * std::sin(angle);
        anchors.push_back({cx + local_x * cos_t - local_y * sin_t,
                           cy + local_x * sin_t + local_y * cos_t});
    }
    return anchors;
}

huxerui::Color WithAlpha(huxerui::Color color, float alpha) {
    color.alpha = alpha;
    return color;
}

} // namespace

[[huxerui::composable]] huxerui::View InkSplash(std::uint32_t seed,
                                                float alpha_scale,
                                                InkSplashAnchor anchor) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::Color ink = theme.colors.on_surface;
    return huxerui::Canvas([seed, alpha_scale, anchor, ink](huxerui::PaintContext& ctx,
                                                       huxerui::Size size) {
        std::uint32_t rng = seed * 0x9E3779B9 ^ 0x6D2B79F5;
        // 尺度与窗口高度解耦（限幅）：泼墨是角部点缀，不随窗口无限放大。
        const float radius =
            std::clamp(size.height * 0.11F, 70.0F, 140.0F);
        float cx = 0.0F;
        float cy = 0.0F;
        if (anchor == InkSplashAnchor::TopEnd) {
            cx = size.width - radius * 1.4F;
            cy = size.height * 0.16F + radius * 0.3F;
        } else {
            cx = radius * 1.4F;
            cy = size.height - radius * 1.2F;
        }

        // 副墨团（先画，被主团压住）：飞溅时常有的第二落点。
        const float sub_angle = RandRange(rng, 0.0F, 6.2831853F);
        const float sub_dist = radius * 1.32F;
        const float sub_cx = cx + sub_dist * std::cos(sub_angle);
        const float sub_cy = cy + sub_dist * std::sin(sub_angle);
        ctx.FillPath(SmoothClosedBlob(EllipseAnchors(
                         sub_cx, sub_cy, radius * RandRange(rng, 0.34F, 0.46F),
                         radius * RandRange(rng, 0.28F, 0.40F), sub_angle, rng,
                         0.24F)),
                     WithAlpha(ink, 0.055F * alpha_scale));

        // 主墨团：12-14 个角度均匀、半径抖动的锚点；略压扁近宣纸上的横向洇开。
        const int lobes = 12 + static_cast<int>(Rand01(rng) * 3.0F);
        std::vector<huxerui::Point> blob;
        blob.reserve(static_cast<std::size_t>(lobes));
        for (int i = 0; i < lobes; ++i) {
            const float angle =
                (static_cast<float>(i) / lobes) * 6.2831853F +
                RandRange(rng, -0.10F, 0.10F);
            const float r = radius * RandRange(rng, 0.68F, 1.24F);
            blob.push_back({cx + r * std::cos(angle),
                            cy + r * std::sin(angle) * 0.88F});
        }
        const auto blob_path = SmoothClosedBlob(blob);
        // 洇边：无偏移模糊影子，模拟墨在纸纤维上的浸润。
        ctx.DrawPathShadow(blob_path, WithAlpha(ink, 0.05F * alpha_scale),
                           {0.0F, 0.0F}, radius * 0.6F);
        ctx.FillPath(blob_path, WithAlpha(ink, 0.085F * alpha_scale));

        // 浓墨内斑：主团重心附近的第二层，给墨团内部层次。
        ctx.FillPath(
            SmoothClosedBlob(EllipseAnchors(
                cx + RandRange(rng, -radius, radius) * 0.18F,
                cy + RandRange(rng, -radius, radius) * 0.18F,
                radius * RandRange(rng, 0.42F, 0.55F),
                radius * RandRange(rng, 0.34F, 0.46F),
                RandRange(rng, 0.0F, 3.1415927F), rng, 0.30F)),
            WithAlpha(ink, 0.10F * alpha_scale));

        // 飞白细枝：从主团边缘甩出的细曲线，Round 帽模拟收笔。
        const int streaks = 2 + (NextRandom(rng) % 2);
        for (int i = 0; i < streaks; ++i) {
            const float angle = RandRange(rng, 0.0F, 6.2831853F);
            const float start = radius * 0.85F;
            const float bend = RandRange(rng, -0.35F, 0.35F);
            huxerui::Path streak;
            streak.MoveTo({cx + start * std::cos(angle),
                           cy + start * std::sin(angle)});
            const float ctrl_r = radius * 1.55F;
            const float end_r = radius * RandRange(rng, 1.9F, 2.5F);
            streak.QuadraticTo(
                {cx + ctrl_r * std::cos(angle + bend),
                 cy + ctrl_r * std::sin(angle + bend)},
                {cx + end_r * std::cos(angle + bend * 0.4F),
                 cy + end_r * std::sin(angle + bend * 0.4F)});
            ctx.StrokePath(streak, WithAlpha(ink, 0.07F * alpha_scale),
                           huxerui::StrokeStyle{
                               .width = std::max(1.2F, radius * 0.035F),
                               .cap = huxerui::StrokeCap::Round,
                               .join = huxerui::StrokeJoin::Round});
        }

        // 飞溅圆点：离主团越远越小越淡，疏密有随机。
        const int droplets = 12 + static_cast<int>(Rand01(rng) * 7.0F);
        for (int i = 0; i < droplets; ++i) {
            const float angle = RandRange(rng, 0.0F, 6.2831853F);
            const float t = Rand01(rng);
            const float dist = radius * (1.2F + 1.4F * t);
            const float dr =
                radius * RandRange(rng, 0.042F, 0.115F) * (1.5F - 0.6F * t);
            ctx.DrawCircle({cx + dist * std::cos(angle),
                            cy + dist * std::sin(angle)},
                           dr, WithAlpha(ink, (0.05F + 0.07F * t) * alpha_scale));
        }

        // 拉长墨点：带方向的旋转椭圆，像甩出去的墨滴。
        const int teardrops = 3 + (NextRandom(rng) % 3);
        for (int i = 0; i < teardrops; ++i) {
            const float angle = RandRange(rng, 0.0F, 6.2831853F);
            const float dist = radius * RandRange(rng, 1.5F, 2.5F);
            const float long_axis = radius * RandRange(rng, 0.10F, 0.17F);
            const float theta = angle + RandRange(rng, -0.5F, 0.5F);
            ctx.FillPath(
                SmoothClosedBlob(EllipseAnchors(
                    cx + dist * std::cos(angle), cy + dist * std::sin(angle),
                    long_axis, long_axis * 0.42F, theta, rng, 0.18F)),
                WithAlpha(ink, 0.06F * alpha_scale + 0.03F));
        }
    });
}

} // namespace llmswitch::ui
