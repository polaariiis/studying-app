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
