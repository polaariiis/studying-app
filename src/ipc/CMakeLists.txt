# Process-level messaging (docs/ARCHITECTURE.md D52): a C++ adapter over IPCFileLab's portable
# C channel, and the messages of the PDF inspection worker (D53). Qt-free.
studyapp_add_module(ipc
    SOURCES
        include/studyapp/ipc/FileChannel.hpp
        include/studyapp/ipc/PdfInspection.hpp
        src/FileChannel.cpp
        src/PdfInspection.cpp
    PUBLIC_DEPS
        studyapp::core
    PRIVATE_DEPS
        IPCFileLab::IPC
)
