# Sanitizer support (GCC/Clang only).
#
# STUDYAPP_SANITIZERS is a list such as "address;undefined" or "thread". The flags are
# attached to studyapp_project_options, i.e. to first-party targets only.
#
# MSVC is intentionally unsupported here: /fsanitize=address conflicts with the default
# Debug runtime checks (/RTC1) and needs extra runtime deployment. Use a Linux or macOS
# build (the `asan` preset / CI job) for sanitizer runs.

include_guard(GLOBAL)

if(NOT STUDYAPP_SANITIZERS)
    return()
endif()

if(MSVC)
    message(FATAL_ERROR
        "STUDYAPP_SANITIZERS is only supported with GCC/Clang. Use the asan preset on Linux/macOS.")
endif()

foreach(sanitizer IN LISTS STUDYAPP_SANITIZERS)
    if(NOT sanitizer MATCHES "^(address|undefined|leak|thread)$")
        message(FATAL_ERROR "Unknown sanitizer '${sanitizer}'")
    endif()
endforeach()

if("thread" IN_LIST STUDYAPP_SANITIZERS AND "address" IN_LIST STUDYAPP_SANITIZERS)
    message(FATAL_ERROR "The thread and address sanitizers cannot be combined")
endif()

list(JOIN STUDYAPP_SANITIZERS "," _studyapp_sanitizer_list)

target_compile_options(studyapp_project_options INTERFACE
    -fsanitize=${_studyapp_sanitizer_list}
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all)
target_link_options(studyapp_project_options INTERFACE
    -fsanitize=${_studyapp_sanitizer_list})

message(STATUS "Sanitizers enabled: ${_studyapp_sanitizer_list}")
