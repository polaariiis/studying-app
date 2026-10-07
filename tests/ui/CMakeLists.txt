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
    studyapp::render_gl
    studyapp::test_support
    Qt6::Test
    Qt6::OpenGL
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
# persistence is linked directly only for fault injection (a second connection).
target_link_libraries(shell_tests PRIVATE
    studyapp::ui
    studyapp::application
    studyapp::canvas
    studyapp::persistence
    studyapp::platform
    studyapp::test_support
    Qt6::Pdf
    Qt6::PrintSupport
    Qt6::Svg
    Qt6::Test
    studyapp::ipc
    studyapp_project_options
    studyapp_warnings)
target_compile_features(shell_tests PRIVATE cxx_std_20)
studyapp_enable_qt(shell_tests)
set_target_properties(shell_tests PROPERTIES FOLDER "tests")
# PDF imports inspect in a worker process (D53): the shell tests start the helper's real
# worker code (their own executable is not StudyBoard).
target_compile_definitions(shell_tests PRIVATE
    STUDYAPP_PDF_WORKER_HELPER="$<TARGET_FILE:pdf_worker_helper>")
add_dependencies(shell_tests pdf_worker_helper)
add_test(NAME ui.ShellTest COMMAND shell_tests)
set_tests_properties(ui.ShellTest PROPERTIES
    LABELS gui
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_property(TEST ui.ShellTest APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# PDF inspection worker (docs/PDF_WORKER.md §19, D53): a helper that plays the worker (the
# real worker code, or one specific misbehaviour) and the client tests that start it and the
# built `studyapp --pdf-worker`.
add_executable(pdf_worker_helper PdfWorkerHelper.cpp)
target_link_libraries(pdf_worker_helper PRIVATE
    studyapp::ui
    studyapp::ipc
    Qt6::Core
    studyapp_project_options
    studyapp_warnings)
target_compile_features(pdf_worker_helper PRIVATE cxx_std_20)
studyapp_enable_qt(pdf_worker_helper)
set_target_properties(pdf_worker_helper PROPERTIES FOLDER "tests")

add_executable(pdf_worker_tests PdfWorkerTest.cpp)
target_include_directories(pdf_worker_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/ui/src")
target_link_libraries(pdf_worker_tests PRIVATE
    studyapp::ui
    studyapp::application
    studyapp::canvas
    studyapp::ipc
    studyapp::test_support
    Qt6::Pdf
    Qt6::Test
    studyapp_project_options
    studyapp_warnings)
target_compile_features(pdf_worker_tests PRIVATE cxx_std_20)
studyapp_enable_qt(pdf_worker_tests)
set_target_properties(pdf_worker_tests PROPERTIES FOLDER "tests")
target_compile_definitions(pdf_worker_tests PRIVATE
    STUDYAPP_PDF_WORKER_HELPER="$<TARGET_FILE:pdf_worker_helper>"
    STUDYAPP_EXECUTABLE="$<TARGET_FILE:studyapp>")
add_dependencies(pdf_worker_tests pdf_worker_helper studyapp)
add_test(NAME ui.PdfWorkerTest COMMAND pdf_worker_tests)
set_tests_properties(ui.PdfWorkerTest PROPERTIES
    LABELS "gui;integration"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_property(TEST ui.PdfWorkerTest APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# Qt 6.8's QTRY_* macros narrow std::chrono::milliseconds::count() to int inside the macro,
# which GCC's -Wconversion reports in these files (Clang and MSVC do not). As a source
# property the option comes after the project warnings on the command line, so it applies.
set_source_files_properties(MainWindowTest.cpp CanvasWidgetTest.cpp ShellTest.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-conversion>")
