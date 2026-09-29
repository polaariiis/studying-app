#pragma once

#include <QString>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

class QPrinter;
class QWidget;

namespace studyapp::ui {

/// Every question and message the shell may put in front of the user, behind one
/// interface: the default implementation uses native Qt dialogs, tests substitute
/// scripted answers.
///
/// By design this has no "Save changes?" question: edits are saved continuously
/// (docs/ARCHITECTURE.md §11). Prompts exist only for exceptional situations: choosing a
/// workspace, a workspace in use or left behind by a crash, deleting structure, and
/// changes that could not be written.
class ShellDialogs {
public:
    ShellDialogs() = default;
    virtual ~ShellDialogs() = default;
    ShellDialogs(const ShellDialogs&) = delete;
    ShellDialogs& operator=(const ShellDialogs&) = delete;
    ShellDialogs(ShellDialogs&&) = delete;
    ShellDialogs& operator=(ShellDialogs&&) = delete;

    /// Directory for a new workspace (it must not exist or be empty); nullopt: cancelled.
    virtual std::optional<std::filesystem::path> chooseNewWorkspace(QWidget* parent) = 0;
    /// An existing workspace directory; nullopt: cancelled.
    virtual std::optional<std::filesystem::path> chooseWorkspaceToOpen(QWidget* parent) = 0;
    /// An image file to insert on the canvas; nullopt: cancelled.
    virtual std::optional<std::filesystem::path> chooseImageToInsert(QWidget* parent) = 0;
    /// A PDF to import as a section of pages; nullopt: cancelled.
    virtual std::optional<std::filesystem::path> chooseDocumentToImport(QWidget* parent) = 0;
    /// Where to export: a .pdf, .png or .svg file (only .pdf if `pdfOnly`), proposed as
    /// `suggestedName`; the dialog asks before replacing a file. nullopt: cancelled.
    virtual std::optional<std::filesystem::path>
    chooseExportTarget(QWidget* parent, const QString& suggestedName, bool pdfOnly) = 0;
    /// Where to write a bundle (.studybundle), proposed as `suggestedName`; the dialog asks
    /// before replacing a file. nullopt: cancelled.
    virtual std::optional<std::filesystem::path>
    chooseBundleTarget(QWidget* parent, const QString& suggestedName) = 0;
    /// A bundle to import or open; nullopt: cancelled.
    virtual std::optional<std::filesystem::path> chooseBundleToOpen(QWidget* parent) = 0;
    /// Lets the user choose and set up `printer` for a section of `pages` pages (all of
    /// them, a range, or the current page); false: cancelled.
    virtual bool setUpPrinter(QWidget* parent, QPrinter& printer, int pages) = 0;

    /// The workspace is open in another process: open it read-only instead?
    virtual bool confirmOpenReadOnly(QWidget* parent, const QString& details) = 0;

    enum class StaleLockChoice {
        Recover,  ///< check the database, take over the lock and continue
        ReadOnly, ///< look without changing anything
        Cancel,
    };
    /// The workspace was not closed properly (crash).
    virtual StaleLockChoice askStaleLock(QWidget* parent, const QString& details) = 0;

    /// Deleting `what` (e.g. "the section “Lecture notes” and its 12 pages"); undoable.
    virtual bool confirmDelete(QWidget* parent, const QString& what) = 0;

    enum class UnsavedChoice {
        Retry,   ///< try writing again
        Discard, ///< close anyway; the unsaved changes are lost
        Cancel,  ///< keep the workspace open
    };
    /// Closing, but `pending` changes could not be written.
    virtual UnsavedChoice askUnsavedChanges(QWidget* parent, std::size_t pending,
                                            const QString& details) = 0;

    /// An operation failed: `summary` for the user, `details` for diagnosis.
    virtual void showError(QWidget* parent, const QString& summary, const QString& details) = 0;

    /// A line of text (e.g. a new course name), starting from `text`; nullopt: cancelled.
    virtual std::optional<QString> askText(QWidget* parent, const QString& title,
                                           const QString& label, const QString& text) = 0;
};

/// Native file dialogs and message boxes.
[[nodiscard]] std::unique_ptr<ShellDialogs> makeQtShellDialogs();

} // namespace studyapp::ui
