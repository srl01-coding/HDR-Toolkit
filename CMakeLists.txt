cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

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
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/shared/filter-io.cpp
    src/shared/filter-io.hpp
    src/shared/obs-color-context.cpp
    src/shared/obs-color-context.hpp
    src/sharpen/sharpen-filter.cpp
    src/sharpen/sharpen-math.cpp
    src/sharpen/sharpen-math.hpp
    src/neutral/neutral-filter.cpp
    src/pattern/pattern-math.cpp
    src/pattern/pattern-math.hpp
    src/pattern/pattern-source.cpp
    src/transform/quad-math.cpp
    src/transform/quad-math.hpp
    src/transform/transform-filter.cpp
    src/color/color-math.cpp
    src/color/color-math.hpp
    src/color/color-filter.cpp
    src/denoise/d3d11-compute.cpp
    src/denoise/d3d11-compute.hpp
    src/denoise/denoise-core.cpp
    src/denoise/denoise-core.hpp
    src/denoise/denoise-filter.cpp
    src/denoise/hqdn3d-math.cpp
    src/denoise/hqdn3d-math.hpp
    src/denoise/noise-meter-math.cpp
    src/denoise/noise-meter-math.hpp
    src/denoise/noise-meter.cpp
    src/denoise/noise-meter.hpp
    src/denoise/noise-profile.cpp
    src/denoise/noise-profile.hpp
    src/denoise/nvof-probe.cpp
    src/denoise/nvof-probe.hpp
    src/denoise/program-denoise.cpp
    src/denoise/spatial-math.cpp
    src/denoise/spatial-math.hpp
)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE third_party/nvof third_party/nvof/shim)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
