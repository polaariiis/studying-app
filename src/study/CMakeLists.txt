# Study & planning domain (docs/ARCHITECTURE.md §3): record values and pure planning logic.
studyapp_add_module(study
    SOURCES
        include/studyapp/study/Planning.hpp
        include/studyapp/study/Records.hpp
        src/Planning.cpp
        src/Records.cpp
    PUBLIC_DEPS
        studyapp::core
)
