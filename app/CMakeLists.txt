# The `studyapp` executable: composition root only. Application logic belongs in modules.
add_executable(studyapp WIN32 MACOSX_BUNDLE
    main.cpp)

# Windows only: embed the .ico so the executable itself carries the StudyBoard icon.
# (Linux/macOS get the icon at runtime from Qt resources; bundle/.desktop icons are
# part of packaging, Phase 9.)
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

studyapp_install_app(studyapp)
studyapp_deploy_qt_to_build_tree(studyapp)
