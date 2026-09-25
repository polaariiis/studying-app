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
