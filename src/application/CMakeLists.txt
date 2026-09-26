# persistence is linked PRIVATE: code above this layer (ui, platform, app) cannot include
# persistence or SQLite headers.
studyapp_add_module(application
    SOURCES
        include/studyapp/application/ComponentVersions.hpp
        include/studyapp/application/PageNavigator.hpp
        include/studyapp/application/WorkspaceLock.hpp
        include/studyapp/application/WorkspaceSession.hpp
        include/studyapp/application/StartPage.hpp
        include/studyapp/application/WorkspaceStructure.hpp
        src/ComponentVersions.cpp
        src/PageNavigator.cpp
        src/StartPage.cpp
        src/WorkspaceSession.cpp
        src/WorkspaceStructure.cpp
    PUBLIC_DEPS
        studyapp::document
        studyapp::study
    PRIVATE_DEPS
        studyapp::persistence
)
