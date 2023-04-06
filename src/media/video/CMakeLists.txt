# ##############################################################################
# Source groups - video
# ##############################################################################

# androidvideo|iosvideo|osxvideo|uwpvideo|v4l2|winvideo

list(
  APPEND
  Source_Files__media__video
  "${CMAKE_CURRENT_SOURCE_DIR}/accel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/accel.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/filter_transpose.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/filter_transpose.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/shm_header.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/sinkclient.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sinkclient.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_base.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_base.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_device.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_device_monitor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_device_monitor.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_input.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_input.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_mixer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_mixer.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_receive_thread.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_receive_thread.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_rtp_session.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_rtp_session.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_scaler.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_scaler.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_sender.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/video_sender.h")

if(APPLE)
  if(IOS)
    list(APPEND Source_Files__media__video__apple
         "${CMAKE_CURRENT_SOURCE_DIR}/iosvideo/video_device_impl.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/iosvideo/video_device_monitor_impl.cpp")
  else()
    list(APPEND Source_Files__media__video__apple
         "${CMAKE_CURRENT_SOURCE_DIR}/osxvideo/video_device_impl.mm"
         "${CMAKE_CURRENT_SOURCE_DIR}/osxvideo/video_device_monitor_impl.mm")
  endif()
  set (Source_Files__media__video__apple ${Source_Files__media__video__apple} PARENT_SCOPE)
endif()

set(Source_Files__media__video
    ${Source_Files__media__video}
    PARENT_SCOPE)

if(MSVC)
  if(WINDOWS_STORE)
    add_subdirectory(uwpvideo)
    set(Source_Files__media__video__uwpvideo
        ${Source_Files__media__video__uwpvideo}
        PARENT_SCOPE)
  endif()

  add_subdirectory(winvideo)
  set(Source_Files__media__video__winvideo
      ${Source_Files__media__video__winvideo}
      PARENT_SCOPE)
endif()

if(APPLE)
  set(Source_Files__media__video__apple
      ${Source_Files__media__video__apple}
      PARENT_SCOPE)
endif()
