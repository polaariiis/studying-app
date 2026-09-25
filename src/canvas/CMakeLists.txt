studyapp_add_module(canvas
    SOURCES
        include/studyapp/canvas/Module.hpp
        src/Module.cpp
    PUBLIC_DEPS
        studyapp::document
        studyapp::render
)
