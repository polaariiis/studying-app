configure_file(
    src/BuildInfo.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/studyapp/core/BuildInfo.hpp"
    @ONLY)

studyapp_add_module(core
    SOURCES
        include/studyapp/core/Clock.hpp
        include/studyapp/core/Color.hpp
        include/studyapp/core/Error.hpp
        include/studyapp/core/Id.hpp
        include/studyapp/core/IdGenerator.hpp
        include/studyapp/core/Ids.hpp
        include/studyapp/core/Log.hpp
        include/studyapp/core/Rect.hpp
        include/studyapp/core/Uuid.hpp
        include/studyapp/core/Vec2.hpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/include/studyapp/core/BuildInfo.hpp"
        src/Clock.cpp
        src/Color.cpp
        src/Error.cpp
        src/IdGenerator.cpp
        src/Log.cpp
        src/Uuid.cpp
    PUBLIC_DEPS
        tl::expected
)

target_include_directories(studyapp_core PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
