#pragma once

#include <studyapp/application/CommandConsole.hpp>

#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QTextBrowser;
class QUrl;

namespace studyapp::ui {

/// The command console panel (1.2-CMD-01, docs/COMMAND_CONSOLE.md): an output view and a
/// command line. It only parses lines with the registry it is given and shows what commands
/// write; what a command does is the registry's (the window's) business. Nothing runs while
/// it is idle: no timers, no polling.
///
/// Keys in the command line: Enter runs it, Up/Down browse the history, Tab completes a
/// command name, Ctrl+L clears the output, Escape asks to close the panel. Lines that point
/// somewhere (a page, a task) are links and are numbered for `go <n>`.
class CommandConsole final : public QWidget {
    Q_OBJECT

public:
    /// Output lines kept (oldest dropped first).
    static constexpr std::size_t kMaxLines = 1000;
    /// Numbered targets kept for `go <n>` (the newest listing only).
    static constexpr std::size_t kMaxTargets = 500;

    /// `registry` must outlive the console.
    explicit CommandConsole(const application::CommandRegistry& registry,
                            QWidget* parent = nullptr);
    ~CommandConsole() override;
    CommandConsole(const CommandConsole&) = delete;
    CommandConsole& operator=(const CommandConsole&) = delete;
    CommandConsole(CommandConsole&&) = delete;
    CommandConsole& operator=(CommandConsole&&) = delete;

    /// Runs `line` as if typed and entered (added to the history). false if it failed.
    bool run(const QString& line);
    /// A sink that writes into this console for as long as it exists — for commands that
    /// finish later (PDF search). Its links start a new numbered listing.
    [[nodiscard]] std::shared_ptr<application::ConsoleSink> laterSink();
    /// The sink of the command running now (valid during run()).
    [[nodiscard]] application::ConsoleSink& sink();

    void clearOutput();
    void focusInput();
    /// Called when a link or `go <n>` activates a target.
    void setTargetHandler(std::function<void(const application::ConsoleTarget&)> handler);
    /// Called when Escape is pressed in the command line.
    void setCloseHandler(std::function<void()> handler);
    /// The target numbered `number` (1-based) in the newest listing.
    [[nodiscard]] std::optional<application::ConsoleTarget>
    numberedTarget(std::size_t number) const;
    /// Activates numberedTarget(number); false if there is none.
    bool activateNumbered(std::size_t number);

    [[nodiscard]] QLineEdit* input() const noexcept { return input_; }
    [[nodiscard]] QTextBrowser* output() const noexcept { return output_; }
    [[nodiscard]] QLabel* hint() const noexcept { return hint_; }
    [[nodiscard]] const application::CommandHistory& history() const noexcept { return history_; }
    [[nodiscard]] const std::deque<application::ConsoleLine>& lines() const noexcept {
        return lines_;
    }
    /// The output as plain text, one line per output line (tests, copy).
    [[nodiscard]] QString plainText() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    class Sink;
    friend class Sink;

    void submit();
    void complete();
    void updateHint();
    void append(application::ConsoleLine line, const Sink* from);
    void render(const application::ConsoleLine& line, std::optional<std::size_t> number);
    void rerender();
    void openLink(const QUrl& link);

    const application::CommandRegistry* registry_;
    application::CommandHistory history_;
    std::deque<application::ConsoleLine> lines_;
    /// The newest listing's targets, numbered from 1; the listing belongs to the sink with
    /// id `listingOwner_` (each command run, and each later sink, has its own id).
    std::vector<application::ConsoleTarget> targets_;
    std::deque<std::optional<std::size_t>> numbers_; ///< per line in lines_: its number
    std::uint64_t listingOwner_ = 0;
    std::uint64_t nextSink_ = 1;
    std::shared_ptr<Sink> current_; ///< the command being run
    std::function<void(const application::ConsoleTarget&)> targetHandler_;
    std::function<void()> closeHandler_;
    bool rerendering_ = false;

    QTextBrowser* output_ = nullptr;
    QLineEdit* input_ = nullptr;
    QLabel* prompt_ = nullptr;
    QLabel* hint_ = nullptr;
};

} // namespace studyapp::ui
