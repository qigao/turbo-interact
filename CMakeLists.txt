cmake_minimum_required(VERSION 3.21)
project(TurboInteract VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(turbo_interact_core STATIC
  src/domain.c
  src/application.c
  src/binding.c)
add_library(TurboInteract::Core ALIAS turbo_interact_core)
set_target_properties(turbo_interact_core PROPERTIES
  EXPORT_NAME Core
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO)
target_include_directories(turbo_interact_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(MSVC)
  target_compile_options(turbo_interact_core PRIVATE /W4 /WX)
else()
  target_compile_options(turbo_interact_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS turbo_interact_core EXPORT TurboInteractTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/turbo_interact DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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

  add_executable(ti_binding_test tests/test_binding.c)
  target_link_libraries(ti_binding_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_binding_test COMMAND ti_binding_test)

  add_executable(ti_application_test tests/test_application.c)
  target_link_libraries(ti_application_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_application_test COMMAND ti_application_test)
endif()
