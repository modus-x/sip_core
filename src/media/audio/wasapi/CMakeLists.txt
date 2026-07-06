################################################################################
# Source groups - wasapi (native Windows audio backend)
################################################################################
list (APPEND Source_Files__media__audio__wasapi
      "${CMAKE_CURRENT_SOURCE_DIR}/wasapilayer.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/wasapilayer.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/wasapi_convert.h"
)

set (Source_Files__media__audio__wasapi ${Source_Files__media__audio__wasapi} PARENT_SCOPE)
