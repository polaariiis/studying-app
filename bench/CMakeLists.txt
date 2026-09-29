# Google Benchmark micro-benchmarks (docs/TESTING.md §2): not run by CTest. Build with
# -DSTUDYAPP_BUILD_BENCHMARKS=ON (ideally a Release build) and run studyapp_benchmarks.
add_executable(studyapp_benchmarks
    CanvasBench.cpp
    PageLoadBench.cpp
    SyntheticPage.hpp)
target_link_libraries(studyapp_benchmarks PRIVATE
    studyapp::canvas
    studyapp::persistence
    studyapp::render_test_support
    benchmark::benchmark_main
    studyapp_project_options
    studyapp_warnings)
target_compile_features(studyapp_benchmarks PRIVATE cxx_std_20)
set_target_properties(studyapp_benchmarks PROPERTIES FOLDER "bench")

# Application-level benchmarks (Phase 9): session, search, planner, assets, text, PDF,
# export and bundles through the real Qt adapters (offscreen platform). Needs Qt.
if(STUDYAPP_BUILD_APP)
    add_executable(studyapp_app_benchmarks AppBench.cpp SyntheticPage.hpp)
    # PageExport and SessionDocumentRasterizer are private to ui.
    target_include_directories(studyapp_app_benchmarks PRIVATE "${PROJECT_SOURCE_DIR}/src/ui/src")
    target_link_libraries(studyapp_app_benchmarks PRIVATE
        studyapp::ui
        studyapp::application
        studyapp::platform
        studyapp::test_support
        Qt6::Gui
        Qt6::Pdf
        benchmark::benchmark
        studyapp_project_options
        studyapp_warnings)
    target_compile_features(studyapp_app_benchmarks PRIVATE cxx_std_20)
    studyapp_enable_qt(studyapp_app_benchmarks)
    set_target_properties(studyapp_app_benchmarks PROPERTIES FOLDER "bench")
endif()
