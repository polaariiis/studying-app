#pragma once

#include <QWidget>

namespace studyapp::ui {

class ThemeManager;

/// Central-area placeholder until the canvas exists (Phase 4). It establishes the canvas
/// look from the design tokens — neutral paper-like background with a subtle dot grid —
/// and shows a short status text. It contains no canvas logic and is replaced by the
/// OpenGL CanvasWidget in Phase 4.
class CanvasPlaceholder final : public QWidget {
    Q_OBJECT

public:
    explicit CanvasPlaceholder(const ThemeManager& themes, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const ThemeManager* themes_;
};

} // namespace studyapp::ui
