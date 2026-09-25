#pragma once

#include <studyapp/canvas/Camera.hpp>
#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/DocumentPort.hpp>
#include <studyapp/canvas/Input.hpp>
#include <studyapp/canvas/Selection.hpp>
#include <studyapp/canvas/StrokeBuilder.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/document/Patch.hpp>

#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

namespace studyapp::canvas::detail {

/// Transient, non-document render state produced by tools (docs/CANVAS.md §5): the live
/// stroke, the marquee, the move offset and elements the eraser is about to remove. Only
/// the controller draws it; nothing here is persisted.
struct Preview {
    std::optional<StrokeBuilder> stroke;
    std::optional<core::DRect> marquee; ///< world
    bool moving = false;
    core::DVec2 moveOffset{}; ///< world, applied to selected elements while moving
    std::unordered_set<core::ElementId> erased; ///< hidden until the eraser gesture commits

    void clear() {
        stroke.reset();
        marquee.reset();
        moving = false;
        moveOffset = {};
        erased.clear();
    }
};

/// Everything a tool may touch — nothing else (docs/CANVAS.md §5).
struct ToolContext {
    Camera& camera;
    const CanvasScene& scene;
    const DocumentPort& document;
    Selection& selection;
    Preview& preview;
    const PenStyle& pen;
    const StrokeOptions& strokeOptions;
    core::IdGenerator& ids;
    /// Executes a command built by the tool through the DocumentPort; failures are
    /// recorded by the controller. The only way a tool changes the document.
    std::function<void(core::Result<document::Command>)> commit;
};

class Tool {
public:
    Tool() = default;
    virtual ~Tool() = default;
    Tool(const Tool&) = delete;
    Tool& operator=(const Tool&) = delete;
    Tool(Tool&&) = delete;
    Tool& operator=(Tool&&) = delete;

    virtual void onPointer(const PointerEvent& event, ToolContext& context) = 0;
    /// Abandons the gesture in progress without changing the document.
    virtual void cancel(ToolContext& context) = 0;
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
    [[nodiscard]] virtual CursorShape cursor() const noexcept = 0;
};

class PenTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::Crosshair; }

private:
    bool active_ = false;
};

class SelectTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return mode_ != Mode::Idle; }
    [[nodiscard]] CursorShape cursor() const noexcept override {
        return mode_ == Mode::Moving ? CursorShape::SizeAll : CursorShape::Arrow;
    }

private:
    enum class Mode : std::uint8_t {
        Idle,
        PendingMove, ///< pressed on a selected element; becomes Moving past the drag threshold
        Moving,
        Marquee,
    };
    Mode mode_ = Mode::Idle;
    core::DVec2 startView_{};
    core::DVec2 startWorld_{};
    bool additive_ = false;
};

class EraserTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::EraserRing; }

    static constexpr double kRadiusViewPx = kEraserRadiusViewPx;

private:
    void eraseAlong(const core::DVec2& a, const core::DVec2& b, ToolContext& context) const;

    bool active_ = false;
    core::DVec2 lastWorld_{};
};

class PanTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override {
        return active_ ? CursorShape::ClosedHand : CursorShape::OpenHand;
    }

private:
    bool active_ = false;
    core::DVec2 lastView_{};
};

class ZoomTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& /*context*/) override {}
    [[nodiscard]] bool isActive() const noexcept override { return false; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::ZoomIn; }

    static constexpr double kStep = 2.0;
};

/// The layer new strokes go to: the topmost visible, unlocked layer of the page.
[[nodiscard]] std::optional<core::LayerId> targetLayer(const document::Workspace& workspace,
                                                       core::PageId page);

/// Topmost element on a visible, unlocked layer whose geometry is within `toleranceWorld`
/// of `world`.
[[nodiscard]] std::optional<core::ElementId> topmostAt(const CanvasScene& scene,
                                                       const document::Workspace& workspace,
                                                       const core::DVec2& world,
                                                       double toleranceWorld);

} // namespace studyapp::canvas::detail
