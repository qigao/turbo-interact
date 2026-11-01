cmake_minimum_required(VERSION 3.21)
project(TurboInteract VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(turbo_interact_core STATIC
  src/domain.c
  src/application.c
  src/binding.c
  src/bridge.c)
add_library(TurboInteract::Core ALIAS turbo_interact_core)
set_target_properties(turbo_interact_core PROPERTIES
  EXPORT_NAME Core
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO)
target_include_directories(turbo_interact_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Product C11 builds use Clang/LLVM; MSVC/GCC are not release toolchains.
target_compile_options(turbo_interact_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Product-level opt-in: actual installed TurboMedia C11 FrameStore owner.
# Never build an independent frame implementation or silently fall back.
option(TI_WITH_TURBOMEDIA_STREAMGRAPH
       "Bind real installed TurboMedia StreamGraph FrameStore to GraphBridge" OFF)

# The real Android H264 product integration is exercised in the EXISTING
# ti_bridge_test on Linux, with upstream exact source from the same pinned
# TurboMedia checkout that supplied the installed StreamGraph library.
# It is NOT a second shipping Codec DSO/engine or a new test project.
option(TI_TEST_M17_REAL_H264
       "Exercise original TurboMedia OpenH264 Codec in existing ti_bridge_test" OFF)
if(TI_TEST_M17_REAL_H264 AND
   (NOT TI_WITH_TURBOMEDIA_STREAMGRAPH OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
    OR CMAKE_CROSSCOMPILING OR NOT TI_TURBOMEDIA_SOURCE_DIR))
  message(FATAL_ERROR "Real H264 GraphBridge test requires Linux host and actual pinned TurboMedia source")
endif()
if(TI_WITH_TURBOMEDIA_STREAMGRAPH)
  find_package(TurboMediaStreamGraph CONFIG REQUIRED)
  if(NOT TARGET TurboMedia::StreamGraph)
    message(FATAL_ERROR "Actual TurboMedia::StreamGraph target unavailable")
  endif()
  add_library(turbo_interact_streamgraph_bridge STATIC
    src/streamgraph_bridge.c
    src/m17e_fact.c)
  add_library(TurboInteract::StreamGraphBridge ALIAS
    turbo_interact_streamgraph_bridge)
  set_target_properties(turbo_interact_streamgraph_bridge PROPERTIES
    EXPORT_NAME StreamGraphBridge
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_include_directories(turbo_interact_streamgraph_bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_options(turbo_interact_streamgraph_bridge PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(turbo_interact_streamgraph_bridge PUBLIC
    TurboInteract::Core TurboMedia::StreamGraph)
  install(TARGETS turbo_interact_streamgraph_bridge
    EXPORT TurboInteractTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/turbo_interact/streamgraph_bridge.h
                include/turbo_interact/m17e_fact.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbo_interact)
endif()

# Optional *actual* installed TurboFlow/SCXML targets, never fake stubs.
# These SDKs currently have no independently verified public release graph
# for M17e, so do not run their gated profile in the default CI and do not
# claim that OFF compiles the external adapter. Missing SDK is a hard error.
option(TI_WITH_TURBOFLOW_SCXML
       "Use actually installed TurboFlow::Graph + TurboSCXML::SCXML SDK" OFF)
if(TI_WITH_TURBOFLOW_SCXML)
  if(NOT TI_WITH_TURBOMEDIA_STREAMGRAPH)
    message(FATAL_ERROR
      "M17e Flow/SCXML requires the actual installed StreamGraphBridge")
  endif()
  find_package(TurboFlow CONFIG REQUIRED COMPONENTS Graph)
  find_package(TurboSCXML CONFIG REQUIRED)
  if(NOT TARGET TurboFlow::Graph OR NOT TARGET TurboSCXML::SCXML)
    message(FATAL_ERROR "M17e requires real TurboFlow/SCXML imported SDK targets")
  endif()
  add_library(turbo_interact_flow_scxml_bridge STATIC src/m17e_flow_scxml.c)
  add_library(TurboInteract::FlowSCXMLBridge ALIAS
    turbo_interact_flow_scxml_bridge)
  set_target_properties(turbo_interact_flow_scxml_bridge PROPERTIES
    EXPORT_NAME FlowSCXMLBridge
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_include_directories(turbo_interact_flow_scxml_bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_options(turbo_interact_flow_scxml_bridge PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(turbo_interact_flow_scxml_bridge PUBLIC
    TurboInteract::StreamGraphBridge TurboFlow::Graph TurboSCXML::SCXML)
  install(TARGETS turbo_interact_flow_scxml_bridge
    EXPORT TurboInteractTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/turbo_interact/m17e_flow_scxml.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbo_interact)
endif()

install(TARGETS turbo_interact_core EXPORT TurboInteractTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/turbo_interact DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "streamgraph_bridge.h" EXCLUDE
  PATTERN "m17e_fact.h" EXCLUDE
  PATTERN "m17e_flow_scxml.h" EXCLUDE)
install(EXPORT TurboInteractTargets FILE TurboInteractTargets.cmake
  NAMESPACE TurboInteract:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboInteract)
configure_package_config_file(cmake/TurboInteractConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TurboInteractConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboInteract)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TurboInteractConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TurboInteractConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TurboInteractConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboInteract)

include(CTest)
if(BUILD_TESTING)
  add_executable(ti_domain_test tests/test_domain.c)
  target_link_libraries(ti_domain_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_domain_test COMMAND ti_domain_test)

  add_executable(ti_bridge_test tests/test_bridge.c)
  target_link_libraries(ti_bridge_test PRIVATE TurboInteract::Core)
  if(TI_WITH_TURBOMEDIA_STREAMGRAPH)
    target_sources(ti_bridge_test PRIVATE tests/test_streamgraph_bridge.c)
    target_link_libraries(ti_bridge_test PRIVATE
      TurboInteract::StreamGraphBridge)
    target_compile_definitions(ti_bridge_test PRIVATE
      TI_STREAMGRAPH_BRIDGE_REAL=1)
    if(TI_TEST_M17_REAL_H264)
      find_path(TI_M17_OPENH264_INCLUDE_DIR NAMES wels/codec_api.h REQUIRED)
      find_library(TI_M17_OPENH264_LIBRARY NAMES openh264 REQUIRED)
      foreach(_m17_upstream_file IN ITEMS
          streamgraph/adapters/codec/src/turbo_streamgraph_h264.c
          codec/codec_registry.c codec/g711_codec.c codec/h264_codec.c)
        if(NOT EXISTS "${TI_TURBOMEDIA_SOURCE_DIR}/${_m17_upstream_file}")
          message(FATAL_ERROR "Actual upstream H264 source missing: ${_m17_upstream_file}")
        endif()
      endforeach()
      target_sources(ti_bridge_test PRIVATE
        tests/test_m17_h264_bridge.c
        ${TI_TURBOMEDIA_SOURCE_DIR}/streamgraph/adapters/codec/src/turbo_streamgraph_h264.c
        ${TI_TURBOMEDIA_SOURCE_DIR}/codec/codec_registry.c
        ${TI_TURBOMEDIA_SOURCE_DIR}/codec/g711_codec.c
        ${TI_TURBOMEDIA_SOURCE_DIR}/codec/h264_codec.c)
      target_include_directories(ti_bridge_test PRIVATE
        ${TI_TURBOMEDIA_SOURCE_DIR}/streamgraph/adapters/codec/include
        ${TI_TURBOMEDIA_SOURCE_DIR}/media/include
        ${TI_M17_OPENH264_INCLUDE_DIR})
      target_compile_definitions(ti_bridge_test PRIVATE
        TI_M17_H264_REAL_TEST=1 TURBO_MEDIA_HAS_H264=1)
      find_package(Threads REQUIRED)
      target_link_libraries(ti_bridge_test PRIVATE
        ${TI_M17_OPENH264_LIBRARY} Threads::Threads)
    endif()
  endif()
  add_test(NAME ti_bridge_test COMMAND ti_bridge_test)

  add_executable(ti_binding_test tests/test_binding.c)
  target_link_libraries(ti_binding_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_binding_test COMMAND ti_binding_test)

  add_executable(ti_application_test tests/test_application.c)
  target_link_libraries(ti_application_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_application_test COMMAND ti_application_test)
endif()
