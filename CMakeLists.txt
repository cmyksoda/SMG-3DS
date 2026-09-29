cmake_minimum_required(VERSION 3.20)
project(SMG_3DS LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PETARI "${CMAKE_CURRENT_SOURCE_DIR}/upstream/Petari")
if(NOT EXISTS "${PETARI}/src/Game/LiveActor/Spine.cpp")
  message(FATAL_ERROR "Run git submodule update --init before configuring")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(JMAP_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/JMapInfo.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-petari.py"
  "${PETARI}" "${CMAKE_CURRENT_BINARY_DIR}/petari" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PETARI}/src/Game/Util/JMapInfo.cpp" "${PETARI}/include/Game/Util/JMapInfo.hpp"
  "${PETARI}/include/Game/Util/StringUtil.hpp" "${PETARI}/libs/JSystem/include/JSystem/JGadget/hashcode.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-petari.py")
file(GLOB_RECURSE PETARI_MATH_HEADERS CONFIGURE_DEPENDS
  "${PETARI}/libs/JSystem/include/JSystem/JMath/*.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JGeometry/*.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PETARI_MATH_HEADERS}
  "${PETARI}/include/math_types.hpp" "${PETARI}/include/Game/System/WPadButton.hpp"
  "${PETARI}/libs/RVL_SDK/include/revolution/hbm/HBMBase.h"
  "${PETARI}/libs/RVL_SDK/include/revolution/wpad.h"
  "${PETARI}/libs/RVL_SDK/include/revolution/os/OSTime.h"
  "${PETARI}/src/JSystem/JMath/JMATrigonometric.cpp")
add_library(petari_core STATIC
  source/compat/StringUtil.cpp
  source/compat/Math.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/JMath/JMATrigonometric.cpp"
  "${JMAP_SOURCE}"
  "${PETARI}/src/JSystem/JGadget/hashcode.cpp"
  "${PETARI}/src/Game/LiveActor/Nerve.cpp"
  "${PETARI}/src/Game/LiveActor/Spine.cpp"
  "${PETARI}/src/Game/LiveActor/ActorStateBase.cpp"
  "${PETARI}/src/Game/LiveActor/ActorStateKeeper.cpp"
  "${PETARI}/src/Game/System/NerveExecutor.cpp"
  "${PETARI}/src/Game/System/WPadButton.cpp"
  "${PETARI}/src/Game/System/WPadStick.cpp"
  "${PETARI}/src/Game/Util/TriggerChecker.cpp")
target_include_directories(petari_core PUBLIC compat "${CMAKE_CURRENT_BINARY_DIR}/petari/include" "${PETARI}/include"
  "${PETARI}/libs/RVL_SDK/include" "${PETARI}/libs/JSystem/include")
# Original resource interfaces reinterpret stored numeric fields through several types.
target_compile_options(petari_core PUBLIC -fno-strict-aliasing)

add_library(smg_platform STATIC source/core/Input.cpp source/core/Session.cpp source/core/Archive.cpp source/core/Bcsv.cpp source/compat/NativePad.cpp)
target_include_directories(smg_platform PUBLIC include)
target_link_libraries(smg_platform PUBLIC petari_core)
target_compile_options(smg_platform PRIVATE -Wall -Wextra -Werror)

if(NINTENDO_3DS)
  target_include_directories(petari_core PUBLIC "${DEVKITPRO}/libctru/include")
  add_executable(SMG-3DS source/platform/Main.cpp source/platform/TouchUi.cpp)
  set_target_properties(SMG-3DS PROPERTIES SUFFIX ".elf")
  target_include_directories(SMG-3DS PRIVATE "${DEVKITPRO}/libctru/include")
  target_link_directories(SMG-3DS PRIVATE "${DEVKITPRO}/libctru/lib")
  target_link_libraries(SMG-3DS PRIVATE smg_platform citro2d citro3d ctru m)
  target_compile_options(SMG-3DS PRIVATE -Wall -Wextra -Werror)
  target_link_options(SMG-3DS PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.map")
  add_custom_command(TARGET SMG-3DS POST_BUILD
    COMMAND "${DEVKITPRO}/tools/bin/smdhtool" --create "SMG-3DS" "Galaxy native port - development" "jaxi / Petari contributors"
      "${DEVKITPRO}/libctru/default_icon.png" "${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.smdh"
    COMMAND "${DEVKITPRO}/tools/bin/3dsxtool" "$<TARGET_FILE:SMG-3DS>"
      "${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.3dsx" "--smdh=${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.smdh"
    VERBATIM)
  option(SMG_BUILD_VALIDATION "Build a separate on-device math/input test app" OFF)
  if(SMG_BUILD_VALIDATION)
    add_executable(native_validation tests/NativeTests.cpp tests/MathTests.cpp tests/PadTests.cpp tests/BcsvTests.cpp)
    set_target_properties(native_validation PROPERTIES SUFFIX ".elf")
    target_compile_definitions(native_validation PRIVATE SMG_TEST_RUNNER)
    target_link_directories(native_validation PRIVATE "${DEVKITPRO}/libctru/lib")
    target_link_libraries(native_validation PRIVATE smg_platform ctru m)
    target_compile_options(native_validation PRIVATE -Wall -Wextra -Werror)
    add_custom_command(TARGET native_validation POST_BUILD
      COMMAND "${DEVKITPRO}/tools/bin/3dsxtool" "$<TARGET_FILE:native_validation>"
        "${CMAKE_CURRENT_BINARY_DIR}/native_validation.3dsx" VERBATIM)
  endif()
else()
  enable_testing()
  add_executable(input_tests tests/InputTests.cpp)
  target_link_libraries(input_tests PRIVATE smg_platform)
  target_compile_options(input_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME input_and_petari_core COMMAND input_tests)
  add_executable(archive_tests tests/ArchiveTests.cpp)
  target_link_libraries(archive_tests PRIVATE smg_platform)
  target_compile_options(archive_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME archive_formats COMMAND archive_tests)
  add_executable(archive_tool tools/ArchiveTool.cpp)
  target_link_libraries(archive_tool PRIVATE smg_platform)
  add_executable(resource_audit tools/AuditResources.cpp)
  target_link_libraries(resource_audit PRIVATE smg_platform)
  target_compile_options(resource_audit PRIVATE -Wall -Wextra -Werror)
  add_executable(bcsv_tests tests/BcsvTests.cpp)
  target_link_libraries(bcsv_tests PRIVATE smg_platform)
  add_test(NAME original_jmap_bcsv COMMAND bcsv_tests)
  add_executable(math_tests tests/MathTests.cpp)
  target_link_libraries(math_tests PRIVATE smg_platform)
  target_compile_options(math_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME portable_math COMMAND math_tests)
  add_executable(pad_tests tests/PadTests.cpp)
  target_link_libraries(pad_tests PRIVATE smg_platform)
  target_compile_options(pad_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME original_wpad_input COMMAND pad_tests)
endif()
