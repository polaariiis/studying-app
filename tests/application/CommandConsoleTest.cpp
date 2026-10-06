// The command console's language (1.2-CMD-01, docs/COMMAND_CONSOLE.md): tokenizer, registry
// (lookup, aliases, arguments, help, completion), running lines, and the history. Qt-free.

#include <studyapp/application/CommandConsole.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace studyapp::application {
namespace {

using core::ErrorCode;

/// Collects what commands write.
class RecordingSink final : public ConsoleSink {
public:
    void write(ConsoleLine line) override { lines.push_back(std::move(line)); }
    [[nodiscard]] std::string text() const {
        std::string out;
        for (const ConsoleLine& line : lines) {
            out += line.text + "\n";
        }
        return out;
    }
    [[nodiscard]] bool hasError() const {
        return std::any_of(lines.begin(), lines.end(), [](const ConsoleLine& l) {
            return l.style == ConsoleLine::Style::Error;
        });
    }
    std::vector<ConsoleLine> lines;
};

std::vector<std::string> words(const std::vector<CommandToken>& tokens) {
    std::vector<std::string> out;
    for (const CommandToken& token : tokens) {
        out.push_back(token.text);
    }
    return out;
}

std::vector<std::string> tokenize(std::string_view line) {
    auto tokens = tokenizeCommandLine(line);
    EXPECT_TRUE(tokens.has_value()) << line;
    return tokens ? words(*tokens) : std::vector<std::string>{};
}

ErrorCode tokenizeError(std::string_view line) {
    auto tokens = tokenizeCommandLine(line);
    return tokens ? ErrorCode::Internal : tokens.error().code;
}

// ---------------------------------------------------------------------------- tokenizer

TEST(CommandConsoleTest, EmptyAndBlankLinesHaveNoWords) {
    EXPECT_TRUE(tokenize("").empty());
    EXPECT_TRUE(tokenize("   \t  ").empty());
}

TEST(CommandConsoleTest, WordsAreSplitAtSpacesAndTabs) {
    EXPECT_EQ(tokenize("  list   pages\tall "), (std::vector<std::string>{"list", "pages", "all"}));
}

TEST(CommandConsoleTest, QuotedWordsKeepSpacesAndAreMarked) {
    auto tokens = tokenizeCommandLine(R"(pdf search "Fourier  transform" x)");
    ASSERT_TRUE(tokens.has_value());
    ASSERT_EQ(tokens->size(), 4U);
    EXPECT_EQ((*tokens)[2].text, "Fourier  transform");
    EXPECT_TRUE((*tokens)[2].quoted);
    EXPECT_FALSE((*tokens)[0].quoted);
    EXPECT_EQ(tokenize(R"("")"), (std::vector<std::string>{""})); // an empty quoted word
}

TEST(CommandConsoleTest, PathsWithSpacesAndBackslashes) {
    EXPECT_EQ(
        tokenize(R"(import pdf "C:\Users\Me\My Lectures\05 Fourier.pdf")"),
        (std::vector<std::string>{"import", "pdf", R"(C:\Users\Me\My Lectures\05 Fourier.pdf)"}));
    EXPECT_EQ(tokenize(R"(import pdf "/home/me/My Notes/a.pdf")"),
              (std::vector<std::string>{"import", "pdf", "/home/me/My Notes/a.pdf"}));
    // Unquoted backslashes are plain characters too.
    EXPECT_EQ(tokenize(R"(import pdf C:\x\y.pdf)"),
              (std::vector<std::string>{"import", "pdf", R"(C:\x\y.pdf)"}));
}

TEST(CommandConsoleTest, EscapesInsideQuotes) {
    EXPECT_EQ(tokenize(R"(search "say \"hi\" \\ now")"),
              (std::vector<std::string>{"search", R"(say "hi" \ now)"}));
    // A path ending in a backslash needs it doubled before the closing quote.
    EXPECT_EQ(tokenize(R"("C:\dir\\")"), (std::vector<std::string>{R"(C:\dir\)"}));
}

TEST(CommandConsoleTest, InvalidSyntaxIsAParseError) {
    EXPECT_EQ(tokenizeError(R"(search "unterminated)"), ErrorCode::ParseError);
    EXPECT_EQ(tokenizeError(R"(search a"b")"), ErrorCode::ParseError);
    EXPECT_EQ(tokenizeError(R"(search "a"b)"), ErrorCode::ParseError);
    EXPECT_EQ(tokenizeError(std::string("search a\x01")), ErrorCode::ParseError);
    EXPECT_EQ(tokenizeError(std::string("search \"a\nb\"")), ErrorCode::ParseError);
    EXPECT_EQ(tokenizeError(std::string(kMaxCommandLineBytes + 1, 'a')), ErrorCode::ParseError);
    EXPECT_TRUE(tokenizeCommandLine(std::string(kMaxCommandLineBytes, 'a')).has_value());
}

TEST(CommandConsoleTest, NonAsciiTextPassesThrough) {
    EXPECT_EQ(tokenize("pdf search \"Eigenwert \xC3\xBC\xE2\x9C\x93\""),
              (std::vector<std::string>{"pdf", "search", "Eigenwert \xC3\xBC\xE2\x9C\x93"}));
}

TEST(CommandConsoleTest, QuotingRoundTrips) {
    for (const std::string text : {"plain", "two words", R"(C:\a b\c.pdf)", R"(say "hi")",
                                   R"(ends\)", "", R"(\\server\share x)"}) {
        auto tokens = tokenizeCommandLine("x " + quoteCommandArgument(text));
        ASSERT_TRUE(tokens.has_value()) << text;
        ASSERT_EQ(tokens->size(), 2U) << text;
        EXPECT_EQ((*tokens)[1].text, text);
    }
    EXPECT_EQ(quoteCommandArgument("plain"), "plain");
}

// ---------------------------------------------------------------------------- registry

struct Calls {
    std::vector<CommandCall> seen;
};

CommandSpec spec(std::vector<std::string> name, std::vector<ArgumentSpec> arguments, Calls& calls,
                 std::vector<std::vector<std::string>> aliases = {}) {
    return {.words = std::move(name),
            .aliases = std::move(aliases),
            .arguments = std::move(arguments),
            .summary = "does things",
            .details = "line one\nline two",
            .examples = {"example 1"},
            .handler = [&calls](const CommandCall& call, ConsoleSink&) -> core::Result<void> {
                calls.seen.push_back(call);
                return {};
            }};
}

class RegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(registry.add(
            spec({"help"}, {{"command", ArgumentKind::OptionalText}}, calls, {{"?"}})));
        ASSERT_TRUE(registry.add(
            spec({"list", "pages"}, {{"all", ArgumentKind::Optional}}, calls, {{"pages"}})));
        ASSERT_TRUE(registry.add(
            spec({"pdf", "search"}, {{"text", ArgumentKind::Text}}, calls, {{"pdf", "find"}})));
        ASSERT_TRUE(
            registry.add(spec({"import", "pdf"}, {{"path", ArgumentKind::Required}}, calls)));
        ASSERT_TRUE(registry.add(
            spec({"export", "page", "pdf"}, {{"path", ArgumentKind::Optional}}, calls)));
        ASSERT_TRUE(registry.add(
            spec({"export", "page", "png"}, {{"path", ArgumentKind::Optional}}, calls)));
        ASSERT_TRUE(registry.add(
            spec({"export", "page", "svg"}, {{"path", ArgumentKind::Optional}}, calls)));
        ASSERT_TRUE(registry.add(
            spec({"export", "section", "pdf"}, {{"path", ArgumentKind::Optional}}, calls)));
        ASSERT_TRUE(registry.add(spec({"workspace", "info"}, {}, calls, {{"workspace"}})));
    }

    core::Result<CommandMatch> parse(std::string_view line) const { return registry.parse(line); }
    std::string parseError(std::string_view line) const {
        auto matched = registry.parse(line);
        EXPECT_FALSE(matched.has_value()) << line;
        return matched ? std::string() : matched.error().message;
    }

    Calls calls;
    CommandRegistry registry;
};

TEST_F(RegistryTest, RegistersAndFindsByNameAndAlias) {
    EXPECT_EQ(registry.size(), 9U);
    ASSERT_NE(registry.find("pdf search"), nullptr);
    EXPECT_EQ(registry.find("PDF  Search"), registry.find("pdf search")); // case, spaces
    EXPECT_EQ(registry.find("pdf find"), registry.find("pdf search"));
    EXPECT_EQ(registry.find("pages"), registry.find("list pages"));
    EXPECT_EQ(registry.find("?"), registry.find("help"));
    EXPECT_EQ(registry.find("pdf"), nullptr);
    EXPECT_EQ(registry.find("nothing"), nullptr);
}

TEST_F(RegistryTest, RefusesBadSpecs) {
    Calls other;
    EXPECT_FALSE(registry.add(spec({}, {}, other)));        // no name
    EXPECT_FALSE(registry.add(spec({"Upper"}, {}, other))); // not a name
    EXPECT_FALSE(registry.add(spec({"has space"}, {}, other)));
    EXPECT_FALSE(registry.add(spec({"pdf", "search"}, {}, other)));      // taken
    EXPECT_FALSE(registry.add(spec({"new"}, {}, other, {{"pages"}})));   // alias taken
    EXPECT_FALSE(registry.add(spec({"twice"}, {}, other, {{"twice"}}))); // own alias
    EXPECT_FALSE(registry.add(
        spec({"x"}, {{"a", ArgumentKind::Text}, {"b", ArgumentKind::Required}}, other)));
    EXPECT_FALSE(registry.add(
        spec({"y"}, {{"a", ArgumentKind::Optional}, {"b", ArgumentKind::Required}}, other)));
    CommandSpec noHandler = spec({"z"}, {}, other);
    noHandler.handler = nullptr;
    EXPECT_FALSE(registry.add(std::move(noHandler)));
    EXPECT_EQ(registry.size(), 9U); // nothing half-added
    EXPECT_EQ(registry.find("new"), nullptr);
}

TEST_F(RegistryTest, MatchesCommandsAndBindsArguments) {
    auto m = parse(R"(PDF Search "Fourier transform")");
    ASSERT_TRUE(m.has_value()) << m.error().message;
    EXPECT_EQ(m->call.command, "pdf search");
    EXPECT_EQ(m->call.arg(0), "Fourier transform");

    m = parse("pdf search eigen value");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->call.arg(0), "eigen value"); // text: the rest of the line

    m = parse("pdf find x");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->call.command, "pdf search"); // aliases run the canonical command

    m = parse(R"(import pdf "/home/me/My Notes/a.pdf")");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->call.arg(0), "/home/me/My Notes/a.pdf");

    m = parse("list pages");
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->call.has(0));
    EXPECT_EQ(m->call.arg(0), "");
    m = parse("pages all");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->call.arg(0), "all");

    // The longest name wins: "workspace info" over the alias "workspace".
    m = parse("workspace info");
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->call.has(0));
    m = parse("help pdf search");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->call.arg(0), "pdf search");
}

TEST_F(RegistryTest, QuotedWordsAreNeverCommandNames) {
    EXPECT_NE(parseError(R"("pdf" search x)").find("Unknown command"), std::string::npos);
    auto m = parse(R"(pdf search "pdf search")");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->call.arg(0), "pdf search");
}

TEST_F(RegistryTest, ExplainsWhatIsWrong) {
    EXPECT_NE(parseError("").find("help"), std::string::npos);
    EXPECT_NE(parseError("   ").find("help"), std::string::npos);
    EXPECT_NE(parseError("rm -rf /").find("Unknown command \u201Crm\u201D"), std::string::npos);
    EXPECT_NE(parseError("pdf search").find("Missing <text>"), std::string::npos);
    EXPECT_NE(parseError("pdf search").find("Usage: pdf search <text…>"), std::string::npos);
    EXPECT_NE(parseError("import pdf").find("Missing <path>"), std::string::npos);
    EXPECT_NE(parseError("import pdf a b").find("Too many arguments (\u201Cb\u201D)"),
              std::string::npos);
    EXPECT_NE(parseError("workspace info now").find("Too many"), std::string::npos);
    const std::string incomplete = parseError("export");
    EXPECT_NE(incomplete.find("Incomplete command"), std::string::npos);
    EXPECT_NE(incomplete.find("export page pdf, export page png, export page svg, "
                              "export section pdf"),
              std::string::npos);
    EXPECT_NE(parseError("export page").find("export page svg"), std::string::npos);
    EXPECT_NE(parseError(R"(search "x)").find("not closed"), std::string::npos);
    EXPECT_TRUE(calls.seen.empty()); // nothing ran
}

TEST_F(RegistryTest, UsageShowsArgumentKinds) {
    EXPECT_EQ(CommandRegistry::usage(*registry.find("pdf search")), "pdf search <text…>");
    EXPECT_EQ(CommandRegistry::usage(*registry.find("import pdf")), "import pdf <path>");
    EXPECT_EQ(CommandRegistry::usage(*registry.find("list pages")), "list pages [all]");
    EXPECT_EQ(CommandRegistry::usage(*registry.find("help")), "help [command…]");
}

TEST_F(RegistryTest, HelpListsEveryCommandAndExplainsOne) {
    RecordingSink all;
    registry.writeHelp(all);
    for (const CommandSpec& command : registry.commands()) {
        EXPECT_NE(all.text().find(CommandRegistry::usage(command)), std::string::npos);
    }
    RecordingSink one;
    ASSERT_TRUE(registry.writeHelp("pdf find", one)); // by alias
    EXPECT_EQ(one.lines.front().text, "pdf search <text…>");
    EXPECT_NE(one.text().find("line two"), std::string::npos);
    EXPECT_NE(one.text().find("Also: pdf find"), std::string::npos);
    EXPECT_NE(one.text().find("Example: example 1"), std::string::npos);
    RecordingSink none;
    auto missing = registry.writeHelp("nope", none);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST_F(RegistryTest, CompletesCommandNamesDeterministically) {
    using V = std::vector<std::string>;
    EXPECT_EQ(registry.complete("p"), (V{"pages", "pdf find", "pdf search"}));
    EXPECT_EQ(registry.complete("pdf"), (V{"pdf find", "pdf search"}));
    EXPECT_EQ(registry.complete("pdf "), (V{"pdf find", "pdf search"}));
    EXPECT_EQ(registry.complete("pdf s"), (V{"pdf search"}));
    EXPECT_EQ(registry.complete("PDF S"), (V{"pdf search"}));
    EXPECT_EQ(registry.complete("export"),
              (V{"export page pdf", "export page png", "export page svg", "export section pdf"}));
    EXPECT_EQ(registry.complete("export page p"), (V{"export page pdf", "export page png"}));
    EXPECT_EQ(registry.complete("w"), (V{"workspace", "workspace info"}));
    EXPECT_EQ(registry.complete("workspace "), (V{"workspace info"}));
    // Past the name, or nothing that fits: no suggestions.
    EXPECT_TRUE(registry.complete("pdf search ").empty());
    EXPECT_TRUE(registry.complete("pdf search x").empty());
    EXPECT_TRUE(registry.complete("zzz").empty());
    EXPECT_TRUE(registry.complete(R"("p)").empty());
    EXPECT_EQ(registry.complete("").size(), 13U); // every name, canonical and alias
}

// ---------------------------------------------------------------------------- running lines

TEST(CommandRunTest, RunsTheCommandAndEchoesTheLine) {
    CommandRegistry registry;
    std::string seen;
    ASSERT_TRUE(registry.add(
        {.words = {"say"},
         .aliases = {},
         .arguments = {{"text", ArgumentKind::Text}},
         .summary = "",
         .details = {},
         .examples = {},
         .handler = [&](const CommandCall& call, ConsoleSink& out) -> core::Result<void> {
             seen = call.arg(0);
             out.text("said " + call.arg(0));
             return {};
         }}));
    RecordingSink out;
    EXPECT_TRUE(runCommandLine(registry, R"(say "hello world")", out));
    EXPECT_EQ(seen, "hello world");
    ASSERT_EQ(out.lines.size(), 2U);
    EXPECT_EQ(out.lines[0].style, ConsoleLine::Style::Echo);
    EXPECT_EQ(out.lines[0].text, R"(> say "hello world")");
    EXPECT_EQ(out.lines[1].text, "said hello world");
}

TEST(CommandRunTest, FailuresAreReportedNotThrown) {
    CommandRegistry registry;
    ASSERT_TRUE(
        registry.add({.words = {"fail"},
                      .aliases = {},
                      .arguments = {},
                      .summary = "",
                      .details = {},
                      .examples = {},
                      .handler = [](const CommandCall&, ConsoleSink&) -> core::Result<void> {
                          return core::makeError(ErrorCode::NotFound, "no such page");
                      }}));
    ASSERT_TRUE(
        registry.add({.words = {"throw"},
                      .aliases = {},
                      .arguments = {},
                      .summary = "",
                      .details = {},
                      .examples = {},
                      .handler = [](const CommandCall&, ConsoleSink&) -> core::Result<void> {
                          throw std::runtime_error("disk on fire");
                      }}));
    RecordingSink out;
    EXPECT_FALSE(runCommandLine(registry, "fail", out));
    EXPECT_EQ(out.lines.back().style, ConsoleLine::Style::Error);
    EXPECT_EQ(out.lines.back().text, "fail: no such page");
    EXPECT_FALSE(runCommandLine(registry, "throw", out));
    EXPECT_EQ(out.lines.back().text, "throw: disk on fire");
    EXPECT_FALSE(runCommandLine(registry, "unknown thing", out));
    EXPECT_NE(out.lines.back().text.find("Unknown command"), std::string::npos);
    EXPECT_FALSE(runCommandLine(registry, R"(fail "open)", out));
    EXPECT_EQ(out.lines.back().style, ConsoleLine::Style::Error);
}

TEST(CommandRunTest, ShellSyntaxIsJustText) {
    // Nothing is interpreted: no pipes, substitutions, variables or redirections.
    CommandRegistry registry;
    std::vector<std::string> seen;
    ASSERT_TRUE(
        registry.add({.words = {"search"},
                      .aliases = {},
                      .arguments = {{"text", ArgumentKind::Text}},
                      .summary = "",
                      .details = {},
                      .examples = {},
                      .handler = [&](const CommandCall& call, ConsoleSink&) -> core::Result<void> {
                          seen.push_back(call.arg(0));
                          return {};
                      }}));
    RecordingSink out;
    for (const char* line : {"search $(whoami)", "search `id`; rm -rf ~", "search a | sh",
                             "search $HOME > /tmp/x", "search a && b"}) {
        EXPECT_TRUE(runCommandLine(registry, line, out)) << line;
    }
    EXPECT_EQ(seen, (std::vector<std::string>{"$(whoami)", "`id`; rm -rf ~", "a | sh",
                                              "$HOME > /tmp/x", "a && b"}));
    EXPECT_FALSE(runCommandLine(registry, "$(whoami)", out));
    EXPECT_FALSE(runCommandLine(registry, "!ls", out));
}

TEST(ConsoleSinkTest, HelpersSetStyleTargetAndIndent) {
    RecordingSink out;
    const core::PageId page{core::Uuid::parse("01890a5d-ac96-774b-bcce-b302099a8057").value()};
    out.link("Page 7", page, 2);
    out.heading("h");
    out.muted("m", 1);
    out.error("e");
    ASSERT_EQ(out.lines.size(), 4U);
    ASSERT_TRUE(out.lines[0].target.has_value());
    EXPECT_EQ(std::get<core::PageId>(*out.lines[0].target), page);
    EXPECT_EQ(out.lines[0].indent, 2);
    EXPECT_EQ(out.lines[1].style, ConsoleLine::Style::Heading);
    EXPECT_EQ(out.lines[2].style, ConsoleLine::Style::Muted);
    EXPECT_EQ(out.lines[3].style, ConsoleLine::Style::Error);
    EXPECT_TRUE(out.hasError());
}

// ---------------------------------------------------------------------------- history

TEST(CommandHistoryTest, NavigatesLikeAShell) {
    CommandHistory history;
    EXPECT_FALSE(history.previous("draft"));
    EXPECT_FALSE(history.next());
    history.add("help");
    history.add("list pages");
    history.add("pdf search x");
    EXPECT_EQ(history.previous("typing"), "pdf search x");
    EXPECT_EQ(history.previous("ignored"), "list pages");
    EXPECT_EQ(history.previous(""), "help");
    EXPECT_FALSE(history.previous("")); // the oldest: stays
    EXPECT_EQ(history.next(), "list pages");
    EXPECT_EQ(history.next(), "pdf search x");
    EXPECT_EQ(history.next(), "typing"); // back to what was being typed
    EXPECT_FALSE(history.next());
}

TEST(CommandHistoryTest, SkipsBlanksAndConsecutiveDuplicates) {
    CommandHistory history;
    history.add("help");
    history.add("help");
    history.add("   ");
    history.add("");
    history.add("list pages");
    history.add("help");
    EXPECT_EQ(history.entries(), (std::deque<std::string>{"help", "list pages", "help"}));
}

TEST(CommandHistoryTest, IsBounded) {
    CommandHistory history(3);
    for (int i = 0; i < 10; ++i) {
        history.add("command " + std::to_string(i));
    }
    EXPECT_EQ(history.size(), 3U);
    EXPECT_EQ(history.capacity(), 3U);
    EXPECT_EQ(history.entries().front(), "command 7");
    EXPECT_EQ(history.entries().back(), "command 9");
    EXPECT_EQ(CommandHistory().capacity(), CommandHistory::kDefaultCapacity);
    EXPECT_EQ(CommandHistory(0).capacity(), 1U);
}

TEST(CommandHistoryTest, AddingOrEditingEndsNavigation) {
    CommandHistory history;
    history.add("a");
    history.add("b");
    EXPECT_EQ(history.previous(""), "b");
    history.resetNavigation();
    EXPECT_FALSE(history.next());
    EXPECT_EQ(history.previous(""), "b"); // starts again at the newest
    history.add("c");
    EXPECT_EQ(history.previous(""), "c");
}

} // namespace
} // namespace studyapp::application
