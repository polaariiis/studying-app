#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace studyapp::application {

// The command console's language (1.2-CMD-01, docs/COMMAND_CONSOLE.md): a line of words and
// double-quoted strings, matched against a registry of named commands. There are no
// variables, pipes, loops, substitutions or shell escapes: a line names one registered
// command and its arguments, and nothing else can run. Qt-free; the shell (ui) registers
// the commands and shows the output.

/// Longest command line accepted, in UTF-8 bytes.
inline constexpr std::size_t kMaxCommandLineBytes = 4096;

/// One word of a command line. `quoted` words were written in double quotes: they are
/// always arguments, never command names.
struct CommandToken {
    std::string text;
    bool quoted = false;

    [[nodiscard]] friend bool operator==(const CommandToken&, const CommandToken&) = default;
};

/// Splits `line` into words at spaces and tabs. "…" quotes a word that contains spaces;
/// inside quotes `\"` is a quote and `\\` a backslash, every other backslash is kept (so
/// "C:\Users\me\a.pdf" needs no escaping). Errors (ParseError): an unterminated quote, a
/// quote that starts or ends inside a word (`a"b"`), control characters, or a line longer
/// than kMaxCommandLineBytes.
[[nodiscard]] core::Result<std::vector<CommandToken>> tokenizeCommandLine(std::string_view line);

/// `text` as one console word: unchanged when it needs no quotes, else in quotes with `"`
/// and `\` escaped as tokenizeCommandLine expects.
[[nodiscard]] std::string quoteCommandArgument(std::string_view text);

/// Where a console line points: a page, or a task in the planner.
using ConsoleTarget = std::variant<core::PageId, core::TaskId>;

/// One line of console output.
struct ConsoleLine {
    enum class Style : std::uint8_t {
        Normal,
        Heading, ///< a result group or a title
        Muted,   ///< context, hints
        Error,
        Echo, ///< the command line as typed
    };
    std::string text;
    Style style = Style::Normal;
    /// Activating the line opens this target. Lines with a target are numbered for `go <n>`.
    std::optional<ConsoleTarget> target;
    /// Indentation level (0 = none).
    int indent = 0;
};

/// Receives a command's output. Commands that finish later (PDF search) keep writing to
/// the same sink while the console exists; the shell owns it.
class ConsoleSink {
public:
    ConsoleSink() = default;
    virtual ~ConsoleSink() = default;
    ConsoleSink(const ConsoleSink&) = delete;
    ConsoleSink& operator=(const ConsoleSink&) = delete;
    ConsoleSink(ConsoleSink&&) = delete;
    ConsoleSink& operator=(ConsoleSink&&) = delete;

    virtual void write(ConsoleLine line) = 0;

    void text(std::string text, int indent = 0) {
        write({.text = std::move(text), .style = ConsoleLine::Style::Normal, .indent = indent});
    }
    void heading(std::string text) {
        write({.text = std::move(text), .style = ConsoleLine::Style::Heading});
    }
    void muted(std::string text, int indent = 0) {
        write({.text = std::move(text), .style = ConsoleLine::Style::Muted, .indent = indent});
    }
    void error(std::string text) {
        write({.text = std::move(text), .style = ConsoleLine::Style::Error});
    }
    void link(std::string text, ConsoleTarget target, int indent = 0) {
        write({.text = std::move(text),
               .style = ConsoleLine::Style::Normal,
               .target = target,
               .indent = indent});
    }
};

/// How a command's arguments are read.
enum class ArgumentKind : std::uint8_t {
    Required,     ///< exactly one word
    Optional,     ///< one word or none
    Text,         ///< the rest of the line as one text (one quoted word, or words joined by spaces)
    OptionalText, ///< Text, or nothing
};

struct ArgumentSpec {
    std::string name; ///< shown in usage: <name> or [name]
    ArgumentKind kind = ArgumentKind::Required;
};

/// A command matched and its arguments bound (by name, in order).
struct CommandCall {
    std::string command;                          ///< canonical name, e.g. "pdf search"
    std::vector<std::optional<std::string>> args; ///< one per ArgumentSpec

    /// The argument `index`, or "" when an optional one is absent.
    [[nodiscard]] const std::string& arg(std::size_t index) const;
    [[nodiscard]] bool has(std::size_t index) const;
};

/// Runs a command. An error is printed as "<command>: <message>"; output goes to `out`.
using CommandHandler = std::function<core::Result<void>(const CommandCall&, ConsoleSink& out)>;

struct CommandSpec {
    /// The canonical name as words, e.g. {"pdf", "search"}. Lower-case ASCII letters, digits,
    /// '-' and '?'.
    std::vector<std::string> words;
    /// Other names for the same command, each as words (e.g. {{"find"}} for "search").
    std::vector<std::vector<std::string>> aliases;
    std::vector<ArgumentSpec> arguments;
    std::string summary; ///< one line for `help`
    std::string details; ///< more lines for `help <command>` (may be empty)
    std::vector<std::string> examples;
    CommandHandler handler;
};

/// What a line means: a command with bound arguments, or why it means nothing.
struct CommandMatch {
    const CommandSpec* spec = nullptr;
    CommandCall call;
};

/// The console's commands. Lookup and completion are deterministic: names are compared
/// as ASCII case-insensitive words, the longest name wins, suggestions are sorted.
class CommandRegistry {
public:
    /// Errors (InvalidArgument): no words, a word that is not a valid name, no handler, a
    /// name (or alias) that is already taken, a text argument that is not the last one, or a
    /// required argument after an optional one.
    [[nodiscard]] core::Result<void> add(CommandSpec spec);

    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] const std::vector<CommandSpec>& commands() const noexcept { return commands_; }
    /// The command named `name` ("pdf search", an alias, any case), or nullptr.
    [[nodiscard]] const CommandSpec* find(std::string_view name) const;

    /// Matches tokens to a command and binds its arguments. Errors (InvalidArgument), with a
    /// message for the user: no command ("Type `help` …"), an unknown command, a command
    /// name that is incomplete ("export" → the commands it starts), a missing argument or
    /// too many (with the usage).
    [[nodiscard]] core::Result<CommandMatch> match(const std::vector<CommandToken>& tokens) const;
    /// tokenizeCommandLine, then match.
    [[nodiscard]] core::Result<CommandMatch> parse(std::string_view line) const;

    /// Command names that complete `input` (the start of a line, while the command name is
    /// still being typed): every name — canonical or alias — that starts with the words
    /// typed so far, the last one possibly partial. Sorted, without duplicates; empty once
    /// the input is past the name or cannot become one.
    [[nodiscard]] std::vector<std::string> complete(std::string_view input) const;

    /// `name <arg> [optional] <text…>` for `spec`.
    [[nodiscard]] static std::string usage(const CommandSpec& spec);
    /// "help": every command's usage and summary, in registration order.
    void writeHelp(ConsoleSink& out) const;
    /// "help <command>": usage, aliases, details and examples; NotFound for an unknown name.
    [[nodiscard]] core::Result<void> writeHelp(std::string_view command, ConsoleSink& out) const;

private:
    struct Name {
        std::vector<std::string> words;
        std::size_t command = 0;
        bool alias = false;
    };
    std::vector<CommandSpec> commands_;
    std::vector<Name> names_; ///< canonical names and aliases
};

/// Parses and runs `line` with `registry`: echoes it, then runs the matched command, or
/// writes the parse error. A command's failure is written as an error line; nothing
/// propagates. Returns false if the line did not run successfully.
bool runCommandLine(const CommandRegistry& registry, std::string_view line, ConsoleSink& out);

/// The console's in-session command history (never persisted): bounded, oldest entries
/// dropped first, blank lines ignored, a line equal to the newest one not added again.
/// Navigation works like a shell's: previous() walks back from the newest entry, next()
/// forward; past the newest it returns the line that was being typed.
class CommandHistory {
public:
    static constexpr std::size_t kDefaultCapacity = 100;

    explicit CommandHistory(std::size_t capacity = kDefaultCapacity);

    /// Adds a line run by the user and ends any navigation.
    void add(std::string_view line);
    /// The entry before the current one; `draft` is the line being typed, kept for when
    /// navigation returns past the newest entry. nullopt at the oldest entry or when empty.
    [[nodiscard]] std::optional<std::string> previous(std::string_view draft);
    /// The entry after the current one, or the draft after the newest; nullopt when not
    /// navigating.
    [[nodiscard]] std::optional<std::string> next();
    /// Ends navigation (e.g. the user edited the line).
    void resetNavigation() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const std::deque<std::string>& entries() const noexcept { return entries_; }

private:
    std::size_t capacity_;
    std::deque<std::string> entries_; ///< oldest first
    std::optional<std::size_t> position_;
    std::string draft_;
};

} // namespace studyapp::application
