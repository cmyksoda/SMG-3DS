cmake_minimum_required(VERSION 3.20)
project(SMG_3DS LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NINTENDO_3DS)
  # The game reads its UTF-16 text through wchar_t, two bytes on the Wii. Every file agrees on that size, and the
  # wide functions the game calls are MSL's own (msl_wide below), not newlib's four-byte ones.
  add_compile_options(-fshort-wchar)
  add_link_options(LINKER:--no-wchar-size-warning)
endif()

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
  source/compat/NativeBcsv.cpp
  source/compat/GameRandom.cpp
  source/compat/Os.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/RVL_SDK/gd/GDBase.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/MathUtil.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/MtxUtil.cpp"
  "${PETARI}/src/JSystem/JMath/random.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/JMath/JMATrigonometric.cpp"
  "${JMAP_SOURCE}"
  "${PETARI}/src/JSystem/JGadget/hashcode.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/JMath/JMath.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphAnimator/J3DAnimationKeys.cpp"
  "${PETARI}/src/JSystem/JUtility/JUTNameTab.cpp"
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
  "${PETARI}/libs/RVL_SDK/include" "${PETARI}/libs/JSystem/include" "${PETARI}/libs/nw4r/include"
  "${PETARI}/libs/RVLFaceLib/include")
# MSL's additions to the standard headers, which the game's sources reach through <algorithm>.
target_compile_options(petari_core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/smg/Msl.hpp>")
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

# JSystem's J3D: the model loader, joints, matrices and materials, as the game builds its models.
set(J3D "${PETARI}/src/JSystem")
add_library(petari_j3d STATIC
  "${J3D}/J3DGraphAnimator/J3DCluster.cpp" "${J3D}/J3DGraphAnimator/J3DJoint.cpp" "${J3D}/J3DGraphAnimator/J3DJointTree.cpp"
  "${J3D}/J3DGraphAnimator/J3DMaterialAnm.cpp" "${J3D}/J3DGraphAnimator/J3DMaterialAttach.cpp" "${J3D}/J3DGraphAnimator/J3DModel.cpp"
  "${J3D}/J3DGraphAnimator/J3DModelData.cpp" "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp"
  "${J3D}/J3DGraphAnimator/J3DShapeTable.cpp" "${J3D}/J3DGraphAnimator/J3DSkinDeform.cpp"
  "${J3D}/J3DGraphBase/J3DDrawBuffer.cpp" "${J3D}/J3DGraphBase/J3DGD.cpp" "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphBase/J3DMatBlock.cpp"
  "${J3D}/J3DGraphBase/J3DMaterial.cpp" "${J3D}/J3DGraphBase/J3DPacket.cpp" "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphBase/J3DShape.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphBase/J3DShapeDraw.cpp" "${J3D}/J3DGraphBase/J3DShapeMtx.cpp" "${J3D}/J3DGraphBase/J3DStruct.cpp"
  "${J3D}/J3DGraphBase/J3DSys.cpp" "${J3D}/J3DGraphBase/J3DTevs.cpp" "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphBase/J3DTransform.cpp"
  "${J3D}/J3DGraphBase/J3DVertex.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphLoader/J3DAnmLoader.cpp" "${J3D}/J3DGraphLoader/J3DJointFactory.cpp" "${J3D}/J3DGraphLoader/J3DMaterialFactory.cpp"
  "${J3D}/J3DGraphLoader/J3DMaterialFactory_v21.cpp" "${J3D}/J3DGraphLoader/J3DModelLoaderCalcSize.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/JSystem/J3DGraphLoader/J3DModelLoader.cpp" "${J3D}/J3DGraphLoader/J3DShapeFactory.cpp"
  source/compat/Gx.cpp)
target_link_libraries(petari_j3d PUBLIC petari_core petari_game_boundaries)
# Loud stops for game classes the whole-game library builds for real. As a library linked after the scene and
# J3D libraries, a program takes a stop only when nothing earlier defines the entry point.
add_library(petari_game_boundaries STATIC source/compat/GameBoundaries.cpp source/compat/GxBoundaries.cpp
  source/compat/GdBoundaries.cpp source/compat/GxFifoBoundary.cpp source/compat/Heap.cpp)
# The stops construct members the scene library defines; the two static libraries link each other.
target_link_libraries(petari_game_boundaries PUBLIC petari_scene)
if(NOT NINTENDO_3DS)
  set_source_files_properties(source/compat/GameBoundaries.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()
if(NOT NINTENDO_3DS)
  # UBSan's vptr check needs the type info of the deform and vertex color classes J3DModel calls through;
  # their key functions are not in the game, so neither is their type info. The 3DS build has no RTTI.
  set_source_files_properties("${J3D}/J3DGraphAnimator/J3DModel.cpp" "${J3D}/J3DGraphAnimator/J3DSkinDeform.cpp"
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${J3D}/J3DGraphBase/J3DTransform.cpp"
  "${J3D}/J3DGraphAnimator/J3DMtxBuffer.cpp" "${J3D}/J3DGraphBase/J3DShape.cpp" "${J3D}/J3DGraphLoader/J3DAnmLoader.cpp"
  "${J3D}/J3DGraphLoader/J3DModelLoader.cpp")

# The game's own animation core: blended BCK tracks as the joints' matrix calc.
add_library(petari_anim STATIC "${PETARI}/src/Game/Animation/XanimeCore.cpp")
target_link_libraries(petari_anim PUBLIC petari_j3d)

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
  "${PETARI}/src/Game/Map/CollisionCode.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/RailRider.cpp"
  "${PETARI}/src/Game/Map/BezierRail.cpp"
  "${PETARI}/src/Game/Map/RailPart.cpp"
  "${PETARI}/src/Game/Map/StageSwitch.cpp"
  "${PETARI}/src/Game/Map/SleepController.cpp"
  "${PETARI}/src/Game/Util/JMapIdInfo.cpp"
  source/compat/Light.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Map/LightDataHolder.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Map/LightZoneDataHolder.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Map/LightFunctionData.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/ObjUtilCsv.cpp"
  source/compat/Area.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/FollowIdCheck.cpp"
  "${PETARI}/src/Game/Util/JMapLinkInfo.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/AreaObj/AreaForm.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/AreaObj/AreaObj.cpp"
  "${PETARI}/src/Game/AreaObj/LightArea.cpp"
  "${PETARI}/src/Game/AreaObj/LightAreaHolder.cpp"
  source/compat/Actor.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/LiveActor.cpp"
  "${PETARI}/src/Game/LiveActor/LiveActorFlag.cpp"
  "${PETARI}/src/Game/LiveActor/HitSensor.cpp"
  "${PETARI}/src/Game/LiveActor/HitSensorKeeper.cpp"
  "${PETARI}/src/Game/LiveActor/HitSensorInfo.cpp"
  "${PETARI}/src/Game/LiveActor/Binder.cpp"
  "${PETARI}/src/Game/Map/HitInfo.cpp"
  "${PETARI}/src/Game/Map/CollisionDirector.cpp"
  "${PETARI}/src/Game/Map/CollisionCategorizedKeeper.cpp"
  "${PETARI}/src/Game/Map/CollisionParts.cpp"
  "${PETARI}/src/Game/LiveActor/LiveActorGroup.cpp"
  "${PETARI}/src/Game/LiveActor/AllLiveActorGroup.cpp"
  "${PETARI}/src/Game/LiveActor/SensorHitChecker.cpp"
  "${PETARI}/src/Game/LiveActor/ClippingDirector.cpp"
  "${PETARI}/src/Game/LiveActor/ClippingActorHolder.cpp"
  "${PETARI}/src/Game/LiveActor/ClippingActorInfo.cpp"
  "${PETARI}/src/Game/LiveActor/ClippingGroupHolder.cpp"
  "${PETARI}/src/Game/LiveActor/ClippingJudge.cpp"
  "${PETARI}/src/Game/LiveActor/ViewGroupCtrl.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/ObjUtilScene.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/LiveActorUtilCore.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/ActorSensorUtilCore.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/ActorMovementUtilCore.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/Util/MapUtilCore.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/ShadowRequests.cpp")
target_link_libraries(petari_scene PUBLIC petari_gravity petari_game_boundaries)
# Metrowerks stored a constant that does not fit, such as -1 in a table of u32, as its bit pattern; so does GCC
# once the narrowing is not an error.
foreach(library petari_core petari_gravity petari_j3d petari_anim petari_scene)
  target_compile_options(${library} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-narrowing>)
endforeach()
if(NOT NINTENDO_3DS)
  # UBSan's vptr check needs the type info of every class LiveActor calls through; its model and sound classes
  # are not brought up, so theirs does not exist yet. The 3DS build has no RTTI at all.
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/LiveActor.cpp" PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()
# The native scene owns the light tables it is handed, so it sees their (header-only) type.
target_include_directories(petari_scene PRIVATE include)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PETARI}/src/Game/NameObj/NameObjHolder.cpp" "${PETARI}/src/Game/Util/HashUtil.cpp"
  "${PETARI}/src/Game/NameObj/NameObjCategoryList.cpp" "${PETARI}/include/Game/NameObj/NameObjCategoryList.hpp"
  "${PETARI}/src/Game/Scene/SceneObjHolder.cpp" "${PETARI}/src/Game/Gravity/PlanetGravityManager.cpp"
  "${PETARI}/src/Game/Gravity/GravityInfo.cpp" "${PETARI}/include/Game/Gravity/GravityInfo.hpp"
  "${PETARI}/src/Game/Gravity/GravityCreator.cpp" "${PETARI}/include/Game/Gravity/GravityCreator.hpp"
  "${PETARI}/src/Game/Util/GravityUtil.cpp" "${PETARI}/src/Game/Util/JMapUtil.cpp"
  "${PETARI}/include/Game/Util/JMapUtil.hpp" "${PETARI}/src/Game/Util/StringUtil.cpp"
  "${PETARI}/src/Game/Util/ObjUtil.cpp" "${PETARI}/src/Game/LiveActor/LiveActor.cpp"
  "${PETARI}/libs/RVL_SDK/include/revolution/gx/GXVert.h" "${PETARI}/libs/JSystem/include/JSystem/J3DGraphBase/J3DFifo.hpp" "${PETARI}/src/Game/Util/SystemUtil.cpp"
  "${PETARI}/include/Game/NameObj/NameObjHolder.hpp" "${PETARI}/include/Game/Util/ObjUtil.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JSupport/JSUList.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JKernel/JKRHeap.hpp"
  "${PETARI}/src/Game/Map/KCollision.cpp" "${PETARI}/include/Game/Map/KCollision.hpp"
  "${PETARI}/src/Game/Map/CollisionCode.cpp" "${PETARI}/include/Game/Camera/CameraPolygonCodeUtil.hpp"
  "${PETARI}/src/Game/LiveActor/RailRider.cpp" "${PETARI}/include/Game/Map/StageSwitch.hpp"
  "${PETARI}/include/Game/Util/JMapIdInfo.hpp" "${PETARI}/src/Game/Map/LightDataHolder.cpp"
  "${PETARI}/src/Game/Map/LightZoneDataHolder.cpp" "${PETARI}/src/Game/Map/LightFunction.cpp"
  "${PETARI}/include/Game/Map/LightDataHolder.hpp" "${PETARI}/include/Game/Map/LightZoneDataHolder.hpp"
  "${PETARI}/src/Game/AreaObj/AreaForm.cpp" "${PETARI}/src/Game/AreaObj/AreaObj.cpp"
  "${PETARI}/include/Game/AreaObj/AreaForm.hpp" "${PETARI}/src/JSystem/J3DGraphAnimator/J3DAnimation.cpp"
  "${PETARI}/libs/RVL_SDK/include/revolution/os.h"
  "${PETARI}/libs/JSystem/include/JSystem/J3DGraphAnimator/J3DAnimation.hpp"
  "${PETARI}/src/Game/Util/LiveActorUtil.cpp" "${PETARI}/src/Game/Util/ActorSensorUtil.cpp"
  "${PETARI}/src/Game/Util/ActorMovementUtil.cpp" "${PETARI}/src/Game/Util/MapUtil.cpp"
  "${PETARI}/src/Game/LiveActor/ShadowController.cpp" "${PETARI}/src/RVL_SDK/gd/GDBase.c" "${PETARI}/src/Game/Util/ObjUtil.cpp"
  "${PETARI}/libs/nw4r/include/nw4r/math/arithmetic.h" "${PETARI}/libs/JSystem/include/JSystem/JAudio2/JASHeapCtrl.hpp"
  "${PETARI}/include/Game/Player/Mario.hpp" "${PETARI}/libs/nw4r/include/nw4r/ut/LinkList.h"
  "${PETARI}/libs/JSystem/include/JSystem/JKernel/JKRDvdRipper.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JAudio2/JASGadget.hpp"
  "${PETARI}/libs/JSystem/include/JSystem/JGadget/linklist.hpp"
  "${PETARI}/libs/RVL_SDK/include/revolution/mem/heapCommon.h" "${PETARI}/libs/JSystem/include/JSystem/JAudio2/JASSeqReader.hpp")

add_library(smg_platform STATIC
  source/core/Input.cpp source/core/Motion.cpp source/core/Settings.cpp source/core/Session.cpp
  source/core/Archive.cpp source/core/Bcsv.cpp source/core/Model.cpp source/core/Texture.cpp source/core/Combiner.cpp
  source/core/Stage.cpp source/core/StageGravity.cpp source/core/Mesh.cpp source/core/Etc1.cpp source/core/Kcl.cpp source/core/StageCollision.cpp source/core/StageLight.cpp source/core/Animation.cpp source/core/Actors.cpp source/core/J3DImage.cpp source/core/Dol.cpp source/core/GxCommands.cpp source/core/Bmg.cpp source/core/Kpad.cpp source/core/Layout.cpp source/core/GxDraw.cpp)
target_include_directories(smg_platform PUBLIC include)
target_link_libraries(smg_platform PUBLIC petari_scene petari_anim)
target_compile_options(smg_platform PRIVATE -Wall -Wextra -Werror)
# The native scene's pad owns its WPad the native way; the whole-game boot builds the game's own over the port's KPAD.
add_library(smg_native_pad STATIC source/compat/NativePad.cpp)
target_link_libraries(smg_native_pad PUBLIC smg_platform)
target_compile_options(smg_native_pad PRIVATE -Wall -Wextra -Werror)

if(NINTENDO_3DS)
  target_include_directories(petari_core PUBLIC "${DEVKITPRO}/libctru/include")
  target_compile_options(petari_core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/smg/NewlibCtype.hpp>")
  set(MODEL_SHADER "${CMAKE_CURRENT_BINARY_DIR}/ModelShader.hpp")
  add_custom_command(OUTPUT "${MODEL_SHADER}"
    COMMAND "${DEVKITPRO}/tools/bin/picasso" -o "${CMAKE_CURRENT_BINARY_DIR}/model.shbin"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.v.pica"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-binary.py"
      "${CMAKE_CURRENT_BINARY_DIR}/model.shbin" "${MODEL_SHADER}"
    DEPENDS shaders/model.v.pica tools/embed-binary.py VERBATIM)
  # What both renderers hand the PICA: tiled textures, combiner environments, pixel tests.
  add_library(smg_pica STATIC source/platform/Pica.cpp)
  target_include_directories(smg_pica PUBLIC "${DEVKITPRO}/libctru/include")
  target_link_libraries(smg_pica PUBLIC smg_platform)
  target_compile_options(smg_pica PRIVATE -Wall -Wextra -Werror)
  add_executable(SMG-3DS source/platform/Main.cpp source/platform/TouchUi.cpp source/platform/SceneRenderer.cpp "${MODEL_SHADER}")
  set_target_properties(SMG-3DS PROPERTIES SUFFIX ".elf")
  target_include_directories(SMG-3DS PRIVATE "${DEVKITPRO}/libctru/include" "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_directories(SMG-3DS PRIVATE "${DEVKITPRO}/libctru/lib")
  target_link_libraries(SMG-3DS PRIVATE smg_native_pad smg_pica smg_platform citro2d citro3d ctru m)
  target_compile_options(SMG-3DS PRIVATE -Wall -Wextra -Werror)
  target_link_options(SMG-3DS PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.map")
  add_custom_command(TARGET SMG-3DS POST_BUILD
    COMMAND "${DEVKITPRO}/tools/bin/smdhtool" --create "SMG-3DS" "Galaxy native port - development" "jaxi / Petari contributors"
      "${DEVKITPRO}/libctru/default_icon.png" "${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.smdh"
    COMMAND "${DEVKITPRO}/tools/bin/3dsxtool" "$<TARGET_FILE:SMG-3DS>"
      "${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.3dsx" "--smdh=${CMAKE_CURRENT_BINARY_DIR}/SMG-3DS.smdh"
    VERBATIM)
  # The rest of the game, JSystem and nw4r, for code the port brings up whole (MarioActor first). Programs link it
  # with --gc-sections, keeping only what they reach. Sources built above stay out; for a file cut into extracts
  # above, tools/prepare-petari.py writes the rest of it.
  # 3DS only for now: 94 of these files still need 64-bit host fixes (docs/PORTING.md, "Road to Mario").
  file(GLOB_RECURSE PETARI_GAME_CANDIDATES CONFIGURE_DEPENDS RELATIVE "${PETARI}/src"
    "${PETARI}/src/Game/*.cpp" "${PETARI}/src/JSystem/*.cpp" "${PETARI}/src/nw4r/*.cpp")
  set(PETARI_BUILT_ELSEWHERE)
  foreach(library petari_core petari_gravity petari_j3d petari_anim petari_scene)
    get_target_property(library_sources ${library} SOURCES)
    foreach(source ${library_sources})
      string(REPLACE "${PETARI}/src/" "" source "${source}")
      string(REPLACE "${CMAKE_CURRENT_BINARY_DIR}/petari/" "" source "${source}")
      list(APPEND PETARI_BUILT_ELSEWHERE "${source}")
    endforeach()
  endforeach()
  set(PETARI_GAME_SOURCES source/compat/Sdk.cpp source/compat/OsThreads.cpp source/compat/OsMemory.cpp source/compat/GameHeap.cpp source/compat/Dvd.cpp source/compat/Video.cpp source/compat/OsReset.cpp source/compat/SystemConfig.cpp source/compat/Wpad.cpp source/compat/NativeResources.cpp source/compat/GxFifo.cpp source/compat/Nwc24.cpp source/compat/WiiAudio.cpp source/compat/Nw4rDb.cpp source/compat/DolTables.cpp source/compat/Kpad.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/RailRiderCamera.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/petari/Game/LiveActor/LiveActorSound.cpp")
  foreach(source ${PETARI_GAME_CANDIDATES})
    if(source IN_LIST PETARI_BUILT_ELSEWHERE)
      continue()
    elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/petari/${source}")
      list(APPEND PETARI_GAME_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/petari/${source}")
    else()
      list(APPEND PETARI_GAME_SOURCES "${PETARI}/src/${source}")
    endif()
  endforeach()
  # SDK libraries that are software through and through, built from their own C sources: the stand-in for the
  # GameCube's ARAM in MEM2 (RVL "Dummy ARAM"), texture palettes (TPL) and MEM's allocator dispatch, through which
  # nw4r reaches the game's own allocators.
  set(PETARI_SDK_SOURCES "${PETARI}/src/RVL_SDK/aralt/aralt.c" "${PETARI}/src/RVL_SDK/tpl/TPL.c"
    "${CMAKE_CURRENT_BINARY_DIR}/petari/RVL_SDK/mem/mem_allocator.c")
  list(APPEND PETARI_GAME_SOURCES ${PETARI_SDK_SOURCES})
  set_source_files_properties(${PETARI_SDK_SOURCES} PROPERTIES LANGUAGE CXX)
  # The SDK's own GX (C sources that include C++ headers, so built as C++): every GX call the game makes writes the
  # Wii's registers and FIFO as on the Wii, for the port to read (docs/PORTING.md, "Running the game's own boot").
  foreach(gx gx/GXAttr gx/GXBump gx/GXDisplayList gx/GXFifo gx/GXFrameBuf gx/GXGeometry gx/GXInit gx/GXLight gx/GXMisc gx/GXPerf
      gx/GXPixel gx/GXTev gx/GXTexture gx/GXTransform gd/GDGeometry gd/GDIndirect gd/GDLight gd/GDPixel gd/GDTev gd/GDTexture gd/GDTransform)
    if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/petari/RVL_SDK/${gx}.c")
      list(APPEND PETARI_GAME_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/petari/RVL_SDK/${gx}.c")
    else()
      list(APPEND PETARI_GAME_SOURCES "${PETARI}/src/RVL_SDK/${gx}.c")
    endif()
  endforeach()
  set_source_files_properties(${PETARI_GAME_SOURCES} PROPERTIES LANGUAGE CXX)
  # The SDK's sources are C: built as C++, they keep C's implicit conversions (void* to a typed pointer, int to enum).
  foreach(source ${PETARI_GAME_SOURCES})
    if(source MATCHES "\\.c$")
      set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -fpermissive)
    endif()
  endforeach()
  # MSL's own wide-string functions (swprintf, vswprintf, wcslen, wcsncpy and what they use), built as C.
  add_library(msl_wide STATIC source/compat/MslWide.c "${CMAKE_CURRENT_BINARY_DIR}/petari/MSL_C/wstring.c"
    "${CMAKE_CURRENT_BINARY_DIR}/petari/MSL_C/wmem.c" "${CMAKE_CURRENT_BINARY_DIR}/petari/MSL_C/mbstring.c"
    "${CMAKE_CURRENT_BINARY_DIR}/petari/MSL_C/wprintf.c")
  target_include_directories(msl_wide PRIVATE compat)
  target_compile_options(msl_wide PRIVATE -w)
  add_library(petari_game STATIC ${PETARI_GAME_SOURCES})
  target_link_libraries(petari_game PUBLIC petari_scene petari_anim)
  target_include_directories(petari_game PRIVATE include)
  target_compile_options(petari_game PRIVATE -Wno-narrowing -ffunction-sections -fdata-sections)

  add_executable(mario_probe source/probe/MarioProbe.cpp)
  set_target_properties(mario_probe PROPERTIES SUFFIX ".elf")
  target_include_directories(mario_probe PRIVATE "${DEVKITPRO}/libctru/include")
  target_link_directories(mario_probe PRIVATE "${DEVKITPRO}/libctru/lib")
  # The game and the port's libraries call each other: GNU ld rescans the group until nothing more resolves. The
  # boundary stops come after it, for what the game does not define.
  target_link_libraries(mario_probe PRIVATE -Wl,--start-group smg_platform petari_game petari_scene petari_anim petari_j3d
    petari_gravity petari_core msl_wide -Wl,--end-group petari_game_boundaries citro2d citro3d ctru m)
  target_link_options(mario_probe PRIVATE -Wl,--gc-sections "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/mario_probe.map")
  add_custom_command(TARGET mario_probe POST_BUILD
    COMMAND "${DEVKITPRO}/tools/bin/3dsxtool" "$<TARGET_FILE:mario_probe>" "${CMAKE_CURRENT_BINARY_DIR}/mario_probe.3dsx" VERBATIM)

  option(SMG_BUILD_VALIDATION "Build a separate on-device math/input test app" OFF)
  if(SMG_BUILD_VALIDATION)
    add_executable(native_validation tests/NativeTests.cpp tests/MathTests.cpp tests/PadTests.cpp tests/BcsvTests.cpp
      tests/ModelTests.cpp tests/CombinerTests.cpp tests/GravityTests.cpp tests/Etc1Tests.cpp tests/GpuTests.cpp
      tests/NameObjTests.cpp tests/SceneTests.cpp tests/CollisionTests.cpp tests/AnimationTests.cpp tests/ActorTests.cpp tests/J3DTransformTests.cpp tests/J3DSkeletonTests.cpp
      tests/GxCommandTests.cpp tests/BmgTests.cpp tests/ColorTests.cpp tests/LayoutTests.cpp tests/WideTests.cpp tests/GxDrawTests.cpp)
    set_target_properties(native_validation PROPERTIES SUFFIX ".elf")
    target_compile_definitions(native_validation PRIVATE SMG_TEST_RUNNER)
    target_link_directories(native_validation PRIVATE "${DEVKITPRO}/libctru/lib")
    target_link_libraries(native_validation PRIVATE smg_native_pad smg_platform petari_gravity petari_scene petari_j3d msl_wide citro2d citro3d ctru m)
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
  add_executable(resource_audit tools/AuditResources.cpp tools/AuditLayouts.cpp)
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
  target_link_libraries(pad_tests PRIVATE smg_native_pad)
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
  add_executable(animation_tests tests/AnimationTests.cpp)
  target_link_libraries(animation_tests PRIVATE smg_platform)
  target_compile_options(animation_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME j3d_animation COMMAND animation_tests)
  add_executable(actor_tests tests/ActorTests.cpp)
  target_link_libraries(actor_tests PRIVATE smg_platform)
  target_compile_options(actor_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME original_actor COMMAND actor_tests)
  add_executable(j3d_transform_tests tests/J3DTransformTests.cpp tests/J3DSkeletonTests.cpp)
  target_link_libraries(j3d_transform_tests PRIVATE smg_platform)
  target_compile_options(j3d_transform_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME j3d_transforms COMMAND j3d_transform_tests)
  add_executable(gx_command_tests tests/GxCommandTests.cpp)
  target_link_libraries(gx_command_tests PRIVATE smg_platform)
  target_compile_options(gx_command_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME gx_commands COMMAND gx_command_tests)
  add_executable(bmg_tests tests/BmgTests.cpp)
  target_link_libraries(bmg_tests PRIVATE smg_platform)
  target_compile_options(bmg_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME bmg_text COMMAND bmg_tests)
  add_executable(gx_draw_tests tests/GxDrawTests.cpp)
  target_link_libraries(gx_draw_tests PRIVATE smg_platform)
  target_compile_options(gx_draw_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME gx_draw COMMAND gx_draw_tests)
  add_executable(layout_tests tests/LayoutTests.cpp)
  target_link_libraries(layout_tests PRIVATE smg_platform)
  target_compile_options(layout_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME layout_family COMMAND layout_tests)
  add_executable(color_tests tests/ColorTests.cpp)
  target_link_libraries(color_tests PRIVATE smg_platform)
  target_compile_options(color_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME colour_packing COMMAND color_tests)
  add_executable(combiner_tests tests/CombinerTests.cpp)
  target_link_libraries(combiner_tests PRIVATE smg_platform)
  target_compile_options(combiner_tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME tev_combiner COMMAND combiner_tests)
endif()
