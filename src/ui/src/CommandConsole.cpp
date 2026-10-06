#include "CommandConsole.hpp"

#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTextBlock>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>

namespace studyapp::ui {

namespace {

using application::ConsoleLine;
using application::ConsoleTarget;

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString href(const ConsoleTarget& target) {
    if (const auto* page = std::get_if<core::PageId>(&target)) {
        return QStringLiteral("page:") + toQString(page->toString());
    }
    return QStringLiteral("task:") + toQString(std::get<core::TaskId>(target).toString());
}

std::string commonPrefix(const std::vector<std::string>& names) {
    if (names.empty()) {
        return {};
    }
    std::string prefix = names.front();
    for (const std::string& name : names) {
        const auto [end, unused] =
            std::mismatch(prefix.begin(), prefix.end(), name.begin(), name.end());
        prefix.erase(end, prefix.end());
    }
    return prefix;
}

} // namespace

/// Writes into the console while it exists; one id per command run (or later sink), so
/// numbered links of different commands never mix.
class CommandConsole::Sink final : public application::ConsoleSink {
public:
    Sink(CommandConsole* console, std::uint64_t id) : console_(console), id_(id) {}
    void write(ConsoleLine line) override {
        if (console_ != nullptr) {
            console_->append(std::move(line), this);
        }
    }
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

private:
    QPointer<CommandConsole> console_;
    std::uint64_t id_;
};

CommandConsole::CommandConsole(const application::CommandRegistry& registry, QWidget* parent)
    : QWidget(parent), registry_(&registry) {
    setObjectName(QStringLiteral("commandConsole"));
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    output_ = new QTextBrowser(this);
    output_->setObjectName(QStringLiteral("commandOutput"));
    output_->setAccessibleName(tr("Command output"));
    output_->setOpenLinks(false);
    output_->setOpenExternalLinks(false);
    output_->setFont(fixed);
    output_->setFrameShape(QFrame::NoFrame);
    output_->setMinimumHeight(48);
    output_->document()->setMaximumBlockCount(static_cast<int>(kMaxLines));
    output_->document()->setDocumentMargin(6);
    connect(output_, &QTextBrowser::anchorClicked, this, &CommandConsole::openLink);

    prompt_ = new QLabel(QStringLiteral(">"), this);
    prompt_->setObjectName(QStringLiteral("commandPrompt"));
    prompt_->setFont(fixed);
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("commandInput"));
    input_->setAccessibleName(tr("Command"));
    input_->setPlaceholderText(tr("Type a command — help lists them"));
    input_->setFont(fixed);
    input_->setMaxLength(static_cast<int>(application::kMaxCommandLineBytes));
    input_->setFrame(false);
    input_->installEventFilter(this);
    connect(input_, &QLineEdit::returnPressed, this, &CommandConsole::submit);
    connect(input_, &QLineEdit::textEdited, this, [this] {
        history_.resetNavigation();
        updateHint();
    });
    hint_ = new QLabel(this);
    hint_->setObjectName(QStringLiteral("commandHint"));
    hint_->setAccessibleName(tr("Command suggestions"));
    hint_->setTextFormat(Qt::PlainText);
    hint_->setMinimumWidth(0);
    hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* line = new QHBoxLayout;
    line->setContentsMargins(8, 4, 8, 0);
    line->setSpacing(6);
    line->addWidget(prompt_);
    line->addWidget(input_, 1);
    auto* hintRow = new QHBoxLayout;
    hintRow->setContentsMargins(8, 0, 8, 4);
    hintRow->addWidget(hint_, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(output_, 1);
    layout->addLayout(line);
    layout->addLayout(hintRow);
    setFocusProxy(input_);

    current_ = std::make_shared<Sink>(this, nextSink_++);
    current_->muted("StudyBoard command console. Type `help` for available commands.");
    updateHint();
}

CommandConsole::~CommandConsole() = default;

bool CommandConsole::run(const QString& line) {
    history_.add(line.toStdString());
    current_ = std::make_shared<Sink>(this, nextSink_++);
    const bool ok = application::runCommandLine(*registry_, line.toStdString(), *current_);
    updateHint();
    return ok;
}

std::shared_ptr<application::ConsoleSink> CommandConsole::laterSink() {
    return std::make_shared<Sink>(this, nextSink_++);
}

application::ConsoleSink& CommandConsole::sink() {
    return *current_;
}

void CommandConsole::clearOutput() {
    lines_.clear();
    numbers_.clear();
    targets_.clear();
    listingOwner_ = 0;
    output_->clear();
}

void CommandConsole::focusInput() {
    input_->setFocus(Qt::OtherFocusReason);
}

void CommandConsole::setTargetHandler(std::function<void(const ConsoleTarget&)> handler) {
    targetHandler_ = std::move(handler);
}

void CommandConsole::setCloseHandler(std::function<void()> handler) {
    closeHandler_ = std::move(handler);
}

std::optional<ConsoleTarget> CommandConsole::numberedTarget(std::size_t number) const {
    if (number == 0 || number > targets_.size()) {
        return std::nullopt;
    }
    return targets_[number - 1];
}

bool CommandConsole::activateNumbered(std::size_t number) {
    const auto target = numberedTarget(number);
    if (!target) {
        return false;
    }
    if (targetHandler_) {
        targetHandler_(*target);
    }
    return true;
}

QString CommandConsole::plainText() const {
    QStringList text;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        QString line = QString(lines_[i].indent * 2, QLatin1Char(' '));
        if (numbers_[i]) {
            line += QStringLiteral("[%1] ").arg(*numbers_[i]);
        }
        text << line + toQString(lines_[i].text);
    }
    return text.join(QLatin1Char('\n'));
}

void CommandConsole::submit() {
    const QString line = input_->text();
    if (line.trimmed().isEmpty()) {
        return;
    }
    input_->clear();
    (void)run(line);
}

void CommandConsole::complete() {
    const std::string typed = input_->text().toStdString();
    const std::vector<std::string> names = registry_->complete(typed);
    if (names.empty()) {
        return;
    }
    if (names.size() == 1) {
        input_->setText(toQString(names.front()) + QLatin1Char(' '));
    } else if (const std::string prefix = commonPrefix(names); prefix.size() > typed.size()) {
        input_->setText(toQString(prefix));
    }
    history_.resetNavigation();
    updateHint();
}

void CommandConsole::updateHint() {
    const std::string typed = input_->text().toStdString();
    QString hint;
    if (input_->text().trimmed().isEmpty()) {
        hint = tr("Enter runs · Tab completes · ↑↓ history · Esc closes");
    } else if (const auto names = registry_->complete(typed); !names.empty()) {
        constexpr std::size_t kShown = 8;
        QStringList shown;
        for (std::size_t i = 0; i < std::min(kShown, names.size()); ++i) {
            shown << toQString(names[i]);
        }
        hint = shown.join(QStringLiteral("  ·  "));
        if (names.size() > kShown) {
            hint += QStringLiteral("  …");
        }
    } else if (auto tokens = application::tokenizeCommandLine(typed); tokens) {
        // The command typed so far: its usage.
        for (std::size_t k = std::min<std::size_t>(tokens->size(), 3); k > 0 && hint.isEmpty();
             --k) {
            std::string name;
            bool quoted = false;
            for (std::size_t i = 0; i < k; ++i) {
                quoted = quoted || (*tokens)[i].quoted;
                name += (i > 0 ? " " : "") + (*tokens)[i].text;
            }
            if (const auto* spec = quoted ? nullptr : registry_->find(name)) {
                hint =
                    toQString(application::CommandRegistry::usage(*spec) + "  —  " + spec->summary);
            }
        }
    }
    if (hint_->text() != hint) {
        hint_->setText(hint);
    }
}

void CommandConsole::append(ConsoleLine line, const Sink* from) {
    std::optional<std::size_t> number;
    if (line.target) {
        if (listingOwner_ != from->id()) {
            listingOwner_ = from->id(); // a new listing: numbers start again at 1
            targets_.clear();
        }
        if (targets_.size() < kMaxTargets) {
            targets_.push_back(*line.target);
            number = targets_.size();
        }
    }
    render(line, number);
    lines_.push_back(std::move(line));
    numbers_.push_back(number);
    while (lines_.size() > kMaxLines) { // the document drops its first block the same way
        lines_.pop_front();
        numbers_.pop_front();
    }
}

void CommandConsole::render(const ConsoleLine& line, std::optional<std::size_t> number) {
    const QPalette& colors = palette();
    QString html = QStringLiteral("&nbsp;").repeated(line.indent * 2);
    if (number) {
        html += QStringLiteral("<span style=\"color:%1\">[%2]</span> ")
                    .arg(colors.color(QPalette::PlaceholderText).name())
                    .arg(*number);
    }
    QString text = toQString(line.text).toHtmlEscaped();
    if (line.target) {
        text = QStringLiteral("<a href=\"%1\" style=\"color:%2\">%3</a>")
                   .arg(href(*line.target), colors.color(QPalette::Text).name(), text);
    }
    QString color = colors.color(QPalette::Text).name();
    QString weight = QStringLiteral("normal");
    switch (line.style) {
    case ConsoleLine::Style::Normal:
        break;
    case ConsoleLine::Style::Heading:
    case ConsoleLine::Style::Echo:
        weight = QStringLiteral("600");
        break;
    case ConsoleLine::Style::Muted:
        color = colors.color(QPalette::PlaceholderText).name();
        break;
    case ConsoleLine::Style::Error:
        color = colors.color(QPalette::BrightText).name();
        break;
    }
    html +=
        QStringLiteral("<span style=\"color:%1; font-weight:%2; white-space:pre-wrap\">%3</span>")
            .arg(color, weight, text);
    output_->append(html);
}

void CommandConsole::rerender() {
    rerendering_ = true;
    output_->clear();
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        render(lines_[i], numbers_[i]);
    }
    rerendering_ = false;
}

void CommandConsole::openLink(const QUrl& link) {
    const auto uuid = core::Uuid::parse(link.path().toStdString());
    if (!uuid || !targetHandler_) {
        return;
    }
    if (link.scheme() == QLatin1String("page")) {
        targetHandler_(core::PageId{*uuid});
    } else if (link.scheme() == QLatin1String("task")) {
        targetHandler_(core::TaskId{*uuid});
    }
}

bool CommandConsole::eventFilter(QObject* watched, QEvent* event) {
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Up:
            if (auto line = history_.previous(input_->text().toStdString())) {
                input_->setText(toQString(*line));
                updateHint();
            }
            return true;
        case Qt::Key_Down:
            if (auto line = history_.next()) {
                input_->setText(toQString(*line));
                updateHint();
            }
            return true;
        case Qt::Key_Tab:
            complete();
            return true;
        case Qt::Key_Escape:
            if (closeHandler_) {
                closeHandler_();
            }
            return true;
        case Qt::Key_L:
            if (key->modifiers() == Qt::ControlModifier) {
                clearOutput();
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandConsole::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && !rerendering_ && output_ != nullptr) {
        rerender(); // the theme changed: colours are part of the rendered lines
    }
}

} // namespace studyapp::ui
