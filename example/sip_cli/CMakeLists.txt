add_executable(sip_cli EXCLUDE_FROM_ALL
    src/main.cpp
    src/SignalHandlers.cpp
    src/CallController.cpp
    src/SDLVideoRenderer.cpp
)

target_include_directories(sip_cli 
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set(SDL_VERSION 3.2.12)

set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/example/sip_cli/extern")

set(SDL_SOURCE_URL
    "https://nexus.svetlocal.ru/repository/github-artifacts/SDL3-${SDL_VERSION}.tar.gz"
)

set(SDL_SOURCE_URL_HASH
    "SHA256=9734f308e130c64a2b4df6bca5884c5aca577ee6c7c77ab3379474ea85e51f96")

# Disable all SDL components by default
set(SDL_SHARED ON CACHE INTERNAL "")
set(SDL_STATIC OFF CACHE INTERNAL "")
set(SDL_TEST OFF CACHE INTERNAL "")

# Disable non-video subsystems
set(SDL_ATOMIC OFF CACHE INTERNAL "")
set(SDL_AUDIO OFF CACHE INTERNAL "")
set(SDL_SENSOR OFF CACHE INTERNAL "")
set(SDL_POWER OFF CACHE INTERNAL "")
set(SDL_FILESYSTEM OFF CACHE INTERNAL "")
set(SDL_CPUINFO OFF CACHE INTERNAL "")
set(SDL_VIDEO_RPI OFF CACHE INTERNAL "")
set(SDL_RENDER OFF CACHE INTERNAL "")
set(SDL_TIMERS OFF CACHE INTERNAL "")

# Enable critical dependencies for SDL_video
set(SDL_VIDEO ON CACHE INTERNAL "")
set(SDL_EVENTS ON CACHE INTERNAL "")
set(SDL_THREADS ON CACHE INTERNAL "")  

include(FetchContent)
FetchContent_Declare(
    sdl_ext
    URL ${SDL_SOURCE_URL}
    URL_HASH ${SDL_SOURCE_URL_HASH}
    SOURCE_DIR ${EXTERNAL_DIR}/SDL3-${SDL_VERSION}
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(sdl_ext)

if(WIN32)
    set(SDL_INSTALL_DIR "${CMAKE_BINARY_DIR}")
endif()

target_link_libraries(sip_cli
  PRIVATE 
    sip_core
    SDL3::SDL3
)

# On Windows, copy SDL3.dll to the build directory
if(WIN32)
    add_custom_command(
        TARGET sip_cli POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:sip_cli>
        VERBATIM
    )
endif()