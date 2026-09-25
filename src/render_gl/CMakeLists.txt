# OpenGL 3.3 core backend (docs/RENDERING.md §6). Qt OpenGL is a PRIVATE dependency: no GL
# or Qt types may appear in this module's public headers, and no other module may call
# OpenGL (tools/check_boundaries.py).
studyapp_add_module(render_gl
    SOURCES
        include/studyapp/render_gl/OpenGLRenderer.hpp
        src/OpenGLRenderer.cpp
    PUBLIC_DEPS
        studyapp::render
    PRIVATE_DEPS
        Qt6::Gui
        Qt6::OpenGL
)
studyapp_enable_qt(studyapp_render_gl)

# GLSL 330 core sources, compiled in as :/shaders/<name>.
set(_studyapp_shader_dir "${PROJECT_SOURCE_DIR}/resources/shaders")
qt_add_resources(studyapp_render_gl "studyapp_shaders"
    PREFIX "/shaders"
    BASE "${_studyapp_shader_dir}"
    FILES
        "${_studyapp_shader_dir}/pattern.frag"
        "${_studyapp_shader_dir}/pattern.vert"
        "${_studyapp_shader_dir}/solid.frag"
        "${_studyapp_shader_dir}/solid.vert")
