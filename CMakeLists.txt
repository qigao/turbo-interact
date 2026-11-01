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
if(TI_WITH_TURBOMEDIA_STREAMGRAPH)
  find_package(TurboMediaStreamGraph CONFIG REQUIRED)
  if(NOT TARGET TurboMedia::StreamGraph)
    message(FATAL_ERROR "Actual TurboMedia::StreamGraph target unavailable")
  endif()
  add_library(turbo_interact_streamgraph_bridge STATIC
    src/streamgraph_bridge.c)
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
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbo_interact)
endif()

install(TARGETS turbo_interact_core EXPORT TurboInteractTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/turbo_interact DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "streamgraph_bridge.h" EXCLUDE)
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
  endif()
  add_test(NAME ti_bridge_test COMMAND ti_bridge_test)

  add_executable(ti_binding_test tests/test_binding.c)
  target_link_libraries(ti_binding_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_binding_test COMMAND ti_binding_test)

  add_executable(ti_application_test tests/test_application.c)
  target_link_libraries(ti_application_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_application_test COMMAND ti_application_test)
endif()
