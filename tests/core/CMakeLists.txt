studyapp_add_gtest(core_tests
    SOURCES
        ColorTest.cpp
        ErrorTest.cpp
        GeometryTest.cpp
        FractionalIndexTest.cpp
        IdGeneratorTest.cpp
        IdTest.cpp
        LogTest.cpp
        RectTest.cpp
        UuidTest.cpp
        Vec2Test.cpp
    LIBS
        studyapp::core
    LABELS unit)
