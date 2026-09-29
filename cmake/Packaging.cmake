# Release packages with CPack (Phase 9; docs/BUILDING.md §8).
#
#   cmake --build --preset release && cpack --config build/release/CPackConfig.cmake
#
# CPack installs the `runtime` component into a staging tree — which runs Qt's deployment
# tooling (studyapp_install_app, Deploy.cmake) — and packs that:
#
#   Windows  ZIP (portable) and, when NSIS is installed, an NSIS installer (Start menu and
#            desktop shortcuts, uninstaller); the MSVC runtime DLLs are included.
#   macOS    a DMG holding studyapp.app (the Qt frameworks and plugins inside the bundle).
#   Linux    a TGZ of the install tree (bin/, lib/, plugins/, share/); CI turns the same
#            tree into an AppImage with linuxdeploy (.github/workflows/package.yml).
#
# Packages are not signed here; the release workflow signs them when certificates are
# configured as repository secrets.

include_guard(GLOBAL)

if(WIN32)
    # msvcp140.dll, vcruntime140*.dll next to studyapp.exe: runs without the VC++
    # redistributable installed.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF) # part of Windows 10+
    include(InstallRequiredSystemLibraries)
endif()

set(CPACK_PACKAGE_NAME "${STUDYAPP_PRODUCT_NAME}")
set(CPACK_PACKAGE_VENDOR "StudyBoard contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Study notes, handwriting, PDFs and planning in one native desktop app")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/polaariiis/studying-app")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${STUDYAPP_PRODUCT_NAME}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_COMPONENTS_ALL runtime)
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_STRIP_FILES ON)
set(CPACK_VERBATIM_VARIABLES ON)

if(WIN32)
    set(_studyapp_platform "windows")
elseif(APPLE)
    set(_studyapp_platform "macos")
else()
    set(_studyapp_platform "linux")
endif()
set(CPACK_PACKAGE_FILE_NAME
    "${STUDYAPP_PRODUCT_NAME}-${PROJECT_VERSION}-${_studyapp_platform}-${CMAKE_SYSTEM_PROCESSOR}")

if(WIN32)
    set(CPACK_GENERATOR ZIP)
    find_program(STUDYAPP_MAKENSIS makensis
        PATHS "$ENV{ProgramFiles\(x86\)}/NSIS" "$ENV{ProgramFiles}/NSIS")
    if(STUDYAPP_MAKENSIS)
        list(APPEND CPACK_GENERATOR NSIS)
    endif()
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_EXECUTABLES "studyapp" "${STUDYAPP_PRODUCT_NAME}")
    set(CPACK_CREATE_DESKTOP_LINKS "studyapp")
    set(CPACK_NSIS_DISPLAY_NAME "${STUDYAPP_PRODUCT_NAME}")
    set(CPACK_NSIS_PACKAGE_NAME "${STUDYAPP_PRODUCT_NAME} ${PROJECT_VERSION}")
    set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/resources/icons/app/studyboard.ico")
    set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/resources/icons/app/studyboard.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "${CMAKE_INSTALL_BINDIR}\\\\studyapp.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
    # Upgrades replace the previous version in place (same install directory); workspaces
    # live elsewhere (the user's documents) and are never touched by install or uninstall.
elseif(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME "${STUDYAPP_PRODUCT_NAME}")
    set(CPACK_DMG_FORMAT UDZO)
else()
    set(CPACK_GENERATOR TGZ)
    set(CPACK_SET_DESTDIR OFF)
endif()

# Signing hooks: active only when credentials are in the environment (SignPackage.cmake).
set(CPACK_PRE_BUILD_SCRIPTS "${PROJECT_SOURCE_DIR}/cmake/SignPackage.cmake")
set(CPACK_POST_BUILD_SCRIPTS "${PROJECT_SOURCE_DIR}/cmake/SignPackage.cmake")

include(CPack)
