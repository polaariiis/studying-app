#include <studyapp/ui/ShellDialogs.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
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

/// A save dialog whose default suffix follows the chosen filter, so a name typed without an
/// extension gets it *before* the dialog asks whether to replace an existing file.
std::optional<std::filesystem::path> saveFile(QWidget* parent, const QString& title,
                                              const QString& suggestedName,
                                              const QStringList& filters,
                                              const QStringList& suffixes) {
    QFileDialog dialog(
        parent, title,
        QDir(documentsDirectory()).filePath(suggestedName + QLatin1Char('.') + suffixes.front()));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setDefaultSuffix(suffixes.front());
    QObject::connect(&dialog, &QFileDialog::filterSelected, &dialog, [&](const QString& filter) {
        dialog.setDefaultSuffix(suffixes.value(filters.indexOf(filter), suffixes.front()));
    });
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return std::nullopt;
    }
    return toPath(dialog.selectedFiles().front());
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

    std::optional<std::filesystem::path> chooseImageToInsert(QWidget* parent) override {
        const QString chosen = QFileDialog::getOpenFileName(
            parent, tr("Insert Image"), documentsDirectory(),
            tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All files (*)"));
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return toPath(chosen);
    }

    std::optional<std::filesystem::path> chooseDocumentToImport(QWidget* parent) override {
        const QString chosen =
            QFileDialog::getOpenFileName(parent, tr("Import PDF"), documentsDirectory(),
                                         tr("PDF documents (*.pdf);;All files (*)"));
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return toPath(chosen);
    }

    std::optional<std::filesystem::path>
    chooseExportTarget(QWidget* parent, const QString& suggestedName, bool pdfOnly) override {
        QStringList filters{tr("PDF document (*.pdf)")};
        QStringList suffixes{QStringLiteral("pdf")};
        if (!pdfOnly) {
            filters << tr("PNG image (*.png)") << tr("SVG image (*.svg)");
            suffixes << QStringLiteral("png") << QStringLiteral("svg");
        }
        return saveFile(parent, pdfOnly ? tr("Export Section as PDF") : tr("Export Page"),
                        suggestedName, filters, suffixes);
    }

    std::optional<std::filesystem::path> chooseBundleTarget(QWidget* parent,
                                                            const QString& suggestedName) override {
        return saveFile(parent, tr("Export Bundle"), suggestedName,
                        {tr("StudyBoard bundle (*.studybundle)")}, {QStringLiteral("studybundle")});
    }

    std::optional<std::filesystem::path> chooseBundleToOpen(QWidget* parent) override {
        const QString chosen =
            QFileDialog::getOpenFileName(parent, tr("Choose Bundle"), documentsDirectory(),
                                         tr("StudyBoard bundle (*.studybundle);;All files (*)"));
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return toPath(chosen);
    }

    bool setUpPrinter(QWidget* parent, QPrinter& printer, int pages) override {
        QPrintDialog dialog(&printer, parent);
        dialog.setMinMax(1, pages);
        dialog.setOption(QAbstractPrintDialog::PrintCurrentPage, true);
        dialog.setOption(QAbstractPrintDialog::PrintPageRange, pages > 1);
        return dialog.exec() == QDialog::Accepted;
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

    std::optional<QString> askText(QWidget* parent, const QString& title, const QString& label,
                                   const QString& text) override {
        bool accepted = false;
        const QString answer =
            QInputDialog::getText(parent, title, label, QLineEdit::Normal, text, &accepted);
        return accepted ? std::optional(answer) : std::nullopt;
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

    void showInformation(QWidget* parent, const QString& summary, const QString& details) override {
        QMessageBox box(QMessageBox::Information, tr("StudyBoard"), summary, QMessageBox::Ok,
                        parent);
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
