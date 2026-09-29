find_package(Threads REQUIRED)

# Interactive canvas engine (docs/CANVAS.md): camera, input events, tools, scene, hit
# testing, selection, render cache. Plain C++: no Qt, no OpenGL, no SQLite.
studyapp_add_module(canvas
    SOURCES
        include/studyapp/canvas/Camera.hpp
        include/studyapp/canvas/CanvasController.hpp
        include/studyapp/canvas/CanvasScene.hpp
        include/studyapp/canvas/DocumentPort.hpp
        include/studyapp/canvas/DocumentRasterizer.hpp
        include/studyapp/canvas/ElementGeometry.hpp
        include/studyapp/canvas/ImageSource.hpp
        include/studyapp/canvas/Input.hpp
        include/studyapp/canvas/RenderBatches.hpp
        include/studyapp/canvas/RenderCache.hpp
        include/studyapp/canvas/Selection.hpp
        include/studyapp/canvas/SelectionHandles.hpp
        include/studyapp/canvas/ShapeBuilder.hpp
        include/studyapp/canvas/SpatialGrid.hpp
        include/studyapp/canvas/StrokeBuilder.hpp
        include/studyapp/canvas/StrokeEraser.hpp
        include/studyapp/canvas/TextLayout.hpp
        include/studyapp/canvas/ToolSettings.hpp
        src/Camera.cpp
        src/CanvasController.cpp
        src/CanvasScene.cpp
        src/DocumentRasterizer.cpp
        src/ElementGeometry.cpp
        src/ParallelFor.hpp
        src/RenderBatches.cpp
        src/RenderCache.cpp
        src/SelectionHandles.cpp
        src/ShapeBuilder.cpp
        src/SpatialGrid.cpp
        src/StrokeBuilder.cpp
        src/StrokeEraser.cpp
        src/TextLayout.cpp
        src/ToolSettings.cpp
        src/Tools.cpp
        src/Tools.hpp
    PUBLIC_DEPS
        studyapp::document
        studyapp::render
    PRIVATE_DEPS
        Threads::Threads # data-parallel tessellation (ParallelFor.hpp, D47)
)
