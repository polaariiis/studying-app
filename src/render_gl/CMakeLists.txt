# OpenGL 3.3 core backend. Qt OpenGL is a PRIVATE dependency: no GL or Qt types may
# appear in this module's public headers, and no other module may call OpenGL.
studyapp_add_module(render_gl
    SOURCES
        include/studyapp/render_gl/Module.hpp
        src/Module.cpp
    PUBLIC_DEPS
        studyapp::render
    PRIVATE_DEPS
        Qt6::Gui
        Qt6::OpenGL
)
