# ##############################################################################
# Source groups - coreaudio_osx
# ##############################################################################

if(IOS)
  list(APPEND Source_Files__media__audio__coreaudio
       "${CMAKE_CURRENT_SOURCE_DIR}/ios/corelayer.h"
       "${CMAKE_CURRENT_SOURCE_DIR}/ios/corelayer.mm")
else()
  list(APPEND Source_Files__media__audio__coreaudio
       "${CMAKE_CURRENT_SOURCE_DIR}/osx/audiodevice.cpp"
       "${CMAKE_CURRENT_SOURCE_DIR}/osx/corelayer.mm"
       "${CMAKE_CURRENT_SOURCE_DIR}/osx/device_signature.cpp")
endif()

set(Source_Files__media__audio__coreaudio
    ${Source_Files__media__audio__coreaudio}
    PARENT_SCOPE)
