#include <studyapp/ui/ShellDialogs.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>

namespace studyapp::ui {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("ShellDialogs", text);
}

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

QString documentsDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

class QtShellDialogs final : public ShellDialogs {
public:
    std::optional<std::filesystem::path> chooseNewWorkspace(QWidget* parent) override {
        // A workspace is a directory; the chosen "file name" becomes it.
        QString chosen = QFileDialog::getSaveFileName(
            parent, tr("New Workspace"),
            QDir(documentsDirectory()).filePath(tr("My Notes.studyws")),
            tr("StudyBoard workspace (*.studyws)"), nullptr, QFileDialog::DontConfirmOverwrite);
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        if (!chosen.endsWith(QStringLiteral(".studyws"), Qt::CaseInsensitive)) {
            chosen += QStringLiteral(".studyws");
        }
        return toPath(chosen);
    }

    std::optional<std::filesystem::path> chooseWorkspaceToOpen(QWidget* parent) override {
        const QString chosen = QFileDialog::getExistingDirectory(
            parent, tr("Open Workspace"), documentsDirectory(), QFileDialog::ShowDirsOnly);
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return toPath(chosen);
    }

    bool confirmOpenReadOnly(QWidget* parent, const QString& details) override {
        QMessageBox box(QMessageBox::Information, tr("Workspace in use"),
                        tr("This workspace is open in another StudyBoard window or on another "
                           "computer. You can open it read-only to look at it."),
                        QMessageBox::NoButton, parent);
        box.setDetailedText(details);
        QPushButton* readOnly = box.addButton(tr("Open Read-Only"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(readOnly);
        box.exec();
        return box.clickedButton() == readOnly;
    }

    StaleLockChoice askStaleLock(QWidget* parent, const QString& details) override {
        QMessageBox box(QMessageBox::Warning, tr("Workspace not closed properly"),
                        tr("StudyBoard did not close this workspace properly last time. It can "
                           "check the workspace and continue, or open it read-only."),
                        QMessageBox::NoButton, parent);
        box.setDetailedText(details);
        QPushButton* recover = box.addButton(tr("Check and Continue"), QMessageBox::AcceptRole);
        QPushButton* readOnly = box.addButton(tr("Open Read-Only"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(recover);
        box.exec();
        if (box.clickedButton() == recover) {
            return StaleLockChoice::Recover;
        }
        return box.clickedButton() == readOnly ? StaleLockChoice::ReadOnly
                                               : StaleLockChoice::Cancel;
    }

    bool confirmDelete(QWidget* parent, const QString& what) override {
        QMessageBox box(QMessageBox::Question, tr("Delete"),
                        tr("Delete %1?").arg(what) + QStringLiteral("\n\n") +
                            tr("You can undo this with Edit ▸ Undo."),
                        QMessageBox::NoButton, parent);
        QPushButton* remove = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        return box.clickedButton() == remove;
    }

    UnsavedChoice askUnsavedChanges(QWidget* parent, std::size_t pending,
                                    const QString& details) override {
        QMessageBox box(QMessageBox::Warning, tr("Changes not saved"),
                        QCoreApplication::translate(
                            "ShellDialogs",
                            "%n change(s) could not be saved to the workspace. Try again, or "
                            "close anyway and lose them?",
                            nullptr, static_cast<int>(pending)),
                        QMessageBox::NoButton, parent);
        box.setDetailedText(details);
        QPushButton* retry = box.addButton(tr("Try Again"), QMessageBox::AcceptRole);
        QPushButton* discard =
            box.addButton(tr("Close Without Saving"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(retry);
        box.exec();
        if (box.clickedButton() == retry) {
            return UnsavedChoice::Retry;
        }
        return box.clickedButton() == discard ? UnsavedChoice::Discard : UnsavedChoice::Cancel;
    }

    void showError(QWidget* parent, const QString& summary, const QString& details) override {
        QMessageBox box(QMessageBox::Critical, tr("StudyBoard"), summary, QMessageBox::Ok, parent);
        if (!details.isEmpty()) {
            box.setDetailedText(details);
        }
        box.exec();
    }
};

} // namespace

std::unique_ptr<ShellDialogs> makeQtShellDialogs() {
    return std::make_unique<QtShellDialogs>();
}

} // namespace studyapp::ui
