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

add_subdirectory("extern/SDL3-3.2.12" EXCLUDE_FROM_ALL)

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