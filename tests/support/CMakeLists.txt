# Header-only helpers shared by test suites: deterministic clock and id generator.
add_library(studyapp_test_support INTERFACE)
add_library(studyapp::test_support ALIAS studyapp_test_support)
target_include_directories(studyapp_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(studyapp_test_support INTERFACE studyapp::core)
