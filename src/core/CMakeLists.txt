configure_file(
    src/BuildInfo.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/studyapp/core/BuildInfo.hpp"
    @ONLY)

studyapp_add_module(core
    SOURCES
        include/studyapp/core/Affine2.hpp
        include/studyapp/core/Clock.hpp
        include/studyapp/core/Color.hpp
        include/studyapp/core/Error.hpp
        include/studyapp/core/FractionalIndex.hpp
        include/studyapp/core/FrameTimings.hpp
        include/studyapp/core/Geometry.hpp
        include/studyapp/core/Id.hpp
        include/studyapp/core/IdGenerator.hpp
        include/studyapp/core/Ids.hpp
        include/studyapp/core/Log.hpp
        include/studyapp/core/Profiler.hpp
        include/studyapp/core/Rect.hpp
        include/studyapp/core/Uuid.hpp
        include/studyapp/core/Vec2.hpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/include/studyapp/core/BuildInfo.hpp"
        src/Clock.cpp
        src/Color.cpp
        src/Error.cpp
        src/FractionalIndex.cpp
        src/FrameTimings.cpp
        src/IdGenerator.cpp
        src/Log.cpp
        src/Profiler.cpp
        src/Uuid.cpp
    PUBLIC_DEPS
        tl::expected
)

target_include_directories(studyapp_core PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

# Profiling scopes (STUDYAPP_PROFILE_SCOPE) are compiled in unless disabled.
target_compile_definitions(studyapp_core PUBLIC
    STUDYAPP_ENABLE_PROFILING=$<BOOL:${STUDYAPP_ENABLE_PROFILING}>)
