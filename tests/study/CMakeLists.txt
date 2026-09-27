# Study domain: record values and pure planning logic. No Qt, SQLite or wall clock.
studyapp_add_gtest(study_tests
    SOURCES
        PlanningTest.cpp
    LIBS
        studyapp::study
    LABELS unit)
