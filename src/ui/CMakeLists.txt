# Qt Widgets presentation layer. Talks to `application` only; persistence and SQLite are
# not reachable from here (application links them PRIVATE).
studyapp_add_module(ui
    SOURCES
        include/studyapp/ui/AppIcon.hpp
        include/studyapp/ui/CanvasPlaceholder.hpp
        include/studyapp/ui/DesignTokens.hpp
        include/studyapp/ui/MainWindow.hpp
        include/studyapp/ui/ThemeManager.hpp
        src/AppIcon.cpp
        src/CanvasPlaceholder.cpp
        src/DesignTokens.cpp
        src/MainWindow.cpp
        src/ThemeManager.cpp
    PUBLIC_DEPS
        Qt6::Widgets
        studyapp::core
    PRIVATE_DEPS
        studyapp::application
        studyapp::canvas
        studyapp::render_gl
)
studyapp_enable_qt(studyapp_ui)

# Stylesheet template (colours come from DesignTokens at runtime).
qt_add_resources(studyapp_ui "studyapp_themes"
    PREFIX "/themes"
    BASE "${PROJECT_SOURCE_DIR}/resources/themes"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/themes/studyboard.qss")

# Application icon, compiled in as :/icons/app/studyboard-<size>.png.
# Generated from resources/icons/app/studyboard-master.png by tools/generate_app_icons.py.
set(_studyapp_icon_dir "${PROJECT_SOURCE_DIR}/resources/icons/app")
qt_add_resources(studyapp_ui "studyapp_app_icon"
    PREFIX "/icons/app"
    BASE "${_studyapp_icon_dir}"
    FILES
        "${_studyapp_icon_dir}/studyboard-16.png"
        "${_studyapp_icon_dir}/studyboard-32.png"
        "${_studyapp_icon_dir}/studyboard-48.png"
        "${_studyapp_icon_dir}/studyboard-64.png"
        "${_studyapp_icon_dir}/studyboard-128.png"
        "${_studyapp_icon_dir}/studyboard-256.png")
