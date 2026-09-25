#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/FractionalIndex.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/core/Vec2.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace studyapp::document {

// Records of the workspace hierarchy (docs/DATA_MODEL.md §3–4):
//
//   Workspace → Notebook → Section → Page → Layer → Element
//
// Records are plain values. They reference their parent by id, never by pointer, and are
// owned exclusively by the Workspace they belong to. Children are ordered by a
// FractionalIndex key (ties broken by id). Records are only changed through Patches.

/// Workspace metadata. Fixed at construction in Phase 2.
struct WorkspaceInfo {
    core::WorkspaceId id;
    std::string name;
    core::Timestamp created{};

    [[nodiscard]] friend bool operator==(const WorkspaceInfo&, const WorkspaceInfo&) = default;
};

struct NotebookInfo {
    core::NotebookId id;
    std::string title; ///< must not be blank
    core::FractionalIndex order;
    core::Timestamp created{};
    core::Timestamp modified{};

    [[nodiscard]] friend bool operator==(const NotebookInfo&, const NotebookInfo&) = default;
};

struct SectionInfo {
    core::SectionId id;
    core::NotebookId notebook; ///< parent
    std::string title;         ///< must not be blank
    core::FractionalIndex order;
    core::Timestamp created{};
    core::Timestamp modified{};

    [[nodiscard]] friend bool operator==(const SectionInfo&, const SectionInfo&) = default;
};

enum class PageExtent : std::uint8_t {
    Infinite, ///< unbounded canvas
    Bounded,  ///< fixed paper size (`PageInfo::size`)
};

enum class BackgroundPattern : std::uint8_t {
    None,
    Ruled,
    Grid,
    Dots
};

struct PageBackground {
    core::Color color = core::Color::white();
    BackgroundPattern pattern = BackgroundPattern::None;
    float spacing = 32.0F; ///< world units between pattern lines/dots

    [[nodiscard]] friend bool operator==(const PageBackground&, const PageBackground&) = default;
};

/// A4 portrait in world units (96 units per inch).
inline constexpr core::DVec2 kA4PortraitSize{794.0, 1123.0};

struct PageInfo {
    core::PageId id;
    core::SectionId section; ///< parent
    std::string title;       ///< may be empty ("Untitled")
    core::FractionalIndex order;
    PageExtent extent = PageExtent::Infinite;
    core::DVec2 size{}; ///< used when `extent == Bounded`; both components > 0
    PageBackground background;
    core::Timestamp created{};
    core::Timestamp modified{};

    [[nodiscard]] friend bool operator==(const PageInfo&, const PageInfo&) = default;
};

/// A named, ordered group of elements on a page. Every page has at least one layer.
struct Layer {
    core::LayerId id;
    core::PageId page; ///< parent
    std::string name;  ///< must not be blank
    core::FractionalIndex order;
    bool visible = true;
    bool locked = false;
    float opacity = 1.0F; ///< 0..1

    [[nodiscard]] friend bool operator==(const Layer&, const Layer&) = default;
};

/// Checks that a user-visible name is not empty or whitespace-only.
/// `what` names the object in the error message (e.g. "notebook title").
[[nodiscard]] core::Result<void> validateName(std::string_view name, std::string_view what);

} // namespace studyapp::document
