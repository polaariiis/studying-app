# Install and deployment rules for the executable.
#
# studyapp_install_app(<target>)
#   Installs the executable (or macOS bundle) and runs Qt's deployment tooling
#   (windeployqt / macdeployqt / Linux runtime-dependency deployment) at install time, so
#   `cmake --install <build-dir> --prefix <dir>` produces a runnable tree.
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
