#include <studyapp/application/PageNavigator.hpp>

#include <studyapp/application/StartPage.hpp>

#include <algorithm>
#include <type_traits>
#include <variant>
#include <vector>

namespace studyapp::application {

namespace {

/// Every page in workspace order.
std::vector<core::PageId> allPages(const document::Workspace& workspace) {
    std::vector<core::PageId> pages;
    for (const core::NotebookId notebook : workspace.notebooks()) {
        for (const core::SectionId section : workspace.sectionsOf(notebook)) {
            const auto inSection = workspace.pagesOf(section);
            pages.insert(pages.end(), inSection.begin(), inSection.end());
        }
    }
    return pages;
}

} // namespace

PageNavigator::PageNavigator(const document::Workspace& workspace) noexcept
    : workspace_(&workspace) {}

std::optional<core::SectionId> PageNavigator::activeSection() const {
    if (!active_) {
        return std::nullopt;
    }
    const document::PageInfo* page = workspace_->findPage(*active_);
    return page != nullptr ? std::optional(page->section) : std::nullopt;
}

std::optional<core::NotebookId> PageNavigator::activeNotebook() const {
    const auto section = activeSection();
    if (!section) {
        return std::nullopt;
    }
    const document::SectionInfo* info = workspace_->findSection(*section);
    return info != nullptr ? std::optional(info->notebook) : std::nullopt;
}

core::Result<bool> PageNavigator::open(core::PageId page) {
    if (workspace_->findPage(page) == nullptr) {
        return core::makeError(core::ErrorCode::NotFound, "the page does not exist");
    }
    const bool changed = active_ != page;
    active_ = page;
    remember();
    return changed;
}

void PageNavigator::clear() noexcept {
    active_.reset();
}

void PageNavigator::remember() {
    const document::PageInfo* page = active_ ? workspace_->findPage(*active_) : nullptr;
    if (page == nullptr) {
        return;
    }
    section_ = page->section;
    const auto siblings = workspace_->pagesOf(page->section);
    index_ = static_cast<std::size_t>(std::find(siblings.begin(), siblings.end(), *active_) -
                                      siblings.begin());
    if (const document::SectionInfo* section = workspace_->findSection(page->section)) {
        notebook_ = section->notebook;
    }
}

std::optional<core::PageId> PageNavigator::fallback() const {
    // Same section: the page that took the removed page's place, else the one before it.
    const auto siblings = workspace_->pagesOf(section_);
    if (!siblings.empty()) {
        return siblings[std::min(index_, siblings.size() - 1)];
    }
    // Same notebook, else anywhere.
    if (workspace_->findNotebook(notebook_) != nullptr) {
        for (const core::SectionId section : workspace_->sectionsOf(notebook_)) {
            const auto pages = workspace_->pagesOf(section);
            if (!pages.empty()) {
                return pages.front();
            }
        }
    }
    return firstPage(*workspace_);
}

bool PageNavigator::onPatch(const document::Patch& /*patch*/) {
    if (!active_) {
        // Nothing was open (e.g. everything had been deleted): open the first page as
        // soon as there is one again (e.g. after undo).
        active_ = firstPage(*workspace_);
        remember();
        return active_.has_value();
    }
    if (workspace_->findPage(*active_) != nullptr) {
        remember(); // it may have moved
        return false;
    }
    active_ = fallback();
    remember();
    return true;
}

std::optional<core::PageId> PageNavigator::previousPage() const {
    if (!active_) {
        return std::nullopt;
    }
    const auto pages = allPages(*workspace_);
    const auto it = std::find(pages.begin(), pages.end(), *active_);
    if (it == pages.end() || it == pages.begin()) {
        return std::nullopt;
    }
    return *(it - 1);
}

std::optional<core::PageId> PageNavigator::nextPage() const {
    if (!active_) {
        return std::nullopt;
    }
    const auto pages = allPages(*workspace_);
    const auto it = std::find(pages.begin(), pages.end(), *active_);
    if (it == pages.end() || it + 1 == pages.end()) {
        return std::nullopt;
    }
    return *(it + 1);
}

std::optional<core::PageId> pageChangedBy(const document::Patch& patch,
                                          const document::Workspace& workspace) {
    std::optional<core::PageId> found;
    bool several = false;
    const auto note = [&](core::PageId page) {
        if (found && *found != page) {
            several = true;
        }
        found = page;
    };
    for (const document::AnyChange& change : patch.changes()) {
        std::visit(
            [&](const auto& c) {
                using Record = std::decay_t<decltype(c.after ? *c.after : *c.before)>;
                const auto& record = c.after ? *c.after : *c.before;
                if constexpr (std::is_same_v<Record, document::Element>) {
                    if (const document::Layer* layer = workspace.findLayer(record.layer)) {
                        note(layer->page);
                    }
                } else if constexpr (std::is_same_v<Record, document::Layer>) {
                    note(record.page);
                } else if constexpr (std::is_same_v<Record, document::PageInfo>) {
                    if (c.after) { // created (e.g. an undone delete) or reformatted
                        note(record.id);
                    }
                }
            },
            change);
    }
    if (several || !found || workspace.findPage(*found) == nullptr) {
        return std::nullopt;
    }
    return found;
}

} // namespace studyapp::application
