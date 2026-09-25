# Backend-neutral render API and CPU tessellation (docs/RENDERING.md). No GL, no Qt.
studyapp_add_module(render
    SOURCES
        include/studyapp/render/Renderer.hpp
        include/studyapp/render/Tessellation.hpp
        src/Tessellation.cpp
    PUBLIC_DEPS
        studyapp::core
)
