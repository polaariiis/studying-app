# The C++ adapter over IPCFileLab (docs/ARCHITECTURE.md D52): real channel files in temporary
# directories. The C library's own tests cover its protocol and multi-process behaviour.
# The PDF worker's messages (D53) are pure functions, tested here too.
studyapp_add_gtest(ipc_tests
    SOURCES
        FileChannelTest.cpp
        PdfInspectionTest.cpp
    LIBS
        studyapp::ipc
        studyapp::test_support
    LABELS integration)
