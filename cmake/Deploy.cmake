# Install and deployment rules for the executable.
#
# studyapp_install_app(<target>)
#   Installs the executable (or macOS bundle) and runs Qt's deployment tooling
#   (windeployqt / macdeployqt / Linux runtime-dependency deployment) at install time, so
#   `cmake --install <build-dir> --prefix <dir>` produces a runnable tree.
#
# studyapp_deploy_qt_to_build_tree(<target>)
#   Windows only: after each build, run windeployqt on the executable *in the build tree*
#   so it can be started directly (Explorer, debugger, command line) without putting Qt's
#   bin directory on PATH. Windows only looks for DLLs next to the executable and on
#   PATH; Linux/macOS executables find Qt through RPATH and need nothing. The copied
#   files are build output (build/<preset>/app/), never part of the repository.
#   Controlled by STUDYAPP_DEPLOY_QT_TO_BUILD_TREE (default ON on Windows).
#
# Packaging with CPack (installers, DMG, AppImage) is Phase 9.

include_guard(GLOBAL)

function(studyapp_install_app target)
    install(TARGETS ${target}
        BUNDLE DESTINATION .
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT runtime)

    qt_generate_deploy_app_script(
        TARGET ${target}
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${deploy_script}" COMPONENT runtime)
endfunction()

function(studyapp_deploy_qt_to_build_tree target)
    if(NOT WIN32 OR NOT STUDYAPP_DEPLOY_QT_TO_BUILD_TREE)
        return()
    endif()
    if(NOT TARGET Qt6::windeployqt)
        message(WARNING "Qt6::windeployqt not found; ${target} will need Qt's bin directory on PATH")
        return()
    endif()
    # windeployqt picks debug/release Qt libraries to match the executable and only copies
    # files that are missing or out of date. Translations and the MSVC runtime are not
    # needed on a development machine; the install tree still gets the full deployment.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime --verbose 0
                "$<TARGET_FILE:${target}>"
        COMMENT "Deploying Qt runtime next to ${target} (build tree only)"
        VERBATIM)
endfunction()
