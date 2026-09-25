# Header-only helpers shared by test suites.
#   test_support           deterministic clock and id generator, Result macros, temp dirs
#   document_test_support  + the document-model fixture (TestWorkspace)
#   render_test_support    + RecordingRenderer (an in-memory render::Renderer)
add_library(studyapp_test_support INTERFACE)
add_library(studyapp::test_support ALIAS studyapp_test_support)
target_include_directories(studyapp_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(studyapp_test_support INTERFACE studyapp::core)

add_library(studyapp_document_test_support INTERFACE)
add_library(studyapp::document_test_support ALIAS studyapp_document_test_support)
target_link_libraries(studyapp_document_test_support INTERFACE
    studyapp::test_support
    studyapp::document)

add_library(studyapp_render_test_support INTERFACE)
add_library(studyapp::render_test_support ALIAS studyapp_render_test_support)
target_link_libraries(studyapp_render_test_support INTERFACE
    studyapp::test_support
    studyapp::render)
