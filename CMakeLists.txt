cmake_minimum_required(VERSION 3.16)

project(
  sip_core
  VERSION 0.0.1
  LANGUAGES C CXX)
set(PACKAGE_NAME "sip_core library")

# ##############################################################################
# Build options
# ##############################################################################
option(SIP_CORE_VIDEO "Build with video support" ON)
option(BUILD_CONTRIB "Build contrib to CONTRIB_PATH" ON)
option(BUILD_EXTRA_TOOLS "Build extra tools" OFF)

set(original_ldflags "${LDFLAGS}")
set(original_cflags "${CFLAGS}")
set(original_cxxflags "${CXXLAGS}")
set(original_asflags "${ASFLAGS}")

if(ANDROID)
  # fix NDK25 ffmpeg link error
  set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-Bsymbolic")

  if(CMAKE_ANDROID_STL_TYPE STREQUAL "c++_static")
    # this is a little hack. Some dependencies via pkgconfig (or something
    # another?) in this script forces usage of shared c++ runtime. Just setting
    # CMAKE_ANDROID_STL_TYPE does not help. So pass to linker this flag to
    # overwrite all another flags and link againt static version!
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")
  endif()

endif()

if(NOT MSVC)
  # ############################################################################
  # Contrib
  # ############################################################################
  if(APPLE)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      message(
        STATUS
          "apple detected non empty CMAKE_OSX_DEPLOYMENT_TARGET, propagading to ${CMAKE_OSX_DEPLOYMENT_TARGET} env"
      )
      set(ENV{CMAKE_OSX_DEPLOYMENT_TARGET} ${CMAKE_OSX_DEPLOYMENT_TARGET})
      set(ENV{MACOSX_DEPLOYMENT_TARGET} ${CMAKE_OSX_DEPLOYMENT_TARGET})
      set(ENV{RECOMMENDED_MACOSX_DEPLOYMENT_TARGET}
          ${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif()
  endif()

  # Android target mapping for contrib
  if(ANDROID_ABI STREQUAL "x86")
    set(TARGETS i686-linux-android)
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(TARGETS x86_64-linux-android)
  elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(TARGETS aarch64-linux-android)
  elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(TARGETS armv7a-linux-androideabi)
  else()
    set(TARGETS ${CMAKE_LIBRARY_ARCHITECTURE})

    if(NOT TARGETS)
      execute_process(
        COMMAND gcc -dumpmachine
        COMMAND tr -d '\n'
        OUTPUT_VARIABLE TARGETS)
    endif()

    string(REGEX MATCH "^([^-]+)" TARGET_ARCH ${TARGETS})
  endif()

  set(CONTRIB_PATHS "")

  if(BUILD_EXTRA_TOOLS)
    set(ENV{PATH}
        "${CMAKE_CURRENT_SOURCE_DIR}/extras/tools/build/bin:$ENV{PATH}")
    execute_process(COMMAND ./bootstrap
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/extras/tools)
    execute_process(COMMAND make
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/extras/tools)
    execute_process(COMMAND make .pkg-config
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/extras/tools)
    execute_process(COMMAND make .gas
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/extras/tools)
  endif()

  if(BUILD_CONTRIB)
    if(ANDROID)
      # We need to set this variables for contrib to be successfully configured
      # See https://developer.android.com/ndk/guides/other_build_systems
      set(ENV{ANDROID_NDK} ${ANDROID_NDK})
      set(ENV{ANDROID_ABI} ${ANDROID_ABI})
      set(ENV{ANDROID_API} ${ANDROID_PLATFORM})
      set(ENV{TOOLCHAIN} ${ANDROID_TOOLCHAIN_ROOT})
      set(ENV{TARGET} ${TARGETS})
      set(ENV{API} ${ANDROID_PLATFORM_LEVEL})
      set(ENV{CC} $ENV{TOOLCHAIN}/bin/${TARGETS}$ENV{API}-clang)
      set(ENV{CXX} $ENV{TOOLCHAIN}/bin/${TARGETS}$ENV{API}-clang++)
      set(ENV{AS} "$ENV{CC} -c")
      set(ENV{AR} ${CMAKE_AR})
      set(ENV{RANLIB} ${CMAKE_RANLIB})
      set(ENV{STRIP} ${CMAKE_STRIP})
      set(ENV{LD} $ENV{TOOLCHAIN}/bin/ld)
    endif()

    if(APPLE)

      if(CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" " " OSX_ARCH "${CMAKE_OSX_ARCHITECTURES}")
        string(REGEX MATCHALL "[^ ]+" OSX_ARCH_LIST "${OSX_ARCH}")
        set(CURRENT_ARCH_TRIPLE ${TARGETS})
        set(TARGETS "")

        foreach(osx_arch ${OSX_ARCH_LIST})
          string(REPLACE ${TARGET_ARCH} ${osx_arch} NEW_TARGET
                         ${CURRENT_ARCH_TRIPLE})
          list(APPEND TARGETS ${NEW_TARGET})
        endforeach()
      endif()

    endif()

    foreach(TARGET ${TARGETS})
      message(STATUS "building native dependencies for ${TARGET}")
      set(CONTRIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/contrib/${TARGET})
      list(APPEND CONTRIB_PATHS ${CONTRIB_PATH})
      set(CONTRIB_BUILD_PATH
          ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-${TARGET})
      file(MAKE_DIRECTORY ${CONTRIB_BUILD_PATH})
      file(MAKE_DIRECTORY ${CONTRIB_PATH})

      if(APPLE)
        string(REGEX MATCH "^([^-]+)" TARGET_ARCH ${TARGET})

        if(TARGET_ARCH STREQUAL "aarch64")
          set(TARGET_ARCH "arm64")
        endif()

        message(STATUS "passing extra darwin flag -arch ${TARGET_ARCH}")
        set(ENV{LDFLAGS} "-arch ${TARGET_ARCH} ${original_ldflags}")
        set(ENV{CFLAGS} "-arch ${TARGET_ARCH} ${original_cflags}")
        set(ENV{CXXFLAGS} "-arch ${TARGET_ARCH} ${original_cxxflags}")

        if(TARGET_ARCH STREQUAL "arm64")
          set(ENV{ASFLAGS} "${original_asflags}")
        endif()
      endif()

      execute_process(
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/contrib/bootstrap --host=${TARGET}
        WORKING_DIRECTORY ${CONTRIB_BUILD_PATH}
        RESULT_VARIABLE CMD_RESULT)

      if(CMD_RESULT)
        message(
          FATAL_ERROR
            "Bootstrap for target ${TARGET} failed with code ${CMD_RESULT}")
      endif()

      execute_process(
        COMMAND make list
        WORKING_DIRECTORY ${CONTRIB_BUILD_PATH}
        RESULT_VARIABLE CMD_RESULT)

      if(CMD_RESULT)
        message(
          FATAL_ERROR
            "Make native deps list for target ${TARGET} failed with code ${CMD_RESULT}"
        )
      endif()

      execute_process(
        COMMAND make
        WORKING_DIRECTORY ${CONTRIB_BUILD_PATH}
        RESULT_VARIABLE CMD_RESULT)

      if(CMD_RESULT)
        message(
          FATAL_ERROR
            "Make native deps for target ${TARGET} failed with code ${CMD_RESULT}"
        )
      endif()
    endforeach()
  endif()
endif()

# ##############################################################################
# Check dependencies
# ##############################################################################
if(NOT MSVC)
  include(FindPkgConfig)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_search_module(udev REQUIRED IMPORTED_TARGET libudev)
    pkg_search_module(pulseaudio REQUIRED IMPORTED_TARGET libpulse)
  endif()
endif()

# ##############################################################################
# Source groups
# ##############################################################################

# compat, src source group
if(MSVC)
  add_subdirectory("compat")
endif()

add_subdirectory("src")
source_group("Source Files" FILES ${Source_Files})
source_group("Source Files\\client" FILES ${Source_Files__client})
source_group("Source Files\\config" FILES ${Source_Files__config})
source_group("Source Files\\connectivity" FILES ${Source_Files__connectivity})
source_group("Source Files\\connectivity\\security"
             FILES ${Source_Files__security})
source_group("Source Files\\connectivity\\sip" FILES ${Source_Files__sip})
source_group("Source Files\\sip_core" FILES ${Source_Files__sip_core})
source_group("Source Files\\im" FILES ${Source_Files__im})
source_group("Source Files\\media" FILES ${Source_Files__media})
source_group("Source Files\\media\\audio" FILES ${Source_Files__media__audio})
source_group("Source Files\\media\\audio\\audio-processing"
             FILES ${Source_Files__media__audio__audio_processing})
source_group("Source Files\\media\\audio\\sound"
             FILES ${Source_Files__media__audio__sound})
source_group("Source Files\\media\\video" FILES ${Source_Files__media__video})

if(APPLE)
  source_group("Source Files\\media\\audio\\coreaudio"
               FILES ${Source_Files__media__audio__coreaudio})
  source_group("Source Files\\media\\video\\apple"
               FILES ${Source_Files__media__video__apple})
endif()

if(MSVC)
  if(WINDOWS_STORE)
    source_group("Source Files\\media\\video\\uwpvideo"
                 FILES ${Source_Files__media__video__uwpvideo})
  endif()

  source_group("Source Files\\media\\audio\\portaudio"
               FILES ${Source_Files__media__audio__portaudio})
  source_group("Source Files\\media\\video\\winvideo"
               FILES ${Source_Files__media__video__winvideo})
endif()

list(
  APPEND
  ALL_FILES
  ${Source_Files}
  ${Source_Files__client}
  ${Source_Files__config}
  ${Source_Files__connectivity}
  ${Source_Files__sip_core}
  ${Source_Files__im}
  ${Source_Files__media}
  ${Source_Files__media__audio}
  ${Source_Files__media__audio__sound}
  ${Source_Files__media__audio__audio_processing}
  ${Source_Files__media__video}
  ${Source_Files__security}
  ${Source_Files__sip})

if(ANDROID)
  list(
    APPEND
    ALL_FILES
    ${ALL_FILES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/media/video/androidvideo/video_device_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/media/video/androidvideo/video_device_monitor_impl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/media/audio/opensl/audio_player.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/media/audio/opensl/audio_recorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/media/audio/opensl/opensllayer.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(SIP_CORE_VIDEO)
    list(
      APPEND
      ALL_FILES
      ${ALL_FILES}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/media/video/v4l2/video_device_impl.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/media/video/v4l2/video_device_monitor_impl.cpp
    )
  endif()

  list(APPEND ALL_FILES ${ALL_FILES}
       ${CMAKE_CURRENT_SOURCE_DIR}/src/media/audio/pulseaudio/audiostream.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/src/media/audio/pulseaudio/pulselayer.cpp)
elseif(MSVC)
  list(APPEND ALL_FILES ${ALL_FILES} "${Source_Files__media__audio__portaudio}"
       "${Source_Files__media__video__uwpvideo}"
       "${Source_Files__media__video__winvideo}")
elseif(APPLE)
  list(APPEND ALL_FILES ${ALL_FILES} "${Source_Files__media__audio__coreaudio}"
       "${Source_Files__media__video__apple}")
endif()

add_library(${PROJECT_NAME} SHARED ${ALL_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "sip_core")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20
                                                 CMAKE_CXX_STANDARD_REQUIRED ON)

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    PACKAGE_NAME="${PACKAGE_NAME}"
    PACKAGE_VERSION="${PROJECT_VERSION}"
    PACKAGE="sip_core"
    # try to enable hardware accel RING_ACCEL for now, force video compilation
    # even when compiling standalone sip_core lib
    ENABLE_VIDEO
    # we are building sip_core, not using it!
    LIBSIP_CORE_BUILD
    # define exports
    # Conditionally add compile definitions based on platform
    $<$<OR:$<BOOL:${LINUX}>,$<BOOL:${MSVC}>,$<BOOL:${ANDROID}>>:HAVE_WEBRTC_AP>
    SIP_CORE_EXPORTS)

if(IOS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE TARGET_OS_IOS)
endif()

if(SIP_CORE_VIDEO)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_VIDEO)
endif()

if(ANDROID)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_OPENSL)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.;"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/config;"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/connectivity;"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/connectivity/security;"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/client;"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/im;"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/media;"
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/sip_core;")

if(MSVC)
  # ############################################################################
  # Target
  # ############################################################################
  include("CMake/Utils.cmake")
  use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}"
            "${DEFAULT_CXX_PROPS}")

  # ############################################################################
  # Include necessary .cmake file (need to init use_props first)
  # ############################################################################
  include("CMake/Default.cmake")
  include("CMake/DefaultCXX.cmake")

  # Supress generation of ZERO_CHECK
  set(CMAKE_SUPPRESS_REGENERATION true)
  set(ROOT_NAMESPACE sip_core)

  # ############################################################################
  # Target name
  # ############################################################################
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    set_target_properties(${PROJECT_NAME}
                          PROPERTIES TARGET_NAME_RELEASELIB_WIN32 "sip_core")
  endif()

  # ############################################################################
  # Output directory
  # ############################################################################
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    set_target_properties(
      ${PROJECT_NAME}
      PROPERTIES
        OUTPUT_DIRECTORY_RELEASELIB_WIN32
        "${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_VS_PLATFORM_NAME}/$<CONFIG>/bin/"
    )
  endif()

  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    set_target_properties(
      ${PROJECT_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASELIB_WIN32
                                 "FALSE")
  endif()

  # ############################################################################
  # Include directories
  # ############################################################################
  target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/fmt/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/http_parser;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/jsoncpp/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/yaml-cpp/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjlib/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjnath/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjlib-util/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjsip/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/third_party;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/speexdsp/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/webrtc-audio-processing;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/portaudio/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjmedia/include;"
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/include;"
  )

  # ############################################################################
  # Compile definitions
  # ############################################################################
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_compile_definitions(
      ${PROJECT_NAME}
      PRIVATE "_WIN32_WINNT=0x0601"
              "_USE_MATH_DEFINES;"
              "_SCL_SECURE_NO_WARNINGS;"
              "_CRT_SECURE_NO_WARNINGS;"
              "NOMINMAX;"
              "HAVE_CONFIG_H;"
              "WIN32_LEAN_AND_MEAN;"
              "WEBRTC_WIN;"
              "WEBRTC_AUDIO_PROCESSING_ONLY_BUILD;"
              "WEBRTC_NS_FLOAT;")
  endif()

  # ############################################################################
  # Compile and link options
  # ############################################################################
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_compile_options(
      ${PROJECT_NAME}
      PRIVATE /Oi;
              ${DEFAULT_CXX_RUNTIME_LIBRARY};
              /MP;
              /Oy-;
              /sdl-;
              /W0;
              /FC;
              /FS
              /nologo;
              /Zi;
              /wd4996;
              /wd4503;
              /wd4180;
              /wd4244;
              /wd4267;
              /Zc:__cplusplus;
              ${DEFAULT_CXX_EXCEPTION_HANDLING})
  endif()

  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_link_options(
      ${PROJECT_NAME}
      PRIVATE
      /MACHINE:X64
      /VERBOSE:LIB
      # all dependencies are build with /MD
      /NODEFAULTLIB:libcmtd
      /NODEFAULTLIB:libcmt
      # Always generate PDB
      /DEBUG
      /ignore:4006)
  endif()

  # ############################################################################
  # Dependencies
  # ############################################################################
  target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
      ws2_32.lib
      advapi32.lib
      Ole32.lib
      Kernel32.lib
      Gdi32.lib
      User32.lib
      Strmiids.lib
      OleAut32.lib
      Shlwapi.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavcodec.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavdevice.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavfilter.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavformat.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libswresample.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libswscale.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libx264.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libopus.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/lib_json.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/yaml-cpp/msvc/Release/libyaml-cppmd.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjsip/lib/pjsip-core-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjsip/lib/pjsip-simple-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjsip/lib/pjsua2-lib-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjsip/lib/pjsua-lib-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjsip/lib/pjsip-ua-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjmedia/lib/pjmedia-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjlib-util/lib/pjlib-util-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjlib/lib/pjlib-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/pjproject/pjnath/lib/pjnath-x86_64-x64-vc15-Release.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/fmt/msvc/Release/fmt.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/http_parser/x64/Release/http-parser.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/speexdsp/lib/libspeexdsp.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/webrtc-audio-processing/build/Release/webrtc-audio-processing.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/portaudio/build/Release/portaudio_static_x64.lib
      # this is for dependencies to work with frames emmitted by our library
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavutil.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libswscale.lib
      Secur32.lib
      bcrypt.lib)

else()
  foreach(CONTRIB_PATH ${CONTRIB_PATHS})
    message("Generated contrib path: ${CONTRIB_PATH}")
    set(CMAKE_FIND_ROOT_PATH ${CONTRIB_PATH})
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
    set(CMAKE_PREFIX_PATH ${CONTRIB_PATH})
    target_link_directories(${PROJECT_NAME} PRIVATE ${CONTRIB_PATH}/lib)

    find_package(jsoncpp REQUIRED)

    # Construct a target name based on the CONTRIB_PATH
    string(REPLACE "/" "_" CONTRIB_TARGET_NAME ${CONTRIB_PATH})
    pkg_search_module(fmt_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET fmt)

    if(LINUX OR ANDROID)
      pkg_search_module(webrtcap_${CONTRIB_TARGET_NAME} REQUIRED
                        IMPORTED_TARGET webrtc-audio-processing)
    endif()

    pkg_search_module(pjproject_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libpjproject)
    pkg_search_module(avformat_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libavformat)
    pkg_search_module(avdevice_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libavdevice)
    pkg_search_module(avfilter_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libavfilter)
    pkg_search_module(avcodec_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libavcodec)
    pkg_search_module(swresample_${CONTRIB_TARGET_NAME} REQUIRED
                      IMPORTED_TARGET libswresample)
    pkg_search_module(swscale_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libswscale)
    pkg_search_module(avutil_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      libavutil)
    pkg_search_module(yaml_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      yaml-cpp)
    pkg_search_module(json_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      jsoncpp)
    pkg_search_module(speex_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      speex)
    pkg_search_module(speexdsp_${CONTRIB_TARGET_NAME} REQUIRED IMPORTED_TARGET
                      speexdsp)

    # LD_FLAGS are broken on Apple, erase them
    if(APPLE)
      set_target_properties(PkgConfig::pjproject_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avformat_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avdevice_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avfilter_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avcodec_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::swresample_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::swscale_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avutil_${CONTRIB_TARGET_NAME}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
    endif()

    target_link_libraries(
      ${PROJECT_NAME}
      PRIVATE PkgConfig::pjproject_${CONTRIB_TARGET_NAME}
              PkgConfig::avformat_${CONTRIB_TARGET_NAME}
              PkgConfig::avdevice_${CONTRIB_TARGET_NAME}
              PkgConfig::avfilter_${CONTRIB_TARGET_NAME}
              PkgConfig::avcodec_${CONTRIB_TARGET_NAME}
              PkgConfig::swresample_${CONTRIB_TARGET_NAME}
              PkgConfig::swscale_${CONTRIB_TARGET_NAME}
              PkgConfig::yaml_${CONTRIB_TARGET_NAME}
              PkgConfig::json_${CONTRIB_TARGET_NAME}
              PkgConfig::speex_${CONTRIB_TARGET_NAME}
              PkgConfig::speexdsp_${CONTRIB_TARGET_NAME}
              PkgConfig::fmt_${CONTRIB_TARGET_NAME}
      PUBLIC PkgConfig::avutil_${CONTRIB_TARGET_NAME})

    if(LINUX)
      target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::webrtcap)
      target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::udev
                                                    PkgConfig::pulseaudio)
    endif()

    if (ANDROID)
      target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::webrtcap_${CONTRIB_TARGET_NAME})
    endif()
  endforeach(CONTRIB_PATH)

  if(APPLE)
    if(IOS)
      target_link_libraries(${PROJECT_NAME} PRIVATE resolv
                                                    "-framework CFNetwork")
    endif()

    target_link_libraries(
      ${PROJECT_NAME} PRIVATE resolv "-framework AudioToolbox"
                              "-framework CoreAudio")
  endif()

  if(ANDROID)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSLES log)
  endif()

  target_compile_options(
    ${PROJECT_NAME}
    PRIVATE -fvisibility=hidden
            -Wall
            -Wextra
            -Wnon-virtual-dtor
            -Wno-deprecated
            -Wno-deprecated-declarations
            -Wno-unknown-pragmas)
  target_compile_definitions(${PROJECT_NAME} PRIVATE PJ_AUTOCONF=1)

  if(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_COREAUDIO)
  endif()
endif()
