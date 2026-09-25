# CPU tessellation (no GL).
studyapp_add_gtest(render_tests
    SOURCES
        TessellationTest.cpp
    LIBS
        studyapp::render
    LABELS unit)
