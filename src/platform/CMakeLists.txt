# Qt-backed implementations of ports defined by Qt-free modules, plus OS-specific code
# (src/os/<platform>/). Qt is PRIVATE: public headers are Qt-free.
if(WIN32)
    set(_studyapp_platform_os_sources src/os/windows/ProcessInfo.cpp)
else()
    set(_studyapp_platform_os_sources src/os/unix/ProcessInfo.cpp)
endif()

studyapp_add_module(platform
    SOURCES
        include/studyapp/platform/QtLogSink.hpp
        include/studyapp/platform/QtWorkspaceLocker.hpp
        src/QtLogSink.cpp
        src/QtWorkspaceLocker.cpp
        src/os/ProcessInfo.hpp
        ${_studyapp_platform_os_sources}
    PUBLIC_DEPS
        studyapp::application
        studyapp::canvas
    PRIVATE_DEPS
        Qt6::Core
)
studyapp_enable_qt(studyapp_platform)
