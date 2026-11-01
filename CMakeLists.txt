cmake_minimum_required(VERSION 3.21)
project(TurboInteract VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(turbo_interact_core STATIC
  src/domain.c
  src/application.c
  src/binding.c
  src/action_owner.c
  src/sandbox_bridge.c
  src/sandbox_ipc.c
  src/android_shell_backend.c)
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

# One-shot trusted worker is a PRODUCT native executable (not a consumer).
# AGP builds executable targets but does NOT package them into the AAR by
# default: packaging/installed nativeLibraryDir execution is a separate gate.
if(UNIX OR ANDROID)
  add_executable(turbo_interact_sandbox_worker src/sandbox_worker.c)
  set_target_properties(turbo_interact_sandbox_worker PROPERTIES
    OUTPUT_NAME libturbo_interact_sandbox_worker.so
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(turbo_interact_sandbox_worker PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# Android JNI is part of the production Android AAR, linked to the same
# TurboInteract::Core C11 owner; never a separate execution engine.
if(ANDROID)
  set_target_properties(turbo_interact_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(turbo_interact_android_jni SHARED
    android/agent/src/main/cpp/native_action_owner_jni.c)
  target_link_libraries(turbo_interact_android_jni PRIVATE TurboInteract::Core)
  set_target_properties(turbo_interact_android_jni PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(turbo_interact_android_jni PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
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

  add_executable(ti_action_owner_test tests/test_action_owner.c)
  target_link_libraries(ti_action_owner_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_action_owner_test COMMAND ti_action_owner_test)

  add_executable(ti_sandbox_bridge_test tests/test_sandbox_bridge.c)
  target_link_libraries(ti_sandbox_bridge_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_sandbox_bridge_test COMMAND ti_sandbox_bridge_test)

  add_executable(ti_sandbox_ipc_test tests/test_sandbox_ipc.c)
  target_link_libraries(ti_sandbox_ipc_test PRIVATE TurboInteract::Core)
  if(TARGET turbo_interact_sandbox_worker)
    target_compile_definitions(ti_sandbox_ipc_test PRIVATE
      TI_TRUSTED_WORKER_PATH="$<TARGET_FILE:turbo_interact_sandbox_worker>")
    add_dependencies(ti_sandbox_ipc_test turbo_interact_sandbox_worker)
  endif()
  add_test(NAME ti_sandbox_ipc_test COMMAND ti_sandbox_ipc_test)

  add_executable(ti_android_shell_backend_test tests/test_android_shell_backend.c)
  target_link_libraries(ti_android_shell_backend_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_android_shell_backend_test COMMAND ti_android_shell_backend_test)

  add_executable(ti_application_test tests/test_application.c)
  target_link_libraries(ti_application_test PRIVATE TurboInteract::Core)
  add_test(NAME ti_application_test COMMAND ti_application_test)
endif()
