cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(
  FFmpeg
  REQUIRED avcodec avdevice avutil avformat swscale
)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE FFmpeg::avcodec FFmpeg::avdevice FFmpeg::avutil FFmpeg::avformat FFmpeg::swscale
)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  # AUTOMOC's compiler-predefines probe runs clang from an Xcode script
  # phase, where Xcode 26 injects conflicting *_DEPLOYMENT_TARGET
  # variables that make clang error out
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON AUTOMOC_COMPILER_PREDEFINES OFF
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/warp-source.c
    src/warp-playlist-source.c
    src/warp-detection-filter.c
    src/warp-websocket.c
    src/media-playback/cache.c
    src/media-playback/decode.c
    src/media-playback/media-playback.c
    src/media-playback/media.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(ENABLE_FRONTEND_API AND ENABLE_QT)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/warp-window.cpp)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WARP_HAVE_FRONTEND)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
