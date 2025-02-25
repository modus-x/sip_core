cmake_minimum_required(VERSION 3.25)

project(
  sip_core
  VERSION 0.10.2
  LANGUAGES C CXX)

set(BUILD_NUMBER 1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(PACKAGE="ru.svetets.sip-core"
                        PACKAGE_NAME="sv-sip-core")

# Generate a version header file
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/version.h")

set(CMAKE_MACOSX_RPATH 1)

include_directories(BEFORE ${CMAKE_CURRENT_BINARY_DIR})

if(CMAKE_GENERATOR STREQUAL "Xcode")
  set(CMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
  add_compile_options(-g)
endif()

# ##############################################################################
# Build options
# ##############################################################################
option(ENABLE_VIDEO "Build with video support" ON)
option(BUILD_DEPS "Build deps" ON)

set(PREBUILD_DEPS_PATHS
    ""
    CACHE
      PATH
      "Paths to prebuilt binaries, each of them named in standart triplet format"
)

if(NOT PREBUILD_DEPS_PATHS STREQUAL "")
  foreach(PREBUILD_DEPS_PATH ${PREBUILD_DEPS_PATHS})

    # let's convert it into absolute!
    if(NOT IS_ABSOLUTE ${PREBUILD_DEPS_PATH})
      cmake_path(ABSOLUTE_PATH PREBUILD_DEPS_PATH NORMALIZE)
    endif()

    if(NOT EXISTS ${PREBUILD_DEPS_PATH})
      message(FATAL_ERROR "${PREBUILD_DEPS_PATH} IS NOT FOUND")
    endif()
    list(APPEND CONTRIB_PATHS ${PREBUILD_DEPS_PATH})
  endforeach()
endif()

if(ANDROID)
  # fix NDK25 ffmpeg link error
  set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-Bsymbolic")

  if(CMAKE_ANDROID_STL_TYPE STREQUAL "c++_static")
    # this is a little hack. Some dependencies via pkgconfig (or something
    # another?) in this script forces usage of shared c++ runtime. Just setting
    # CMAKE_ANDROID_STL_TYPE does not help. So pass to linker this flag to
    # overwrite all another flags and link against static version!
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")
  endif()
endif()

function(GETARCH TRIPLE)
  string(REGEX MATCH "^([^-]+)" ARCH ${TRIPLE})

  if(ARCH STREQUAL "aarch64")
    set(ARCH "arm64")
  endif()

  return(PROPAGATE ARCH)
endfunction()

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
    execute_process(
      COMMAND gcc -dumpmachine
      COMMAND tr -d '\n'
      OUTPUT_VARIABLE TARGETS)

    getarch(${TARGETS})
  endif()

  if(BUILD_DEPS)
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
      if(CMAKE_OSX_ARCHITECTURES STREQUAL "" OR NOT DEFINED
                                                CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES ${ARCH})
      endif()

      if(NOT IOS)
        # create list of passed-in architectures, add them to targets, because
        # we need to build contrib for every darwin arch!
        string(REPLACE ";" " " OSX_ARCH "${CMAKE_OSX_ARCHITECTURES}")
        string(REGEX MATCHALL "[^ ]+" OSX_ARCH_LIST "${OSX_ARCH}")
        set(CURRENT_ARCH_TRIPLE ${TARGETS})
        set(TARGETS "")

        foreach(osx_arch ${OSX_ARCH_LIST})
          string(REPLACE ${ARCH} ${osx_arch} NEW_TARGET ${CURRENT_ARCH_TRIPLE})
          list(APPEND TARGETS ${NEW_TARGET})
        endforeach()
      endif()

      if(IOS)
        # ios specific flags and contrib build root
        execute_process(
          COMMAND xcrun --sdk "${CMAKE_OSX_SYSROOT}" --show-sdk-version
          OUTPUT_VARIABLE SDK_VERSION
          OUTPUT_STRIP_TRAILING_WHITESPACE)

        if(NOT DEFINED ENV{IOS_TARGET_PLATFORM})
          message(
            FATAL_ERROR
              "IOS_TARGET_PLATFORM environment variable"
              " should be set to make deps for iphone simulator or real device")
        endif()

        if($ENV{IOS_TARGET_PLATFORM} STREQUAL "iPhoneSimulator")
          if(${CMAKE_OSX_ARCHITECTURES} MATCHES "arm64")
            message("-- iOS Build for simulator arm64")
          elseif(${CMAKE_OSX_ARCHITECTURES} MATCHES "x86_64")
            message("-- iOS Build for simulator x86_64")
          endif()

          set(ENV{LDFLAGS}
              "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(ENV{CFLAGS}
              "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(ENV{CXXFLAGS}
              "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(ENV{ASFLAGS}
              "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(TARGETS ${TARGETS}-ios${SDK_VERSION}-simulator)
        elseif($ENV{IOS_TARGET_PLATFORM} STREQUAL "iPhoneOS")
          message("-- iOS Build for iphone")
          set(ENV{LDFLAGS}
              "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(ENV{CFLAGS}
              "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(ENV{CXXFLAGS}
              "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(ENV{ASFLAGS}
              "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
          set(TARGETS ${TARGETS}-ios${SDK_VERSION}-real)
        else()
          message(FATAL_ERROR "-- iOS undefined arch for contrib build")
        endif()
      endif()
    endif()

    foreach(TARGET ${TARGETS})
      message(STATUS "building native dependencies for ${TARGET}")
      set(CONTRIB_BUILD_PATH
          ${CMAKE_CURRENT_SOURCE_DIR}/contrib/build-${TARGET})
      set(CONTRIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/contrib/${TARGET})

      # append our path to list of contrib paths!
      list(APPEND CONTRIB_PATHS ${CONTRIB_PATH})

      # pass some args from cmake to dependencies
      if(APPLE)
        if(NOT IOS)
          getarch(${TARGET})

          set(ENV{LDFLAGS}
              "-arch ${ARCH} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
          )
          set(ENV{CFLAGS}
              "-arch ${ARCH} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
          )
          set(ENV{CXXFLAGS}
              "-arch ${ARCH} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
          )
        endif()
      endif()

      if(IOS)
        set(REAL_TARGET "${CMAKE_OSX_ARCHITECTURES}-apple-darwin_ios")
      else()
        set(REAL_TARGET ${TARGET})
      endif()

      if(NOT EXISTS ${CONTRIB_BUILD_PATH})
        file(MAKE_DIRECTORY ${CONTRIB_BUILD_PATH})
        execute_process(
          COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/contrib/bootstrap
                  --prefix=${CONTRIB_PATH}
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
add_subdirectory("src")
source_group("Source Files" FILES ${Source_Files})
source_group("Source Files\\client" FILES ${Source_Files__client})
source_group("Source Files\\config" FILES ${Source_Files__config})
source_group("Source Files\\connectivity" FILES ${Source_Files__connectivity})
source_group("Source Files\\connectivity\\security"
             FILES ${Source_Files__security})
source_group("Source Files\\sip" FILES ${Source_Files__sip})
source_group("Source Files\\im" FILES ${Source_Files__im})
source_group("Source Files\\media" FILES ${Source_Files__media})
source_group("Source Files\\media\\audio" FILES ${Source_Files__media__audio})
source_group("Source Files\\media\\audio\\audio-processing"
             FILES ${Source_Files__media__audio__audio_processing})
source_group("Source Files\\media\\audio\\sound"
             FILES ${Source_Files__media__audio__sound})
source_group("Source Files\\media\\video" FILES ${Source_Files__media__video})

file(GLOB_RECURSE Source_Files__sip_core
     ${CMAKE_CURRENT_SOURCE_DIR}/src/sip_core/*.h)

source_group("Source Files\\sip_core" FILES ${Source_Files__sip_core})

if(APPLE)
  source_group("Source Files\\media\\audio\\coreaudio"
               FILES ${Source_Files__media__audio__coreaudio})
  source_group("Source Files\\media\\video\\apple"
               FILES ${Source_Files__media__video__apple})
endif()

if(MSVC)
  # use unicode
  add_definitions(-DUNICODE -D_UNICODE)

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
  add_definitions(-DHAVE_PULSE)

  if(ENABLE_VIDEO)
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

add_library(${PROJECT_NAME} STATIC ${ALL_FILES})

if(LINUX)
  # compile with -rtlib=compiler-rt, to include all special builtin functions
  # into binary
  target_link_options(${PROJECT_NAME} PRIVATE -rtlib=compiler-rt)
  # GCC specific CXX, which was used when deps were built
  find_library(
    GCC_CXX libstdc++.a
    PATHS /opt/rh/devtoolset-11/root/usr/lib/gcc/x86_64-redhat-linux/11
          REQUIRED NO_CACHE)

  target_link_libraries(${PROJECT_NAME} PRIVATE ${GCC_CXX})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "sip_core")

target_sources(${PROJECT_NAME} PUBLIC FILE_SET HEADERS BASE_DIRS src FILES
                                      ${Source_Files__sip_core})

# compat, src source group + create deps with proper configuration
if(MSVC)
  if(BUILD_DEPS)
    add_custom_command(
      TARGET ${PROJECT_NAME}
      PRE_BUILD
      COMMAND
        python ${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc/winmake.py --build
        sip_core -v -i -d DEBUG --deps_only --build_type
        $<IF:$<CONFIG:Debug>,Debug,Release>)

    if(CMD_RESULT)
      message(
        FATAL_ERROR
          "Compiling native deps for target win32 failed with code ${CMD_RESULT}"
      )
    endif()
  endif()

  add_subdirectory("compat")
endif()

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    # we are building sip_core, not using it!
    LIBSIP_CORE_BUILD
    # define exports Conditionally add compile definitions based on platform
    $<$<OR:$<BOOL:${LINUX}>,$<BOOL:${MSVC}>,$<BOOL:${ANDROID}>>:HAVE_WEBRTC_AP>
    $<$<OR:$<BOOL:${LINUX}>,$<BOOL:${MSVC}>,$<BOOL:${ANDROID}>>:HAVE_SPEEXDSP>
    # Building shared library
    SIP_CORE_EXPORTS)

if(IOS)
  target_compile_definitions(${PROJECT_NAME} PUBLIC TARGET_OS_IOS)
endif()

if(ENABLE_VIDEO)
  target_compile_definitions(${PROJECT_NAME} PUBLIC ENABLE_VIDEO)
endif()

if(ANDROID)
  target_compile_definitions(${PROJECT_NAME} PUBLIC HAVE_OPENSL)
endif()

if(LINUX OR ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/.;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/config;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/connectivity;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/connectivity/security;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/client;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/im;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/media;"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/sip_core;")

if(MSVC)
  # Supress generation of ZERO_CHECK
  set(CMAKE_SUPPRESS_REGENERATION true)

  set(DEPS_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/contrib/win32/Release)
  set(DEPS_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/contrib/win32/Debug)

  # ############################################################################
  # Include directories
  # ############################################################################
  target_include_directories(
    ${PROJECT_NAME}
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/msvc/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/fmt/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/http_parser;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/jsoncpp/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/yaml-cpp/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/speexdsp/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/webrtc-audio-processing;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/portaudio/include;"
      $<$<CONFIG:Debug>:${DEPS_DEBUG}/include>
      $<$<CONFIG:Release>:${DEPS_RELEASE}/include>
      $<$<CONFIG:RelWithDebInfo>:${DEPS_RELEASE}/include>
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjlib/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjnath/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjlib-util/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/include;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/third_party;"
      "${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjmedia/include;"
  )

  # ############################################################################
  # Compile definitions
  # ############################################################################
  target_compile_definitions(
    ${PROJECT_NAME}
    PUBLIC "_WIN32_WINNT=0x0601;"
            "_USE_MATH_DEFINES;"
            "_SCL_SECURE_NO_WARNINGS;"
            "_CRT_SECURE_NO_WARNINGS;"
            "NOMINMAX;"
            "HAVE_CONFIG_H;"
            "WIN32_LEAN_AND_MEAN;"
            "WEBRTC_WIN;"
            "WEBRTC_AUDIO_PROCESSING_ONLY_BUILD;"
            "WEBRTC_NS_FLOAT;")

  # ############################################################################
  # Compile and link options
  # ############################################################################
  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_compile_options(
      ${PROJECT_NAME}
      PUBLIC /Oi;
              /MP;
              /Oy-;
              /sdl-;
              /W0;
              /FC;
              /FS;
              /nologo;
              /wd4996;
              /wd4503;
              /wd4180;
              /wd4244;
              /wd4267;
              /Zc:__cplusplus)
  endif()

  # always store debug symbols in pdb files
  set_property(
    TARGET ${PROJECT_NAME}
    PROPERTY
      MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug,RelWithDebInfo,Release,MinSizeRel>:ProgramDatabase>")

  if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    target_link_options(
      ${PROJECT_NAME}
      PUBLIC
      /MACHINE:X64
      /VERBOSE:LIB
      # all dependencies are build with /MD
      /NODEFAULTLIB:libcmtd
      /NODEFAULTLIB:libcmt
      # Always generate PDB
      /ignore:4006)
  endif()

  # ############################################################################
  # Dependencies
  # ############################################################################
  target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE # System
            ws2_32.lib
            advapi32.lib
            Ole32.lib
            Kernel32.lib
            Gdi32.lib
            User32.lib
            Strmiids.lib
            OleAut32.lib
            Shlwapi.lib
            optimized
            ${DEPS_RELEASE}/lib/libavcodec.lib
            optimized
            ${DEPS_RELEASE}/lib/libavdevice.lib
            optimized
            ${DEPS_RELEASE}/lib/libavfilter.lib
            optimized
            ${DEPS_RELEASE}/lib/libavformat.lib
            optimized
            ${DEPS_RELEASE}/lib/libswresample.lib
            optimized
            ${DEPS_RELEASE}/lib/libswscale.lib
            optimized
            ${DEPS_RELEASE}/lib/x64/libx264.lib
            optimized
            ${DEPS_RELEASE}/lib/x64/libopus.lib
            optimized
            ${DEPS_RELEASE}/lib/x64/libvpx.lib
            optimized
            ${DEPS_RELEASE}/lib/x64/lib_json.lib
            optimized
            ${DEPS_RELEASE}/libyaml-cppmd.lib
            optimized
            ${DEPS_RELEASE}/fmt.lib
            optimized
            ${DEPS_RELEASE}/libspeexdsp.lib
            optimized
            ${DEPS_RELEASE}/webrtc-audio-processing.lib
            optimized
            ${DEPS_RELEASE}/portaudio_static_x64.lib
            debug
            ${DEPS_DEBUG}/lib/libavcodec.lib
            debug
            ${DEPS_DEBUG}/lib/libavdevice.lib
            debug
            ${DEPS_DEBUG}/lib/libavfilter.lib
            debug
            ${DEPS_DEBUG}/lib/libavformat.lib
            debug
            ${DEPS_DEBUG}/lib/libswresample.lib
            debug
            ${DEPS_DEBUG}/lib/libswscale.lib
            debug
            ${DEPS_DEBUG}/lib/x64/libx264.lib
            debug
            ${DEPS_DEBUG}/lib/x64/libopus.lib
            debug
            ${DEPS_DEBUG}/lib/x64/libvpx.lib
            debug
            ${DEPS_DEBUG}/lib/x64/lib_json.lib
            debug
            ${DEPS_DEBUG}/libyaml-cppmdd.lib
            debug
            ${DEPS_DEBUG}/fmtd.lib
            debug
            ${DEPS_DEBUG}/libspeexdsp.lib
            debug
            ${DEPS_DEBUG}/webrtc-audio-processing.lib
            debug
            ${DEPS_DEBUG}/portaudio_static_x64.lib
            # this is for dependencies to work with frames emmitted by our
            # library
    PUBLIC
      Secur32.lib
      bcrypt.lib
      debug
      ${DEPS_DEBUG}/lib/libavutil.lib
      debug
      ${DEPS_DEBUG}/lib/libswscale.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsip-core-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsip-simple-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsua2-lib-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsua-lib-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsip-ua-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjmedia/lib/pjmedia-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjlib-util/lib/pjlib-util-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjlib/lib/pjlib-x86_64-x64-vc15-Debug.lib
      debug
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjnath/lib/pjnath-x86_64-x64-vc15-Debug.lib
      optimized
      ${DEPS_RELEASE}/lib/libavutil.lib
      optimized
      ${DEPS_RELEASE}/lib/libswscale.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsip-core-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsip-simple-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsua2-lib-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsua-lib-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjsip/lib/pjsip-ua-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjmedia/lib/pjmedia-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjlib-util/lib/pjlib-util-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjlib/lib/pjlib-x86_64-x64-vc15-Release.lib
      optimized
      ${CMAKE_CURRENT_SOURCE_DIR}/contrib/native-win32/pjproject/pjnath/lib/pjnath-x86_64-x64-vc15-Release.lib
  )

else()
  foreach(CONTRIB_PATH ${CONTRIB_PATHS})
    # every contrib path is a triplet, which is guaranteed to have arch at the
    # beginning

    cmake_path(GET CONTRIB_PATH FILENAME CONTRIB_FOLDER)
    string(REPLACE "-" ";" SEPARATED ${CONTRIB_FOLDER})

    list(GET SEPARATED 0 TARGET)

    set(CMAKE_PREFIX_PATH ${CONTRIB_PATH})

    set(CMAKE_FIND_ROOT_PATH ${CONTRIB_PATH})
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

    pkg_search_module(fmt_${TARGET} REQUIRED IMPORTED_TARGET fmt)

    if(LINUX OR ANDROID)
      pkg_search_module(webrtcap_${TARGET} REQUIRED IMPORTED_TARGET
                        webrtc-audio-processing)
    endif()

    pkg_search_module(pjproject_${TARGET} REQUIRED IMPORTED_TARGET libpjproject)
    pkg_search_module(avformat_${TARGET} REQUIRED IMPORTED_TARGET libavformat)
    pkg_search_module(vpx_${TARGET} REQUIRED IMPORTED_TARGET vpx)
    pkg_search_module(avdevice_${TARGET} REQUIRED IMPORTED_TARGET libavdevice)
    pkg_search_module(avfilter_${TARGET} REQUIRED IMPORTED_TARGET libavfilter)
    pkg_search_module(avcodec_${TARGET} REQUIRED IMPORTED_TARGET libavcodec)
    pkg_search_module(swresample_${TARGET} REQUIRED IMPORTED_TARGET
                      libswresample)
    pkg_search_module(swscale_${TARGET} REQUIRED IMPORTED_TARGET libswscale)
    pkg_search_module(avutil_${TARGET} REQUIRED IMPORTED_TARGET GLOBAL
                      libavutil)
    pkg_search_module(yaml_${TARGET} REQUIRED IMPORTED_TARGET yaml-cpp)
    pkg_search_module(json_${TARGET} REQUIRED IMPORTED_TARGET jsoncpp)
    pkg_search_module(speex_${TARGET} REQUIRED IMPORTED_TARGET speex)
    pkg_search_module(speexdsp_${TARGET} REQUIRED IMPORTED_TARGET speexdsp)

    # LD_FLAGS are broken on Apple, erase them
    if(APPLE)
      set_target_properties(PkgConfig::pjproject_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::vpx_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avformat_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avdevice_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avfilter_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avcodec_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::swresample_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::swscale_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
      set_target_properties(PkgConfig::avutil_${TARGET}
                            PROPERTIES INTERFACE_LINK_OPTIONS "")
    endif()

    target_link_libraries(
      ${PROJECT_NAME}
      PUBLIC PkgConfig::avformat_${TARGET}
             PkgConfig::pjproject_${TARGET}
             PkgConfig::avdevice_${TARGET}
             PkgConfig::avfilter_${TARGET}
             PkgConfig::vpx_${TARGET}
             PkgConfig::avcodec_${TARGET}
             PkgConfig::swresample_${TARGET}
             PkgConfig::swscale_${TARGET}
             PkgConfig::yaml_${TARGET}
             PkgConfig::json_${TARGET}
             PkgConfig::speex_${TARGET}
             PkgConfig::speexdsp_${TARGET}
             PkgConfig::fmt_${TARGET}
      # for passing video frames to external libraries
      PUBLIC PkgConfig::avutil_${TARGET})

    if(LINUX)
      target_link_libraries(${PROJECT_NAME}
                            PRIVATE PkgConfig::webrtcap_${TARGET})
      target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::udev
                                                    PkgConfig::pulseaudio)
    endif()

    if(ANDROID)
      target_link_libraries(${PROJECT_NAME}
                            PRIVATE PkgConfig::webrtcap_${TARGET})
    endif()
  endforeach()

  if(APPLE)
    if(IOS)
      target_link_libraries(${PROJECT_NAME} PRIVATE resolv
                                                    "-framework CFNetwork")
    endif()

    target_link_libraries(
      ${PROJECT_NAME} PRIVATE resolv "-framework AudioToolbox"
                              "-framework CoreAudio" "-framework Accelerate")
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
    target_compile_definitions(${PROJECT_NAME} PUBLIC HAVE_COREAUDIO)
  endif()
endif()

# native clients

set(RQM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/rqm/main.cpp)

add_executable(rqm ${RQM_SOURCES})

target_link_libraries(
  rqm ${PROJECT_NAME})

# install
install(TARGETS ${PROJECT_NAME} FILE_SET HEADERS)
