# Document model: workspace hierarchy, elements, patches, commands, undo/redo.
# Depends on core only; must stay free of Qt, SQLite, OpenGL and I/O.
studyapp_add_module(document
    SOURCES
        include/studyapp/document/Commands.hpp
        include/studyapp/document/Editor.hpp
        include/studyapp/document/Element.hpp
        include/studyapp/document/Patch.hpp
        include/studyapp/document/Records.hpp
        include/studyapp/document/UndoStack.hpp
        include/studyapp/document/Workspace.hpp
        src/Commands.cpp
        src/Editor.cpp
        src/Element.cpp
        src/Patch.cpp
        src/Records.cpp
        src/UndoStack.cpp
        src/Workspace.cpp
    PUBLIC_DEPS
        studyapp::core
)
