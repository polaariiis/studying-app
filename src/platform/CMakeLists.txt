# Qt-backed implementations of ports defined by Qt-free modules, plus OS-specific code
# (src/os/<platform>/, added when first needed). Qt is PRIVATE: public headers are Qt-free.
studyapp_add_module(platform
    SOURCES
        include/studyapp/platform/QtLogSink.hpp
        src/QtLogSink.cpp
    PUBLIC_DEPS
        studyapp::application
        studyapp::canvas
    PRIVATE_DEPS
        Qt6::Core
)
studyapp_enable_qt(studyapp_platform)
