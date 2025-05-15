add_executable(sip_cli EXCLUDE_FROM_ALL
    src/main.cpp
    src/SignalHandlers.cpp
)

target_include_directories(sip_cli 
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(sip_cli 
  PRIVATE 
    sip_core
    SDL3::SDL3
)