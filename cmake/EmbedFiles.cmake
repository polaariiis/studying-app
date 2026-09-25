# Embeds files (e.g. SQL migrations) into a target at build time, without Qt resources.
#
# studyapp_embed_files(<target>
#     FUNCTION <qualified C++ function name>   e.g. studyapp::persistence::detail::migrationFiles
#     HEADER   <header declaring EmbeddedFile and the function, relative to the include path>
#     FILES    <files...>)
#
# Generates <binary dir>/generated/<function>.cpp defining
#     std::span<const EmbeddedFile> <function>()
# that returns { file name, file content } in the order given. The file is regenerated
# whenever an input changes. Content is emitted as a byte array, so any bytes (and any
# length) are safe; a terminating NUL is appended but not counted.

include_guard(GLOBAL)

set(_studyapp_embed_script "${CMAKE_CURRENT_LIST_DIR}/EmbedFilesScript.cmake")

function(studyapp_embed_files target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "FUNCTION;HEADER" "FILES")
    if(NOT arg_FUNCTION OR NOT arg_HEADER OR NOT arg_FILES)
        message(FATAL_ERROR "studyapp_embed_files(${target}): FUNCTION, HEADER and FILES are required")
    endif()

    string(REPLACE "::" "_" file_stem "${arg_FUNCTION}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/${file_stem}.cpp")

    set(inputs "")
    foreach(file IN LISTS arg_FILES)
        get_filename_component(absolute "${file}" ABSOLUTE)
        list(APPEND inputs "${absolute}")
    endforeach()
    # Lists cannot be passed through -D unchanged; use '|' as the separator.
    string(REPLACE ";" "|" encoded_inputs "${inputs}")

    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT=${output}"
            "-DFUNCTION=${arg_FUNCTION}"
            "-DHEADER=${arg_HEADER}"
            "-DINPUTS=${encoded_inputs}"
            -P "${_studyapp_embed_script}"
        DEPENDS ${inputs} "${_studyapp_embed_script}"
        COMMENT "Embedding files for ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()
