# Test executables mirror the module structure. Labels:
#   unit          fast, Qt-free unit tests
#   integration   tests against real libraries (e.g. SQLite)
#   architecture  module-boundary checks
#   gui           Qt Widgets tests (run with QT_QPA_PLATFORM=offscreen)

add_subdirectory(support)
add_subdirectory(core)
add_subdirectory(document)
add_subdirectory(persistence)
add_subdirectory(application)
add_subdirectory(architecture)

if(STUDYAPP_BUILD_APP)
    add_subdirectory(ui)
endif()
