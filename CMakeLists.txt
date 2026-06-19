cmake_minimum_required(VERSION 3.16)
project(DiscordSentinel VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/main.cpp
    src/utils.cpp
    src/logger.cpp
    src/process_utils.cpp
    src/monitoring.cpp
    src/token_clear.cpp
)

add_executable(DiscordSentinel ${SOURCES})

# Windows-specific settings
if(WIN32)
    target_compile_definitions(DiscordSentinel PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    target_link_libraries(DiscordSentinel PRIVATE
        shlwapi
        wintrust
        advapi32
    )

    # Require admin for SeDebugPrivilege (optional manifest)
    if(MSVC)
        target_compile_options(DiscordSentinel PRIVATE /W4 /utf-8)
    endif()
endif()

# Output ke folder bin
set_target_properties(DiscordSentinel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)