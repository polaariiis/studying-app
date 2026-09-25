# The `studyapp` executable: composition root only. Application logic belongs in modules.
add_executable(studyapp WIN32 MACOSX_BUNDLE
    main.cpp)

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
