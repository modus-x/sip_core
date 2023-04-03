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

if(NOT MSVC)
  # ############################################################################
  # Contrib
  # ############################################################################

  # Android target mapping for contrib
  if(ANDROID_ABI STREQUAL "x86")
    set(TARGET i686-linux-android)
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(TARGET x86_64-linux-android)
  elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(TARGET aarch64-linux-android)
  elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(TARGET armv7a-linux-androideabi)
  else()
    set(TARGET ${CMAKE_LIBRARY_ARCHITECTURE})
    if(NOT TARGET)
      execute_process(
        COMMAND gcc -dumpmachine
        COMMAND tr -d '\n'
        OUTPUT_VARIABLE TARGET)
    endif()
  endif()
  set(CONTRIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/contrib/${TARGET})

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
    set(CONTRIB_BUILD_PATH ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-${TARGET})
    file(MAKE_DIRECTORY ${CONTRIB_BUILD_PATH})
    file(MAKE_DIRECTORY ${CONTRIB_PATH})

    if(ANDROID)
      # See https://developer.android.com/ndk/guides/other_build_systems
      set(ENV{ANDROID_NDK} ${ANDROID_NDK})
      set(ENV{ANDROID_ABI} ${ANDROID_ABI})
      set(ENV{ANDROID_API} ${ANDROID_PLATFORM})
      set(ENV{TOOLCHAIN} ${ANDROID_TOOLCHAIN_ROOT})
      set(ENV{TARGET} ${TARGET})
      set(ENV{API} ${ANDROID_PLATFORM_LEVEL})
      set(ENV{CC} $ENV{TOOLCHAIN}/bin/${TARGET}$ENV{API}-clang)
      set(ENV{CXX} $ENV{TOOLCHAIN}/bin/${TARGET}$ENV{API}-clang++)
      set(ENV{AS} "$ENV{CC} -c")
      set(ENV{AR} ${CMAKE_AR})
      set(ENV{RANLIB} ${CMAKE_RANLIB})
      set(ENV{STRIP} ${CMAKE_STRIP})
      set(ENV{LD} $ENV{TOOLCHAIN}/bin/ld)
    endif()

    execute_process(
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/contrib/bootstrap --host=${TARGET}
      WORKING_DIRECTORY ${CONTRIB_BUILD_PATH})
    execute_process(COMMAND make list WORKING_DIRECTORY ${CONTRIB_BUILD_PATH})
    execute_process(COMMAND make WORKING_DIRECTORY ${CONTRIB_BUILD_PATH})

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
endif()

add_library(${PROJECT_NAME} STATIC ${ALL_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "sip_core")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20)

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE PACKAGE_NAME="${PACKAGE_NAME}"
          PACKAGE_VERSION="${PROJECT_VERSION}"
          PACKAGE="sip_core"
          # we are building sip_core, not using it!
          LIBSIP_CORE_BUILD)
if(SIP_CORE_VIDEO)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_VIDEO)
endif()
if(ANDROID)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_OPENSL)
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
  set(ROOT_NAMESPACE ringdaemon)

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
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_include_directories(
      ${PROJECT_NAME}
      PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/include/upnp;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/sndfile/src;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/openssl/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/asio/asio/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/restinio/dev;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/fmt/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/http_parser;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/natpmp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/msgpack-c/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/opendht/include;"
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/libarchive/libarchive;"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/libgit2/include")
  endif()

  # ############################################################################
  # Compile definitions
  # ############################################################################
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_compile_definitions(
      ${PROJECT_NAME}
      PRIVATE "_WIN32_WINNT=0x0A00;"
              "STATIC_GETOPT;"
              "LIBARCHIVE_STATIC;"
              "OPENDHT_PROXY_CLIENT;"
              "OPENDHT_PROXY_SERVER;"
              "OPENDHT_PUSH_NOTIFICATIONS;"
              "NATPMP_STATICLIB;"
              "NDEBUG;"
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
      PRIVATE /O2;
              /Oi;
              ${DEFAULT_CXX_RUNTIME_LIBRARY};
              /Gy;
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
      /MACHINE:X64;
      /OPT:REF;
      /OPT:ICF
      /VERBOSE:LIB;
      /NODEFAULTLIB:libcmtd;
      /NODEFAULTLIB:libcmt;
      /OPT:NOLBR;
      /OPT:NOICF;
      /LTCG)
  endif()

  # ############################################################################
  # Dependencies
  # ############################################################################

  set(libAdditionalDependencies
      "${CMAKE_STATIC_LINKER_FLAGS} /LTCG
        ws2_32.lib
        advapi32.lib
        Secur32.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavcodec.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavdevice.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavfilter.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavformat.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libavutil.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libswresample.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/ffmpeg/Build/win32/x64/lib/libswscale.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libx264.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libvpx.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libopus.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libmfx.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libgnutls.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/lib_json.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/opendht/build/Release/libopendht.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/argon2/vs2015/Argon2Ref/vs2015/build/Argon2Ref.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/secp256k1.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/yaml-cpp/msvc/Release/libyaml-cppmd.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/libupnp.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/natpmp/build/Release/natpmp.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/lib/x64/archive_static.lib
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
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/openssl/libcrypto.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/openssl/libssl.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/speexdsp/lib/libspeexdsp.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/webrtc-audio-processing/build/Release/webrtc-audio-processing.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/portaudio/build/Release/portaudio_static_x64.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build/libgit2/msvc/Release/git2.lib
        /ignore:4006")

  set(CMAKE_STATIC_LINKER_FLAGS ${libAdditionalDependencies})
else()
  message("Contrib path: ${CONTRIB_PATH}")
  list(APPEND CMAKE_FIND_ROOT_PATH ${CONTRIB_PATH})
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
  list(APPEND CMAKE_PREFIX_PATH ${CONTRIB_PATH})

  find_package(jsoncpp REQUIRED)
  find_package(yaml-cpp REQUIRED)
  pkg_search_module(fmt REQUIRED IMPORTED_TARGET fmt)
  if(LINUX)
    pkg_search_module(webrtcap REQUIRED IMPORTED_TARGET webrtc-audio-processing)
  endif()
  pkg_search_module(pjproject REQUIRED IMPORTED_TARGET libpjproject)
  pkg_search_module(avformat REQUIRED IMPORTED_TARGET libavformat)
  pkg_search_module(avdevice REQUIRED IMPORTED_TARGET libavdevice)
  pkg_search_module(avfilter REQUIRED IMPORTED_TARGET libavfilter)
  pkg_search_module(avcodec REQUIRED IMPORTED_TARGET libavcodec)
  pkg_search_module(swresample REQUIRED IMPORTED_TARGET libswresample)
  pkg_search_module(swscale REQUIRED IMPORTED_TARGET libswscale)
  pkg_search_module(avutil REQUIRED IMPORTED_TARGET libavutil)

  target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE PkgConfig::pjproject
            PkgConfig::avformat
            PkgConfig::avdevice
            PkgConfig::avfilter
            PkgConfig::avcodec
            PkgConfig::swresample
            PkgConfig::swscale
            jsoncpp
            yaml-cpp
            PkgConfig::fmt
    PUBLIC PkgConfig::avutil)

  if(LINUX)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::webrtcap)
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
  target_compile_definitions(
    ${PROJECT_NAME} PRIVATE SIP_CORE_DATADIR="${SIP_CORE_DATADIR}"
                            PJ_AUTOCONF=1)
  if(ANDROID)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSLES log)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::udev
                                                  PkgConfig::pulseaudio)
  endif()

endif()
