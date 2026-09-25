# Modules are added in dependency order. See docs/ARCHITECTURE.md §2 for the allowed edges.

# Qt-free modules (always built)
add_subdirectory(core)
add_subdirectory(document)
add_subdirectory(study)
add_subdirectory(render)
add_subdirectory(canvas)
add_subdirectory(persistence)
add_subdirectory(application)

# Qt-dependent modules
if(STUDYAPP_BUILD_APP)
    add_subdirectory(render_gl)
    add_subdirectory(platform)
    add_subdirectory(ui)
endif()
