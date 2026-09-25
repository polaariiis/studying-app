# Headless canvas engine tests: synthetic input, an Editor-backed DocumentPort and a
# recording renderer. No Qt, GPU, SQLite or wall clock.
studyapp_add_gtest(canvas_tests
    SOURCES
        CameraTest.cpp
        CanvasControllerTest.cpp
        CanvasTestSupport.hpp
        SceneTest.cpp
        StrokeBuilderTest.cpp
    LIBS
        studyapp::canvas
        studyapp::document_test_support
        studyapp::render_test_support
    LABELS unit)
