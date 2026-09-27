#pragma once

// Deterministic benchmark data: a page of hand-writing-like strokes (smooth random walks
// with varying pressure, 30–90 points each, 2 world units wide) spread over an area of
// roughly 6000 × 3600 world units — about 60 × 36 A4-sized handwriting areas. Optionally
// every n-th stroke is a highlighter stroke (translucent yellow, 14 units wide).

#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/document/Workspace.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/SequentialIds.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace studyapp::bench {

inline std::vector<document::StrokePoint> handwritingStroke(std::mt19937& random) {
    std::normal_distribution<float> turn(0.0F, 0.35F);
    std::uniform_int_distribution<int> length(30, 90);
    std::vector<document::StrokePoint> points;
    float x = 0.0F;
    float y = 0.0F;
    float heading = turn(random) * 4.0F;
    const int n = length(random);
    for (int k = 0; k < n; ++k) {
        points.push_back({x, y, 0.4F + 0.6F * std::abs(std::sin(static_cast<float>(k) * 0.2F))});
        heading += turn(random);
        x += 3.0F * std::cos(heading);
        y += 3.0F * std::sin(heading);
    }
    return points;
}

/// A workspace with one page holding `strokes` strokes, built through document commands
/// (one patch), exactly as the application would store them.
struct SyntheticPage {
    testing::ManualClock clock;
    testing::SequentialIds ids;
    document::Workspace workspace{document::WorkspaceInfo{
        .id = core::WorkspaceId::generate(ids), .name = "Benchmark", .created = clock.now()}};
    core::PageId page;
    core::LayerId layer;
    document::Patch contentPatch; ///< the patch that created the strokes

    /// `writingOrder`: strokes are created line by line, left to right (draw order follows
    /// position, as in real notes) instead of in random order.
    explicit SyntheticPage(int strokes, int highlighterEvery = 0, bool writingOrder = false) {
        namespace commands = document::commands;
        const auto run = [this](auto created) {
            auto id = created->id;
            (void)workspace.apply(created->command.patch);
            return id;
        };
        const auto notebook = run(commands::createNotebook(workspace, "N", clock, ids));
        const auto section = run(commands::createSection(workspace, notebook, "S", clock, ids));
        page = run(commands::createPage(workspace, section, "P", {}, clock, ids));
        layer = workspace.layersOf(page).front();
        std::mt19937 random(20260925);
        std::uniform_real_distribution<double> position(0.0, 6000.0);
        struct Planned {
            core::DVec2 at;
            document::Stroke stroke;
        };
        std::vector<Planned> planned;
        planned.reserve(static_cast<std::size_t>(strokes));
        for (int i = 0; i < strokes; ++i) {
            document::Stroke stroke{
                .baseWidth = 2.0F, .points = document::makeStrokePoints(handwritingStroke(random))};
            if (highlighterEvery > 0 && i % highlighterEvery == 0) {
                stroke.brush = document::Brush::Highlighter;
                stroke.color = core::Color::fromRgba(0xF2, 0xC9, 0x4C, 0x73);
                stroke.baseWidth = 14.0F;
            }
            const double x = position(random);
            const double y = position(random) * 0.6;
            planned.push_back({{x, y}, std::move(stroke)});
        }
        if (writingOrder) {
            std::stable_sort(planned.begin(), planned.end(),
                             [](const Planned& a, const Planned& b) {
                                 const auto lineA = static_cast<int>(a.at.y / 40.0);
                                 const auto lineB = static_cast<int>(b.at.y / 40.0);
                                 return lineA != lineB ? lineA < lineB : a.at.x < b.at.x;
                             });
        }
        for (Planned& p : planned) {
            auto created = commands::createElement(
                workspace, layer, {.transform = {.position = p.at}, .payload = std::move(p.stroke)},
                ids);
            (void)workspace.apply(created->command.patch);
            for (const auto& change : created->command.patch.changes()) {
                contentPatch.add(change);
            }
        }
    }
};

} // namespace studyapp::bench
