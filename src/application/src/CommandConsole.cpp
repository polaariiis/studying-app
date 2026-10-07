#include <studyapp/application/CommandConsole.hpp>

#include <algorithm>
#include <set>
#include <utility>

namespace studyapp::application {

namespace {

using core::ErrorCode;

char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lowerAscii);
    return out;
}

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t';
}

bool validNameWord(std::string_view word) {
    return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '?';
    });
}

std::string join(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& word : words) {
        if (!out.empty()) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

/// Splits a command name ("pdf search") into lower-case words.
std::vector<std::string> nameWords(std::string_view name) {
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < name.size()) {
        while (at < name.size() && isSpace(name[at])) {
            ++at;
        }
        const std::size_t start = at;
        while (at < name.size() && !isSpace(name[at])) {
            ++at;
        }
        if (at > start) {
            words.push_back(lowered(name.substr(start, at - start)));
        }
    }
    return words;
}

core::Error userError(std::string message) {
    return core::Error{ErrorCode::InvalidArgument, std::move(message)};
}

} // namespace

// ---------------------------------------------------------------------------- tokens

core::Result<std::vector<CommandToken>> tokenizeCommandLine(std::string_view line) {
    if (line.size() > kMaxCommandLineBytes) {
        return core::makeError(ErrorCode::ParseError, "the command is too long (at most " +
                                                          std::to_string(kMaxCommandLineBytes) +
                                                          " bytes)");
    }
    std::vector<CommandToken> tokens;
    std::size_t at = 0;
    while (at < line.size()) {
        const char c = line[at];
        if (isSpace(c)) {
            ++at;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20U || c == 0x7F) {
            return core::makeError(ErrorCode::ParseError,
                                   "the command contains a control character");
        }
        CommandToken token;
        if (c == '"') {
            token.quoted = true;
            ++at;
            bool closed = false;
            while (at < line.size()) {
                const char d = line[at];
                if (d == '"') {
                    closed = true;
                    ++at;
                    break;
                }
                if (static_cast<unsigned char>(d) < 0x20U || d == 0x7F) {
                    return core::makeError(ErrorCode::ParseError,
                                           "the command contains a control character");
                }
                if (d == '\\' && at + 1 < line.size() &&
                    (line[at + 1] == '"' || line[at + 1] == '\\')) {
                    token.text += line[at + 1];
                    at += 2;
                    continue;
                }
                token.text += d;
                ++at;
            }
            if (!closed) {
                return core::makeError(ErrorCode::ParseError, "a quote (\") is not closed");
            }
            if (at < line.size() && !isSpace(line[at])) {
                return core::makeError(ErrorCode::ParseError,
                                       "put a space after the closing quote (\")");
            }
        } else {
            while (at < line.size() && !isSpace(line[at])) {
                const char d = line[at];
                if (d == '"') {
                    return core::makeError(ErrorCode::ParseError,
                                           "a quote (\") must start a word; quote the whole word");
                }
                if (static_cast<unsigned char>(d) < 0x20U || d == 0x7F) {
                    return core::makeError(ErrorCode::ParseError,
                                           "the command contains a control character");
                }
                token.text += d;
                ++at;
            }
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

std::string quoteCommandArgument(std::string_view text) {
    const bool plain = !text.empty() && std::none_of(text.begin(), text.end(),
                                                     [](char c) { return isSpace(c) || c == '"'; });
    if (plain) {
        return std::string(text);
    }
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\' &&
                   (i + 1 == text.size() || text[i + 1] == '"' || text[i + 1] == '\\')) {
            out += "\\\\"; // a backslash the tokenizer would otherwise read as an escape
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

// ---------------------------------------------------------------------------- calls

const std::string& CommandCall::arg(std::size_t index) const {
    static const std::string kNone;
    return index < args.size() && args[index] ? *args[index] : kNone;
}

bool CommandCall::has(std::size_t index) const {
    return index < args.size() && args[index].has_value();
}

// ---------------------------------------------------------------------------- registry

core::Result<void> CommandRegistry::add(CommandSpec spec) {
    if (spec.words.empty()) {
        return core::makeError(ErrorCode::InvalidArgument, "a command needs a name");
    }
    if (!spec.handler) {
        return core::makeError(ErrorCode::InvalidArgument,
                               "command '" + join(spec.words) + "' has no handler");
    }
    for (std::size_t i = 0; i < spec.arguments.size(); ++i) {
        const bool text = spec.arguments[i].kind == ArgumentKind::Text ||
                          spec.arguments[i].kind == ArgumentKind::OptionalText;
        if (text && i + 1 != spec.arguments.size()) {
            return core::makeError(ErrorCode::InvalidArgument,
                                   "command '" + join(spec.words) +
                                       "': a text argument must be the last one");
        }
        if (spec.arguments[i].kind == ArgumentKind::Required && i > 0 &&
            spec.arguments[i - 1].kind == ArgumentKind::Optional) {
            return core::makeError(ErrorCode::InvalidArgument,
                                   "command '" + join(spec.words) +
                                       "': a required argument cannot follow an optional one");
        }
    }
    std::vector<std::vector<std::string>> names{spec.words};
    names.insert(names.end(), spec.aliases.begin(), spec.aliases.end());
    for (const auto& words : names) {
        if (words.empty() || !std::all_of(words.begin(), words.end(), validNameWord)) {
            return core::makeError(ErrorCode::InvalidArgument,
                                   "'" + join(words) + "' is not a valid command name");
        }
        const bool taken = std::any_of(names_.begin(), names_.end(),
                                       [&](const Name& name) { return name.words == words; });
        const auto copies = std::count(names.begin(), names.end(), words);
        if (taken || copies > 1) {
            return core::makeError(ErrorCode::InvalidArgument,
                                   "the command name '" + join(words) + "' is already taken");
        }
    }
    const std::size_t index = commands_.size();
    for (std::size_t i = 0; i < names.size(); ++i) {
        names_.push_back({.words = names[i], .command = index, .alias = i > 0});
    }
    commands_.push_back(std::move(spec));
    return {};
}

const CommandSpec* CommandRegistry::find(std::string_view name) const {
    const std::vector<std::string> words = nameWords(name);
    for (const Name& candidate : names_) {
        if (candidate.words == words) {
            return &commands_[candidate.command];
        }
    }
    return nullptr;
}

std::string CommandRegistry::usage(const CommandSpec& spec) {
    std::string out = join(spec.words);
    for (const ArgumentSpec& argument : spec.arguments) {
        switch (argument.kind) {
        case ArgumentKind::Required:
            out += " <" + argument.name + ">";
            break;
        case ArgumentKind::Optional:
            out += " [" + argument.name + "]";
            break;
        case ArgumentKind::Text:
            out += " <" + argument.name + "…>";
            break;
        case ArgumentKind::OptionalText:
            out += " [" + argument.name + "…]";
            break;
        }
    }
    return out;
}

core::Result<CommandMatch> CommandRegistry::match(const std::vector<CommandToken>& tokens) const {
    if (tokens.empty()) {
        return tl::unexpected(userError("Type a command, or `help` for the list."));
    }
    // The longest name made of the line's leading unquoted words.
    const Name* best = nullptr;
    for (const Name& name : names_) {
        if (name.words.size() > tokens.size() ||
            (best && best->words.size() >= name.words.size())) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.words.size() && same; ++i) {
            same = !tokens[i].quoted && lowered(tokens[i].text) == name.words[i];
        }
        if (same) {
            best = &name;
        }
    }
    if (best == nullptr) {
        // An incomplete name ("export", "pdf") lists what it can become.
        std::vector<std::string> typed;
        for (const CommandToken& token : tokens) {
            if (token.quoted) {
                break;
            }
            typed.push_back(lowered(token.text));
        }
        std::set<std::string> longer;
        for (const Name& name : names_) {
            if (!typed.empty() && name.words.size() > typed.size() &&
                std::equal(typed.begin(), typed.end(), name.words.begin())) {
                longer.insert(join(name.words));
            }
        }
        if (!longer.empty() && typed.size() == tokens.size()) {
            std::string list;
            for (const std::string& name : longer) {
                list += (list.empty() ? "" : ", ") + name;
            }
            return tl::unexpected(userError("Incomplete command. Did you mean: " + list + "?"));
        }
        return tl::unexpected(userError("Unknown command “" + tokens.front().text +
                                        "”. Type `help` for the list of commands."));
    }

    const CommandSpec& spec = commands_[best->command];
    CommandMatch result{.spec = &spec, .call = {.command = join(spec.words), .args = {}}};
    std::size_t at = best->words.size();
    for (const ArgumentSpec& argument : spec.arguments) {
        const std::size_t left = tokens.size() - at;
        switch (argument.kind) {
        case ArgumentKind::Required:
            if (left == 0) {
                return tl::unexpected(
                    userError("Missing <" + argument.name + ">. Usage: " + usage(spec)));
            }
            result.call.args.emplace_back(tokens[at++].text);
            break;
        case ArgumentKind::Optional:
            if (left == 0) {
                result.call.args.emplace_back(std::nullopt);
            } else {
                result.call.args.emplace_back(tokens[at++].text);
            }
            break;
        case ArgumentKind::Text:
        case ArgumentKind::OptionalText: {
            if (left == 0) {
                if (argument.kind == ArgumentKind::OptionalText) {
                    result.call.args.emplace_back(std::nullopt);
                    break;
                }
                return tl::unexpected(
                    userError("Missing <" + argument.name + ">. Usage: " + usage(spec)));
            }
            std::string text;
            for (; at < tokens.size(); ++at) {
                text += (text.empty() ? "" : " ") + tokens[at].text;
            }
            result.call.args.emplace_back(std::move(text));
            break;
        }
        }
    }
    if (at < tokens.size()) {
        return tl::unexpected(
            userError("Too many arguments (“" + tokens[at].text + "”). Usage: " + usage(spec)));
    }
    return result;
}

core::Result<CommandMatch> CommandRegistry::parse(std::string_view line) const {
    auto tokens = tokenizeCommandLine(line);
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    return match(*tokens);
}

std::vector<std::string> CommandRegistry::complete(std::string_view input) const {
    auto tokens = tokenizeCommandLine(input);
    if (!tokens || std::any_of(tokens->begin(), tokens->end(),
                               [](const CommandToken& token) { return token.quoted; })) {
        return {};
    }
    // Words typed so far; the last one is still being typed unless the input ends in a space.
    std::vector<std::string> typed;
    for (const CommandToken& token : *tokens) {
        typed.push_back(lowered(token.text));
    }
    const bool partial = !input.empty() && !isSpace(input.back()) && !typed.empty();
    std::set<std::string> found;
    for (const Name& name : names_) {
        const std::size_t whole = partial ? typed.size() - 1 : typed.size();
        if (typed.size() > name.words.size() || (!partial && typed.size() == name.words.size())) {
            continue; // the input is past this name
        }
        bool fits = std::equal(typed.begin(), typed.begin() + static_cast<std::ptrdiff_t>(whole),
                               name.words.begin());
        if (fits && partial) {
            fits = name.words[whole].starts_with(typed.back());
        }
        if (fits) {
            found.insert(join(name.words));
        }
    }
    return {found.begin(), found.end()};
}

void CommandRegistry::writeHelp(ConsoleSink& out) const {
    out.heading("Commands");
    std::size_t width = 0;
    for (const CommandSpec& spec : commands_) {
        width = std::max(width, usage(spec).size());
    }
    for (const CommandSpec& spec : commands_) {
        std::string line = usage(spec);
        line.append(width + 2 - std::min(width + 1, line.size()), ' ');
        out.text(line + spec.summary, 1);
    }
    out.muted("Quote text with spaces: pdf search \"Fourier transform\". "
              "Tab completes, ↑/↓ browse the history, `help <command>` explains one.");
}

core::Result<void> CommandRegistry::writeHelp(std::string_view command, ConsoleSink& out) const {
    const CommandSpec* spec = find(command);
    if (spec == nullptr) {
        return core::makeError(ErrorCode::NotFound, "no command “" + std::string(command) +
                                                        "”. Type `help` for the list of commands.");
    }
    out.heading(usage(*spec));
    out.text(spec->summary, 1);
    if (!spec->details.empty()) {
        std::size_t at = 0;
        while (at <= spec->details.size()) {
            const std::size_t end = std::min(spec->details.find('\n', at), spec->details.size());
            out.text(spec->details.substr(at, end - at), 1);
            at = end + 1;
        }
    }
    if (!spec->aliases.empty()) {
        std::string list;
        for (const auto& alias : spec->aliases) {
            list += (list.empty() ? "" : ", ") + join(alias);
        }
        out.muted("Also: " + list, 1);
    }
    for (const std::string& example : spec->examples) {
        out.muted("Example: " + example, 1);
    }
    return {};
}

bool runCommandLine(const CommandRegistry& registry, std::string_view line, ConsoleSink& out) {
    out.write({.text = "> " + std::string(line), .style = ConsoleLine::Style::Echo});
    auto matched = registry.parse(line);
    if (!matched) {
        out.error(matched.error().message);
        return false;
    }
    core::Result<void> ran;
    try {
        ran = matched->spec->handler(matched->call, out);
    } catch (const std::exception& error) { // e.g. std::bad_alloc, filesystem errors
        ran = core::makeError(ErrorCode::Internal, error.what());
    }
    if (!ran) {
        out.error(matched->call.command + ": " + ran.error().message);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------- history

CommandHistory::CommandHistory(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)) {}

void CommandHistory::add(std::string_view line) {
    resetNavigation();
    const bool blank = std::all_of(line.begin(), line.end(), isSpace);
    if (blank || (!entries_.empty() && entries_.back() == line)) {
        return;
    }
    entries_.emplace_back(line);
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
}

std::optional<std::string> CommandHistory::previous(std::string_view draft) {
    if (entries_.empty()) {
        return std::nullopt;
    }
    if (!position_) {
        draft_ = std::string(draft);
        position_ = entries_.size() - 1;
        return entries_[*position_];
    }
    if (*position_ == 0) {
        return std::nullopt;
    }
    --*position_;
    return entries_[*position_];
}

std::optional<std::string> CommandHistory::next() {
    if (!position_) {
        return std::nullopt;
    }
    if (*position_ + 1 >= entries_.size()) {
        position_.reset();
        return std::exchange(draft_, {});
    }
    ++*position_;
    return entries_[*position_];
}

void CommandHistory::resetNavigation() noexcept {
    position_.reset();
    draft_.clear();
}

} // namespace studyapp::application
