# The `studyapp` executable: composition root only. Application logic belongs in modules.
add_executable(studyapp WIN32 MACOSX_BUNDLE
    main.cpp)

# Windows: embed the .ico so the executable itself carries the StudyBoard icon. (The window
# icon comes from Qt resources on every platform; the macOS bundle and Linux desktop icons
# are set up below.)
if(WIN32)
    set(STUDYAPP_WINDOWS_ICON "${PROJECT_SOURCE_DIR}/resources/icons/app/studyboard.ico")
    configure_file(studyapp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/studyapp.rc" @ONLY)
    target_sources(studyapp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/studyapp.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/studyapp.rc" APPEND PROPERTY
        OBJECT_DEPENDS "${STUDYAPP_WINDOWS_ICON}")
endif()

target_link_libraries(studyapp PRIVATE
    studyapp::ui
    studyapp::platform
    studyapp::core
    Qt6::Widgets
    studyapp_project_options
    studyapp_warnings)
target_compile_features(studyapp PRIVATE cxx_std_20)
studyapp_enable_qt(studyapp)

set_target_properties(studyapp PROPERTIES
    OUTPUT_NAME studyapp
    MACOSX_BUNDLE_BUNDLE_NAME "${STUDYAPP_PRODUCT_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.polaariiis.studyboard"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

# macOS bundle icon: an .icns made from the committed PNGs at build time (sips/iconutil are
# part of macOS), so no generated binary is kept in the repository.
if(APPLE)
    set(_icons "${PROJECT_SOURCE_DIR}/resources/icons/app")
    set(_iconset "${CMAKE_CURRENT_BINARY_DIR}/studyboard.iconset")
    set(_icns "${CMAKE_CURRENT_BINARY_DIR}/studyboard.icns")
    add_custom_command(OUTPUT "${_icns}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-16.png" "${_iconset}/icon_16x16.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-32.png" "${_iconset}/icon_16x16@2x.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-32.png" "${_iconset}/icon_32x32.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-64.png" "${_iconset}/icon_32x32@2x.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-128.png" "${_iconset}/icon_128x128.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-256.png" "${_iconset}/icon_128x128@2x.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${_icons}/studyboard-256.png" "${_iconset}/icon_256x256.png"
        COMMAND iconutil -c icns "${_iconset}" -o "${_icns}"
        DEPENDS "${_icons}/studyboard-16.png" "${_icons}/studyboard-32.png"
                "${_icons}/studyboard-64.png" "${_icons}/studyboard-128.png"
                "${_icons}/studyboard-256.png"
        COMMENT "Building the macOS app icon"
        VERBATIM)
    target_sources(studyapp PRIVATE "${_icns}")
    set_source_files_properties("${_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(studyapp PROPERTIES MACOSX_BUNDLE_ICON_FILE studyboard.icns)
    # The deployed Qt frameworks are inside the bundle (macdeployqt, Deploy.cmake).
    set_target_properties(studyapp PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
endif()

studyapp_install_app(studyapp)
studyapp_deploy_qt_to_build_tree(studyapp)

# Linux desktop integration (menus, AppImage): a .desktop entry and the icon.
if(UNIX AND NOT APPLE)
    # The installed executable finds the deployed Qt in ../lib.
    set_target_properties(studyapp PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    # GCC 13's output needs a newer libstdc++ than Ubuntu 22.04 ships: packages carry the
    # compiler's runtime (docs/ARCHITECTURE.md §13) in a directory outside RPATH. The
    # launcher bin/studyboard (the AppImage's AppRun) puts it on the library path only when
    # the system's libstdc++ lacks a version the application needs (docs/BUILDING.md §8).
    # The launcher finds it relative to its own directory.
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        message(FATAL_ERROR "CMAKE_INSTALL_LIBDIR must be relative for the packages "
                            "(it is ${CMAKE_INSTALL_LIBDIR})")
    endif()
    set(STUDYAPP_CXXRT_DIR "${CMAKE_INSTALL_LIBDIR}/studyboard-cxxrt")
    configure_file("${PROJECT_SOURCE_DIR}/resources/linux/studyboard.in"
        "${CMAKE_CURRENT_BINARY_DIR}/studyboard" @ONLY)
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/studyboard"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT runtime)
    if(STUDYAPP_BUILD_TESTS)
        # Its runtime choice, arguments, exit status and layouts, with fake runtimes.
        add_test(NAME packaging.linux_launcher
            COMMAND sh "${PROJECT_SOURCE_DIR}/tools/test_linux_launcher.sh"
                "${CMAKE_CURRENT_BINARY_DIR}/studyboard" "${STUDYAPP_CXXRT_DIR}")
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        foreach(runtime libstdc++.so.6 libgcc_s.so.1)
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${runtime}
                OUTPUT_VARIABLE runtime_path
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(IS_ABSOLUTE "${runtime_path}" AND EXISTS "${runtime_path}")
                # The soname is usually a symlink: install the file it points to, named
                # as the loader looks for it.
                get_filename_component(runtime_file "${runtime_path}" REALPATH)
                install(FILES "${runtime_file}"
                    DESTINATION "${STUDYAPP_CXXRT_DIR}"
                    RENAME "${runtime}"
                    COMPONENT runtime)
            endif()
        endforeach()
    endif()
    install(FILES "${PROJECT_SOURCE_DIR}/resources/linux/studyboard.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
        COMPONENT runtime)
    foreach(size 16 32 48 64 128 256)
        install(FILES "${PROJECT_SOURCE_DIR}/resources/icons/app/studyboard-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME studyboard.png
            COMPONENT runtime)
    endforeach()
endif()
