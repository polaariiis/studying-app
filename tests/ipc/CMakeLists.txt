# The C++ adapter over IPCFileLab (docs/ARCHITECTURE.md D52): real channel files in temporary
# directories. The C library's own tests cover its protocol and multi-process behaviour.
studyapp_add_gtest(ipc_tests
    SOURCES
        FileChannelTest.cpp
    LIBS
        studyapp::ipc
        studyapp::test_support
    LABELS integration)
