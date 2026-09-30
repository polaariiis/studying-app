# Process-level messaging (docs/ARCHITECTURE.md D52): a C++ adapter over IPCFileLab's portable
# C channel. Qt-free; no other module depends on it yet.
studyapp_add_module(ipc
    SOURCES
        include/studyapp/ipc/FileChannel.hpp
        src/FileChannel.cpp
    PUBLIC_DEPS
        studyapp::core
    PRIVATE_DEPS
        IPCFileLab::IPC
)
