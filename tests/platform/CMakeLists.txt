# Qt adapters of the platform module. Qt Test (not GoogleTest) because the tests need Qt's
# event-loop-free QCoreApplication and link Qt anyway; no display is required.
add_executable(platform_tests QtWorkspaceLockerTest.cpp)
target_link_libraries(platform_tests PRIVATE
    studyapp::platform
    Qt6::Core
    Qt6::Test
    studyapp_project_options
    studyapp_warnings)
target_compile_features(platform_tests PRIVATE cxx_std_20)
studyapp_enable_qt(platform_tests)
set_target_properties(platform_tests PROPERTIES FOLDER "tests")

add_test(NAME platform.QtWorkspaceLockerTest COMMAND platform_tests)
set_tests_properties(platform.QtWorkspaceLockerTest PROPERTIES LABELS integration)

if(WIN32)
    # Let the test find the Qt DLLs without deploying them next to the executable.
    set_property(TEST platform.QtWorkspaceLockerTest APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
