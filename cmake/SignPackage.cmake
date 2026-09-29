# CPack pre- and post-build script (Phase 9, docs/BUILDING.md §8): signs what is packaged
# when signing credentials are provided through the environment, and does nothing
# otherwise (unsigned packages). Run by CPack, not by the build.
#
#   Windows  STUDYAPP_SIGN_PFX       path to a code-signing certificate (.pfx)
#            STUDYAPP_SIGN_PASSWORD  its password
#            -> signtool signs studyapp.exe before packing and the installer after.
#   macOS    STUDYAPP_CODESIGN_IDENTITY  a "Developer ID Application: …" identity in the
#            keychain -> codesign (hardened runtime) signs the app bundle before packing.
#            Notarising the DMG is a separate CI step (it needs Apple credentials).
#
# CPACK_TEMPORARY_INSTALL_DIRECTORY is the staged install tree (pre-build);
# CPACK_PACKAGE_FILES lists the packages made (post-build).

if(DEFINED CPACK_PACKAGE_FILES)
    set(_stage post)
else()
    set(_stage pre)
endif()

if(WIN32 AND DEFINED ENV{STUDYAPP_SIGN_PFX} AND NOT "$ENV{STUDYAPP_SIGN_PFX}" STREQUAL "")
    find_program(_signtool signtool REQUIRED)
    if(_stage STREQUAL "pre")
        file(GLOB_RECURSE _files "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*/studyapp.exe")
    else()
        set(_files "")
        foreach(_package IN LISTS CPACK_PACKAGE_FILES)
            if(_package MATCHES "\\.exe$")
                list(APPEND _files "${_package}")
            endif()
        endforeach()
    endif()
    foreach(_file IN LISTS _files)
        message(STATUS "Signing ${_file}")
        execute_process(
            COMMAND "${_signtool}" sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256
                    /f "$ENV{STUDYAPP_SIGN_PFX}" /p "$ENV{STUDYAPP_SIGN_PASSWORD}" "${_file}"
            COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
elseif(APPLE AND _stage STREQUAL "pre" AND DEFINED ENV{STUDYAPP_CODESIGN_IDENTITY}
       AND NOT "$ENV{STUDYAPP_CODESIGN_IDENTITY}" STREQUAL "")
    file(GLOB_RECURSE _bundles LIST_DIRECTORIES true "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*.app")
    foreach(_bundle IN LISTS _bundles)
        if(IS_DIRECTORY "${_bundle}" AND _bundle MATCHES "\\.app$")
            message(STATUS "Signing ${_bundle}")
            execute_process(
                COMMAND codesign --force --deep --options runtime --timestamp
                        --sign "$ENV{STUDYAPP_CODESIGN_IDENTITY}" "${_bundle}"
                COMMAND_ERROR_IS_FATAL ANY)
        endif()
    endforeach()
else()
    message(STATUS "Packages are not signed (no signing credentials in the environment)")
endif()
