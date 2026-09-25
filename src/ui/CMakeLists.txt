# Qt Widgets presentation layer. Talks to `application` only; persistence and SQLite are
# not reachable from here (application links them PRIVATE).
studyapp_add_module(ui
    SOURCES
        include/studyapp/ui/MainWindow.hpp
        include/studyapp/ui/ThemeManager.hpp
        src/MainWindow.cpp
        src/ThemeManager.cpp
    PUBLIC_DEPS
        Qt6::Widgets
    PRIVATE_DEPS
        studyapp::application
        studyapp::canvas
        studyapp::render_gl
)
studyapp_enable_qt(studyapp_ui)

qt_add_resources(studyapp_ui "studyapp_themes"
    PREFIX "/themes"
    BASE "${PROJECT_SOURCE_DIR}/resources/themes"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/themes/light.qss"
        "${PROJECT_SOURCE_DIR}/resources/themes/dark.qss")
