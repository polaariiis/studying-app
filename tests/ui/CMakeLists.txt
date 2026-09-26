# Qt Widgets tests use Qt Test (they need a QApplication) and run on the offscreen
# platform so they work on headless CI machines.
add_executable(ui_tests MainWindowTest.cpp)
target_link_libraries(ui_tests PRIVATE
    studyapp::ui
    Qt6::Test
    studyapp_project_options
    studyapp_warnings)
target_compile_features(ui_tests PRIVATE cxx_std_20)
studyapp_enable_qt(ui_tests)
set_target_properties(ui_tests PROPERTIES FOLDER "tests")

add_test(NAME ui.MainWindowTest COMMAND ui_tests)
set_tests_properties(ui.MainWindowTest PROPERTIES
    LABELS gui
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(WIN32)
    # Let the test find the Qt DLLs without deploying them next to the executable.
    set_property(TEST ui.MainWindowTest APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# End-to-end canvas test (real widget, OpenGL renderer and workspace session). It skips
# itself without an OpenGL 3.3 context, which is the case on the offscreen platform used
# here and in CI; run build/<preset>/tests/ui/canvas_widget_tests directly on a desktop.
add_executable(canvas_widget_tests CanvasWidgetTest.cpp)
# Includes the private CanvasWidget header for its paint counter.
target_include_directories(canvas_widget_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/ui/src")
target_link_libraries(canvas_widget_tests PRIVATE
    studyapp::ui
    studyapp::application
    studyapp::platform
    studyapp::test_support
    Qt6::Test
    Qt6::OpenGLWidgets
    studyapp_project_options
    studyapp_warnings)
target_compile_features(canvas_widget_tests PRIVATE cxx_std_20)
studyapp_enable_qt(canvas_widget_tests)
set_target_properties(canvas_widget_tests PROPERTIES FOLDER "tests")
add_test(NAME ui.CanvasWidgetTest COMMAND canvas_widget_tests)
set_tests_properties(ui.CanvasWidgetTest PROPERTIES
    LABELS "gui;gpu"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_property(TEST ui.CanvasWidgetTest APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# Phase 5 shell: navigation tree model and MainWindow workflows with real sessions and
# scripted dialogs. Needs no OpenGL (the canvas widget simply stays blank offscreen). It
# includes the private WorkspaceTreeModel header to test the model on its own.
add_executable(shell_tests ShellTest.cpp)
target_include_directories(shell_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/ui/src")
target_link_libraries(shell_tests PRIVATE
    studyapp::ui
    studyapp::application
    studyapp::platform
    studyapp::test_support
    Qt6::Test
    studyapp_project_options
    studyapp_warnings)
target_compile_features(shell_tests PRIVATE cxx_std_20)
studyapp_enable_qt(shell_tests)
set_target_properties(shell_tests PROPERTIES FOLDER "tests")
add_test(NAME ui.ShellTest COMMAND shell_tests)
set_tests_properties(ui.ShellTest PROPERTIES
    LABELS gui
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_property(TEST ui.ShellTest APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
