@echo off
setlocal enableextensions enabledelayedexpansion

set NAME=game
set CONFIG=D

set CPP_FLAGS=/std:c++20 /W4 /wd4127 /WX /GR- /EHsc /nologo /MP /Gm- /Zc:inline^
 /fp:except- /fp:precise^
 /D"_CRT_SECURE_NO_WARNINGS"^
 /I"external/d3d12"^
 /I"external/imgui"^
 /I"external/directxmath"^
 /I"external"

if %CONFIG%==D set CPP_FLAGS=%CPP_FLAGS% /GS /Zi /Od /D"_DEBUG" /MTd /RTCs
if %CONFIG%==R set CPP_FLAGS=%CPP_FLAGS% /O2 /Gy /MT /D"NDEBUG" /Oi /Ot /GS-

set LIB_FLAGS=/NOLOGO
if %CONFIG%==D set LIB_FLAGS=%LIB_FLAGS%
if %CONFIG%==R set LIB_FLAGS=%LIB_FLAGS%

set LINK_FLAGS=/INCREMENTAL:NO /OUT:%NAME%.exe
if %CONFIG%==D set LINK_FLAGS=%LINK_FLAGS% /DEBUG:FULL
if %CONFIG%==R set LINK_FLAGS=%LINK_FLAGS%

set DXC="d3d12/dxc.exe"
set HLSL_OUT_DIR="assets"
set HLSL_SM=6_6
set HLSL_FLAGS=/WX /Ges /HV 2021 /nologo
if %CONFIG%==D set HLSL_FLAGS=%HLSL_FLAGS% /Od /Zi /Qembed_debug
if %CONFIG%==R set HLSL_FLAGS=%HLSL_FLAGS% /O3

set SRC_IMGUI_ROOT=external/imgui
set SRC_IMGUI=%SRC_IMGUI_ROOT%/imgui.cpp^
 %SRC_IMGUI_ROOT%/imgui_demo.cpp^
 %SRC_IMGUI_ROOT%/imgui_draw.cpp^
 %SRC_IMGUI_ROOT%/imgui_tables.cpp^
 %SRC_IMGUI_ROOT%/imgui_widgets.cpp^
 %SRC_IMGUI_ROOT%/imgui_impl_win32.cpp^
 %SRC_IMGUI_ROOT%/imgui_impl_dx12.cpp

set SRC_JOLT_ROOT=external/Jolt
set SRC_JOLT=%SRC_JOLT_ROOT%/AABBTree/AABBTreeBuilder.cpp^
 %SRC_JOLT_ROOT%/Core/Color.cpp^
 %SRC_JOLT_ROOT%/Core/Factory.cpp^
 %SRC_JOLT_ROOT%/Core/IssueReporting.cpp^
 %SRC_JOLT_ROOT%/Core/JobSystemSingleThreaded.cpp^
 %SRC_JOLT_ROOT%/Core/JobSystemThreadPool.cpp^
 %SRC_JOLT_ROOT%/Core/JobSystemWithBarrier.cpp^
 %SRC_JOLT_ROOT%/Core/LinearCurve.cpp^
 %SRC_JOLT_ROOT%/Core/Memory.cpp^
 %SRC_JOLT_ROOT%/Core/Profiler.cpp^
 %SRC_JOLT_ROOT%/Core/RTTI.cpp^
 %SRC_JOLT_ROOT%/Core/Semaphore.cpp^
 %SRC_JOLT_ROOT%/Core/StringTools.cpp^
 %SRC_JOLT_ROOT%/Core/TickCounter.cpp^
 %SRC_JOLT_ROOT%/Geometry/ConvexHullBuilder.cpp^
 %SRC_JOLT_ROOT%/Geometry/ConvexHullBuilder2D.cpp^
 %SRC_JOLT_ROOT%/Geometry/Indexify.cpp^
 %SRC_JOLT_ROOT%/Geometry/OrientedBox.cpp^
 %SRC_JOLT_ROOT%/Math/Vec3.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStream.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStreamBinaryIn.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStreamBinaryOut.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStreamIn.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStreamOut.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStreamTextIn.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/ObjectStreamTextOut.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/SerializableObject.cpp^
 %SRC_JOLT_ROOT%/ObjectStream/TypeDeclarations.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/Body.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/BodyAccess.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/BodyCreationSettings.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/BodyInterface.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/BodyManager.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/MassProperties.cpp^
 %SRC_JOLT_ROOT%/Physics/Body/MotionProperties.cpp^
 %SRC_JOLT_ROOT%/Physics/Character/Character.cpp^
 %SRC_JOLT_ROOT%/Physics/Character/CharacterBase.cpp^
 %SRC_JOLT_ROOT%/Physics/Character/CharacterVirtual.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/BroadPhase/BroadPhase.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/BroadPhase/BroadPhaseBruteForce.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/BroadPhase/BroadPhaseQuadTree.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/BroadPhase/QuadTree.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/CastConvexVsTriangles.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/CastSphereVsTriangles.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/CollideConvexVsTriangles.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/CollideSphereVsTriangles.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/CollisionDispatch.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/CollisionGroup.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/EstimateCollisionResponse.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/GroupFilter.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/GroupFilterTable.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/ManifoldBetweenTwoFaces.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/NarrowPhaseQuery.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/NarrowPhaseStats.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/PhysicsMaterial.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/PhysicsMaterialSimple.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/BoxShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/CapsuleShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/CompoundShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/ConvexHullShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/ConvexShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/CylinderShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/DecoratedShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/HeightFieldShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/MeshShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/MutableCompoundShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/OffsetCenterOfMassShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/RotatedTranslatedShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/ScaledShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/Shape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/SphereShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/StaticCompoundShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/TaperedCapsuleShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/Shape/TriangleShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Collision/TransformedShape.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/ConeConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/Constraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/ConstraintManager.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/ContactConstraintManager.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/DistanceConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/FixedConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/GearConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/HingeConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/MotorSettings.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/PathConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/PathConstraintPath.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/PathConstraintPathHermite.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/PointConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/PulleyConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/RackAndPinionConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/SixDOFConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/SliderConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/SpringSettings.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/SwingTwistConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Constraints/TwoBodyConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/DeterminismLog.cpp^
 %SRC_JOLT_ROOT%/Physics/IslandBuilder.cpp^
 %SRC_JOLT_ROOT%/Physics/LargeIslandSplitter.cpp^
 %SRC_JOLT_ROOT%/Physics/PhysicsLock.cpp^
 %SRC_JOLT_ROOT%/Physics/PhysicsScene.cpp^
 %SRC_JOLT_ROOT%/Physics/PhysicsSystem.cpp^
 %SRC_JOLT_ROOT%/Physics/PhysicsUpdateContext.cpp^
 %SRC_JOLT_ROOT%/Physics/Ragdoll/Ragdoll.cpp^
 %SRC_JOLT_ROOT%/Physics/SoftBody/SoftBodyCreationSettings.cpp^
 %SRC_JOLT_ROOT%/Physics/SoftBody/SoftBodyMotionProperties.cpp^
 %SRC_JOLT_ROOT%/Physics/SoftBody/SoftBodyShape.cpp^
 %SRC_JOLT_ROOT%/Physics/SoftBody/SoftBodySharedSettings.cpp^
 %SRC_JOLT_ROOT%/Physics/StateRecorderImpl.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/MotorcycleController.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/TrackedVehicleController.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleAntiRollBar.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleCollisionTester.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleConstraint.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleController.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleDifferential.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleEngine.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleTrack.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/VehicleTransmission.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/Wheel.cpp^
 %SRC_JOLT_ROOT%/Physics/Vehicle/WheeledVehicleController.cpp^
 %SRC_JOLT_ROOT%/RegisterTypes.cpp^
 %SRC_JOLT_ROOT%/Renderer/DebugRenderer.cpp^
 %SRC_JOLT_ROOT%/Renderer/DebugRendererPlayback.cpp^
 %SRC_JOLT_ROOT%/Renderer/DebugRendererRecorder.cpp^
 %SRC_JOLT_ROOT%/Skeleton/SkeletalAnimation.cpp^
 %SRC_JOLT_ROOT%/Skeleton/Skeleton.cpp^
 %SRC_JOLT_ROOT%/Skeleton/SkeletonMapper.cpp^
 %SRC_JOLT_ROOT%/Skeleton/SkeletonPose.cpp^
 %SRC_JOLT_ROOT%/TriangleGrouper/TriangleGrouperClosestCentroid.cpp^
 %SRC_JOLT_ROOT%/TriangleGrouper/TriangleGrouperMorton.cpp^
 %SRC_JOLT_ROOT%/TriangleSplitter/TriangleSplitter.cpp^
 %SRC_JOLT_ROOT%/TriangleSplitter/TriangleSplitterBinning.cpp^
 %SRC_JOLT_ROOT%/TriangleSplitter/TriangleSplitterFixedLeafSize.cpp^
 %SRC_JOLT_ROOT%/TriangleSplitter/TriangleSplitterLongestAxis.cpp^
 %SRC_JOLT_ROOT%/TriangleSplitter/TriangleSplitterMean.cpp^
 %SRC_JOLT_ROOT%/TriangleSplitter/TriangleSplitterMorton.cpp

IF "%1"=="clean" (
 IF EXIST *.pch DEL *.pch
 IF EXIST *.obj DEL *.obj
 IF EXIST *.lib DEL *.lib
 IF EXIST *.pdb DEL *.pdb
 IF EXIST *.exe DEL *.exe
)

IF EXIST %NAME%.exe DEL %NAME%.exe

IF NOT EXIST imgui.lib (
 cl %CPP_FLAGS% /c %SRC_IMGUI%
 lib %LIB_FLAGS% *.obj /OUT:"imgui.lib"
 IF EXIST *.obj DEL *.obj
) & if ERRORLEVEL 1 GOTO error

IF NOT EXIST jolt.lib (
 cl %CPP_FLAGS% /c %SRC_JOLT%^
  /D"JPH_CROSS_PLATFORM_DETERMINISTIC"^
  /D"JPH_DEBUG_RENDERER"
 lib %LIB_FLAGS% *.obj /OUT:"jolt.lib"
 IF EXIST *.obj DEL *.obj
) & if ERRORLEVEL 1 GOTO error

IF NOT EXIST pch.pch (
 cl %CPP_FLAGS% /Fo"pch.lib" /c /Yc"pch.h" "pch.cpp"
) & if ERRORLEVEL 1 GOTO error

IF NOT "%1"=="hlsl" (
 cl %CPP_FLAGS% /Yu"pch.h" main.cpp /link %LINK_FLAGS%^
  pch.lib imgui.lib jolt.lib kernel32.lib user32.lib dxgi.lib d3d12.lib d2d1.lib
) & if ERRORLEVEL 1 GOTO error

GOTO end

:error
echo ---------------------------------------------------------------------------

:end
IF EXIST game.lib DEL game.lib
IF EXIST *.obj DEL *.obj
IF EXIST *.exp DEL *.exp

IF "%1" == "run" IF EXIST %NAME%.exe %NAME%.exe

IF "%1"=="hlsl" (
 %DXC% %HLSL_FLAGS% /T vs_%HLSL_SM% /E s00_vs /D_S00 shaders.hlsl /Fo %HLSL_OUT_DIR%/s00_vs.cso
 %DXC% %HLSL_FLAGS% /T ps_%HLSL_SM% /E s00_ps /D_S00 shaders.hlsl /Fo %HLSL_OUT_DIR%/s00_ps.cso
)
