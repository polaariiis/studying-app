#pragma once

// The state of the workspace a MainWindow shows (private to the window's sources:
// MainWindow.cpp and MainWindowConsole.cpp).

#include "SessionDocumentPort.hpp"
#include "SessionDocumentRasterizer.hpp"
#include "SessionImageSource.hpp"

#include <studyapp/application/PageNavigator.hpp>
#include <studyapp/application/PdfTextSearch.hpp>
#include <studyapp/application/Planner.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/ui/MainWindow.hpp>

#include <atomic>
#include <memory>
#include <unordered_map>

namespace studyapp::ui {

/// Everything that belongs to one open workspace; recreated when another one is opened.
struct MainWindow::OpenWorkspace {
    struct View {
        core::DVec2 center;
        double zoom = 1.0;
    };

    OpenWorkspace(application::WorkspaceSession& shown,
                  std::unique_ptr<application::WorkspaceSession> ownedSession,
                  const core::Clock& shellClock, core::IdGenerator& shellIds)
        : owned(std::move(ownedSession)), session(&shown), clock(&shellClock), ids(&shellIds),
          port(std::make_unique<SessionDocumentPort>(shown)),
          images(std::make_unique<SessionImageSource>(shown)),
          documents(std::make_unique<SessionDocumentRasterizer>(shown)),
          controller(std::make_unique<canvas::CanvasController>(*port, shellIds)),
          navigator(shown.workspace()), structure(shown, shellClock, shellIds),
          planner(shown, shellClock, shellIds) {}

    std::unique_ptr<application::WorkspaceSession> owned; ///< null when borrowed
    application::WorkspaceSession* session;
    const core::Clock* clock;
    core::IdGenerator* ids;
    std::unique_ptr<SessionDocumentPort> port;
    std::unique_ptr<SessionImageSource> images;
    std::unique_ptr<SessionDocumentRasterizer> documents; ///< PDF pages (Phase 8)
    std::unique_ptr<canvas::CanvasController> controller;
    application::PageNavigator navigator;
    application::WorkspaceStructure structure;
    application::Planner planner;
    /// Where the user left each page in this session (not persisted).
    std::unordered_map<core::PageId, View> views;
    /// Expires when this workspace is closed: background results for it are dropped.
    std::shared_ptr<void> alive = std::make_shared<int>(0);
    /// Set when this workspace is released: running PDF worker jobs end at once (D53 §10).
    std::shared_ptr<std::atomic<bool>> importStop = std::make_shared<std::atomic<bool>>(false);
    /// Text extracted from this workspace's PDFs for `pdf search`, bounded (1.2-CMD-01).
    std::shared_ptr<application::PdfTextCache> pdfText =
        std::make_shared<application::PdfTextCache>();
    /// The running PDF search's stop flag: set by `cancel`, a new search, or release.
    std::shared_ptr<std::atomic<bool>> pdfSearchStop;
};

} // namespace studyapp::ui
