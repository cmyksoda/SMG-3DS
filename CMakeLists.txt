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
  source/compat/GameRandom.cpp
  source/compat/Heap.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/MathUtil.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/MtxUtil.cpp"
  "${PETARI}/src/JSystem/JMath/random.cpp"
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
target_include_directories(petari_core PUBLIC compat)
# Upstream headers are compiled as they are; their warnings are not ours to fail on.
target_include_directories(petari_core SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/petari/include" "${PETARI}/include"
  "${PETARI}/libs/RVL_SDK/include" "${PETARI}/libs/JSystem/include")
# Original resource interfaces reinterpret stored numeric fields through several types.
target_compile_options(petari_core PUBLIC -fno-strict-aliasing)
# Plain char is unsigned on the Wii and the 3DS; hashes of Shift-JIS names depend on it, so hosts match.
target_compile_options(petari_core PUBLIC -funsigned-char)

set(GRAVITY_SHAPES PlanetGravity PointGravity ParallelGravity CubeGravity DiskGravity DiskTorusGravity
  ConeGravity SegmentGravity WireGravity)
set(GRAVITY_SOURCES)
foreach(shape IN LISTS GRAVITY_SHAPES)
  list(APPEND GRAVITY_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Gravity/${shape}.cpp")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PETARI}/src/Game/Gravity/${shape}.cpp")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PETARI}/src/Game/Util/MathUtil.cpp"
  "${PETARI}/src/Game/Util/MtxUtil.cpp"
  "${PETARI}/include/Game/Util/MathUtil.hpp" "${PETARI}/libs/JSystem/include/JSystem/JGeometry/TMatrix.hpp")
add_library(petari_gravity STATIC ${GRAVITY_SOURCES})
target_link_libraries(petari_gravity PUBLIC petari_core)

add_library(petari_scene STATIC
  source/compat/Scene.cpp
  "${PETARI}/src/Game/NameObj/NameObj.cpp"
  "${PETARI}/src/Game/NameObj/NameObjRegister.cpp"
  "${PETARI}/src/Game/NameObj/NameObjGroup.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/NameObj/NameObjCategoryList.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/NameObj/NameObjHolder.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/HashUtil.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/ObjUtilNameObj.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/SystemUtilSceneNameObj.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Scene/SceneObjHolderMethods.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Gravity/PlanetGravityManager.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Gravity/GravityInfo.cpp"
  source/compat/GlobalGravity.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Gravity/GravityCreator.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/GravityUtil.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/JMapUtil.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/StringUtilCompare.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Map/KCollision.cpp"
  "${PETARI}/src/Game/Map/KCollisionPlus.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Map/CollisionCode.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/RailRider.cpp"
  "${PETARI}/src/Game/Map/BezierRail.cpp"
  "${PETARI}/src/Game/Map/RailPart.cpp")
target_link_libraries(petari_scene PUBLIC petari_gravity)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PETARI}/src/Game/NameObj/NameObjHolder.cpp" "${PETARI}/src/Game/Util/HashUtil.cpp"
  "${PETARI}/src/Game/NameObj/NameObjCategoryList.cpp" "${PETARI}/include/Game/NameObj/NameObjCategoryList.hpp"
  "${PETARI}/src/Game/Scene/SceneObjHolder.cpp" "${PETARI}/src/Game/Gravity/PlanetGravityManager.cpp"
  "${PETARI}/src/Game/Gravity/GravityInfo.cpp" "${PETARI}/include/Game/Gravity/GravityInfo.hpp"
  "${PETARI}/src/Game/Gravity/GravityCreator.cpp" "${PETARI}/include/Game/Gravity/GravityCreator.hpp"
  "${PETARI}/src/Game/Util/GravityUtil.cpp" "${PETARI}/src/Game/Util/JMapUtil.cpp"
  "${PETARI}/include/Game/Util/JMapUtil.hpp" "${PETARI}/src/Game/Util/StringUtil.cpp"
  "${PETARI}/src/Game/Util/ObjUtil.cpp" "${PETARI}/src/Game/Util/SystemUtil.cpp"
  "${PETARI}/include/Game/NameObj/NameObjHolder.hpp" "${PETARI}/include/Game/Util/ObjUtil.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JSupport/JSUList.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JKernel/JKRHeap.hpp"
  "${PETARI}/src/Game/Map/KCollision.cpp" "${PETARI}/include/Game/Map/KCollision.hpp"
  "${PETARI}/src/Game/Map/CollisionCode.cpp" "${PETARI}/include/Game/Camera/CameraPolygonCodeUtil.hpp"
  "${PETARI}/src/Game/LiveActor/RailRider.cpp")

add_library(smg_platform STATIC
  source/core/Input.cpp source/core/Motion.cpp source/core/Settings.cpp source/core/Session.cpp
  source/core/Archive.cpp source/core/Bcsv.cpp source/core/Model.cpp source/core/Texture.cpp source/core/Combiner.cpp
  source/core/Stage.cpp source/core/StageGravity.cpp source/core/Mesh.cpp source/core/Etc1.cpp source/core/Kcl.cpp source/core/StageCollision.cpp
  source/compat/NativePad.cpp)
target_include_directories(smg_platform PUBLIC include)
target_link_libraries(smg_platform PUBLIC petari_scene)
target_compile_options(smg_platform PRIVATE -Wall -Wextra -Werror)

if(NINTENDO_3DS)
  target_include_directories(petari_core PUBLIC "${DEVKITPRO}/libctru/include")
  target_compile_options(petari_core PUBLIC -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/smg/NewlibCtype.hpp")
  set(MODEL_SHADER "${CMAKE_CURRENT_BINARY_DIR}/ModelShader.hpp")
  add_custom_command(OUTPUT "${MODEL_SHADER}"
    COMMAND "${DEVKITPRO}/tools/bin/picasso" -o "${CMAKE_CURRENT_BINARY_DIR}/model.shbin"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.v.pica"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-binary.py"
      "${CMAKE_CURRENT_BINARY_DIR}/model.shbin" "${MODEL_SHADER}"
    DEPENDS shaders/model.v.pica tools/embed-binary.py VERBATIM)
  add_executable(SMG-3DS source/platform/Main.cpp source/platform/TouchUi.cpp source/platform/SceneRenderer.cpp "${MODEL_SHADER}")
  set_target_properties(SMG-3DS PROPERTIES SUFFIX ".elf")
  target_include_directories(SMG-3DS PRIVATE "${DEVKITPRO}/libctru/include" "${CMAKE_CURRENT_BINARY_DIR}")
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
    add_executable(native_validation tests/NativeTests.cpp tests/MathTests.cpp tests/PadTests.cpp tests/BcsvTests.cpp
      tests/ModelTests.cpp tests/CombinerTests.cpp tests/GravityTests.cpp tests/Etc1Tests.cpp tests/GpuTests.cpp
      tests/NameObjTests.cpp tests/SceneTests.cpp tests/CollisionTests.cpp)
    set_target_properties(native_validation PROPERTIES SUFFIX ".elf")
    target_compile_definitions(native_validation PRIVATE SMG_TEST_RUNNER)
    target_link_directories(native_validation PRIVATE "${DEVKITPRO}/libctru/lib")
    target_link_libraries(native_validation PRIVATE smg_platform petari_gravity petari_scene citro2d citro3d ctru m)
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
  add_executable(bcsv_tool tools/BcsvTool.cpp)
  target_link_libraries(bcsv_tool PRIVATE smg_platform)
  add_executable(model_tool tools/ModelTool.cpp)
  target_link_libraries(model_tool PRIVATE smg_platform)
  add_executable(stage_tool tools/StageTool.cpp)
  target_link_libraries(stage_tool PRIVATE smg_platform)
  add_executable(gravity_tool tools/GravityTool.cpp)
  target_link_libraries(gravity_tool PRIVATE smg_platform)
  target_compile_options(gravity_tool PRIVATE -Wall -Wextra -Werror)
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
  add_executable(model_tests tests/ModelTests.cpp)
  target_link_libraries(model_tests PRIVATE smg_platform)
  target_compile_options(model_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME j3d_models COMMAND model_tests)
  add_executable(etc1_tests tests/Etc1Tests.cpp)
  target_link_libraries(etc1_tests PRIVATE smg_platform)
  target_compile_options(etc1_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME etc1_textures COMMAND etc1_tests)
  add_executable(gravity_tests tests/GravityTests.cpp)
  target_link_libraries(gravity_tests PRIVATE petari_gravity)
  target_compile_options(gravity_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME original_gravity COMMAND gravity_tests)
  add_executable(nameobj_tests tests/NameObjTests.cpp)
  target_link_libraries(nameobj_tests PRIVATE petari_scene)
  target_compile_options(nameobj_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME original_nameobj COMMAND nameobj_tests)
  add_executable(scene_tests tests/SceneTests.cpp)
  target_link_libraries(scene_tests PRIVATE smg_platform)
  target_compile_options(scene_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME original_scene_gravity COMMAND scene_tests)
  add_executable(collision_tests tests/CollisionTests.cpp)
  target_link_libraries(collision_tests PRIVATE smg_platform)
  target_compile_options(collision_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME original_collision COMMAND collision_tests)
  add_executable(combiner_tests tests/CombinerTests.cpp)
  target_link_libraries(combiner_tests PRIVATE smg_platform)
  target_compile_options(combiner_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME tev_combiner COMMAND combiner_tests)
endif()
