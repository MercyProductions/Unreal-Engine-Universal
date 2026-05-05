#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DebugOverlay.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <dwmapi.h>
#include <gl/GL.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Settings.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "RuntimeSDK/RuntimeAccess.h"
#include "RuntimeSDK/RuntimeSDK.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "opengl32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
	using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	using ExecuteCommandListsFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
	using SwapBuffersFn = BOOL(WINAPI*)(HDC);

	constexpr size_t PresentVTableIndex = 8;
	constexpr size_t ResizeBuffersVTableIndex = 13;
	constexpr size_t ExecuteCommandListsVTableIndex = 10;
	constexpr size_t AbsoluteJumpSize = 14;
	constexpr int kOverlayConfigVersion = 2;

	enum class RenderBackend
	{
		Unknown,
		D3D11,
		D3D12,
		OpenGL,
		Vulkan
	};

	enum class ActorCaptureSource
	{
		Auto = 0,
		WorldLevels = 1,
		GObjects = 2,
		Both = 3
	};

	const char* BackendName(RenderBackend Backend)
	{
		switch (Backend)
		{
		case RenderBackend::D3D11:
			return "D3D11";
		case RenderBackend::D3D12:
			return "D3D12";
		case RenderBackend::OpenGL:
			return "OpenGL";
		case RenderBackend::Vulkan:
			return "Vulkan";
		default:
			return "Unknown";
		}
	}

	const char* RendererRouteName(int RendererRoute)
	{
		switch (std::clamp(RendererRoute, 0, 2))
		{
		case 1:
			return "Internal only";
		case 2:
			return "External only";
		default:
			return "Auto";
		}
	}

	const char* BoundsModeName(int BoundsMode)
	{
		switch (std::clamp(BoundsMode, 0, 3))
		{
		case 1:
			return "Actor";
		case 2:
			return "Root component";
		case 3:
			return "Fallback";
		default:
			return "Auto";
		}
	}

	const char* ProjectionSpaceName(int ProjectionSpace)
	{
		switch (std::clamp(ProjectionSpace, 0, 2))
		{
		case 1:
			return "Viewport";
		case 2:
			return "Desktop";
		default:
			return "Auto";
		}
	}

	const char* ProjectionRouteName(int ProjectionRoute)
	{
		switch (std::clamp(ProjectionRoute, 0, 2))
		{
		case 1:
			return "Native only";
		case 2:
			return "Fallback only";
		default:
			return "Auto";
		}
	}

	const char* ActorSourceName(ActorCaptureSource Source)
	{
		switch (Source)
		{
		case ActorCaptureSource::WorldLevels:
			return "World levels";
		case ActorCaptureSource::GObjects:
			return "GObjects";
		case ActorCaptureSource::Both:
			return "World + GObjects";
		default:
			return "Auto";
		}
	}

	enum class ActorFilterReason
	{
		None = 0,
		MissingLocation,
		LocalPlayer,
		Environment,
		Bot,
		NPC,
		Civilian,
		AI,
		Camera,
		Item,
		Weapon,
		Vehicle,
		Objective,
		ClassFilter,
		ClassExcludeFilter,
		TargetMode,
		Distance,
		ExcludeFilter,
		IncludeFilter,
		NotInView
	};

	const char* FilterReasonName(ActorFilterReason Reason)
	{
		switch (Reason)
		{
		case ActorFilterReason::MissingLocation:
			return "No location";
		case ActorFilterReason::LocalPlayer:
			return "Local player";
		case ActorFilterReason::Environment:
			return "Environment";
		case ActorFilterReason::Bot:
			return "Bot";
		case ActorFilterReason::NPC:
			return "NPC";
		case ActorFilterReason::Civilian:
			return "Civilian";
		case ActorFilterReason::AI:
			return "AI";
		case ActorFilterReason::Camera:
			return "Camera";
		case ActorFilterReason::Item:
			return "Item";
		case ActorFilterReason::Weapon:
			return "Weapon";
		case ActorFilterReason::Vehicle:
			return "Vehicle";
		case ActorFilterReason::Objective:
			return "Objective";
		case ActorFilterReason::ClassFilter:
			return "Class filter";
		case ActorFilterReason::ClassExcludeFilter:
			return "Class exclude";
		case ActorFilterReason::TargetMode:
			return "Target mode";
		case ActorFilterReason::Distance:
			return "Distance";
		case ActorFilterReason::ExcludeFilter:
			return "Exclude filter";
		case ActorFilterReason::IncludeFilter:
			return "Include filter";
		case ActorFilterReason::NotInView:
			return "Not in view";
		default:
			return "Kept";
		}
	}

	struct Vec2
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct Vec3
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
	};

	struct SkeletonBonePoint
	{
		int32 Index = 0;
		std::string Name;
		Vec3 Location;
		Vec2 Screen;
		bool HasScreen = false;
	};

	struct SkeletonSegment
	{
		int32 A = -1;
		int32 B = -1;
	};

	struct RawNameValue
	{
		std::array<uint8, 16> Bytes = {};
		int32 Size = 0;
		std::string Text;
	};

	struct ActorDebugInfo
	{
		uintptr_t Address = 0;
		uintptr_t ClassAddress = 0;
		int32 Index = 0;
		std::string Name;
		std::string ClassName;
		std::string ClassPath;
		std::string FullName;
		Vec3 Location;
		Vec3 BoundsOrigin;
		Vec3 BoundsExtent;
		Vec3 BoundsOffset;
		float SphereRadius = 0.0f;
		float DistanceMeters = 0.0f;
		int PlayerScore = 0;
		int PositionCandidateCount = 0;
		int ProjectionAttemptCount = 0;
		std::string PlayerScoreReasons;
		std::string LocationSource;
		std::string ProjectionSource;
		std::string ProjectionFailure;
		bool HasLocation = false;
		bool HasBounds = false;
		bool HasDistance = false;
		bool HasScreen = false;
		bool HasBox = false;
		bool IsPawn = false;
		bool IsCharacter = false;
		bool IsGameCharacter = false;
		bool IsGameEnemy = false;
		bool IsGamePlayer = false;
		bool IsLikelyPlayer = false;
		bool IsEnvironment = false;
		bool IsLocalPlayer = false;
		bool IsInView = false;
		bool IsBot = false;
		bool IsNPC = false;
		bool IsCivilian = false;
		bool IsAI = false;
		bool IsCameraActor = false;
		bool IsItem = false;
		bool IsWeapon = false;
		bool IsVehicle = false;
		bool IsObjective = false;
		bool HasPlayerState = false;
		bool IsRuntimePlayer = false;
		ActorFilterReason FilterReason = ActorFilterReason::None;
		uintptr_t PlayerStateAddress = 0;
		Vec2 Screen;
		Vec2 ScreenTop;
		Vec2 ScreenBottom;
		Vec2 BoxMin;
		Vec2 BoxMax;
		std::vector<SkeletonBonePoint> SkeletonBones;
		std::vector<SkeletonSegment> SkeletonSegments;
		std::string SkeletonSource;
		DWORD LastReflectedPositionTick = 0;
		bool HasSkeleton = false;
	};

	struct CaptureStats
	{
		int32 ObjectCount = 0;
		int32 ScannedObjects = 0;
		int32 ActorCandidates = 0;
		int32 CapturedActors = 0;
		int32 LocatedActors = 0;
		int32 ProjectedActors = 0;
		int32 ProjectionFailures = 0;
		int32 SkeletonActors = 0;
		int32 SkeletonBones = 0;
		int32 SkeletonSegments = 0;
		int32 ProjectionCandidateAttempts = 0;
		int32 ReflectedPositionHits = 0;
		int32 NativeProjectionAttempts = 0;
		int32 NativeProjectionSuccesses = 0;
		int32 NativeProjectionFailures = 0;
		int32 FallbackProjectionAttempts = 0;
		int32 FallbackProjectionSuccesses = 0;
		int32 FallbackProjectionFailures = 0;
		int32 InViewActors = 0;
		int32 BoundedActors = 0;
		int32 BoxedActors = 0;
		int32 FilteredActors = 0;
		int32 FilteredMissingLocation = 0;
		int32 FilteredLocalPlayer = 0;
		int32 FilteredEnvironment = 0;
		int32 FilteredBot = 0;
		int32 FilteredNPC = 0;
		int32 FilteredCivilian = 0;
		int32 FilteredAI = 0;
		int32 FilteredCamera = 0;
		int32 FilteredItem = 0;
		int32 FilteredWeapon = 0;
		int32 FilteredVehicle = 0;
		int32 FilteredObjective = 0;
		int32 FilteredClass = 0;
		int32 FilteredClassExclude = 0;
		int32 FilteredTargetMode = 0;
		int32 FilteredDistance = 0;
		int32 FilteredExclude = 0;
		int32 FilteredInclude = 0;
		int32 FilteredNotInView = 0;
		int32 BotActors = 0;
		int32 NpcActors = 0;
		int32 CivilianActors = 0;
		int32 AiActors = 0;
		int32 CameraActors = 0;
		int32 ItemActors = 0;
		int32 WeaponActors = 0;
		int32 VehicleActors = 0;
		int32 ObjectiveActors = 0;
		int32 RuntimeContextActors = 0;
		int32 PlayerStateActors = 0;
		int32 RuntimePlayerStates = 0;
		int32 RuntimeLocalPlayers = 0;
		int32 LikelyClassLockClasses = 0;
		int32 FrameProcessedActors = 0;
		int32 PositionProbeCacheHits = 0;
		int32 PositionProbeCacheMisses = 0;
		int32 MeshProbeCacheHits = 0;
		int32 MeshProbeCacheMisses = 0;
		bool UsedDesktopProjection = false;
		bool SymbolsReady = false;
		bool HasPlayerController = false;
		bool HasRuntimeContext = false;
		bool HasGameInstance = false;
		bool HasGameState = false;
		bool HasLocalPawn = false;
		bool HasCameraLocation = false;
		bool HasCameraRotation = false;
		bool HasCameraFov = false;
		bool HasProjection = false;
		bool UsedProjectionFallback = false;
		bool HasWorld = false;
		bool UsedWorldActors = false;
		bool UsedGObjects = false;
		bool HasStreamline = false;
		uintptr_t WorldAddress = 0;
		LONG ProjectionLeft = 0;
		LONG ProjectionTop = 0;
		LONG ProjectionRight = 0;
		LONG ProjectionBottom = 0;
		float ProjectionWidth = 0.0f;
		float ProjectionHeight = 0.0f;
		int32 WorldCount = 0;
		int32 LevelCount = 0;
		int32 LevelActorSlots = 0;
		std::string ActorSource = "Auto";
		std::string CameraLocationSource = "missing";
		std::string CameraRotationSource = "missing";
		std::string RhiModules = "none";
		DWORD LastCaptureTick = 0;
		std::string Status = "Waiting for Unreal symbols";
		std::string LikelyClassLock = "disabled";
	};

	struct OverlayConfig
	{
		bool Enabled = true;
		bool DrawLines = true;
		bool DrawBoxes = true;
		bool DrawNames = true;
		bool DrawDistance = true;
		bool DrawBounds = false;
		bool DrawCenterDot = false;
		bool DrawCrosshair = false;
		bool DrawTargetPreview = false;
		bool DrawSkeletons = true;
		bool DrawSkeletonBoneIds = false;
		bool DrawSkeletonBoneNames = false;
		bool UseReflectedPositionFallback = true;
		bool CaptureOnRenderFrame = false;
		bool OnlyOnScreen = true;
		bool OnlyWithLocation = true;
		bool OnlyInView = false;
		bool ExternalOverlayOnStreamline = true;
		bool UseProjectionFallback = true;
		bool ClampLargeBoxes = true;
		bool HideEnvironmentActors = true;
		bool HideLocalPlayer = true;
		bool HideBots = false;
		bool HideNPCs = false;
		bool HideCivilians = false;
		bool HideAI = false;
		bool HideCameras = false;
		bool HideItems = false;
		bool HideWeapons = false;
		bool HideVehicles = false;
		bool HideObjectives = false;
		bool UseRuntimePlayerContext = true;
		bool PreferRuntimePlayers = true;
		bool IncludeGameStatePlayers = true;
		bool LockLikelyPlayerClasses = false;
		bool FastOverlayMode = true;
		bool ProbeReflectedPositionsOnLocatedActors = false;
		bool ThrottleLiveReflectionFallback = true;
		bool EnableClassFilter = false;
		bool DeveloperAutoCycleClasses = false;
		bool DeveloperPreviewEnabled = false;
		bool DeveloperPreviewDrawLines = true;
		bool DeveloperPreviewDrawBoxes = true;
		bool DeveloperShowInheritedMembers = true;
		bool EnableDeveloperOptions = false;
		int ActorSource = static_cast<int>(ActorCaptureSource::Auto);
		int TargetMode = 1;
		int ProjectionSpace = 0;
		int ProjectionRoute = 0;
		int BoundsMode = 0;
		int RendererRoute = 0;
		int LineOrigin = 2;
		int LineTarget = 2;
		int RefreshMs = 1000;
		int FrameCaptureMinMs = 16;
		int FrameProjectionMaxActors = 64;
		int FrameSkeletonMinMs = 1000;
		int ReflectedPositionRefreshMs = 1000;
		int MaxActors = 256;
		int DeveloperClassCycleMs = 900;
		int LikelyPlayerScoreThreshold = 35;
		int LikelyClassLockMinActors = 1;
		int LikelyClassLockMaxClasses = 4;
		int PositionProbeMaxFields = 80;
		int SkeletonMaxBones = 64;
		float MaxDistanceMeters = 0.0f;
		float CrosshairSize = 9.0f;
		float CrosshairGap = 4.0f;
		float CrosshairThickness = 1.5f;
		float TargetPreviewRadius = 250.0f;
		float TargetPreviewLineThickness = 1.75f;
		float SkeletonThickness = 1.5f;
		float SkeletonPointRadius = 2.0f;
		float ProjectionOffsetX = 0.0f;
		float ProjectionOffsetY = 0.0f;
		float ProjectionScaleX = 1.0f;
		float ProjectionScaleY = 1.0f;
		float LineThickness = 1.5f;
		float BoxThickness = 1.5f;
		float BoxWidthRatio = 0.38f;
		float MaxBoxScreenFraction = 0.40f;
		float BoxPaddingPixels = 2.0f;
		float MinBoxHeightPixels = 8.0f;
		float FallbackHalfHeight = 70.0f;
		float FallbackHalfWidth = 28.0f;
		char Filter[128] = {};
		char ExcludeFilter[128] = {};
		char EnvironmentFilter[512] = {};
		char BotFilter[256] = {};
		char NpcFilter[256] = {};
		char CivilianFilter[256] = {};
		char AiFilter[256] = {};
		char PlayerFilter[384] = {};
		char NonPlayerFilter[512] = {};
		char PositionFieldFilter[512] = {};
		char CameraFilter[256] = {};
		char ItemFilter[256] = {};
		char WeaponFilter[256] = {};
		char VehicleFilter[256] = {};
		char ObjectiveFilter[256] = {};
		char ClassFilter[256] = {};
		char ClassExcludeFilter[256] = {};
		char DeveloperPreviewClassFilter[256] = {};
		char DeveloperProbeFilter[128] = {};
		int DeveloperMaxRows = 80;
		ImVec4 BoxColor = ImVec4(0.12f, 0.82f, 0.58f, 1.0f);
		ImVec4 LineColor = ImVec4(1.0f, 0.72f, 0.25f, 1.0f);
		ImVec4 TextColor = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);
		ImVec4 BoundsColor = ImVec4(0.42f, 0.68f, 1.0f, 1.0f);
		ImVec4 CrosshairColor = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);
		ImVec4 TargetPreviewColor = ImVec4(1.0f, 0.25f, 0.35f, 1.0f);
		ImVec4 SkeletonColor = ImVec4(0.36f, 0.84f, 1.0f, 1.0f);
	};

	struct UnrealSymbols
	{
		UEClass ActorClass;
		UEClass WorldClass;
		UEClass LevelClass;
		UEClass PawnClass;
		UEClass CharacterClass;
		UEClass SceneComponentClass;
		UEClass PrimitiveComponentClass;
		UEClass SkinnedMeshComponentClass;
		UEClass SkeletalMeshComponentClass;
		UEClass PlayerControllerClass;
		UEClass PlayerCameraManagerClass;
		UEClass GameInstanceClass;
		UEClass LocalPlayerClass;
		UEClass GameStateBaseClass;
		UEClass PlayerStateClass;
		UEProperty PersistentLevelProperty;
		UEProperty LevelsProperty;
		UEProperty OwningGameInstanceProperty;
		UEProperty WorldGameStateProperty;
		UEProperty LocalPlayersProperty;
		UEProperty LocalPlayerControllerProperty;
		UEProperty AcknowledgedPawnProperty;
		UEProperty ControllerCharacterProperty;
		UEProperty ControllerCameraManagerProperty;
		UEProperty PawnPlayerStateProperty;
		UEProperty GameStatePlayerArrayProperty;
		UEProperty PlayerStatePawnProperty;
		UEProperty PlayerStateOwnerProperty;
		UEProperty RootComponentProperty;
		UEProperty ComponentBoundsProperty;
		UEFunction GetActorLocation;
		UEFunction GetActorBounds;
		UEFunction GetPawn;
		UEFunction GetComponentLocation;
		UEFunction ProjectWorldLocationToScreen;
		UEFunction GetNumBones;
		UEFunction GetBoneName;
		UEFunction GetBoneLocation;
		UEFunction GetPlayerViewPoint;
		UEFunction GetControlRotation;
		UEFunction GetControllerActorRotation;
		UEFunction GetCameraLocation;
		UEFunction GetCameraRotation;
		UEFunction GetCameraFov;
		bool Ready = false;
	};

	struct RuntimeOverlaySymbols
	{
		uintptr_t actorClass = 0;
		uintptr_t worldClass = 0;
		uintptr_t levelClass = 0;
		uintptr_t pawnClass = 0;
		uintptr_t characterClass = 0;
		uintptr_t sceneComponentClass = 0;
		uintptr_t primitiveComponentClass = 0;
		uintptr_t skinnedMeshComponentClass = 0;
		uintptr_t skeletalMeshComponentClass = 0;
		uintptr_t playerControllerClass = 0;
		uintptr_t playerCameraManagerClass = 0;
		uintptr_t gameInstanceClass = 0;
		uintptr_t localPlayerClass = 0;
		uintptr_t gameStateBaseClass = 0;
		uintptr_t playerStateClass = 0;
		uintptr_t engineClass = 0;
		uintptr_t gameEngineClass = 0;
		uintptr_t gameViewportClientClass = 0;
		uintptr_t consoleClass = 0;
		uintptr_t gameplayStaticsClass = 0;
		uintptr_t inputSettingsClass = 0;
		uintptr_t kismetStringLibraryClass = 0;
		uintptr_t kismetSystemLibraryClass = 0;
		uintptr_t crabCharacterClass = 0;
		uintptr_t crabEnemyClass = 0;
		uintptr_t crabPlayerCharacterClass = 0;

		int32_t worldPersistentLevelOffset = -1;
		int32_t worldLevelsOffset = -1;
		int32_t worldGameInstanceOffset = -1;
		int32_t worldGameStateOffset = -1;

		int32_t gameInstanceLocalPlayersOffset = -1;
		int32_t localPlayerControllerOffset = -1;
		int32_t localPlayerViewportClientOffset = -1;
		int32_t playerControllerAcknowledgedPawnOffset = -1;
		int32_t playerControllerCharacterOffset = -1;
		int32_t playerControllerCameraManagerOffset = -1;
		int32_t pawnPlayerStateOffset = -1;
		int32_t playerStatePawnOffset = -1;
		int32_t playerStateOwnerOffset = -1;
		int32_t gameStatePlayerArrayOffset = -1;

		int32_t levelActorsOffset = -1;
		int32_t actorRootComponentOffset = -1;
		int32_t primitiveBoundsOffset = -1;
		int32_t primitiveBoundsSize = 0;
		int32_t engineConsoleClassOffset = -1;
		int32_t engineGameViewportOffset = -1;
		int32_t gameViewportConsoleOffset = -1;
		int32_t consoleTargetPlayerOffset = -1;
		int32_t inputSettingsConsoleKeyOffset = -1;
		int32_t inputSettingsConsoleKeysOffset = -1;
		int32_t keyNameOffset = -1;

		RuntimeFunctionInfo getActorLocation;
		RuntimeFunctionInfo getActorBounds;
		RuntimeFunctionInfo getPawn;
		RuntimeFunctionInfo getComponentLocation;
		RuntimeFunctionInfo projectWorldLocationToScreen;
		RuntimeFunctionInfo getNumBones;
		RuntimeFunctionInfo getBoneName;
		RuntimeFunctionInfo getBoneLocation;
		RuntimeFunctionInfo getPlayerViewPoint;
		RuntimeFunctionInfo getControlRotation;
		RuntimeFunctionInfo getControllerActorRotation;
		RuntimeFunctionInfo getCameraLocation;
		RuntimeFunctionInfo getCameraRotation;
		RuntimeFunctionInfo getCameraFov;
		RuntimeFunctionInfo spawnObject;
		RuntimeFunctionInfo convStringToName;
		RuntimeFunctionInfo setConsoleTarget;
		RuntimeFunctionInfo consoleKey;
		RuntimeFunctionInfo sendToConsole;
		RuntimeFunctionInfo executeConsoleCommand;

		bool ready = false;
	};

	struct RawTArrayView
	{
		void* Data = nullptr;
		int32 Num = 0;
		int32 Max = 0;
	};

	struct PositionCandidate
	{
		Vec3 Location;
		std::string Source;
	};

	struct RuntimePlayerContext
	{
		UEObject World;
		UEObject GameInstance;
		UEObject LocalPlayer;
		UEObject PlayerController;
		UEObject CameraManager;
		UEObject LocalPawn;
		UEObject LocalCharacter;
		UEObject GameState;
		std::unordered_set<uintptr_t> PlayerStates;
		std::unordered_set<uintptr_t> RuntimeActors;
		std::unordered_map<uintptr_t, uintptr_t> ActorToPlayerState;
		int32 LocalPlayers = 0;
		int32 PlayerStateCount = 0;
		bool HasWorld = false;
		bool HasGameInstance = false;
		bool HasLocalPlayer = false;
		bool HasPlayerController = false;
		bool HasLocalPawn = false;
		bool HasGameState = false;
	};

	struct ConsoleUnlockState
	{
		bool attempted = false;
		bool unlocked = false;
		uintptr_t engine = 0;
		uintptr_t gameViewport = 0;
		uintptr_t console = 0;
		DWORD tick = 0;
		std::string status = "Not attempted";
	};

	struct ConsoleCommandPreset
	{
		const char* label;
		const char* command;
		const char* note;
	};

	constexpr ConsoleCommandPreset kConsoleCommandPresets[] = {
		{ "Console: Help", "Help", "Print console help if available in this build." },
		{ "Console: Dump Commands", "DumpConsoleCommands", "Ask Unreal to dump registered console commands to the log." },
		{ "Stats: FPS", "stat fps", "Toggle the FPS stat display." },
		{ "Stats: Unit", "stat unit", "Frame, game, draw, GPU, and RHIT timings." },
		{ "Stats: Unit Graph", "stat unitgraph", "Graph frame timing over time." },
		{ "Stats: Game", "stat game", "Game-thread timing groups." },
		{ "Stats: Scene Rendering", "stat scenerendering", "Renderer stats." },
		{ "Stats: Memory", "stat memory", "Memory stats." },
		{ "Stats: Streaming", "stat streaming", "Asset streaming stats." },
		{ "Stats: RHI", "stat rhi", "RHI stats." },
		{ "Stats: GPU", "stat gpu", "GPU stats when available in this build." },
		{ "Stats: Clear", "stat none", "Clear stat overlays." },
		{ "Debug: General", "showdebug", "Toggle general showdebug output." },
		{ "Debug: Camera", "showdebug camera", "Camera-related debug output." },
		{ "Debug: Input", "showdebug input", "Input debug output." },
		{ "Debug: Collision", "showdebug collision", "Collision debug output." },
		{ "Debug: Animation", "showdebug animation", "Animation debug output." },
		{ "Debug: Camera Toggle", "toggledebugcamera", "Toggle Unreal debug camera if enabled." },
		{ "Render: Windowed 1080p", "r.SetRes 1920x1080w", "Set resolution to 1920x1080 windowed." },
		{ "Render: Fullscreen 1080p", "r.SetRes 1920x1080f", "Set resolution to 1920x1080 fullscreen." },
		{ "Render: Screen Percentage", "r.ScreenPercentage 100", "Adjust render screen percentage." },
		{ "Render: View Distance", "r.ViewDistanceScale 1", "Adjust view distance scale." },
		{ "Render: Shadows", "r.ShadowQuality 3", "Adjust shadow quality." },
		{ "Render: Anti Aliasing", "r.PostProcessAAQuality 4", "Adjust post-process AA quality." },
		{ "Render: Motion Blur Off", "r.MotionBlurQuality 0", "Disable motion blur." },
		{ "Render: Bloom", "r.BloomQuality 3", "Adjust bloom quality." },
		{ "Render: VSync Off", "r.VSync 0", "Disable vsync." },
		{ "Frame Cap: 60", "t.MaxFPS 60", "Set max FPS to 60." },
		{ "Frame Cap: 144", "t.MaxFPS 144", "Set max FPS to 144." },
		{ "Scalability: View Distance", "sg.ViewDistanceQuality 3", "Adjust scalability view distance quality." },
		{ "Scalability: AA", "sg.AntiAliasingQuality 3", "Adjust scalability anti-aliasing quality." },
		{ "Scalability: Shadows", "sg.ShadowQuality 3", "Adjust scalability shadow quality." },
		{ "Scalability: Post Process", "sg.PostProcessQuality 3", "Adjust scalability post-process quality." },
		{ "Scalability: Textures", "sg.TextureQuality 3", "Adjust scalability texture quality." },
		{ "Scalability: Effects", "sg.EffectsQuality 3", "Adjust scalability effects quality." },
		{ "Scalability: Foliage", "sg.FoliageQuality 3", "Adjust scalability foliage quality." },
	};

	std::atomic_bool gRunning = false;
	std::atomic_bool gHookInstalled = false;
	std::atomic_bool gImGuiInitialized = false;
	std::atomic_bool gShutdownRequested = false;
	std::atomic_bool gExternalOverlay = false;
	std::atomic_bool gExternalInputPassthrough = false;

	std::thread gCaptureThread;
	std::thread gExternalRenderThread;
	std::mutex gActorMutex;
	std::mutex gConfigMutex;
	std::mutex gCaptureMutex;
	std::mutex gClassObjectCacheMutex;
	std::mutex gPositionProbeCacheMutex;
	std::mutex gMeshProbeCacheMutex;

	std::vector<ActorDebugInfo> gActors;
	std::vector<ActorDebugInfo> gFilteredActors;
	CaptureStats gStats;
	OverlayConfig gConfig;
	UnrealSymbols gSymbols;
	RuntimeOverlaySymbols gRuntimeSymbols;
	ConsoleUnlockState gConsoleState;
	char gConsoleCommandBuffer[128] = "stat fps";
	int gConsoleCommandPresetIndex = 0;
	std::atomic<DWORD> gLastRenderFrameCaptureTick = 0;
	std::atomic<DWORD> gLastFrameSkeletonTick = 0;
	std::atomic<std::uint64_t> gRenderFrameCapturePasses = 0;
	std::atomic<std::uint64_t> gRenderFrameCaptureSkips = 0;
	DWORD gLastConsoleHotkeyTick = 0;
	DWORD gLastClassCycleTick = 0;
	int gClassCycleIndex = -1;
	std::mutex gLikelyClassLockMutex;
	std::string gLikelyClassLockFilter;
	int gLikelyClassLockClassCount = 0;
	DWORD gLikelyClassLockTick = 0;

	struct CachedClassObject
	{
		uintptr_t Address = 0;
		DWORD Tick = 0;
	};

	enum class PositionProbeKind
	{
		VectorProperty,
		ComponentProperty,
		GetterFunction
	};

	struct CachedPositionProbe
	{
		PositionProbeKind Kind = PositionProbeKind::VectorProperty;
		UEProperty Property;
		UEFunction Function;
		std::string Source;
	};

	struct CachedPositionProbePlan
	{
		std::string Signature;
		std::vector<CachedPositionProbe> Probes;
		DWORD BuiltTick = 0;
	};

	struct CachedMeshProbePlan
	{
		bool Built = false;
		bool ActorIsMesh = false;
		UEProperty Property;
		std::string Source;
		DWORD BuiltTick = 0;
	};

	std::unordered_map<uintptr_t, CachedClassObject> gClassObjectCache;
	std::unordered_map<uintptr_t, CachedPositionProbePlan> gPositionProbeCache;
	std::unordered_map<uintptr_t, CachedMeshProbePlan> gMeshProbeCache;

	RenderBackend gBackend = RenderBackend::Unknown;
	void** gSwapChainVTable = nullptr;
	PresentFn gOriginalPresent = nullptr;
	ResizeBuffersFn gOriginalResizeBuffers = nullptr;

	ID3D11Device* gDevice = nullptr;
	ID3D11DeviceContext* gDeviceContext = nullptr;
	ID3D11RenderTargetView* gRenderTargetView = nullptr;

	void** gD3D12CommandQueueVTable = nullptr;
	ExecuteCommandListsFn gOriginalExecuteCommandLists = nullptr;
	ID3D12Device* gD3D12Device = nullptr;
	ID3D12CommandQueue* gD3D12CommandQueue = nullptr;
	ID3D12DescriptorHeap* gD3D12RtvHeap = nullptr;
	ID3D12DescriptorHeap* gD3D12SrvHeap = nullptr;
	ID3D12GraphicsCommandList* gD3D12CommandList = nullptr;
	ID3D12Fence* gD3D12Fence = nullptr;
	HANDLE gD3D12FenceEvent = nullptr;
	UINT64 gD3D12FenceValue = 0;
	UINT gD3D12BufferCount = 0;
	DXGI_FORMAT gD3D12RtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	std::vector<ID3D12CommandAllocator*> gD3D12CommandAllocators;
	std::vector<ID3D12Resource*> gD3D12RenderTargets;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> gD3D12RtvHandles;
	std::vector<UINT64> gD3D12FrameFenceValues;
	std::vector<bool> gD3D12SrvDescriptorUsed;
	UINT gD3D12RtvDescriptorSize = 0;
	UINT gD3D12SrvDescriptorSize = 0;

	SwapBuffersFn gOriginalSwapBuffers = nullptr;
	void* gSwapBuffersTrampoline = nullptr;
	uint8 gSwapBuffersOriginalBytes[AbsoluteJumpSize] = {};

	HWND gWindow = nullptr;
	WNDPROC gOriginalWndProc = nullptr;
	HWND gExternalWindow = nullptr;
	HWND gTargetWindow = nullptr;
	IDXGISwapChain* gExternalSwapChain = nullptr;
	ID3D11Device* gExternalDevice = nullptr;
	ID3D11DeviceContext* gExternalDeviceContext = nullptr;
	ID3D11RenderTargetView* gExternalRenderTargetView = nullptr;
	IDCompositionDevice* gCompositionDevice = nullptr;
	IDCompositionTarget* gCompositionTarget = nullptr;
	IDCompositionVisual* gCompositionVisual = nullptr;
	bool gMenuOpen = true;
	bool gLastExternalInputPassthrough = true;
	bool gLastExternalMenuOpen = false;
	bool gExternalInputModeApplied = false;
	uintptr_t gSelectedActorAddress = 0;

	struct FeaturePlaceholderState
	{
		bool bEnableAimbot = false;
		bool bAimbotFovCheck = false;
		bool bRainbowAimbotTargetColor = false;
		bool bTargetPreview = true;
		float fAimbotFov = 80.0f;
		float fAimbotSmoothness = 0.5f;
		float fHeadPosOffset = 2.0f;
		float fFeetPosOffset = 0.0f;
		int nAimTargetBone = 0;
		int nAimActivationKey = VK_RBUTTON;
		ImVec4 cAimbotTargetColor = ImVec4(1.0f, 0.20f, 0.25f, 1.0f);

		bool bPlayersSnapline = true;
		bool bRainbowPlayersSnapline = false;
		bool bPlayersBox = true;
		bool bRainbowPlayersBox = false;
		bool bPlayersBoxFilled = false;
		bool bPlayersBox3D = false;
		bool bPlayerSkeleton = true;
		bool bRainbowPlayerSkeleton = false;
		bool bPlayersHealth = false;
		bool bBotChecker = false;
		bool bBotCheckerText = true;
		bool bPlayerChams = false;
		bool bRainbowPlayerChams = false;
		bool bShowNames = true;
		bool bShowDistance = true;
		int nPlayersSnaplineType = 2;
		int nBoxStyle = 0;
		float fBoxThickness = 1.5f;
		float fSnaplineThickness = 1.5f;
		float fSkeletonThickness = 1.5f;
		ImVec4 cPlayersSnaplineColor = ImVec4(1.0f, 0.72f, 0.25f, 1.0f);
		ImVec4 cPlayersBoxColor = ImVec4(0.12f, 0.82f, 0.58f, 1.0f);
		ImVec4 cPlayerSkeletonColor = ImVec4(0.36f, 0.84f, 1.0f, 1.0f);
		ImVec4 cTargetNotVisibleColor = ImVec4(1.0f, 0.25f, 0.35f, 1.0f);
		ImVec4 cBotCheckerColor = ImVec4(0.25f, 0.40f, 1.0f, 1.0f);
		ImVec4 cChamsColorTargetVisible = ImVec4(0.12f, 0.82f, 0.58f, 1.0f);
		ImVec4 cChamsColorTargetHidden = ImVec4(1.0f, 0.25f, 0.35f, 1.0f);

		bool bGodMode = false;
		bool bNoClip = false;
		bool bFly = false;
		bool bNoGravity = false;
		bool bTimeScaleChanger = false;
		bool bSpeedHack = false;
		bool bNoRecoil = false;
		bool bNoSpread = false;
		bool bRapidFire = false;
		bool bOneShot = false;
		bool bInfiniteAmmo = false;
		bool bKillAll = false;
		float fTimeScale = 1.0f;
		float fSpeedValue = 1.0f;
		float fProjectileScale = 1.0f;
		int nExploitProfile = 0;

		bool bWatermark = true;
		bool bShowMouse = true;
		bool bRainbowMouse = false;
		bool bCrosshair = false;
		bool bRainbowCrosshair = false;
		bool bCameraFovChanger = false;
		bool bShowInspector = false;
		bool bUpdateTargets = true;
		bool bUpdateTargetsInDifferentThread = false;
		float fCrosshairSize = 5.0f;
		float fCameraCustomFov = 80.0f;
		int nMouseType = 0;
		int nCrosshairType = 0;
		int nTargetFetch = 2;
		ImVec4 cMouseColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		ImVec4 cCrosshairColor = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);

		char szAimTargetGroup[64] = "Likely players";
		char szEspFilterProfile[64] = "Runtime likely players";
		char szMiscProfileName[64] = "Default diagnostics";
		char szDeveloperNote[128] = "UI-only placeholder state";
	};

	FeaturePlaceholderState gFeatureState;

	float Distance(const Vec3& A, const Vec3& B)
	{
		const double DX = A.X - B.X;
		const double DY = A.Y - B.Y;
		const double DZ = A.Z - B.Z;
		return static_cast<float>(std::sqrt((DX * DX) + (DY * DY) + (DZ * DZ)));
	}

	std::string ToLower(std::string Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
		return Text;
	}

	bool ContainsNoCase(const std::string& Haystack, const char* Needle)
	{
		if (!Needle || Needle[0] == '\0')
			return true;

		const std::string LowerHaystack = ToLower(Haystack);
		const std::string LowerNeedle = ToLower(Needle);
		return LowerHaystack.find(LowerNeedle) != std::string::npos;
	}

	bool MatchesTokenListNoCase(const std::string& Haystack, const char* Tokens)
	{
		if (!Tokens || Tokens[0] == '\0')
			return false;

		const std::string LowerHaystack = ToLower(Haystack);
		std::string LowerTokens = ToLower(Tokens);
		size_t Start = 0;

		while (Start < LowerTokens.size())
		{
			size_t End = LowerTokens.find_first_of(",;|", Start);
			if (End == std::string::npos)
				End = LowerTokens.size();

			std::string Token = LowerTokens.substr(Start, End - Start);
			Token.erase(Token.begin(), std::find_if(Token.begin(), Token.end(), [](unsigned char Ch) { return !std::isspace(Ch); }));
			Token.erase(std::find_if(Token.rbegin(), Token.rend(), [](unsigned char Ch) { return !std::isspace(Ch); }).base(), Token.end());

			if (!Token.empty() && LowerHaystack.find(Token) != std::string::npos)
				return true;

			Start = End + 1;
		}

		return false;
	}

	void EnsureTokenListContains(char* Buffer, size_t BufferSize, const char* Token)
	{
		if (!Buffer || BufferSize == 0 || !Token || Token[0] == '\0')
			return;

		if (MatchesTokenListNoCase(Token, Buffer))
			return;

		const size_t CurrentLength = strnlen_s(Buffer, BufferSize);
		const size_t TokenLength = std::strlen(Token);
		const size_t SeparatorLength = CurrentLength > 0 ? 1 : 0;
		if (CurrentLength + SeparatorLength + TokenLength + 1 > BufferSize)
			return;

		if (SeparatorLength)
			Buffer[CurrentLength] = ',';
		std::memcpy(Buffer + CurrentLength + SeparatorLength, Token, TokenLength + 1);
	}

	bool ActorTextMatchesTokens(const ActorDebugInfo& Actor, const char* Tokens)
	{
		return MatchesTokenListNoCase(Actor.Name, Tokens)
			|| MatchesTokenListNoCase(Actor.ClassName, Tokens)
			|| MatchesTokenListNoCase(Actor.ClassPath, Tokens)
			|| MatchesTokenListNoCase(Actor.FullName, Tokens);
	}

	bool ActorClassMatchesTokens(const ActorDebugInfo& Actor, const char* Tokens)
	{
		return MatchesTokenListNoCase(Actor.ClassName, Tokens)
			|| MatchesTokenListNoCase(Actor.ClassPath, Tokens);
	}

	std::string GetLikelyClassLockFilter()
	{
		std::scoped_lock Lock(gLikelyClassLockMutex);
		return gLikelyClassLockFilter;
	}

	void ClearLikelyClassLock()
	{
		std::scoped_lock Lock(gLikelyClassLockMutex);
		gLikelyClassLockFilter.clear();
		gLikelyClassLockClassCount = 0;
		gLikelyClassLockTick = 0;
	}

	bool ShouldUseLikelyClassLock(const OverlayConfig& Config)
	{
		return Config.LockLikelyPlayerClasses
			&& Config.TargetMode == 1
			&& !(Config.EnableClassFilter && Config.ClassFilter[0] != '\0');
	}

	void CopyLikelyClassLockToStats(CaptureStats& Stats)
	{
		std::scoped_lock Lock(gLikelyClassLockMutex);
		Stats.LikelyClassLock = gLikelyClassLockFilter.empty() ? "disabled" : gLikelyClassLockFilter;
		Stats.LikelyClassLockClasses = gLikelyClassLockClassCount;
	}

	const char* DefaultEnvironmentTokens()
	{
		return "sm_env,bp_env,env_,_env,environment,world,staticmeshactor,instancedstaticmesh,"
			"instancedfoliage,foliage,landscape,terrain,road,edge,wall,floor,ceiling,roof,"
			"building,warehouse,door,window,prop,decal,spline,water,rock,tree,grass,bush,"
			"sky,fog,postprocess,light,audio,volume,trigger,nav,meshbounds,csg,section,brush,blocking";
	}

	const char* DefaultBotTokens()
	{
		return "bot,bot_,_bot,enemybot,aibot,ai_bot,botpawn,botcharacter";
	}

	const char* DefaultNpcTokens()
	{
		return "npc,npc_,_npc,nonplayer,non_player,non-player,ai_npc,npcpawn,npccharacter";
	}

	const char* DefaultCivilianTokens()
	{
		return "civilian,civilian_,_civilian,civ_,_civ,crowd,pedestrian,ped_,_ped,passerby";
	}

	const char* DefaultAiTokens()
	{
		return "ai_,_ai,bp_ai,aiagent,aicharacter,aipawn,aicontroller,behavior,behaviour,blackboard,bot,npc";
	}

	const char* DefaultCameraTokens()
	{
		return "camera,camerabp,playercamera,scene_capture,scenecapture,spectatorcamera,viewtarget";
	}

	const char* DefaultItemTokens()
	{
		return "item,pickup,collectible,collectable,loot,ammo,health,medkit,powerup,inventory";
	}

	const char* DefaultWeaponTokens()
	{
		return "weapon,gun,rifle,pistol,shotgun,smg,knife,grenade,projectile,bullet";
	}

	const char* DefaultVehicleTokens()
	{
		return "vehicle,car,truck,van,bike,boat,helicopter,plane,drone";
	}

	const char* DefaultObjectiveTokens()
	{
		return "objective,mission,quest,goal,checkpoint,target,interact,triggerobjective";
	}

	const char* DefaultPlayerTokens()
	{
		return "player,pawn,character,hero,unit,avatar,survivor,soldier,enemy,hostile,monster,creature,crabenemy,crabplayer,teammate,agent,operator,champion";
	}

	const char* DefaultNonPlayerTokens()
	{
		return "controller,playercontroller,camera,manager,gamemode,gamestate,playerstate,weapon,projectile,pickup,item,vehicle,objective,trigger,volume,component,mesh,material,light,sky,fog,water,door,wall,floor,chest,shop,pedestal,crystal,cosmetic,reward,portal";
	}

	const char* DefaultPositionFieldTokens()
	{
		return "location,position,worldlocation,actorlocation,componentlocation,rootlocation,meshlocation,pawnlocation,spawnlocation,center,origin,pivot";
	}

	void ResetMainOverlayTargetingDefaults(OverlayConfig& Config)
	{
		Config.EnableClassFilter = false;
		Config.LockLikelyPlayerClasses = false;
		Config.TargetMode = 1;
		Config.ActorSource = static_cast<int>(ActorCaptureSource::Auto);
		Config.HideLocalPlayer = true;
		Config.HideEnvironmentActors = true;
		Config.OnlyOnScreen = true;
		Config.OnlyWithLocation = true;
		Config.OnlyInView = false;
		Config.Filter[0] = '\0';
		Config.ExcludeFilter[0] = '\0';
		Config.ClassFilter[0] = '\0';
		Config.ClassExcludeFilter[0] = '\0';
		Config.DeveloperPreviewClassFilter[0] = '\0';
		Config.DeveloperAutoCycleClasses = false;
		Config.DeveloperPreviewEnabled = false;
	}

	void EnsureRuntimeTokenDefaults(OverlayConfig& Config)
	{
		if (Config.PlayerFilter[0] == '\0')
			std::snprintf(Config.PlayerFilter, sizeof(Config.PlayerFilter), "%s", DefaultPlayerTokens());
		if (Config.NonPlayerFilter[0] == '\0')
			std::snprintf(Config.NonPlayerFilter, sizeof(Config.NonPlayerFilter), "%s", DefaultNonPlayerTokens());
		if (Config.PositionFieldFilter[0] == '\0')
			std::snprintf(Config.PositionFieldFilter, sizeof(Config.PositionFieldFilter), "%s", DefaultPositionFieldTokens());

		EnsureTokenListContains(Config.PlayerFilter, sizeof(Config.PlayerFilter), "crabenemy");
		EnsureTokenListContains(Config.PlayerFilter, sizeof(Config.PlayerFilter), "crabplayer");
		EnsureTokenListContains(Config.PlayerFilter, sizeof(Config.PlayerFilter), "hostile");
		EnsureTokenListContains(Config.PlayerFilter, sizeof(Config.PlayerFilter), "monster");
	}

	bool IsBotLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.BotFilter);
	}

	bool IsNpcLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.NpcFilter);
	}

	bool IsCivilianLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.CivilianFilter);
	}

	bool IsAiLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.AiFilter);
	}

	bool IsCameraLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.CameraFilter);
	}

	bool IsItemLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.ItemFilter);
	}

	bool IsWeaponLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.WeaponFilter);
	}

	bool IsVehicleLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.VehicleFilter);
	}

	bool IsObjectiveLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return ActorTextMatchesTokens(Actor, Config.ObjectiveFilter);
	}

	bool IsEnvironmentLikeActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		if (Actor.IsLocalPlayer || Actor.IsPawn || Actor.IsCharacter || Actor.IsGameCharacter || Actor.IsGameEnemy || Actor.IsGamePlayer || Actor.IsLikelyPlayer
			|| Actor.IsBot || Actor.IsNPC || Actor.IsCivilian || Actor.IsAI)
			return false;

		return ActorTextMatchesTokens(Actor, Config.ExcludeFilter)
			|| ActorTextMatchesTokens(Actor, Config.EnvironmentFilter);
	}

	void AddScoreReason(int& Score, std::string& Reasons, int Delta, const char* Reason)
	{
		Score += Delta;
		if (!Reasons.empty())
			Reasons += ", ";
		Reasons += Reason;
	}

	int ScoreLikelyPlayerActor(const ActorDebugInfo& Actor, const OverlayConfig& Config, std::string* OutReasons = nullptr)
	{
		int Score = 0;
		std::string Reasons;

		if (Actor.IsPawn)
			AddScoreReason(Score, Reasons, 35, "Pawn class");
		if (Actor.IsCharacter)
			AddScoreReason(Score, Reasons, 45, "Character class");
		if (Actor.IsRuntimePlayer)
			AddScoreReason(Score, Reasons, 55, "runtime player context");
		if (Actor.IsGamePlayer)
			AddScoreReason(Score, Reasons, 55, "game player class");
		if (Actor.IsGameEnemy)
			AddScoreReason(Score, Reasons, 52, "game enemy class");
		if (Actor.IsGameCharacter)
			AddScoreReason(Score, Reasons, 42, "game character class");
		if (Actor.HasPlayerState)
			AddScoreReason(Score, Reasons, 30, "PlayerState link");
		if (Actor.IsBot)
			AddScoreReason(Score, Reasons, 24, "bot token");
		if (Actor.IsNPC)
			AddScoreReason(Score, Reasons, 18, "NPC token");
		if (Actor.IsCivilian)
			AddScoreReason(Score, Reasons, 10, "civilian token");
		if (Actor.IsAI)
			AddScoreReason(Score, Reasons, 14, "AI token");
		if (Actor.HasLocation)
			AddScoreReason(Score, Reasons, 4, "world location");
		if (Actor.HasBounds)
			AddScoreReason(Score, Reasons, 4, "bounds");
		if (Actor.HasScreen)
			AddScoreReason(Score, Reasons, 3, "projected");
		if (Actor.IsInView)
			AddScoreReason(Score, Reasons, 3, "in view");
		if (Actor.HasDistance && Actor.DistanceMeters > 0.0f && Actor.DistanceMeters < 250.0f)
			AddScoreReason(Score, Reasons, 3, "near camera");

		if (ActorTextMatchesTokens(Actor, Config.PlayerFilter))
		{
			const bool HasClassSignal = Actor.IsPawn || Actor.IsCharacter || Actor.IsGameCharacter || Actor.IsGameEnemy || Actor.IsGamePlayer || Actor.HasPlayerState;
			AddScoreReason(Score, Reasons, HasClassSignal ? 22 : 8, HasClassSignal ? "player token" : "weak player token");
		}
		if (ActorTextMatchesTokens(Actor, Config.NonPlayerFilter))
			AddScoreReason(Score, Reasons, -35, "non-player token");
		if (Actor.IsCameraActor)
			AddScoreReason(Score, Reasons, -45, "camera actor");
		if (Actor.IsItem)
			AddScoreReason(Score, Reasons, -25, "item actor");
		if (Actor.IsWeapon)
			AddScoreReason(Score, Reasons, -25, "weapon actor");
		if (Actor.IsVehicle)
			AddScoreReason(Score, Reasons, -20, "vehicle actor");
		if (Actor.IsObjective)
			AddScoreReason(Score, Reasons, -20, "objective actor");
		if (Actor.IsEnvironment)
			AddScoreReason(Score, Reasons, -40, "environment actor");
		if (Actor.IsLocalPlayer)
			AddScoreReason(Score, Reasons, -100, "local pawn");

		if (OutReasons)
			*OutReasons = Reasons.empty() ? "no strong player signals" : Reasons;

		return std::clamp(Score, -100, 100);
	}

	void UpdateLikelyPlayerScore(ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		Actor.PlayerScore = ScoreLikelyPlayerActor(Actor, Config, &Actor.PlayerScoreReasons);
		Actor.IsLikelyPlayer = Actor.PlayerScore >= Config.LikelyPlayerScoreThreshold;
	}

	const char* ActorKindText(const ActorDebugInfo& Actor)
	{
		if (Actor.IsLocalPlayer)
			return "Local";
		if (Actor.IsRuntimePlayer)
			return "Runtime";
		if (Actor.HasPlayerState)
			return "PlayerState";
		if (Actor.IsGamePlayer)
			return "GamePlayer";
		if (Actor.IsGameEnemy)
			return "GameEnemy";
		if (Actor.IsGameCharacter)
			return "GameChar";
		if (Actor.IsCivilian)
			return "Civilian";
		if (Actor.IsNPC)
			return "NPC";
		if (Actor.IsBot)
			return "Bot";
		if (Actor.IsAI)
			return "AI";
		if (Actor.IsCharacter)
			return "Character";
		if (Actor.IsPawn)
			return "Pawn";
		if (Actor.IsCameraActor)
			return "Camera";
		if (Actor.IsObjective)
			return "Objective";
		if (Actor.IsVehicle)
			return "Vehicle";
		if (Actor.IsWeapon)
			return "Weapon";
		if (Actor.IsItem)
			return "Item";
		if (Actor.IsLikelyPlayer)
			return "Likely";
		if (Actor.IsEnvironment)
			return "World";
		return "Actor";
	}

	void AppendActorFlag(std::string& Flags, const char* Label)
	{
		if (!Flags.empty())
			Flags += ", ";
		Flags += Label;
	}

	std::string ActorFlagText(const ActorDebugInfo& Actor)
	{
		std::string Flags;
		if (Actor.IsRuntimePlayer)
			AppendActorFlag(Flags, "Runtime");
		if (Actor.HasPlayerState)
			AppendActorFlag(Flags, "PlayerState");
		if (Actor.IsGamePlayer)
			AppendActorFlag(Flags, "GamePlayer");
		if (Actor.IsGameEnemy)
			AppendActorFlag(Flags, "GameEnemy");
		if (Actor.IsGameCharacter)
			AppendActorFlag(Flags, "GameChar");
		if (Actor.IsBot)
			AppendActorFlag(Flags, "Bot");
		if (Actor.IsNPC)
			AppendActorFlag(Flags, "NPC");
		if (Actor.IsCivilian)
			AppendActorFlag(Flags, "Civilian");
		if (Actor.IsAI)
			AppendActorFlag(Flags, "AI");
		if (Actor.IsCameraActor)
			AppendActorFlag(Flags, "Camera");
		if (Actor.IsItem)
			AppendActorFlag(Flags, "Item");
		if (Actor.IsWeapon)
			AppendActorFlag(Flags, "Weapon");
		if (Actor.IsVehicle)
			AppendActorFlag(Flags, "Vehicle");
		if (Actor.IsObjective)
			AppendActorFlag(Flags, "Objective");
		if (Actor.IsEnvironment)
			AppendActorFlag(Flags, "Environment");
		return Flags.empty() ? "-" : Flags;
	}

	OverlayConfig GetConfigSnapshot()
	{
		std::scoped_lock Lock(gConfigMutex);
		return gConfig;
	}

	void SetStatus(std::string Status)
	{
		std::scoped_lock Lock(gActorMutex);
		gStats.Status = std::move(Status);
	}

	bool ReadConfigBool(const char* Key, bool DefaultValue)
	{
		return GetPrivateProfileIntA("DebugOverlay", Key, DefaultValue ? 1 : 0, Settings::GlobalConfigPath) != 0;
	}

	int ReadConfigInt(const char* Key, int DefaultValue)
	{
		return GetPrivateProfileIntA("DebugOverlay", Key, DefaultValue, Settings::GlobalConfigPath);
	}

	float ReadConfigFloat(const char* Key, float DefaultValue)
	{
		char Buffer[64] = {};
		GetPrivateProfileStringA("DebugOverlay", Key, "", Buffer, sizeof(Buffer), Settings::GlobalConfigPath);
		if (Buffer[0] == '\0')
			return DefaultValue;

		return static_cast<float>(std::atof(Buffer));
	}

	ImVec4 ReadConfigColor(const char* Key, ImVec4 DefaultValue)
	{
		char Buffer[128] = {};
		GetPrivateProfileStringA("DebugOverlay", Key, "", Buffer, sizeof(Buffer), Settings::GlobalConfigPath);
		if (Buffer[0] == '\0')
			return DefaultValue;

		ImVec4 Value = DefaultValue;
		if (sscanf_s(Buffer, "%f,%f,%f,%f", &Value.x, &Value.y, &Value.z, &Value.w) == 4)
			return Value;

		return DefaultValue;
	}

	void WriteConfigBool(const char* Key, bool Value)
	{
		WritePrivateProfileStringA("DebugOverlay", Key, Value ? "1" : "0", Settings::GlobalConfigPath);
	}

	void WriteConfigInt(const char* Key, int Value)
	{
		const std::string Text = std::to_string(Value);
		WritePrivateProfileStringA("DebugOverlay", Key, Text.c_str(), Settings::GlobalConfigPath);
	}

	void WriteConfigFloat(const char* Key, float Value)
	{
		char Buffer[64] = {};
		std::snprintf(Buffer, sizeof(Buffer), "%.4f", Value);
		WritePrivateProfileStringA("DebugOverlay", Key, Buffer, Settings::GlobalConfigPath);
	}

	void WriteConfigColor(const char* Key, ImVec4 Value)
	{
		char Buffer[128] = {};
		std::snprintf(Buffer, sizeof(Buffer), "%.4f,%.4f,%.4f,%.4f", Value.x, Value.y, Value.z, Value.w);
		WritePrivateProfileStringA("DebugOverlay", Key, Buffer, Settings::GlobalConfigPath);
	}

	void LoadOverlayConfig()
	{
		CreateDirectoryA("C:\\Dumper-7", nullptr);
		const int LoadedConfigVersion = ReadConfigInt("ConfigVersion", 0);

		std::scoped_lock Lock(gConfigMutex);
		gConfig.Enabled = ReadConfigBool("Enabled", gConfig.Enabled);
		gConfig.DrawLines = ReadConfigBool("DrawLines", gConfig.DrawLines);
		gConfig.DrawBoxes = ReadConfigBool("DrawBoxes", gConfig.DrawBoxes);
		gConfig.DrawNames = ReadConfigBool("DrawNames", gConfig.DrawNames);
		gConfig.DrawDistance = ReadConfigBool("DrawDistance", gConfig.DrawDistance);
		gConfig.DrawBounds = ReadConfigBool("DrawBounds", gConfig.DrawBounds);
		gConfig.DrawCenterDot = ReadConfigBool("DrawCenterDot", gConfig.DrawCenterDot);
		gConfig.DrawCrosshair = ReadConfigBool("DrawCrosshair", gConfig.DrawCrosshair);
		gConfig.DrawTargetPreview = ReadConfigBool("DrawTargetPreview", gConfig.DrawTargetPreview);
		gConfig.DrawSkeletons = ReadConfigBool("DrawSkeletons", gConfig.DrawSkeletons);
		gConfig.DrawSkeletonBoneIds = ReadConfigBool("DrawSkeletonBoneIds", gConfig.DrawSkeletonBoneIds);
		gConfig.DrawSkeletonBoneNames = ReadConfigBool("DrawSkeletonBoneNames", gConfig.DrawSkeletonBoneNames);
		gConfig.UseReflectedPositionFallback = ReadConfigBool("UseReflectedPositionFallback", gConfig.UseReflectedPositionFallback);
		gConfig.CaptureOnRenderFrame = ReadConfigBool("CaptureOnRenderFrame", gConfig.CaptureOnRenderFrame);
		gConfig.OnlyOnScreen = ReadConfigBool("OnlyOnScreen", gConfig.OnlyOnScreen);
		gConfig.OnlyWithLocation = ReadConfigBool("OnlyWithLocation", gConfig.OnlyWithLocation);
		gConfig.OnlyInView = ReadConfigBool("OnlyInView", gConfig.OnlyInView);
		gConfig.ExternalOverlayOnStreamline = ReadConfigBool("ExternalOverlayOnStreamline", gConfig.ExternalOverlayOnStreamline);
		gConfig.UseProjectionFallback = ReadConfigBool("UseProjectionFallback", gConfig.UseProjectionFallback);
		gConfig.ClampLargeBoxes = ReadConfigBool("ClampLargeBoxes", gConfig.ClampLargeBoxes);
		gConfig.HideEnvironmentActors = ReadConfigBool("HideEnvironmentActors", gConfig.HideEnvironmentActors);
		gConfig.HideLocalPlayer = ReadConfigBool("HideLocalPlayer", gConfig.HideLocalPlayer);
		gConfig.HideBots = ReadConfigBool("HideBots", gConfig.HideBots);
		gConfig.HideNPCs = ReadConfigBool("HideNPCs", gConfig.HideNPCs);
		gConfig.HideCivilians = ReadConfigBool("HideCivilians", gConfig.HideCivilians);
		gConfig.HideAI = ReadConfigBool("HideAI", gConfig.HideAI);
		gConfig.HideCameras = ReadConfigBool("HideCameras", gConfig.HideCameras);
		gConfig.HideItems = ReadConfigBool("HideItems", gConfig.HideItems);
		gConfig.HideWeapons = ReadConfigBool("HideWeapons", gConfig.HideWeapons);
		gConfig.HideVehicles = ReadConfigBool("HideVehicles", gConfig.HideVehicles);
		gConfig.HideObjectives = ReadConfigBool("HideObjectives", gConfig.HideObjectives);
		gConfig.UseRuntimePlayerContext = ReadConfigBool("UseRuntimePlayerContext", gConfig.UseRuntimePlayerContext);
		gConfig.PreferRuntimePlayers = ReadConfigBool("PreferRuntimePlayers", gConfig.PreferRuntimePlayers);
		gConfig.IncludeGameStatePlayers = ReadConfigBool("IncludeGameStatePlayers", gConfig.IncludeGameStatePlayers);
		gConfig.LockLikelyPlayerClasses = ReadConfigBool("LockLikelyPlayerClasses", gConfig.LockLikelyPlayerClasses);
		gConfig.FastOverlayMode = ReadConfigBool("FastOverlayMode", gConfig.FastOverlayMode);
		gConfig.ProbeReflectedPositionsOnLocatedActors = ReadConfigBool("ProbeReflectedPositionsOnLocatedActors", gConfig.ProbeReflectedPositionsOnLocatedActors);
		gConfig.ThrottleLiveReflectionFallback = ReadConfigBool("ThrottleLiveReflectionFallback", gConfig.ThrottleLiveReflectionFallback);
		gConfig.EnableClassFilter = ReadConfigBool("EnableClassFilter", gConfig.EnableClassFilter);
		gConfig.DeveloperShowInheritedMembers = ReadConfigBool("DeveloperShowInheritedMembers", gConfig.DeveloperShowInheritedMembers);
		gConfig.EnableDeveloperOptions = ReadConfigBool("EnableDeveloperOptions", gConfig.EnableDeveloperOptions);
		gConfig.DeveloperAutoCycleClasses = gConfig.EnableDeveloperOptions && ReadConfigBool("DeveloperAutoCycleClasses", gConfig.DeveloperAutoCycleClasses);
		gConfig.DeveloperPreviewEnabled = gConfig.EnableDeveloperOptions && ReadConfigBool("DeveloperPreviewEnabled", gConfig.DeveloperPreviewEnabled);
		gConfig.DeveloperPreviewDrawLines = ReadConfigBool("DeveloperPreviewDrawLines", gConfig.DeveloperPreviewDrawLines);
		gConfig.DeveloperPreviewDrawBoxes = ReadConfigBool("DeveloperPreviewDrawBoxes", gConfig.DeveloperPreviewDrawBoxes);
		gConfig.ActorSource = std::clamp(ReadConfigInt("ActorSource", gConfig.ActorSource), 0, 3);
		gConfig.TargetMode = std::clamp(ReadConfigInt("TargetMode", gConfig.TargetMode), 0, 6);
		if (!gConfig.EnableDeveloperOptions && gConfig.TargetMode == 0)
			gConfig.TargetMode = 1;
		gConfig.ProjectionSpace = std::clamp(ReadConfigInt("ProjectionSpace", gConfig.ProjectionSpace), 0, 2);
		gConfig.ProjectionRoute = std::clamp(ReadConfigInt("ProjectionRoute", gConfig.ProjectionRoute), 0, 2);
		gConfig.BoundsMode = std::clamp(ReadConfigInt("BoundsMode", gConfig.BoundsMode), 0, 3);
		gConfig.RendererRoute = std::clamp(ReadConfigInt("RendererRoute", gConfig.RendererRoute), 0, 2);
		gConfig.LineOrigin = std::clamp(ReadConfigInt("LineOrigin", gConfig.LineOrigin), 0, 2);
		gConfig.LineTarget = std::clamp(ReadConfigInt("LineTarget", gConfig.LineTarget), 0, 2);
		gConfig.RefreshMs = std::clamp(ReadConfigInt("RefreshMs", gConfig.RefreshMs), 250, 10000);
		gConfig.FrameCaptureMinMs = std::clamp(ReadConfigInt("FrameCaptureMinMs", gConfig.FrameCaptureMinMs), 0, 100);
		gConfig.FrameProjectionMaxActors = std::clamp(ReadConfigInt("FrameProjectionMaxActors", gConfig.FrameProjectionMaxActors), 16, 4096);
		gConfig.FrameSkeletonMinMs = std::clamp(ReadConfigInt("FrameSkeletonMinMs", gConfig.FrameSkeletonMinMs), 0, 3000);
		gConfig.ReflectedPositionRefreshMs = std::clamp(ReadConfigInt("ReflectedPositionRefreshMs", gConfig.ReflectedPositionRefreshMs), 0, 3000);
		gConfig.MaxActors = std::clamp(ReadConfigInt("MaxActors", gConfig.MaxActors), 1, 4096);
		gConfig.DeveloperMaxRows = std::clamp(ReadConfigInt("DeveloperMaxRows", gConfig.DeveloperMaxRows), 10, 500);
		gConfig.DeveloperClassCycleMs = std::clamp(ReadConfigInt("DeveloperClassCycleMs", gConfig.DeveloperClassCycleMs), 250, 10000);
		gConfig.LikelyPlayerScoreThreshold = std::clamp(ReadConfigInt("LikelyPlayerScoreThreshold", gConfig.LikelyPlayerScoreThreshold), 0, 100);
		gConfig.LikelyClassLockMinActors = std::clamp(ReadConfigInt("LikelyClassLockMinActors", gConfig.LikelyClassLockMinActors), 1, 32);
		gConfig.LikelyClassLockMaxClasses = std::clamp(ReadConfigInt("LikelyClassLockMaxClasses", gConfig.LikelyClassLockMaxClasses), 1, 16);
		gConfig.PositionProbeMaxFields = std::clamp(ReadConfigInt("PositionProbeMaxFields", gConfig.PositionProbeMaxFields), 8, 500);
		gConfig.SkeletonMaxBones = std::clamp(ReadConfigInt("SkeletonMaxBones", gConfig.SkeletonMaxBones), 4, 256);
		gConfig.MaxDistanceMeters = std::max(0.0f, ReadConfigFloat("MaxDistanceMeters", gConfig.MaxDistanceMeters));
		gConfig.CrosshairSize = std::clamp(ReadConfigFloat("CrosshairSize", gConfig.CrosshairSize), 1.0f, 80.0f);
		gConfig.CrosshairGap = std::clamp(ReadConfigFloat("CrosshairGap", gConfig.CrosshairGap), 0.0f, 40.0f);
		gConfig.CrosshairThickness = std::clamp(ReadConfigFloat("CrosshairThickness", gConfig.CrosshairThickness), 0.5f, 12.0f);
		gConfig.TargetPreviewRadius = std::clamp(ReadConfigFloat("TargetPreviewRadius", gConfig.TargetPreviewRadius), 25.0f, 2000.0f);
		gConfig.TargetPreviewLineThickness = std::clamp(ReadConfigFloat("TargetPreviewLineThickness", gConfig.TargetPreviewLineThickness), 0.5f, 12.0f);
		gConfig.SkeletonThickness = std::clamp(ReadConfigFloat("SkeletonThickness", gConfig.SkeletonThickness), 0.5f, 12.0f);
		gConfig.SkeletonPointRadius = std::clamp(ReadConfigFloat("SkeletonPointRadius", gConfig.SkeletonPointRadius), 0.0f, 12.0f);
		gConfig.ProjectionOffsetX = std::clamp(ReadConfigFloat("ProjectionOffsetX", gConfig.ProjectionOffsetX), -4000.0f, 4000.0f);
		gConfig.ProjectionOffsetY = std::clamp(ReadConfigFloat("ProjectionOffsetY", gConfig.ProjectionOffsetY), -4000.0f, 4000.0f);
		gConfig.ProjectionScaleX = std::clamp(ReadConfigFloat("ProjectionScaleX", gConfig.ProjectionScaleX), 0.10f, 4.0f);
		gConfig.ProjectionScaleY = std::clamp(ReadConfigFloat("ProjectionScaleY", gConfig.ProjectionScaleY), 0.10f, 4.0f);
		gConfig.LineThickness = std::clamp(ReadConfigFloat("LineThickness", gConfig.LineThickness), 0.5f, 12.0f);
		gConfig.BoxThickness = std::clamp(ReadConfigFloat("BoxThickness", gConfig.BoxThickness), 0.5f, 12.0f);
		gConfig.BoxWidthRatio = std::clamp(ReadConfigFloat("BoxWidthRatio", gConfig.BoxWidthRatio), 0.1f, 2.0f);
		gConfig.MaxBoxScreenFraction = std::clamp(ReadConfigFloat("MaxBoxScreenFraction", gConfig.MaxBoxScreenFraction), 0.15f, 1.0f);
		if (gConfig.FastOverlayMode)
			gConfig.MaxBoxScreenFraction = std::min(gConfig.MaxBoxScreenFraction, 0.40f);
		gConfig.BoxPaddingPixels = std::clamp(ReadConfigFloat("BoxPaddingPixels", gConfig.BoxPaddingPixels), 0.0f, 80.0f);
		gConfig.MinBoxHeightPixels = std::clamp(ReadConfigFloat("MinBoxHeightPixels", gConfig.MinBoxHeightPixels), 0.0f, 250.0f);
		gConfig.FallbackHalfHeight = std::clamp(ReadConfigFloat("FallbackHalfHeight", gConfig.FallbackHalfHeight), 5.0f, 500.0f);
		gConfig.FallbackHalfWidth = std::clamp(ReadConfigFloat("FallbackHalfWidth", gConfig.FallbackHalfWidth), 5.0f, 500.0f);
		gConfig.BoxColor = ReadConfigColor("BoxColor", gConfig.BoxColor);
		gConfig.LineColor = ReadConfigColor("LineColor", gConfig.LineColor);
		gConfig.TextColor = ReadConfigColor("TextColor", gConfig.TextColor);
		gConfig.BoundsColor = ReadConfigColor("BoundsColor", gConfig.BoundsColor);
		gConfig.CrosshairColor = ReadConfigColor("CrosshairColor", gConfig.CrosshairColor);
		gConfig.TargetPreviewColor = ReadConfigColor("TargetPreviewColor", gConfig.TargetPreviewColor);
		gConfig.SkeletonColor = ReadConfigColor("SkeletonColor", gConfig.SkeletonColor);
		GetPrivateProfileStringA("DebugOverlay", "Filter", gConfig.Filter, gConfig.Filter, sizeof(gConfig.Filter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ExcludeFilter", gConfig.ExcludeFilter, gConfig.ExcludeFilter, sizeof(gConfig.ExcludeFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "EnvironmentFilter", DefaultEnvironmentTokens(), gConfig.EnvironmentFilter, sizeof(gConfig.EnvironmentFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "BotFilter", DefaultBotTokens(), gConfig.BotFilter, sizeof(gConfig.BotFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "NpcFilter", DefaultNpcTokens(), gConfig.NpcFilter, sizeof(gConfig.NpcFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "CivilianFilter", DefaultCivilianTokens(), gConfig.CivilianFilter, sizeof(gConfig.CivilianFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "AiFilter", DefaultAiTokens(), gConfig.AiFilter, sizeof(gConfig.AiFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "PlayerFilter", DefaultPlayerTokens(), gConfig.PlayerFilter, sizeof(gConfig.PlayerFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "NonPlayerFilter", DefaultNonPlayerTokens(), gConfig.NonPlayerFilter, sizeof(gConfig.NonPlayerFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "PositionFieldFilter", DefaultPositionFieldTokens(), gConfig.PositionFieldFilter, sizeof(gConfig.PositionFieldFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "CameraFilter", DefaultCameraTokens(), gConfig.CameraFilter, sizeof(gConfig.CameraFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ItemFilter", DefaultItemTokens(), gConfig.ItemFilter, sizeof(gConfig.ItemFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "WeaponFilter", DefaultWeaponTokens(), gConfig.WeaponFilter, sizeof(gConfig.WeaponFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "VehicleFilter", DefaultVehicleTokens(), gConfig.VehicleFilter, sizeof(gConfig.VehicleFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ObjectiveFilter", DefaultObjectiveTokens(), gConfig.ObjectiveFilter, sizeof(gConfig.ObjectiveFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ClassFilter", gConfig.ClassFilter, gConfig.ClassFilter, sizeof(gConfig.ClassFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ClassExcludeFilter", gConfig.ClassExcludeFilter, gConfig.ClassExcludeFilter, sizeof(gConfig.ClassExcludeFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "DeveloperPreviewClassFilter", gConfig.DeveloperPreviewClassFilter, gConfig.DeveloperPreviewClassFilter, sizeof(gConfig.DeveloperPreviewClassFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "DeveloperProbeFilter", gConfig.DeveloperProbeFilter, gConfig.DeveloperProbeFilter, sizeof(gConfig.DeveloperProbeFilter), Settings::GlobalConfigPath);

		if (LoadedConfigVersion < kOverlayConfigVersion)
			ResetMainOverlayTargetingDefaults(gConfig);
		EnsureRuntimeTokenDefaults(gConfig);
	}

	void SaveOverlayConfig()
	{
		CreateDirectoryA("C:\\Dumper-7", nullptr);

		const OverlayConfig Config = GetConfigSnapshot();
		WriteConfigInt("ConfigVersion", kOverlayConfigVersion);
		WriteConfigBool("Enabled", Config.Enabled);
		WriteConfigBool("DrawLines", Config.DrawLines);
		WriteConfigBool("DrawBoxes", Config.DrawBoxes);
		WriteConfigBool("DrawNames", Config.DrawNames);
		WriteConfigBool("DrawDistance", Config.DrawDistance);
		WriteConfigBool("DrawBounds", Config.DrawBounds);
		WriteConfigBool("DrawCenterDot", Config.DrawCenterDot);
		WriteConfigBool("DrawCrosshair", Config.DrawCrosshair);
		WriteConfigBool("DrawTargetPreview", Config.DrawTargetPreview);
		WriteConfigBool("DrawSkeletons", Config.DrawSkeletons);
		WriteConfigBool("DrawSkeletonBoneIds", Config.DrawSkeletonBoneIds);
		WriteConfigBool("DrawSkeletonBoneNames", Config.DrawSkeletonBoneNames);
		WriteConfigBool("UseReflectedPositionFallback", Config.UseReflectedPositionFallback);
		WriteConfigBool("CaptureOnRenderFrame", Config.CaptureOnRenderFrame);
		WriteConfigBool("OnlyOnScreen", Config.OnlyOnScreen);
		WriteConfigBool("OnlyWithLocation", Config.OnlyWithLocation);
		WriteConfigBool("OnlyInView", Config.OnlyInView);
		WriteConfigBool("ExternalOverlayOnStreamline", Config.ExternalOverlayOnStreamline);
		WriteConfigBool("UseProjectionFallback", Config.UseProjectionFallback);
	WriteConfigBool("ClampLargeBoxes", Config.ClampLargeBoxes);
	WriteConfigBool("HideEnvironmentActors", Config.HideEnvironmentActors);
	WriteConfigBool("HideLocalPlayer", Config.HideLocalPlayer);
	WriteConfigBool("HideBots", Config.HideBots);
	WriteConfigBool("HideNPCs", Config.HideNPCs);
	WriteConfigBool("HideCivilians", Config.HideCivilians);
	WriteConfigBool("HideAI", Config.HideAI);
	WriteConfigBool("HideCameras", Config.HideCameras);
	WriteConfigBool("HideItems", Config.HideItems);
	WriteConfigBool("HideWeapons", Config.HideWeapons);
	WriteConfigBool("HideVehicles", Config.HideVehicles);
	WriteConfigBool("HideObjectives", Config.HideObjectives);
	WriteConfigBool("UseRuntimePlayerContext", Config.UseRuntimePlayerContext);
	WriteConfigBool("PreferRuntimePlayers", Config.PreferRuntimePlayers);
	WriteConfigBool("IncludeGameStatePlayers", Config.IncludeGameStatePlayers);
	WriteConfigBool("LockLikelyPlayerClasses", Config.LockLikelyPlayerClasses);
	WriteConfigBool("FastOverlayMode", Config.FastOverlayMode);
	WriteConfigBool("ProbeReflectedPositionsOnLocatedActors", Config.ProbeReflectedPositionsOnLocatedActors);
	WriteConfigBool("ThrottleLiveReflectionFallback", Config.ThrottleLiveReflectionFallback);
		WriteConfigBool("EnableClassFilter", Config.EnableClassFilter);
		WriteConfigBool("DeveloperAutoCycleClasses", Config.DeveloperAutoCycleClasses);
		WriteConfigBool("DeveloperPreviewEnabled", Config.DeveloperPreviewEnabled);
		WriteConfigBool("DeveloperPreviewDrawLines", Config.DeveloperPreviewDrawLines);
		WriteConfigBool("DeveloperPreviewDrawBoxes", Config.DeveloperPreviewDrawBoxes);
		WriteConfigBool("DeveloperShowInheritedMembers", Config.DeveloperShowInheritedMembers);
	WriteConfigBool("EnableDeveloperOptions", Config.EnableDeveloperOptions);
		WriteConfigInt("ActorSource", Config.ActorSource);
		WriteConfigInt("TargetMode", Config.TargetMode);
		WriteConfigInt("ProjectionSpace", Config.ProjectionSpace);
		WriteConfigInt("ProjectionRoute", Config.ProjectionRoute);
		WriteConfigInt("BoundsMode", Config.BoundsMode);
		WriteConfigInt("RendererRoute", Config.RendererRoute);
		WriteConfigInt("LineOrigin", Config.LineOrigin);
		WriteConfigInt("LineTarget", Config.LineTarget);
		WriteConfigInt("RefreshMs", Config.RefreshMs);
		WriteConfigInt("FrameCaptureMinMs", Config.FrameCaptureMinMs);
		WriteConfigInt("FrameProjectionMaxActors", Config.FrameProjectionMaxActors);
		WriteConfigInt("FrameSkeletonMinMs", Config.FrameSkeletonMinMs);
	WriteConfigInt("ReflectedPositionRefreshMs", Config.ReflectedPositionRefreshMs);
	WriteConfigInt("MaxActors", Config.MaxActors);
	WriteConfigInt("DeveloperMaxRows", Config.DeveloperMaxRows);
	WriteConfigInt("DeveloperClassCycleMs", Config.DeveloperClassCycleMs);
	WriteConfigInt("LikelyPlayerScoreThreshold", Config.LikelyPlayerScoreThreshold);
	WriteConfigInt("LikelyClassLockMinActors", Config.LikelyClassLockMinActors);
	WriteConfigInt("LikelyClassLockMaxClasses", Config.LikelyClassLockMaxClasses);
	WriteConfigInt("PositionProbeMaxFields", Config.PositionProbeMaxFields);
	WriteConfigInt("SkeletonMaxBones", Config.SkeletonMaxBones);
	WriteConfigFloat("MaxDistanceMeters", Config.MaxDistanceMeters);
		WriteConfigFloat("CrosshairSize", Config.CrosshairSize);
		WriteConfigFloat("CrosshairGap", Config.CrosshairGap);
		WriteConfigFloat("CrosshairThickness", Config.CrosshairThickness);
		WriteConfigFloat("TargetPreviewRadius", Config.TargetPreviewRadius);
		WriteConfigFloat("TargetPreviewLineThickness", Config.TargetPreviewLineThickness);
		WriteConfigFloat("SkeletonThickness", Config.SkeletonThickness);
		WriteConfigFloat("SkeletonPointRadius", Config.SkeletonPointRadius);
		WriteConfigFloat("ProjectionOffsetX", Config.ProjectionOffsetX);
		WriteConfigFloat("ProjectionOffsetY", Config.ProjectionOffsetY);
		WriteConfigFloat("ProjectionScaleX", Config.ProjectionScaleX);
		WriteConfigFloat("ProjectionScaleY", Config.ProjectionScaleY);
		WriteConfigFloat("LineThickness", Config.LineThickness);
		WriteConfigFloat("BoxThickness", Config.BoxThickness);
		WriteConfigFloat("BoxWidthRatio", Config.BoxWidthRatio);
		WriteConfigFloat("MaxBoxScreenFraction", Config.MaxBoxScreenFraction);
		WriteConfigFloat("BoxPaddingPixels", Config.BoxPaddingPixels);
		WriteConfigFloat("MinBoxHeightPixels", Config.MinBoxHeightPixels);
		WriteConfigFloat("FallbackHalfHeight", Config.FallbackHalfHeight);
		WriteConfigFloat("FallbackHalfWidth", Config.FallbackHalfWidth);
		WriteConfigColor("BoxColor", Config.BoxColor);
		WriteConfigColor("LineColor", Config.LineColor);
		WriteConfigColor("TextColor", Config.TextColor);
		WriteConfigColor("BoundsColor", Config.BoundsColor);
		WriteConfigColor("CrosshairColor", Config.CrosshairColor);
		WriteConfigColor("TargetPreviewColor", Config.TargetPreviewColor);
		WriteConfigColor("SkeletonColor", Config.SkeletonColor);
	WritePrivateProfileStringA("DebugOverlay", "Filter", Config.Filter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ExcludeFilter", Config.ExcludeFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "EnvironmentFilter", Config.EnvironmentFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "BotFilter", Config.BotFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "NpcFilter", Config.NpcFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "CivilianFilter", Config.CivilianFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "AiFilter", Config.AiFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "PlayerFilter", Config.PlayerFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "NonPlayerFilter", Config.NonPlayerFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "PositionFieldFilter", Config.PositionFieldFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "CameraFilter", Config.CameraFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ItemFilter", Config.ItemFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "WeaponFilter", Config.WeaponFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "VehicleFilter", Config.VehicleFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ObjectiveFilter", Config.ObjectiveFilter, Settings::GlobalConfigPath);
		WritePrivateProfileStringA("DebugOverlay", "ClassFilter", Config.ClassFilter, Settings::GlobalConfigPath);
		WritePrivateProfileStringA("DebugOverlay", "ClassExcludeFilter", Config.ClassExcludeFilter, Settings::GlobalConfigPath);
		WritePrivateProfileStringA("DebugOverlay", "DeveloperPreviewClassFilter", Config.DeveloperPreviewClassFilter, Settings::GlobalConfigPath);
		WritePrivateProfileStringA("DebugOverlay", "DeveloperProbeFilter", Config.DeveloperProbeFilter, Settings::GlobalConfigPath);
	}

	bool PatchVTable(void** VTable, size_t Index, void* Hook, void** Original)
	{
		if (!VTable || !Hook || !Original)
			return false;

		DWORD OldProtect = 0;
		if (!VirtualProtect(&VTable[Index], sizeof(void*), PAGE_EXECUTE_READWRITE, &OldProtect))
			return false;

		*Original = VTable[Index];
		VTable[Index] = Hook;

		DWORD Ignored = 0;
		VirtualProtect(&VTable[Index], sizeof(void*), OldProtect, &Ignored);
		return true;
	}

	bool RestoreVTable(void** VTable, size_t Index, void* Original)
	{
		if (!VTable || !Original)
			return false;

		DWORD OldProtect = 0;
		if (!VirtualProtect(&VTable[Index], sizeof(void*), PAGE_EXECUTE_READWRITE, &OldProtect))
			return false;

		VTable[Index] = Original;

		DWORD Ignored = 0;
		VirtualProtect(&VTable[Index], sizeof(void*), OldProtect, &Ignored);
		return true;
	}

	UEFunction FindFunctionByName(UEClass Class, const char* Name)
	{
		if (!Class || !Name)
			return {};

		for (UEStruct Struct = Class; Struct; Struct = Struct.GetSuper())
		{
			for (UEFunction Function : Struct.GetFunctions())
			{
				if (Function.GetName() == Name)
					return Function;
			}
		}

		return {};
	}

	UEFunction FindFirstFunction(UEClass Class, std::initializer_list<const char*> Names)
	{
		for (const char* Name : Names)
		{
			UEFunction Function = FindFunctionByName(Class, Name);
			if (Function)
				return Function;
		}

		return {};
	}

	UEProperty FindFirstProperty(UEFunction Function, std::initializer_list<const char*> Names)
	{
		if (!Function)
			return {};

		for (const char* Name : Names)
		{
			UEProperty Property = Function.FindMember(Name);
			if (Property)
				return Property;
		}

		return {};
	}

	UEProperty FindFirstMember(UEStruct Struct, std::initializer_list<const char*> Names)
	{
		if (!Struct)
			return {};

		for (const char* Name : Names)
		{
			UEProperty Property = Struct.FindMember(Name);
			if (Property)
				return Property;
		}

		return {};
	}

	UEProperty FindFirstMemberOfType(UEStruct Struct, std::initializer_list<const char*> Names, EClassCastFlags Type)
	{
		if (!Struct)
			return {};

		for (const char* Name : Names)
		{
			UEProperty Property = Struct.FindMember(Name, Type);
			if (Property)
				return Property;
		}

		return {};
	}

	bool EnsureParamSize(std::vector<uint8>& Params, UEProperty Property)
	{
		if (!Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = std::max(Property.GetSize(), 1);
		if (Offset < 0 || Size <= 0)
			return false;

		if (static_cast<size_t>(Offset + Size) > Params.size())
			Params.resize(static_cast<size_t>(Offset + Size), 0);

		return true;
	}

	bool IsUsefulBounds(const Vec3& Origin, const Vec3& Extent, float SphereRadius)
	{
		if (!std::isfinite(Origin.X) || !std::isfinite(Origin.Y) || !std::isfinite(Origin.Z)
			|| !std::isfinite(Extent.X) || !std::isfinite(Extent.Y) || !std::isfinite(Extent.Z)
			|| !std::isfinite(SphereRadius))
		{
			return false;
		}

		const double AbsX = std::abs(Extent.X);
		const double AbsY = std::abs(Extent.Y);
		const double AbsZ = std::abs(Extent.Z);
		const double MaxExtent = std::max({ AbsX, AbsY, AbsZ });
		const double MinExtent = std::min({ AbsX, AbsY, AbsZ });
		return MaxExtent >= 1.0
			&& MaxExtent <= 5000.0
			&& MinExtent >= 0.0;
	}

	std::vector<uint8> MakeParamBuffer(UEFunction Function)
	{
		const int32 Size = Function ? Function.GetStructSize() : 0;
		return std::vector<uint8>(static_cast<size_t>(std::max(Size, 1)), 0);
	}

	bool WriteBool(std::vector<uint8>& Params, UEProperty Property, bool Value)
	{
		if (!EnsureParamSize(Params, Property))
			return false;

		const int32 Offset = Property.GetOffset();
		if (!Property.IsA(EClassCastFlags::BoolProperty))
		{
			Params[Offset] = Value ? 1 : 0;
			return true;
		}

		UEBoolProperty BoolProperty = Property.Cast<UEBoolProperty>();
		if (BoolProperty.IsNativeBool())
		{
			Params[Offset] = Value ? 1 : 0;
			return true;
		}

		const size_t ByteIndex = static_cast<size_t>(Offset + BoolProperty.GetByteOffset());
		if (ByteIndex >= Params.size())
			Params.resize(ByteIndex + 1, 0);

		const uint8 Mask = BoolProperty.GetFieldMask();
		if (Value)
			Params[ByteIndex] |= Mask;
		else
			Params[ByteIndex] &= ~Mask;

		return true;
	}

	bool WriteInteger(std::vector<uint8>& Params, UEProperty Property, int64 Value)
	{
		if (!EnsureParamSize(Params, Property))
			return false;

		if (Property.IsA(EClassCastFlags::EnumProperty))
			Property = Property.Cast<UEEnumProperty>().GetUnderlayingProperty();

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		uint8* Data = Params.data() + Offset;
		if (Size >= static_cast<int32>(sizeof(int64)))
			*reinterpret_cast<int64*>(Data) = Value;
		else if (Size >= static_cast<int32>(sizeof(int32)))
			*reinterpret_cast<int32*>(Data) = static_cast<int32>(Value);
		else if (Size >= static_cast<int32>(sizeof(int16)))
			*reinterpret_cast<int16*>(Data) = static_cast<int16>(Value);
		else
			*reinterpret_cast<uint8*>(Data) = static_cast<uint8>(Value);

		return true;
	}

	bool ReadBool(const std::vector<uint8>& Params, UEProperty Property, bool DefaultValue = true)
	{
		if (!Property)
			return DefaultValue;

		const int32 Offset = Property.GetOffset();
		if (Offset < 0 || static_cast<size_t>(Offset) >= Params.size())
			return DefaultValue;

		if (!Property.IsA(EClassCastFlags::BoolProperty))
			return Params[Offset] != 0;

		UEBoolProperty BoolProperty = Property.Cast<UEBoolProperty>();
		if (BoolProperty.IsNativeBool())
			return Params[Offset] != 0;

		const size_t ByteIndex = static_cast<size_t>(Offset + BoolProperty.GetByteOffset());
		if (ByteIndex >= Params.size())
			return DefaultValue;

		return (Params[ByteIndex] & BoolProperty.GetFieldMask()) != 0;
	}

	bool ReadInteger(const std::vector<uint8>& Params, UEProperty Property, int64& OutValue)
	{
		if (!Property)
			return false;

		if (Property.IsA(EClassCastFlags::EnumProperty))
			Property = Property.Cast<UEEnumProperty>().GetUnderlayingProperty();

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		const uint8* Data = Params.data() + Offset;
		if (Size >= static_cast<int32>(sizeof(int64)))
			OutValue = *reinterpret_cast<const int64*>(Data);
		else if (Size >= static_cast<int32>(sizeof(int32)))
			OutValue = *reinterpret_cast<const int32*>(Data);
		else if (Size >= static_cast<int32>(sizeof(int16)))
			OutValue = *reinterpret_cast<const int16*>(Data);
		else
			OutValue = *reinterpret_cast<const uint8*>(Data);

		return true;
	}

	bool ReadObjectValue(const std::vector<uint8>& Params, UEProperty Property, UEObject& OutObject)
	{
		if (!Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size < static_cast<int32>(sizeof(void*)) || static_cast<size_t>(Offset + sizeof(void*)) > Params.size())
			return false;

		void* ObjectPointer = *reinterpret_cast<void* const*>(Params.data() + Offset);
		if (!ObjectPointer)
			return false;

		OutObject = UEObject(ObjectPointer);
		return true;
	}

	bool WriteVector(std::vector<uint8>& Params, UEProperty Property, const Vec3& Value)
	{
		if (!EnsureParamSize(Params, Property))
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		uint8* Data = Params.data() + Offset;

		if (Size >= static_cast<int32>(sizeof(double) * 3))
		{
			double* Vector = reinterpret_cast<double*>(Data);
			Vector[0] = Value.X;
			Vector[1] = Value.Y;
			Vector[2] = Value.Z;
			return true;
		}

		if (Size >= static_cast<int32>(sizeof(float) * 3))
		{
			float* Vector = reinterpret_cast<float*>(Data);
			Vector[0] = static_cast<float>(Value.X);
			Vector[1] = static_cast<float>(Value.Y);
			Vector[2] = static_cast<float>(Value.Z);
			return true;
		}

		return false;
	}

	bool ReadVector(const std::vector<uint8>& Params, UEProperty Property, Vec3& OutValue)
	{
		if (!Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		const uint8* Data = Params.data() + Offset;
		if (Size >= static_cast<int32>(sizeof(double) * 3))
		{
			const double* Vector = reinterpret_cast<const double*>(Data);
			OutValue = { Vector[0], Vector[1], Vector[2] };
			return true;
		}

		if (Size >= static_cast<int32>(sizeof(float) * 3))
		{
			const float* Vector = reinterpret_cast<const float*>(Data);
			OutValue = { Vector[0], Vector[1], Vector[2] };
			return true;
		}

		return false;
	}

	bool ReadVector2(const std::vector<uint8>& Params, UEProperty Property, Vec2& OutValue)
	{
		if (!Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		const uint8* Data = Params.data() + Offset;
		if (Size >= static_cast<int32>(sizeof(double) * 2))
		{
			const double* Vector = reinterpret_cast<const double*>(Data);
			OutValue = { static_cast<float>(Vector[0]), static_cast<float>(Vector[1]) };
			return true;
		}

		if (Size >= static_cast<int32>(sizeof(float) * 2))
		{
			const float* Vector = reinterpret_cast<const float*>(Data);
			OutValue = { Vector[0], Vector[1] };
			return true;
		}

		return false;
	}

	bool ReadNameValue(const std::vector<uint8>& Params, UEProperty Property, RawNameValue& OutValue)
	{
		if (!Property || !Property.IsA(EClassCastFlags::NameProperty))
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || Size > static_cast<int32>(OutValue.Bytes.size()) || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		const uint8* Data = Params.data() + Offset;
		std::copy(Data, Data + Size, OutValue.Bytes.begin());
		OutValue.Size = Size;
		OutValue.Text = FName(Data).ToString();
		return true;
	}

	bool WriteNameValue(std::vector<uint8>& Params, UEProperty Property, const RawNameValue& Value)
	{
		if (!Property || !Property.IsA(EClassCastFlags::NameProperty) || Value.Size <= 0)
			return false;

		if (!EnsureParamSize(Params, Property))
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || Size > static_cast<int32>(Value.Bytes.size()) || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		std::copy(Value.Bytes.begin(), Value.Bytes.begin() + std::min(Size, Value.Size), Params.data() + Offset);
		return true;
	}

	bool ReadScalar(const std::vector<uint8>& Params, UEProperty Property, float& OutValue)
	{
		if (!Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0 || static_cast<size_t>(Offset + Size) > Params.size())
			return false;

		const uint8* Data = Params.data() + Offset;
		if (Size >= static_cast<int32>(sizeof(double)))
			OutValue = static_cast<float>(*reinterpret_cast<const double*>(Data));
		else if (Size >= static_cast<int32>(sizeof(float)))
			OutValue = *reinterpret_cast<const float*>(Data);
		else
			return false;

		return true;
	}

	bool ProcessEventWithRuntimeFlags(UEObject Target, UEFunction Function, void* Params, std::string* OutFailure = nullptr);

	bool CallNoArgVectorFunction(UEObject Object, UEFunction Function, Vec3& OutValue)
	{
		if (!Object || !Function)
			return false;

		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!ReturnProperty)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		EnsureParamSize(Params, ReturnProperty);
		if (!ProcessEventWithRuntimeFlags(Object, Function, Params.data()))
			return false;

		return ReadVector(Params, ReturnProperty, OutValue);
	}

	bool CallNoArgFloatFunction(UEObject Object, UEFunction Function, float& OutValue)
	{
		if (!Object || !Function)
			return false;

		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!ReturnProperty)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		EnsureParamSize(Params, ReturnProperty);
		if (!ProcessEventWithRuntimeFlags(Object, Function, Params.data()))
			return false;

		return ReadScalar(Params, ReturnProperty, OutValue);
	}

	bool CallNoArgIntFunction(UEObject Object, UEFunction Function, int32& OutValue)
	{
		if (!Object || !Function)
			return false;

		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!ReturnProperty)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		EnsureParamSize(Params, ReturnProperty);
		if (!ProcessEventWithRuntimeFlags(Object, Function, Params.data()))
			return false;

		int64 Value = 0;
		if (!ReadInteger(Params, ReturnProperty, Value))
			return false;

		OutValue = static_cast<int32>(Value);
		return true;
	}

	bool CallNoArgObjectFunction(UEObject Object, UEFunction Function, UEObject& OutObject)
	{
		if (!Object || !Function)
			return false;

		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!ReturnProperty)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		EnsureParamSize(Params, ReturnProperty);
		if (!ProcessEventWithRuntimeFlags(Object, Function, Params.data()))
			return false;

		return ReadObjectValue(Params, ReturnProperty, OutObject);
	}

	bool IsThreeComponentStructParam(const UEProperty& Property)
	{
		return Property
			&& Property.IsA(EClassCastFlags::StructProperty)
			&& Property.GetSize() >= static_cast<int32>(sizeof(float) * 3);
	}

	bool CallViewPointFunction(UEObject Object, UEFunction Function, Vec3& OutLocation, Vec3& OutRotation)
	{
		if (!Object || !Function)
			return false;

		UEProperty LocationProperty;
		UEProperty RotationProperty;
		for (const UEProperty& Property : Function.GetProperties())
		{
			if (!Property.HasPropertyFlags(EPropertyFlags::Parm) || !IsThreeComponentStructParam(Property))
				continue;

			const std::string Name = ToLower(Property.GetName());
			const std::string Type = ToLower(Property.GetCppType());
			if (!LocationProperty && (Name.find("location") != std::string::npos || Type.find("vector") != std::string::npos))
				LocationProperty = Property;
			else if (!RotationProperty && (Name.find("rotation") != std::string::npos || Name.find("rot") != std::string::npos || Type.find("rotator") != std::string::npos))
				RotationProperty = Property;
		}

		if (!LocationProperty || !RotationProperty)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		EnsureParamSize(Params, LocationProperty);
		EnsureParamSize(Params, RotationProperty);
		if (!ProcessEventWithRuntimeFlags(Object, Function, Params.data()))
			return false;

		return ReadVector(Params, LocationProperty, OutLocation)
			&& ReadVector(Params, RotationProperty, OutRotation);
	}

	bool CallBoneNameFunction(UEObject Component, UEFunction Function, int32 BoneIndex, RawNameValue& OutName)
	{
		if (!Component || !Function)
			return false;

		UEProperty IndexProperty = FindFirstProperty(Function, { "BoneIndex", "Index" });
		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!IndexProperty || !ReturnProperty || !ReturnProperty.IsA(EClassCastFlags::NameProperty))
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		WriteInteger(Params, IndexProperty, BoneIndex);
		EnsureParamSize(Params, ReturnProperty);
		if (!ProcessEventWithRuntimeFlags(Component, Function, Params.data()))
			return false;

		return ReadNameValue(Params, ReturnProperty, OutName);
	}

	bool CallBoneLocationFunction(UEObject Component, UEFunction Function, const RawNameValue& BoneName, Vec3& OutLocation)
	{
		if (!Component || !Function)
			return false;

		UEProperty BoneNameProperty = FindFirstProperty(Function, { "BoneName", "InBoneName", "Name" });
		UEProperty SpaceProperty = FindFirstProperty(Function, { "Space", "BoneSpace", "CoordinateSpace" });
		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!BoneNameProperty || !ReturnProperty || !ReturnProperty.IsA(EClassCastFlags::StructProperty))
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		WriteNameValue(Params, BoneNameProperty, BoneName);
		if (SpaceProperty)
			WriteInteger(Params, SpaceProperty, 0);
		EnsureParamSize(Params, ReturnProperty);
		if (!ProcessEventWithRuntimeFlags(Component, Function, Params.data()))
			return false;

		return ReadVector(Params, ReturnProperty, OutLocation);
	}

	bool ReadActorBounds(UEObject Actor, UEFunction Function, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius)
	{
		if (!Actor || !Function)
			return false;

		UEProperty OnlyColliding = Function.FindMember("bOnlyCollidingComponents");
		UEProperty IncludeChildren = Function.FindMember("bIncludeFromChildActors");
		UEProperty Origin = Function.FindMember("Origin");
		UEProperty BoxExtent = Function.FindMember("BoxExtent");
		UEProperty SphereRadius = Function.FindMember("SphereRadius");

		if (!Origin || !BoxExtent)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		WriteBool(Params, OnlyColliding, false);
		WriteBool(Params, IncludeChildren, false);
		EnsureParamSize(Params, Origin);
		EnsureParamSize(Params, BoxExtent);
		EnsureParamSize(Params, SphereRadius);

		if (!ProcessEventWithRuntimeFlags(Actor, Function, Params.data()))
			return false;

		if (!ReadVector(Params, Origin, OutOrigin) || !ReadVector(Params, BoxExtent, OutExtent))
			return false;

		if (!ReadScalar(Params, SphereRadius, OutSphereRadius))
			OutSphereRadius = Distance(OutExtent, {});

		return true;
	}

	bool ProjectWorldToScreen(UEObject PlayerController, UEFunction Function, const Vec3& WorldLocation, Vec2& OutScreen)
	{
		if (!PlayerController || !Function)
			return false;

		UEProperty WorldLocationProp = FindFirstProperty(Function, { "WorldLocation", "WorldPosition" });
		UEProperty ScreenLocationProp = FindFirstProperty(Function, { "ScreenLocation", "ScreenPosition" });
		UEProperty ViewportRelativeProp = Function.FindMember("bPlayerViewportRelative");
		UEProperty ReturnProperty = Function.GetReturnProperty();
		if (!ReturnProperty)
			ReturnProperty = Function.FindMember("ReturnValue");

		if (!WorldLocationProp || !ScreenLocationProp)
			return false;

		std::vector<uint8> Params = MakeParamBuffer(Function);
		WriteVector(Params, WorldLocationProp, WorldLocation);
		WriteBool(Params, ViewportRelativeProp, false);
		EnsureParamSize(Params, ScreenLocationProp);
		EnsureParamSize(Params, ReturnProperty);

		if (!ProcessEventWithRuntimeFlags(PlayerController, Function, Params.data()))
			return false;

		const bool bProjected = ReadBool(Params, ReturnProperty, true);
		return bProjected && ReadVector2(Params, ScreenLocationProp, OutScreen);
	}

	bool ProjectBoundsToScreenBox(UEObject PlayerController, UEFunction Function, const Vec3& Origin, const Vec3& Extent, Vec2& OutMin, Vec2& OutMax)
	{
		if (!PlayerController || !Function)
			return false;

		const double MinExtent = 1.0;
		const Vec3 ClampedExtent =
		{
			std::max(std::abs(Extent.X), MinExtent),
			std::max(std::abs(Extent.Y), MinExtent),
			std::max(std::abs(Extent.Z), MinExtent)
		};

		Vec2 MinScreen = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		Vec2 MaxScreen = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
		int ProjectedCorners = 0;

		for (int X = -1; X <= 1; X += 2)
		{
			for (int Y = -1; Y <= 1; Y += 2)
			{
				for (int Z = -1; Z <= 1; Z += 2)
				{
					const Vec3 Corner =
					{
						Origin.X + (ClampedExtent.X * X),
						Origin.Y + (ClampedExtent.Y * Y),
						Origin.Z + (ClampedExtent.Z * Z)
					};

					Vec2 Screen;
					if (!ProjectWorldToScreen(PlayerController, Function, Corner, Screen))
						continue;

					MinScreen.X = std::min(MinScreen.X, Screen.X);
					MinScreen.Y = std::min(MinScreen.Y, Screen.Y);
					MaxScreen.X = std::max(MaxScreen.X, Screen.X);
					MaxScreen.Y = std::max(MaxScreen.Y, Screen.Y);
					ProjectedCorners++;
				}
			}
		}

		if (ProjectedCorners < 2)
			return false;

		OutMin = MinScreen;
		OutMax = MaxScreen;
		return true;
	}

	bool GetTargetClientRect(RECT& OutRect);
	HWND FindMainProcessWindow();

	void GetProjectionViewport(RECT& OutRect, float& OutWidth, float& OutHeight)
	{
		OutWidth = static_cast<float>(GetSystemMetrics(SM_CXSCREEN));
		OutHeight = static_cast<float>(GetSystemMetrics(SM_CYSCREEN));
		OutRect = { 0, 0, static_cast<LONG>(OutWidth), static_cast<LONG>(OutHeight) };

		if (GetTargetClientRect(OutRect))
		{
			const int Width = OutRect.right - OutRect.left;
			const int Height = OutRect.bottom - OutRect.top;
			if (Width > 0 && Height > 0)
			{
				OutWidth = static_cast<float>(Width);
				OutHeight = static_cast<float>(Height);
				return;
			}
		}

		RECT Rect = {};
		HWND Window = gTargetWindow ? gTargetWindow : gWindow;
		if (Window && GetClientRect(Window, &Rect))
		{
			const int Width = Rect.right - Rect.left;
			const int Height = Rect.bottom - Rect.top;
			if (Width > 0 && Height > 0)
			{
				OutWidth = static_cast<float>(Width);
				OutHeight = static_cast<float>(Height);
			}
		}
	}

	bool IsFiniteScreenPoint(const Vec2& Screen)
	{
		return std::isfinite(Screen.X) && std::isfinite(Screen.Y);
	}

	bool IsScreenPointNearViewport(const Vec2& Screen, float ViewWidth, float ViewHeight, float Margin)
	{
		return Screen.X >= -Margin
			&& Screen.Y >= -Margin
			&& Screen.X <= ViewWidth + Margin
			&& Screen.Y <= ViewHeight + Margin;
	}

	bool ScreenBoxIntersectsViewport(const Vec2& MinScreen, const Vec2& MaxScreen, float ViewWidth, float ViewHeight)
	{
		if (!IsFiniteScreenPoint(MinScreen) || !IsFiniteScreenPoint(MaxScreen))
			return false;

		const float Left = std::min(MinScreen.X, MaxScreen.X);
		const float Right = std::max(MinScreen.X, MaxScreen.X);
		const float Top = std::min(MinScreen.Y, MaxScreen.Y);
		const float Bottom = std::max(MinScreen.Y, MaxScreen.Y);
		return Right >= 0.0f && Bottom >= 0.0f && Left <= ViewWidth && Top <= ViewHeight;
	}

	bool ActorProjectionInView(const ActorDebugInfo& Actor, float ViewWidth, float ViewHeight)
	{
		if (!Actor.HasScreen)
			return false;

		if (Actor.HasBox && ScreenBoxIntersectsViewport(Actor.BoxMin, Actor.BoxMax, ViewWidth, ViewHeight))
			return true;

		return IsScreenPointNearViewport(Actor.Screen, ViewWidth, ViewHeight, 0.0f);
	}

	void NormalizeProjectedPoint(Vec2& Screen, const OverlayConfig& Config, CaptureStats& Stats, const RECT& TargetRect, float ViewWidth, float ViewHeight)
	{
		if (!IsFiniteScreenPoint(Screen))
			return;

		const int ProjectionSpace = std::clamp(Config.ProjectionSpace, 0, 2);
		const Vec2 DesktopRelative =
		{
			Screen.X - static_cast<float>(TargetRect.left),
			Screen.Y - static_cast<float>(TargetRect.top)
		};

		if (ProjectionSpace == 1)
			return;

		if (ProjectionSpace == 2)
		{
			Screen = DesktopRelative;
			Stats.UsedDesktopProjection = true;
			return;
		}

		const float Margin = std::max(ViewWidth, ViewHeight) * 0.10f;
		if (IsScreenPointNearViewport(Screen, ViewWidth, ViewHeight, Margin))
			return;

		if (IsScreenPointNearViewport(DesktopRelative, ViewWidth, ViewHeight, Margin))
		{
			Screen = DesktopRelative;
			Stats.UsedDesktopProjection = true;
		}
	}

	void ApplyProjectionCalibration(Vec2& Screen, const OverlayConfig& Config, float ViewWidth, float ViewHeight)
	{
		if (!IsFiniteScreenPoint(Screen))
			return;

		const float CenterX = ViewWidth * 0.5f;
		const float CenterY = ViewHeight * 0.5f;
		Screen.X = CenterX + ((Screen.X - CenterX) * Config.ProjectionScaleX) + Config.ProjectionOffsetX;
		Screen.Y = CenterY + ((Screen.Y - CenterY) * Config.ProjectionScaleY) + Config.ProjectionOffsetY;
	}

	float ProjectionQualityScore(const Vec2& Screen, float ViewWidth, float ViewHeight)
	{
		if (!IsFiniteScreenPoint(Screen))
			return std::numeric_limits<float>::max();

		const float ClampedX = std::clamp(Screen.X, 0.0f, std::max(ViewWidth, 1.0f));
		const float ClampedY = std::clamp(Screen.Y, 0.0f, std::max(ViewHeight, 1.0f));
		const float OutsideX = Screen.X - ClampedX;
		const float OutsideY = Screen.Y - ClampedY;
		const float OutsideDistance = std::sqrt((OutsideX * OutsideX) + (OutsideY * OutsideY));
		const float CenterX = ViewWidth * 0.5f;
		const float CenterY = ViewHeight * 0.5f;
		const float CenterDistance = std::sqrt(((Screen.X - CenterX) * (Screen.X - CenterX)) + ((Screen.Y - CenterY) * (Screen.Y - CenterY)));
		return (OutsideDistance * 1000.0f) + CenterDistance;
	}

	bool IsSaneScreenBox(const Vec2& MinScreen, const Vec2& MaxScreen, const OverlayConfig& Config, float ViewWidth, float ViewHeight)
	{
		if (!Config.ClampLargeBoxes)
			return true;

		if (!IsFiniteScreenPoint(MinScreen) || !IsFiniteScreenPoint(MaxScreen))
			return false;

		const float Width = std::abs(MaxScreen.X - MinScreen.X);
		const float Height = std::abs(MaxScreen.Y - MinScreen.Y);
		const float MaxFraction = Config.FastOverlayMode
			? std::min(Config.MaxBoxScreenFraction, 0.40f)
			: Config.MaxBoxScreenFraction;
		const float EffectiveFraction = std::clamp(MaxFraction, 0.15f, 0.85f);
		const float MaxWidth = std::max(ViewWidth, 1.0f) * EffectiveFraction;
		const float MaxHeight = std::max(ViewHeight, 1.0f) * EffectiveFraction;
		const float MaxArea = std::max(ViewWidth * ViewHeight, 1.0f) * EffectiveFraction * EffectiveFraction;
		return Width >= 1.0f
			&& Height >= 1.0f
			&& Width <= MaxWidth
			&& Height <= MaxHeight
			&& (Width * Height) <= MaxArea;
	}

	bool ProjectWorldToScreenCameraFallback(const Vec3& WorldLocation, const Vec3& CameraLocation, const Vec3& CameraRotation, float CameraFov, float ViewWidth, float ViewHeight, Vec2& OutScreen)
	{
		if (ViewWidth <= 1.0f || ViewHeight <= 1.0f || CameraFov <= 1.0f)
			return false;

		constexpr double Pi = 3.14159265358979323846;
		const double Pitch = CameraRotation.X * (Pi / 180.0);
		const double Yaw = CameraRotation.Y * (Pi / 180.0);
		const double CosPitch = std::cos(Pitch);
		const double SinPitch = std::sin(Pitch);
		const double CosYaw = std::cos(Yaw);
		const double SinYaw = std::sin(Yaw);

		const Vec3 Forward = { CosPitch * CosYaw, CosPitch * SinYaw, SinPitch };
		const Vec3 Right = { -SinYaw, CosYaw, 0.0 };
		const Vec3 Up = { -SinPitch * CosYaw, -SinPitch * SinYaw, CosPitch };
		const Vec3 Delta =
		{
			WorldLocation.X - CameraLocation.X,
			WorldLocation.Y - CameraLocation.Y,
			WorldLocation.Z - CameraLocation.Z
		};

		const double Depth = (Delta.X * Forward.X) + (Delta.Y * Forward.Y) + (Delta.Z * Forward.Z);
		if (Depth <= 1.0)
			return false;

		const double ViewX = (Delta.X * Right.X) + (Delta.Y * Right.Y) + (Delta.Z * Right.Z);
		const double ViewY = (Delta.X * Up.X) + (Delta.Y * Up.Y) + (Delta.Z * Up.Z);
		const double Focal = (static_cast<double>(ViewWidth) * 0.5) / std::tan((static_cast<double>(CameraFov) * Pi / 180.0) * 0.5);

		OutScreen.X = static_cast<float>((static_cast<double>(ViewWidth) * 0.5) + ((ViewX * Focal) / Depth));
		OutScreen.Y = static_cast<float>((static_cast<double>(ViewHeight) * 0.5) - ((ViewY * Focal) / Depth));
		return std::isfinite(OutScreen.X) && std::isfinite(OutScreen.Y);
	}

	bool ProjectWorldToScreenAny(UEObject PlayerController, UEFunction Function, const OverlayConfig& Config, CaptureStats& Stats,
		const Vec3& CameraLocation, const Vec3& CameraRotation, float CameraFov, const RECT& ProjectionRect, float ViewWidth, float ViewHeight, const Vec3& WorldLocation, Vec2& OutScreen)
	{
		const int ProjectionRoute = std::clamp(Config.ProjectionRoute, 0, 2);
		Vec2 NativeScreen;
		Vec2 FallbackScreen;
		bool NativeOk = false;
		bool FallbackOk = false;

		if (ProjectionRoute != 2 && PlayerController && Function)
		{
			Stats.NativeProjectionAttempts++;
			if (ProjectWorldToScreen(PlayerController, Function, WorldLocation, NativeScreen))
			{
				Stats.NativeProjectionSuccesses++;
				NormalizeProjectedPoint(NativeScreen, Config, Stats, ProjectionRect, ViewWidth, ViewHeight);
				ApplyProjectionCalibration(NativeScreen, Config, ViewWidth, ViewHeight);
				NativeOk = IsFiniteScreenPoint(NativeScreen);
			}
			else
			{
				Stats.NativeProjectionFailures++;
			}
		}

		if (ProjectionRoute != 1 && Config.UseProjectionFallback && Stats.HasCameraLocation && Stats.HasCameraRotation)
		{
			Stats.FallbackProjectionAttempts++;
			if (ProjectWorldToScreenCameraFallback(WorldLocation, CameraLocation, CameraRotation, CameraFov, ViewWidth, ViewHeight, FallbackScreen))
			{
				Stats.FallbackProjectionSuccesses++;
				ApplyProjectionCalibration(FallbackScreen, Config, ViewWidth, ViewHeight);
				FallbackOk = IsFiniteScreenPoint(FallbackScreen);
			}
			else
			{
				Stats.FallbackProjectionFailures++;
			}
		}

		if (NativeOk && FallbackOk && ProjectionRoute == 0)
		{
			const float Margin = std::max(ViewWidth, ViewHeight) * 0.25f;
			const bool NativeNearViewport = IsScreenPointNearViewport(NativeScreen, ViewWidth, ViewHeight, Margin);
			const bool FallbackNearViewport = IsScreenPointNearViewport(FallbackScreen, ViewWidth, ViewHeight, Margin);

			if (!NativeNearViewport && FallbackNearViewport)
			{
				OutScreen = FallbackScreen;
				Stats.UsedProjectionFallback = true;
				return true;
			}

			OutScreen = NativeScreen;
			return true;
		}

		if (FallbackOk && (ProjectionRoute == 2 || !NativeOk))
		{
			OutScreen = FallbackScreen;
			Stats.UsedProjectionFallback = true;
			return true;
		}

		if (NativeOk)
		{
			OutScreen = NativeScreen;
			return true;
		}

		return false;
	}

	bool ProjectBestPositionCandidate(const std::vector<PositionCandidate>& Candidates, UEObject PlayerController, UEFunction Function, const OverlayConfig& Config,
		CaptureStats& Stats, const Vec3& CameraLocation, const Vec3& CameraRotation, float CameraFov, const RECT& ProjectionRect, float ViewWidth, float ViewHeight,
		ActorDebugInfo& Info)
	{
		if (Candidates.empty())
		{
			Info.ProjectionFailure = "no position candidates";
			return false;
		}

		if (!PlayerController && (!Config.UseProjectionFallback || !Stats.HasCameraLocation || !Stats.HasCameraRotation))
		{
			Info.ProjectionFailure = "no player controller or camera fallback";
			return false;
		}

		for (const PositionCandidate& Candidate : Candidates)
		{
			Vec2 Screen;
			Info.ProjectionAttemptCount++;
			Stats.ProjectionCandidateAttempts++;
			if (!ProjectWorldToScreenAny(PlayerController, Function, Config, Stats, CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Candidate.Location, Screen))
				continue;

			Info.Location = Candidate.Location;
			Info.LocationSource = Candidate.Source;
			Info.Screen = Screen;
			Info.HasLocation = true;
			Info.HasScreen = true;
			Info.ProjectionSource = Candidate.Source;
			Info.ProjectionFailure.clear();
			return true;
		}

		Info.ProjectionFailure = "all position candidates failed projection";
		return false;
	}

	bool ProjectBoundsToScreenBoxAny(UEObject PlayerController, UEFunction Function, const OverlayConfig& Config, CaptureStats& Stats,
		const Vec3& CameraLocation, const Vec3& CameraRotation, float CameraFov, const RECT& ProjectionRect, float ViewWidth, float ViewHeight,
		const Vec3& Origin, const Vec3& Extent, Vec2& OutMin, Vec2& OutMax)
	{
		const double MinExtent = 1.0;
		const Vec3 ClampedExtent =
		{
			std::max(std::abs(Extent.X), MinExtent),
			std::max(std::abs(Extent.Y), MinExtent),
			std::max(std::abs(Extent.Z), MinExtent)
		};

		Vec2 MinScreen = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		Vec2 MaxScreen = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
		int ProjectedCorners = 0;

		for (int X = -1; X <= 1; X += 2)
		{
			for (int Y = -1; Y <= 1; Y += 2)
			{
				for (int Z = -1; Z <= 1; Z += 2)
				{
					const Vec3 Corner =
					{
						Origin.X + (ClampedExtent.X * X),
						Origin.Y + (ClampedExtent.Y * Y),
						Origin.Z + (ClampedExtent.Z * Z)
					};

					Vec2 Screen;
					if (!ProjectWorldToScreenAny(PlayerController, Function, Config, Stats, CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Corner, Screen))
						continue;

					MinScreen.X = std::min(MinScreen.X, Screen.X);
					MinScreen.Y = std::min(MinScreen.Y, Screen.Y);
					MaxScreen.X = std::max(MaxScreen.X, Screen.X);
					MaxScreen.Y = std::max(MaxScreen.Y, Screen.Y);
					ProjectedCorners++;
				}
			}
		}

		if (ProjectedCorners < 2)
			return false;

		if (!IsSaneScreenBox(MinScreen, MaxScreen, Config, ViewWidth, ViewHeight))
			return false;

		OutMin = MinScreen;
		OutMax = MaxScreen;
		return true;
	}

	Vec3 BoundsOriginForCurrentLocation(const ActorDebugInfo& Info)
	{
		if (!Info.HasBounds)
			return Info.Location;

		return
		{
			Info.Location.X + Info.BoundsOffset.X,
			Info.Location.Y + Info.BoundsOffset.Y,
			Info.Location.Z + Info.BoundsOffset.Z
		};
	}

	void UpdateBoundsOffset(ActorDebugInfo& Info)
	{
		if (!Info.HasBounds || !Info.HasLocation)
		{
			Info.BoundsOffset = {};
			return;
		}

		Info.BoundsOffset =
		{
			Info.BoundsOrigin.X - Info.Location.X,
			Info.BoundsOrigin.Y - Info.Location.Y,
			Info.BoundsOrigin.Z - Info.Location.Z
		};
	}

	bool SetVerticalScreenBoxFromTopBottom(ActorDebugInfo& Info, const OverlayConfig& Config, float ViewWidth, float ViewHeight)
	{
		const float BoxTop = std::min(Info.ScreenTop.Y, Info.ScreenBottom.Y);
		const float BoxBottom = std::max(Info.ScreenTop.Y, Info.ScreenBottom.Y);
		const float BoxHeight = std::max(BoxBottom - BoxTop, 8.0f);
		const float BoxWidth = std::max(BoxHeight * Config.BoxWidthRatio, 4.0f);
		const float CenterX = (Info.ScreenTop.X + Info.ScreenBottom.X) * 0.5f;

		Info.BoxMin = { CenterX - (BoxWidth * 0.5f), BoxTop };
		Info.BoxMax = { CenterX + (BoxWidth * 0.5f), BoxBottom };
		return IsSaneScreenBox(Info.BoxMin, Info.BoxMax, Config, ViewWidth, ViewHeight);
	}

	bool SetScreenBoxFromSkeleton(ActorDebugInfo& Info, const OverlayConfig& Config, float ViewWidth, float ViewHeight)
	{
		if (!Info.HasSkeleton || Info.SkeletonBones.empty())
			return false;

		float MinX = std::numeric_limits<float>::max();
		float MinY = std::numeric_limits<float>::max();
		float MaxX = std::numeric_limits<float>::lowest();
		float MaxY = std::numeric_limits<float>::lowest();
		int32 ProjectedBones = 0;

		for (const SkeletonBonePoint& Bone : Info.SkeletonBones)
		{
			if (!Bone.HasScreen || !IsFiniteScreenPoint(Bone.Screen))
				continue;

			MinX = std::min(MinX, Bone.Screen.X);
			MinY = std::min(MinY, Bone.Screen.Y);
			MaxX = std::max(MaxX, Bone.Screen.X);
			MaxY = std::max(MaxY, Bone.Screen.Y);
			ProjectedBones++;
		}

		if (ProjectedBones < 2)
			return false;

		const float Height = std::max(MaxY - MinY, 1.0f);
		const float Width = std::max(MaxX - MinX, 1.0f);
		const float Pad = std::max(Config.BoxPaddingPixels, 0.0f) + std::clamp(Height * 0.04f, 2.0f, 10.0f);
		const float DesiredWidth = std::max(Width + (Pad * 2.0f), Height * std::max(Config.BoxWidthRatio, 0.10f));
		const float CenterX = (MinX + MaxX) * 0.5f;

		Info.BoxMin = { CenterX - (DesiredWidth * 0.5f), MinY - Pad };
		Info.BoxMax = { CenterX + (DesiredWidth * 0.5f), MaxY + Pad };
		return IsSaneScreenBox(Info.BoxMin, Info.BoxMax, Config, ViewWidth, ViewHeight);
	}

	bool IsReadablePointer(const void* Pointer)
	{
		return Pointer && !Platform::IsBadReadPtr(Pointer);
	}

	bool IsReadableRange(const void* Pointer, size_t Size)
	{
		if (!Pointer || Size == 0)
			return false;

		const auto* Bytes = static_cast<const uint8*>(Pointer);
		return IsReadablePointer(Bytes) && IsReadablePointer(Bytes + Size - 1);
	}

	bool IsReadableObject(void* Pointer)
	{
		if (!IsReadablePointer(Pointer))
			return false;

		void* Vft = *reinterpret_cast<void**>(Pointer);
		return IsReadablePointer(Vft);
	}

	bool ReadObjectPropertyAtOffset(UEObject Object, int32_t Offset, UEObject& OutObject);
	bool ReadBoxSphereBoundsAtOffset(UEObject Object, int32_t Offset, int32_t Size, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius);
	bool ReadArrayPropertyAtOffset(UEObject Object, int32_t Offset, RawTArrayView& OutArray);
	bool GetActorLocation(UEObject Actor, Vec3& OutLocation, std::string* OutSource);
	bool IsSaneWorldPosition(const Vec3& Value);
	bool IsVectorLikeStructProperty(const UEProperty& Property);
	bool ReadVectorPropertyValue(UEObject Object, UEProperty Property, Vec3& OutValue);

	bool ReadObjectProperty(UEObject Object, UEProperty Property, UEObject& OutObject)
	{
		if (!Object || !Property)
			return false;

		const int32 Offset = Property.GetOffset();
		return ReadObjectPropertyAtOffset(Object, Offset, OutObject);
	}

	bool ReadObjectPropertyAtOffset(UEObject Object, int32_t Offset, UEObject& OutObject)
	{
		if (!Object)
			return false;

		if (Offset < 0)
			return false;

		uint8* Address = static_cast<uint8*>(Object.GetAddress()) + Offset;
		if (!IsReadablePointer(Address))
			return false;

		void* ObjectPointer = *reinterpret_cast<void**>(Address);
		if (!IsReadableObject(ObjectPointer))
			return false;

		OutObject = UEObject(ObjectPointer);
		return true;
	}

	bool WriteObjectPropertyAtOffset(UEObject Object, int32_t Offset, UEObject Value)
	{
		if (!Object || Offset < 0)
			return false;

		uint8* Address = static_cast<uint8*>(Object.GetAddress()) + Offset;
		if (!IsReadablePointer(Address))
			return false;

		*reinterpret_cast<void**>(Address) = Value.GetAddress();
		return true;
	}

	bool ReadBoxSphereBoundsProperty(UEObject Object, UEProperty Property, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius)
	{
		if (!Object || !Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		return ReadBoxSphereBoundsAtOffset(Object, Offset, Size, OutOrigin, OutExtent, OutSphereRadius);
	}

	bool ReadBoxSphereBoundsAtOffset(UEObject Object, int32_t Offset, int32_t Size, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius)
	{
		if (!Object)
			return false;

		if (Offset < 0 || Size <= 0)
			return false;

		const uint8* Data = static_cast<const uint8*>(Object.GetAddress()) + Offset;
		if (!IsReadablePointer(Data))
			return false;

		if (Size >= static_cast<int32>(sizeof(double) * 7))
		{
			const double* Values = reinterpret_cast<const double*>(Data);
			OutOrigin = { Values[0], Values[1], Values[2] };
			OutExtent = { Values[3], Values[4], Values[5] };
			OutSphereRadius = static_cast<float>(Values[6]);
			return IsUsefulBounds(OutOrigin, OutExtent, OutSphereRadius);
		}

		if (Size >= static_cast<int32>((sizeof(double) * 6) + sizeof(float)))
		{
			const double* Values = reinterpret_cast<const double*>(Data);
			OutOrigin = { Values[0], Values[1], Values[2] };
			OutExtent = { Values[3], Values[4], Values[5] };
			OutSphereRadius = *reinterpret_cast<const float*>(Data + (sizeof(double) * 6));
			return IsUsefulBounds(OutOrigin, OutExtent, OutSphereRadius);
		}

		if (Size >= static_cast<int32>((sizeof(float) * 6) + sizeof(float)))
		{
			const float* Values = reinterpret_cast<const float*>(Data);
			OutOrigin = { Values[0], Values[1], Values[2] };
			OutExtent = { Values[3], Values[4], Values[5] };
			OutSphereRadius = Values[6];
			return IsUsefulBounds(OutOrigin, OutExtent, OutSphereRadius);
		}

		return false;
	}

	bool ReadRootComponentBounds(UEObject Actor, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius)
	{
		UEObject RootComponent;
		if (!ReadObjectPropertyAtOffset(Actor, gRuntimeSymbols.actorRootComponentOffset, RootComponent))
			return false;

		if (gSymbols.PrimitiveComponentClass && !RootComponent.IsA(gSymbols.PrimitiveComponentClass))
			return false;

		return ReadBoxSphereBoundsAtOffset(RootComponent, gRuntimeSymbols.primitiveBoundsOffset, gRuntimeSymbols.primitiveBoundsSize, OutOrigin, OutExtent, OutSphereRadius);
	}

	bool ReadBestActorBounds(UEObject Actor, const OverlayConfig& Config, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius)
	{
		const int BoundsMode = std::clamp(Config.BoundsMode, 0, 3);
		if (BoundsMode == 3)
			return false;

		auto TryActorBounds = [&]()
		{
			return ReadActorBounds(Actor, gSymbols.GetActorBounds, OutOrigin, OutExtent, OutSphereRadius)
				&& IsUsefulBounds(OutOrigin, OutExtent, OutSphereRadius);
		};

		auto TryRootBounds = [&]()
		{
			return ReadRootComponentBounds(Actor, OutOrigin, OutExtent, OutSphereRadius);
		};

		if (BoundsMode == 1)
			return TryActorBounds();

		if (BoundsMode == 2)
			return TryRootBounds();

		auto TryInferredVectorBounds = [&]()
		{
			if (!Actor)
				return false;

			UEClass Class = Actor.GetClass();
			if (!Class)
				return false;

			Vec3 Location;
			std::string LocationSource;
			const bool HasActorLocation = GetActorLocation(Actor, Location, &LocationSource);

			Vec3 BestOrigin = HasActorLocation ? Location : Vec3{};
			Vec3 BestExtent = {};
			bool HasOrigin = HasActorLocation;
			bool HasExtent = false;
			int OriginScore = HasActorLocation ? 1 : -1;
			int ExtentScore = -1;
			int Visited = 0;
			const int MaxFields = std::clamp(Config.PositionProbeMaxFields, 8, 500);

			for (UEStruct Struct = Class; Struct && Visited < MaxFields; Struct = Struct.GetSuper())
			{
				for (const UEProperty& Property : Struct.GetProperties())
				{
					if (++Visited > MaxFields)
						break;
					if (!IsVectorLikeStructProperty(Property))
						continue;

					Vec3 Value;
					if (!ReadVectorPropertyValue(Actor, Property, Value))
						continue;

					const std::string Name = ToLower(Property.GetName());
					const bool NameLooksBounds = Name.find("bound") != std::string::npos || Name.find("box") != std::string::npos;
					const bool NameLooksOrigin = Name.find("origin") != std::string::npos || Name.find("center") != std::string::npos || Name.find("centre") != std::string::npos;
					const bool NameLooksExtent = Name.find("extent") != std::string::npos || Name.find("half") != std::string::npos || Name.find("radius") != std::string::npos;

					if (NameLooksOrigin || (NameLooksBounds && !NameLooksExtent))
					{
						const int Score = (NameLooksBounds ? 4 : 0) + (NameLooksOrigin ? 4 : 0);
						if (Score > OriginScore && IsSaneWorldPosition(Value))
						{
							BestOrigin = Value;
							HasOrigin = true;
							OriginScore = Score;
						}
					}

					const Vec3 AbsValue = { std::abs(Value.X), std::abs(Value.Y), std::abs(Value.Z) };
					const double MaxExtent = std::max({ AbsValue.X, AbsValue.Y, AbsValue.Z });
					const double MinExtent = std::min({ AbsValue.X, AbsValue.Y, AbsValue.Z });
					const bool LooksLikeExtentValue = MaxExtent >= 1.0 && MaxExtent <= 5000.0 && MinExtent >= 0.0;
					if ((NameLooksExtent || NameLooksBounds) && LooksLikeExtentValue)
					{
						const int Score = (NameLooksBounds ? 3 : 0) + (NameLooksExtent ? 5 : 0);
						if (Score > ExtentScore)
						{
							BestExtent = AbsValue;
							HasExtent = true;
							ExtentScore = Score;
						}
					}
				}
			}

			if (!HasOrigin || !HasExtent)
				return false;

			OutOrigin = BestOrigin;
			OutExtent =
			{
				std::clamp(BestExtent.X, 1.0, 5000.0),
				std::clamp(BestExtent.Y, 1.0, 5000.0),
				std::clamp(BestExtent.Z, 1.0, 5000.0)
			};
			OutSphereRadius = Distance(OutExtent, {});
			return IsUsefulBounds(OutOrigin, OutExtent, OutSphereRadius);
		};

		if (TryActorBounds())
			return true;

		if (TryRootBounds())
			return true;

		return TryInferredVectorBounds();
	}

	bool IsSaneWorldPosition(const Vec3& Value)
	{
		constexpr double MaxCoordinate = 100000000.0;
		return std::isfinite(Value.X)
			&& std::isfinite(Value.Y)
			&& std::isfinite(Value.Z)
			&& std::abs(Value.X) < MaxCoordinate
			&& std::abs(Value.Y) < MaxCoordinate
			&& std::abs(Value.Z) < MaxCoordinate;
	}

	bool IsDuplicatePositionCandidate(const std::vector<PositionCandidate>& Candidates, const Vec3& Value)
	{
		for (const PositionCandidate& Candidate : Candidates)
		{
			if (Distance(Candidate.Location, Value) < 1.0)
				return true;
		}

		return false;
	}

	void AddPositionCandidate(std::vector<PositionCandidate>& Candidates, const Vec3& Value, const std::string& Source)
	{
		if (!IsSaneWorldPosition(Value) || IsDuplicatePositionCandidate(Candidates, Value))
			return;

		Candidates.push_back({ Value, Source });
	}

	bool IsVectorLikeStructProperty(const UEProperty& Property)
	{
		if (!Property || !Property.IsA(EClassCastFlags::StructProperty))
			return false;

		const std::string Type = ToLower(Property.GetCppType());
		if (Type.find("vector") != std::string::npos)
			return true;

		UEStruct Struct = Property.Cast<UEStructProperty>().GetUnderlayingStruct();
		return Struct && ToLower(Struct.GetName()).find("vector") != std::string::npos;
	}

	bool ReadVectorPropertyValue(UEObject Object, UEProperty Property, Vec3& OutValue)
	{
		if (!Object || !Property || !IsVectorLikeStructProperty(Property))
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
		if (Offset < 0 || Size <= 0)
			return false;

		const uint8* Data = static_cast<const uint8*>(Object.GetAddress()) + Offset;
		if (!IsReadableRange(Data, static_cast<size_t>(Size)))
			return false;

		if (Size >= static_cast<int32>(sizeof(double) * 3))
		{
			const double* Values = reinterpret_cast<const double*>(Data);
			OutValue = { Values[0], Values[1], Values[2] };
			return IsSaneWorldPosition(OutValue);
		}

		if (Size >= static_cast<int32>(sizeof(float) * 3))
		{
			const float* Values = reinterpret_cast<const float*>(Data);
			OutValue = { Values[0], Values[1], Values[2] };
			return IsSaneWorldPosition(OutValue);
		}

		return false;
	}

	bool PropertyLooksLikePosition(const UEProperty& Property, const OverlayConfig& Config)
	{
		const std::string Name = Property.GetName();
		const std::string Type = Property.GetCppType();
		return MatchesTokenListNoCase(Name, Config.PositionFieldFilter)
			|| MatchesTokenListNoCase(Type, Config.PositionFieldFilter);
	}

	bool PropertyLooksLikeComponent(const UEProperty& Property)
	{
		const std::string Name = ToLower(Property.GetName());
		const std::string Type = ToLower(Property.GetCppType());
		return Name.find("component") != std::string::npos
			|| Name.find("root") != std::string::npos
			|| Name.find("mesh") != std::string::npos
			|| Name.find("capsule") != std::string::npos
			|| Name.find("body") != std::string::npos
			|| Type.find("component") != std::string::npos;
	}

	bool FunctionLooksLikePositionGetter(const UEFunction& Function, const OverlayConfig& Config)
	{
		const std::string Name = Function.GetName();
		const std::string LowerName = ToLower(Name);
		if (!(LowerName.rfind("get", 0) == 0 || LowerName.rfind("k2_get", 0) == 0))
			return false;

		if (!MatchesTokenListNoCase(Name, Config.PositionFieldFilter))
			return false;

		int InputParams = 0;
		bool HasVectorReturn = false;
		for (const UEProperty& Property : Function.GetProperties())
		{
			if (!Property.HasPropertyFlags(EPropertyFlags::Parm))
				continue;

			if (Property.HasPropertyFlags(EPropertyFlags::ReturnParm))
				HasVectorReturn = IsVectorLikeStructProperty(Property);
			else
				InputParams++;
		}

		return HasVectorReturn && InputParams == 0;
	}

	void AddComponentPositionCandidates(UEObject Component, const std::string& Source, std::vector<PositionCandidate>& Candidates)
	{
		if (!Component || (gSymbols.SceneComponentClass && !Component.IsA(gSymbols.SceneComponentClass)))
			return;

		Vec3 Location;
		if (CallNoArgVectorFunction(Component, gSymbols.GetComponentLocation, Location))
			AddPositionCandidate(Candidates, Location, Source + ".GetComponentLocation()");

		if (gSymbols.PrimitiveComponentClass && Component.IsA(gSymbols.PrimitiveComponentClass) && gRuntimeSymbols.primitiveBoundsOffset >= 0)
		{
			Vec3 BoundsOrigin;
			Vec3 BoundsExtent;
			float SphereRadius = 0.0f;
			if (ReadBoxSphereBoundsAtOffset(Component, gRuntimeSymbols.primitiveBoundsOffset, gRuntimeSymbols.primitiveBoundsSize, BoundsOrigin, BoundsExtent, SphereRadius))
				AddPositionCandidate(Candidates, BoundsOrigin, Source + ".Bounds.Origin");
		}
	}

	std::string PositionProbeSignature(const OverlayConfig& Config)
	{
		return std::string(Config.PositionFieldFilter) + "|" + std::to_string(std::clamp(Config.PositionProbeMaxFields, 8, 500));
	}

	CachedPositionProbePlan BuildPositionProbePlan(UEClass Class, const OverlayConfig& Config)
	{
		CachedPositionProbePlan Plan;
		Plan.Signature = PositionProbeSignature(Config);
		Plan.BuiltTick = GetTickCount();

		int InspectedFields = 0;
		const int MaxFields = std::clamp(Config.PositionProbeMaxFields, 8, 500);
		for (UEStruct Struct = Class; Struct && InspectedFields < MaxFields; Struct = Struct.GetSuper())
		{
			const std::string StructName = Struct.GetName();
			for (const UEProperty& Property : Struct.GetProperties())
			{
				if (++InspectedFields > MaxFields)
					break;

				const std::string FieldName = StructName + "." + Property.GetName();
				if (PropertyLooksLikePosition(Property, Config) && IsVectorLikeStructProperty(Property))
					Plan.Probes.push_back({ PositionProbeKind::VectorProperty, Property, {}, FieldName });

				if (Property.IsA(EClassCastFlags::ObjectPropertyBase) && PropertyLooksLikeComponent(Property))
					Plan.Probes.push_back({ PositionProbeKind::ComponentProperty, Property, {}, FieldName });
			}

			for (const UEFunction& Function : Struct.GetFunctions())
			{
				if (++InspectedFields > MaxFields)
					break;

				if (FunctionLooksLikePositionGetter(Function, Config))
					Plan.Probes.push_back({ PositionProbeKind::GetterFunction, {}, Function, StructName + "." + Function.GetName() + "()" });
			}
		}

		return Plan;
	}

	CachedPositionProbePlan GetCachedPositionProbePlan(UEClass Class, const OverlayConfig& Config, CaptureStats* Stats)
	{
		if (!Class)
			return {};

		const uintptr_t ClassAddress = reinterpret_cast<uintptr_t>(Class.GetAddress());
		const std::string Signature = PositionProbeSignature(Config);
		{
			std::scoped_lock Lock(gPositionProbeCacheMutex);
			auto It = gPositionProbeCache.find(ClassAddress);
			if (It != gPositionProbeCache.end() && It->second.Signature == Signature)
			{
				if (Stats)
					Stats->PositionProbeCacheHits++;
				return It->second;
			}
		}

		if (Stats)
			Stats->PositionProbeCacheMisses++;

		CachedPositionProbePlan Plan = BuildPositionProbePlan(Class, Config);
		{
			std::scoped_lock Lock(gPositionProbeCacheMutex);
			gPositionProbeCache[ClassAddress] = Plan;
		}

		return Plan;
	}

	void CollectReflectedPositionCandidates(UEObject Object, const OverlayConfig& Config, std::vector<PositionCandidate>& Candidates, CaptureStats* Stats = nullptr)
	{
		if (!Config.UseReflectedPositionFallback || !Object)
			return;

		UEClass Class = Object.GetClass();
		const CachedPositionProbePlan Plan = GetCachedPositionProbePlan(Class, Config, Stats);
		for (const CachedPositionProbe& Probe : Plan.Probes)
		{
			if (Probe.Kind == PositionProbeKind::VectorProperty)
			{
				Vec3 Value;
				if (ReadVectorPropertyValue(Object, Probe.Property, Value))
					AddPositionCandidate(Candidates, Value, Probe.Source);
				continue;
			}

			if (Probe.Kind == PositionProbeKind::ComponentProperty)
			{
				UEObject Component;
				if (ReadObjectProperty(Object, Probe.Property, Component))
					AddComponentPositionCandidates(Component, Probe.Source, Candidates);
				continue;
			}

			if (Probe.Kind == PositionProbeKind::GetterFunction)
			{
				Vec3 Value;
				if (CallNoArgVectorFunction(Object, Probe.Function, Value))
					AddPositionCandidate(Candidates, Value, Probe.Source);
			}
		}
	}

	bool IsSkinnedMeshComponent(UEObject Object)
	{
		return Object
			&& ((gSymbols.SkinnedMeshComponentClass && Object.IsA(gSymbols.SkinnedMeshComponentClass))
				|| (gSymbols.SkeletalMeshComponentClass && Object.IsA(gSymbols.SkeletalMeshComponentClass)));
	}

	bool PropertyLooksLikeMeshComponent(const UEProperty& Property)
	{
		const std::string Name = ToLower(Property.GetName());
		const std::string Type = ToLower(Property.GetCppType());
		return Name.find("mesh") != std::string::npos
			|| Name.find("skeletal") != std::string::npos
			|| Name.find("skinned") != std::string::npos
			|| Name == "body"
			|| Type.find("mesh") != std::string::npos
			|| Type.find("skeletal") != std::string::npos
			|| Type.find("skinned") != std::string::npos;
	}

	bool FindSkinnedMeshComponent(UEObject Actor, UEObject& OutComponent, std::string& OutSource)
	{
		if (!Actor)
			return false;

		if (IsSkinnedMeshComponent(Actor))
		{
			OutComponent = Actor;
			OutSource = "actor is mesh component";
			return true;
		}

		UEClass Class = Actor.GetClass();
		const uintptr_t ClassAddress = reinterpret_cast<uintptr_t>(Class.GetAddress());
		const DWORD Now = GetTickCount();
		{
			std::scoped_lock Lock(gMeshProbeCacheMutex);
			auto It = gMeshProbeCache.find(ClassAddress);
			if (It != gMeshProbeCache.end() && It->second.Built)
			{
				if (It->second.ActorIsMesh)
				{
					OutComponent = Actor;
					OutSource = "actor is mesh component";
					return true;
				}

				if (!It->second.Property && Now - It->second.BuiltTick < 3000)
					return false;

				UEObject Component;
				if (It->second.Property && ReadObjectProperty(Actor, It->second.Property, Component) && IsSkinnedMeshComponent(Component))
				{
					OutComponent = Component;
					OutSource = It->second.Source;
					return true;
				}

				if (It->second.Property)
					return false;
			}
		}

		int InspectedFields = 0;
		for (UEStruct Struct = Class; Struct && InspectedFields < 180; Struct = Struct.GetSuper())
		{
			for (const UEProperty& Property : Struct.GetProperties())
			{
				if (++InspectedFields > 180)
					break;

				if (!Property.IsA(EClassCastFlags::ObjectPropertyBase) || !PropertyLooksLikeMeshComponent(Property))
					continue;

				UEObject Component;
				if (!ReadObjectProperty(Actor, Property, Component) || !IsSkinnedMeshComponent(Component))
					continue;

				OutComponent = Component;
				OutSource = Struct.GetName() + "." + Property.GetName();
				{
					std::scoped_lock Lock(gMeshProbeCacheMutex);
					CachedMeshProbePlan& Plan = gMeshProbeCache[ClassAddress];
					Plan.Built = true;
					Plan.ActorIsMesh = false;
					Plan.Property = Property;
					Plan.Source = OutSource;
					Plan.BuiltTick = GetTickCount();
				}
				return true;
			}
		}

		{
			std::scoped_lock Lock(gMeshProbeCacheMutex);
			CachedMeshProbePlan& Plan = gMeshProbeCache[ClassAddress];
			Plan.Built = true;
			Plan.ActorIsMesh = false;
			Plan.Property = {};
			Plan.Source.clear();
			Plan.BuiltTick = GetTickCount();
		}

		return false;
	}

	std::string NormalizeBoneName(std::string Name)
	{
		Name = ToLower(Name);
		for (char& Ch : Name)
		{
			if (Ch == '-' || Ch == ' ')
				Ch = '_';
		}
		return Name;
	}

	int FindBoneIndexByAliases(const std::vector<SkeletonBonePoint>& Bones, std::initializer_list<const char*> Aliases)
	{
		for (const char* Alias : Aliases)
		{
			const std::string LowerAlias = NormalizeBoneName(Alias);
			for (int Index = 0; Index < static_cast<int>(Bones.size()); ++Index)
			{
				if (NormalizeBoneName(Bones[Index].Name) == LowerAlias)
					return Index;
			}
		}

		for (const char* Alias : Aliases)
		{
			const std::string LowerAlias = NormalizeBoneName(Alias);
			for (int Index = 0; Index < static_cast<int>(Bones.size()); ++Index)
			{
				if (NormalizeBoneName(Bones[Index].Name).find(LowerAlias) != std::string::npos)
					return Index;
			}
		}

		return -1;
	}

	void AddSkeletonSegment(std::vector<SkeletonSegment>& Segments, int A, int B)
	{
		if (A < 0 || B < 0 || A == B)
			return;

		for (const SkeletonSegment& Segment : Segments)
		{
			if ((Segment.A == A && Segment.B == B) || (Segment.A == B && Segment.B == A))
				return;
		}

		Segments.push_back({ A, B });
	}

	void BuildSkeletonSegments(const std::vector<SkeletonBonePoint>& Bones, std::vector<SkeletonSegment>& Segments)
	{
		auto Bone = [&](std::initializer_list<const char*> Aliases)
		{
			return FindBoneIndexByAliases(Bones, Aliases);
		};
		auto Pair = [&](std::initializer_list<const char*> A, std::initializer_list<const char*> B)
		{
			AddSkeletonSegment(Segments, Bone(A), Bone(B));
		};

		Pair({ "root" }, { "pelvis", "hips" });
		Pair({ "pelvis", "hips" }, { "spine_01", "spine1", "spine" });
		Pair({ "spine_01", "spine1", "spine" }, { "spine_02", "spine2", "chest" });
		Pair({ "spine_02", "spine2" }, { "spine_03", "spine3", "chest" });
		Pair({ "spine_03", "spine3", "chest" }, { "neck_01", "neck" });
		Pair({ "neck_01", "neck" }, { "head", "head_01" });

		Pair({ "spine_03", "chest", "neck" }, { "clavicle_l", "leftclavicle", "l_clavicle" });
		Pair({ "clavicle_l", "leftclavicle", "l_clavicle" }, { "upperarm_l", "lowerarm_l", "leftarm", "l_upperarm" });
		Pair({ "upperarm_l", "leftarm", "l_upperarm" }, { "lowerarm_l", "leftforearm", "l_lowerarm" });
		Pair({ "lowerarm_l", "leftforearm", "l_lowerarm" }, { "hand_l", "lefthand", "l_hand" });

		Pair({ "spine_03", "chest", "neck" }, { "clavicle_r", "rightclavicle", "r_clavicle" });
		Pair({ "clavicle_r", "rightclavicle", "r_clavicle" }, { "upperarm_r", "rightarm", "r_upperarm" });
		Pair({ "upperarm_r", "rightarm", "r_upperarm" }, { "lowerarm_r", "rightforearm", "r_lowerarm" });
		Pair({ "lowerarm_r", "rightforearm", "r_lowerarm" }, { "hand_r", "righthand", "r_hand" });

		Pair({ "pelvis", "hips" }, { "thigh_l", "leftupleg", "l_thigh", "leg_l" });
		Pair({ "thigh_l", "leftupleg", "l_thigh", "leg_l" }, { "calf_l", "leftleg", "l_calf" });
		Pair({ "calf_l", "leftleg", "l_calf" }, { "foot_l", "leftfoot", "l_foot" });
		Pair({ "foot_l", "leftfoot", "l_foot" }, { "ball_l", "toe_l", "lefttoe" });

		Pair({ "pelvis", "hips" }, { "thigh_r", "rightupleg", "r_thigh", "leg_r" });
		Pair({ "thigh_r", "rightupleg", "r_thigh", "leg_r" }, { "calf_r", "rightleg", "r_calf" });
		Pair({ "calf_r", "rightleg", "r_calf" }, { "foot_r", "rightfoot", "r_foot" });
		Pair({ "foot_r", "rightfoot", "r_foot" }, { "ball_r", "toe_r", "righttoe" });

		if (!Segments.empty())
			return;

		for (int Index = 1; Index < static_cast<int>(Bones.size()); ++Index)
			AddSkeletonSegment(Segments, Index - 1, Index);
	}

	bool CaptureSkeleton(UEObject Actor, const OverlayConfig& Config, CaptureStats& Stats, UEObject PlayerController, UEFunction ProjectionFunction,
		const Vec3& CameraLocation, const Vec3& CameraRotation, float CameraFov, const RECT& ProjectionRect, float ViewWidth, float ViewHeight,
		ActorDebugInfo& Info)
	{
		if ((!Config.DrawSkeletons && !Config.DrawBoxes) || !gSymbols.GetNumBones || !gSymbols.GetBoneName || !gSymbols.GetBoneLocation)
			return false;

		UEObject MeshComponent;
		std::string MeshSource;
		if (!FindSkinnedMeshComponent(Actor, MeshComponent, MeshSource))
			return false;

		int32 NumBones = 0;
		if (!CallNoArgIntFunction(MeshComponent, gSymbols.GetNumBones, NumBones))
			return false;

		NumBones = std::clamp(NumBones, 0, std::clamp(Config.SkeletonMaxBones, 4, 256));
		if (NumBones <= 1)
			return false;

		Info.SkeletonBones.clear();
		Info.SkeletonSegments.clear();
		Info.HasSkeleton = false;
		Info.SkeletonSource.clear();
		Info.SkeletonBones.reserve(static_cast<size_t>(NumBones));

		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			RawNameValue BoneName;
			if (!CallBoneNameFunction(MeshComponent, gSymbols.GetBoneName, BoneIndex, BoneName))
				continue;

			Vec3 BoneLocation;
			if (!CallBoneLocationFunction(MeshComponent, gSymbols.GetBoneLocation, BoneName, BoneLocation) || !IsSaneWorldPosition(BoneLocation))
				continue;

			SkeletonBonePoint Bone;
			Bone.Index = BoneIndex;
			Bone.Name = BoneName.Text.empty() ? ("bone_" + std::to_string(BoneIndex)) : BoneName.Text;
			Bone.Location = BoneLocation;
			Bone.HasScreen = ProjectWorldToScreenAny(PlayerController, ProjectionFunction, Config, Stats,
				CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, BoneLocation, Bone.Screen);
			if (Bone.HasScreen)
				Info.SkeletonBones.push_back(std::move(Bone));
		}

		if (Info.SkeletonBones.size() < 2)
		{
			Info.SkeletonBones.clear();
			return false;
		}

		BuildSkeletonSegments(Info.SkeletonBones, Info.SkeletonSegments);
		if (Info.SkeletonSegments.empty())
		{
			Info.SkeletonBones.clear();
			return false;
		}

		Info.HasSkeleton = true;
		Info.SkeletonSource = MeshSource;
		Stats.SkeletonActors++;
		Stats.SkeletonBones += static_cast<int32>(Info.SkeletonBones.size());
		Stats.SkeletonSegments += static_cast<int32>(Info.SkeletonSegments.size());
		return true;
	}

	bool ReprojectCachedSkeleton(ActorDebugInfo& Info, const OverlayConfig& Config, CaptureStats& Stats, UEObject PlayerController, UEFunction ProjectionFunction,
		const Vec3& CameraLocation, const Vec3& CameraRotation, float CameraFov, const RECT& ProjectionRect, float ViewWidth, float ViewHeight,
		const Vec3& LocationDelta)
	{
		if ((!Config.DrawSkeletons && !Config.DrawBoxes) || !Info.HasSkeleton || Info.SkeletonBones.size() < 2 || Info.SkeletonSegments.empty())
			return false;

		int32 ProjectedBones = 0;
		for (SkeletonBonePoint& Bone : Info.SkeletonBones)
		{
			Bone.Location.X += LocationDelta.X;
			Bone.Location.Y += LocationDelta.Y;
			Bone.Location.Z += LocationDelta.Z;
			Bone.HasScreen = ProjectWorldToScreenAny(PlayerController, ProjectionFunction, Config, Stats,
				CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Bone.Location, Bone.Screen);
			if (Bone.HasScreen)
				ProjectedBones++;
		}

		if (ProjectedBones < 2)
		{
			Info.HasSkeleton = false;
			Info.SkeletonBones.clear();
			Info.SkeletonSegments.clear();
			return false;
		}

		Stats.SkeletonActors++;
		Stats.SkeletonBones += ProjectedBones;
		Stats.SkeletonSegments += static_cast<int32>(Info.SkeletonSegments.size());
		return true;
	}

	bool ReadArrayProperty(UEObject Object, UEProperty Property, RawTArrayView& OutArray)
	{
		if (!Object || !Property)
			return false;

		const int32 Offset = Property.GetOffset();
		return ReadArrayPropertyAtOffset(Object, Offset, OutArray);
	}

	bool ReadArrayPropertyAtOffset(UEObject Object, int32_t Offset, RawTArrayView& OutArray)
	{
		if (!Object)
			return false;

		if (Offset < 0)
			return false;

		uint8* Address = static_cast<uint8*>(Object.GetAddress()) + Offset;
		if (!IsReadablePointer(Address))
			return false;

		const RawTArrayView Array = *reinterpret_cast<RawTArrayView*>(Address);
		if (Array.Num < 0 || Array.Max < Array.Num || Array.Num > 0x20000)
			return false;

		if (Array.Num > 0 && !IsReadablePointer(Array.Data))
			return false;

		OutArray = Array;
		return true;
	}

	size_t RuntimeParamBufferSize(const RuntimeFunctionInfo& Function)
	{
		size_t Size = 1;
		auto AddProperty = [&](const RuntimePropertyInfo& Property)
		{
			if (Property.offset >= 0 && Property.size > 0)
				Size = std::max(Size, static_cast<size_t>(Property.offset + Property.size));
		};

		for (const RuntimePropertyInfo& Param : Function.params)
			AddProperty(Param);

		if (Function.hasReturnValue)
			AddProperty(Function.returnValue);

		return Size;
	}

	std::vector<uint8> MakeRuntimeParamBuffer(const RuntimeFunctionInfo& Function)
	{
		return std::vector<uint8>(RuntimeParamBufferSize(Function), 0);
	}

	std::string NormalizeRuntimeParamName(std::string Name)
	{
		std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char Ch)
		{
			return static_cast<char>(std::tolower(Ch));
		});

		while (!Name.empty() && std::isspace(static_cast<unsigned char>(Name.back())))
			Name.pop_back();

		const size_t Underscore = Name.find_last_of('_');
		if (Underscore != std::string::npos && Underscore + 1 < Name.size())
		{
			bool NumericSuffix = true;
			for (size_t Index = Underscore + 1; Index < Name.size(); ++Index)
			{
				if (!std::isdigit(static_cast<unsigned char>(Name[Index])))
				{
					NumericSuffix = false;
					break;
				}
			}

			if (NumericSuffix)
				Name.erase(Underscore);
		}

		return Name;
	}

	bool RuntimeParamNameMatches(const std::string& ActualName, const char* WantedName)
	{
		if (!WantedName)
			return false;

		const std::string Actual = NormalizeRuntimeParamName(ActualName);
		const std::string Wanted = NormalizeRuntimeParamName(WantedName);
		if (Actual == Wanted)
			return true;

		return Actual.size() > Wanted.size()
			&& Actual.rfind(Wanted, 0) == 0
			&& (Actual[Wanted.size()] == '_' || Actual[Wanted.size()] == '.');
	}

	const RuntimePropertyInfo* FindRuntimeParam(const RuntimeFunctionInfo& Function, std::initializer_list<const char*> Names)
	{
		for (const char* Name : Names)
		{
			for (const RuntimePropertyInfo& Param : Function.params)
			{
				if (RuntimeParamNameMatches(Param.propertyName, Name))
					return &Param;
			}
		}

		return nullptr;
	}

	std::string DescribeRuntimeFunctionParams(const RuntimeFunctionInfo& Function)
	{
		std::ostringstream Stream;
		Stream << Function.fullName << " params=[";
		for (size_t Index = 0; Index < Function.params.size(); ++Index)
		{
			const RuntimePropertyInfo& Param = Function.params[Index];
			if (Index > 0)
				Stream << ", ";
			Stream << Param.propertyName << "@0x" << std::hex << Param.offset << std::dec << ":" << Param.size;
		}
		Stream << "]";
		if (Function.hasReturnValue)
			Stream << " return=" << Function.returnValue.propertyName << "@0x" << std::hex << Function.returnValue.offset << std::dec << ":" << Function.returnValue.size;
		return Stream.str();
	}

	bool EnsureRuntimeParamSize(std::vector<uint8>& Params, const RuntimePropertyInfo& Property)
	{
		if (Property.offset < 0 || Property.size <= 0)
			return false;

		const size_t RequiredSize = static_cast<size_t>(Property.offset + Property.size);
		if (RequiredSize > Params.size())
			Params.resize(RequiredSize, 0);

		return true;
	}

	bool WriteRuntimeObjectParam(std::vector<uint8>& Params, const RuntimePropertyInfo* Property, UEObject Value)
	{
		if (!Property || !EnsureRuntimeParamSize(Params, *Property) || Property->size < static_cast<int32_t>(sizeof(void*)))
			return false;

		*reinterpret_cast<void**>(Params.data() + Property->offset) = Value.GetAddress();
		return true;
	}

	bool WriteRuntimeIntParam(std::vector<uint8>& Params, const RuntimePropertyInfo* Property, int32_t Value)
	{
		if (!Property || !EnsureRuntimeParamSize(Params, *Property) || Property->size < static_cast<int32_t>(sizeof(int32_t)))
			return false;

		*reinterpret_cast<int32_t*>(Params.data() + Property->offset) = Value;
		return true;
	}

	bool WriteRuntimeStringParam(std::vector<uint8>& Params, const RuntimePropertyInfo* Property, const wchar_t* Value)
	{
		if (!Property || !Value || !EnsureRuntimeParamSize(Params, *Property) || Property->size < static_cast<int32_t>(sizeof(FString)))
			return false;

		FString StringValue(Value);
		std::memcpy(Params.data() + Property->offset, &StringValue, sizeof(FString));
		return true;
	}

	bool ReadRuntimeObjectReturn(const std::vector<uint8>& Params, const RuntimeFunctionInfo& Function, UEObject& OutObject)
	{
		if (!Function.hasReturnValue || Function.returnValue.offset < 0 || Function.returnValue.size < static_cast<int32_t>(sizeof(void*)))
			return false;

		const size_t Offset = static_cast<size_t>(Function.returnValue.offset);
		if (Offset + sizeof(void*) > Params.size())
			return false;

		void* Pointer = *reinterpret_cast<void* const*>(Params.data() + Offset);
		if (!IsReadableObject(Pointer))
			return false;

		OutObject = UEObject(Pointer);
		return true;
	}

	bool ReadRuntimeNameReturn(const std::vector<uint8>& Params, const RuntimeFunctionInfo& Function, RawNameValue& OutName)
	{
		if (!Function.hasReturnValue || Function.returnValue.offset < 0 || Function.returnValue.size <= 0)
			return false;

		const int32_t Size = std::min<int32_t>(Function.returnValue.size, static_cast<int32_t>(OutName.Bytes.size()));
		const size_t Offset = static_cast<size_t>(Function.returnValue.offset);
		if (Offset + static_cast<size_t>(Size) > Params.size())
			return false;

		std::copy(Params.data() + Offset, Params.data() + Offset + Size, OutName.Bytes.begin());
		OutName.Size = Size;
		OutName.Text = FName(OutName.Bytes.data()).ToString();
		return true;
	}

	bool ReadObjectArrayProperty(UEObject Object, UEProperty Property, std::vector<UEObject>& OutObjects, int32 MaxObjects = 4096)
	{
		OutObjects.clear();

		RawTArrayView Array;
		if (!ReadArrayProperty(Object, Property, Array))
			return false;

		const int32 Count = std::clamp(Array.Num, 0, std::max(MaxObjects, 0));
		if (Count <= 0)
			return true;

		void** RawData = static_cast<void**>(Array.Data);
		if (!IsReadableRange(RawData, static_cast<size_t>(Count) * sizeof(void*)))
			return false;

		OutObjects.reserve(static_cast<size_t>(Count));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			void* Pointer = RawData[Index];
			if (IsReadableObject(Pointer))
				OutObjects.emplace_back(Pointer);
		}

		return true;
	}

	bool ReadObjectArrayPropertyAtOffset(UEObject Object, int32_t Offset, std::vector<UEObject>& OutObjects, int32 MaxObjects = 4096)
	{
		OutObjects.clear();

		RawTArrayView Array;
		if (!ReadArrayPropertyAtOffset(Object, Offset, Array))
			return false;

		const int32 Count = std::clamp(Array.Num, 0, std::max(MaxObjects, 0));
		if (Count <= 0)
			return true;

		void** RawData = static_cast<void**>(Array.Data);
		if (!IsReadableRange(RawData, static_cast<size_t>(Count) * sizeof(void*)))
			return false;

		OutObjects.reserve(static_cast<size_t>(Count));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			void* Pointer = RawData[Index];
			if (IsReadableObject(Pointer))
				OutObjects.emplace_back(Pointer);
		}

		return true;
	}

	bool ReadLevelActors(UEObject Level, std::vector<UEObject>& OutActors, CaptureStats& Stats)
	{
		const int32_t ActorsOffset = gRuntimeSymbols.levelActorsOffset >= 0
			? gRuntimeSymbols.levelActorsOffset
			: Off::InSDK::ULevel::Actors;

		if (!Level || ActorsOffset <= 0)
			return false;

		uint8* Address = static_cast<uint8*>(Level.GetAddress()) + ActorsOffset;
		if (!IsReadablePointer(Address))
			return false;

		const RawTArrayView ActorArray = *reinterpret_cast<RawTArrayView*>(Address);
		if (ActorArray.Num < 0 || ActorArray.Max < ActorArray.Num || ActorArray.Num > 0x40000)
			return false;

		Stats.LevelActorSlots += ActorArray.Num;

		if (ActorArray.Num > 0 && !IsReadablePointer(ActorArray.Data))
			return false;

		void** ActorData = static_cast<void**>(ActorArray.Data);
		for (int32 Index = 0; Index < ActorArray.Num; ++Index)
		{
			void* ActorPointer = ActorData[Index];
			if (IsReadableObject(ActorPointer))
				OutActors.emplace_back(ActorPointer);
		}

		return true;
	}

	UEObject ReadGWorldObject()
	{
		const int32_t GWorldOffset = RuntimeSDK::IsReady()
			? RuntimeSDK::GetDatabase().Globals().gWorld
			: Off::InSDK::World::GWorld;

		if (GWorldOffset <= 0)
			return {};

		const uintptr_t ModuleBase = Platform::GetModuleBase(Settings::General::DefaultModuleName);
		void** WorldAddress = reinterpret_cast<void**>(ModuleBase + GWorldOffset);
		if (!IsReadablePointer(WorldAddress))
			return {};

		void* WorldPointer = *WorldAddress;
		if (!IsReadableObject(WorldPointer))
			return {};

		return UEObject(WorldPointer);
	}

	bool GetActorLocation(UEObject Actor, Vec3& OutLocation, std::string* OutSource = nullptr)
	{
		if (CallNoArgVectorFunction(Actor, gSymbols.GetActorLocation, OutLocation))
		{
			if (OutSource)
				*OutSource = "Actor.GetActorLocation()";
			return true;
		}

		UEObject RootComponent;
		if (!ReadObjectPropertyAtOffset(Actor, gRuntimeSymbols.actorRootComponentOffset, RootComponent))
			return false;

		if (CallNoArgVectorFunction(RootComponent, gSymbols.GetComponentLocation, OutLocation))
		{
			if (OutSource)
				*OutSource = "RootComponent.GetComponentLocation()";
			return true;
		}

		return false;
	}

	const RuntimeStructInfo* FindRuntimeStructAny(const RuntimeDatabase& Db, std::initializer_list<std::string> Names)
	{
		for (const std::string& Name : Names)
		{
			if (const RuntimeStructInfo* Struct = Db.FindStruct(Name))
				return Struct;
		}

		return nullptr;
	}

	uintptr_t RuntimeStructAddressAny(const RuntimeDatabase& Db, std::initializer_list<std::string> Names)
	{
		const RuntimeStructInfo* Struct = FindRuntimeStructAny(Db, Names);
		return Struct ? Struct->address : 0;
	}

	RuntimeFunctionInfo RuntimeFunctionAny(const RuntimeDatabase& Db, std::initializer_list<std::string> Names)
	{
		for (const std::string& Name : Names)
		{
			if (const RuntimeFunctionInfo* Function = Db.FindFunction(Name))
				return *Function;
		}

		return {};
	}

	int32_t RuntimePropertySizeAny(const RuntimeDatabase& Db, std::initializer_list<std::string> Names)
	{
		for (const std::string& Name : Names)
		{
			if (const RuntimePropertyInfo* Property = Db.FindProperty(Name))
				return Property->size;
		}

		return 0;
	}

	UEClass ClassFromAddress(uintptr_t Address)
	{
		return Address ? UEClass(reinterpret_cast<void*>(Address)) : UEClass();
	}

	UEFunction FunctionFromRuntime(const RuntimeFunctionInfo& Function)
	{
		return Function.address ? UEFunction(reinterpret_cast<void*>(Function.address)) : UEFunction();
	}

	bool ResolveOverlaySymbolsFromRuntimeDB(RuntimeOverlaySymbols& Symbols)
	{
		if (Symbols.ready)
			return true;

		if (!RuntimeSDK::IsReady())
			return false;

		const RuntimeDatabase& Db = RuntimeSDK::GetDatabase();

		Symbols.actorClass = RuntimeStructAddressAny(Db, { "AActor", "Actor", "Engine.Actor" });
		Symbols.worldClass = RuntimeStructAddressAny(Db, { "UWorld", "World", "Engine.World" });
		Symbols.levelClass = RuntimeStructAddressAny(Db, { "ULevel", "Level", "Engine.Level" });
		Symbols.pawnClass = RuntimeStructAddressAny(Db, { "APawn", "Pawn", "Engine.Pawn" });
		Symbols.characterClass = RuntimeStructAddressAny(Db, { "ACharacter", "Character", "Engine.Character" });
		Symbols.sceneComponentClass = RuntimeStructAddressAny(Db, { "USceneComponent", "SceneComponent", "Engine.SceneComponent" });
		Symbols.primitiveComponentClass = RuntimeStructAddressAny(Db, { "UPrimitiveComponent", "PrimitiveComponent", "Engine.PrimitiveComponent" });
		Symbols.skinnedMeshComponentClass = RuntimeStructAddressAny(Db, { "USkinnedMeshComponent", "SkinnedMeshComponent", "Engine.SkinnedMeshComponent" });
		Symbols.skeletalMeshComponentClass = RuntimeStructAddressAny(Db, { "USkeletalMeshComponent", "SkeletalMeshComponent", "Engine.SkeletalMeshComponent" });
		Symbols.playerControllerClass = RuntimeStructAddressAny(Db, { "APlayerController", "PlayerController", "Engine.PlayerController" });
		Symbols.playerCameraManagerClass = RuntimeStructAddressAny(Db, { "APlayerCameraManager", "PlayerCameraManager", "Engine.PlayerCameraManager" });
		Symbols.gameInstanceClass = RuntimeStructAddressAny(Db, { "UGameInstance", "GameInstance", "Engine.GameInstance" });
		Symbols.localPlayerClass = RuntimeStructAddressAny(Db, { "ULocalPlayer", "LocalPlayer", "Engine.LocalPlayer" });
		Symbols.gameStateBaseClass = RuntimeStructAddressAny(Db, { "AGameStateBase", "GameStateBase", "Engine.GameStateBase", "AGameState", "GameState", "Engine.GameState" });
		Symbols.playerStateClass = RuntimeStructAddressAny(Db, { "APlayerState", "PlayerState", "Engine.PlayerState" });
		Symbols.engineClass = RuntimeStructAddressAny(Db, { "UEngine", "Engine", "Engine.Engine" });
		Symbols.gameEngineClass = RuntimeStructAddressAny(Db, { "UGameEngine", "GameEngine", "Engine.GameEngine" });
		Symbols.gameViewportClientClass = RuntimeStructAddressAny(Db, { "UGameViewportClient", "GameViewportClient", "Engine.GameViewportClient" });
		Symbols.consoleClass = RuntimeStructAddressAny(Db, { "UConsole", "Console", "Engine.Console" });
		Symbols.gameplayStaticsClass = RuntimeStructAddressAny(Db, { "UGameplayStatics", "GameplayStatics", "Engine.GameplayStatics" });
		Symbols.inputSettingsClass = RuntimeStructAddressAny(Db, { "UInputSettings", "InputSettings", "Engine.InputSettings" });
		Symbols.kismetStringLibraryClass = RuntimeStructAddressAny(Db, { "UKismetStringLibrary", "KismetStringLibrary", "Engine.KismetStringLibrary" });
		Symbols.kismetSystemLibraryClass = RuntimeStructAddressAny(Db, { "UKismetSystemLibrary", "KismetSystemLibrary", "Engine.KismetSystemLibrary" });
		Symbols.crabCharacterClass = RuntimeStructAddressAny(Db, { "ACrabC", "CrabC", "CrabChampions.CrabC" });
		Symbols.crabEnemyClass = RuntimeStructAddressAny(Db, { "ACrabEnemyC", "CrabEnemyC", "CrabChampions.CrabEnemyC" });
		Symbols.crabPlayerCharacterClass = RuntimeStructAddressAny(Db, { "ACrabPlayerC", "CrabPlayerC", "CrabChampions.CrabPlayerC" });

		Symbols.worldPersistentLevelOffset = RuntimeAccess::OffsetAny({ "UWorld::PersistentLevel", "World::PersistentLevel", "Engine.World::PersistentLevel" });
		Symbols.worldLevelsOffset = RuntimeAccess::OffsetAny({ "UWorld::Levels", "World::Levels", "Engine.World::Levels", "UWorld::StreamingLevels" });
		Symbols.worldGameInstanceOffset = RuntimeAccess::OffsetAny({ "UWorld::OwningGameInstance", "World::OwningGameInstance", "UWorld::GameInstance" });
		Symbols.worldGameStateOffset = RuntimeAccess::OffsetAny({ "UWorld::GameState", "World::GameState", "UWorld::AuthorityGameState" });
		Symbols.gameInstanceLocalPlayersOffset = RuntimeAccess::OffsetAny({ "UGameInstance::LocalPlayers", "GameInstance::LocalPlayers", "Engine.GameInstance::LocalPlayers" });
		Symbols.localPlayerControllerOffset = RuntimeAccess::OffsetAny({ "ULocalPlayer::PlayerController", "LocalPlayer::PlayerController", "Engine.LocalPlayer::PlayerController" });
		Symbols.localPlayerViewportClientOffset = RuntimeAccess::OffsetAny({ "ULocalPlayer::ViewportClient", "LocalPlayer::ViewportClient", "Engine.LocalPlayer::ViewportClient" });
		Symbols.playerControllerAcknowledgedPawnOffset = RuntimeAccess::OffsetAny({ "APlayerController::AcknowledgedPawn", "PlayerController::AcknowledgedPawn", "APlayerController::Pawn" });
		Symbols.playerControllerCharacterOffset = RuntimeAccess::OffsetAny({ "APlayerController::Character", "PlayerController::Character" });
		Symbols.playerControllerCameraManagerOffset = RuntimeAccess::OffsetAny({ "APlayerController::PlayerCameraManager", "PlayerController::PlayerCameraManager", "APlayerController::CameraManager" });
		Symbols.pawnPlayerStateOffset = RuntimeAccess::OffsetAny({ "APawn::PlayerState", "Pawn::PlayerState", "Engine.Pawn::PlayerState" });
		Symbols.playerStatePawnOffset = RuntimeAccess::OffsetAny({ "APlayerState::PawnPrivate", "PlayerState::PawnPrivate", "APlayerState::Pawn", "APlayerState::Character", "APlayerState::ControlledPawn", "APlayerState::AcknowledgedPawn" });
		Symbols.playerStateOwnerOffset = RuntimeAccess::OffsetAny({ "APlayerState::Owner", "PlayerState::Owner", "APlayerState::PlayerController", "APlayerState::Controller" });
		Symbols.gameStatePlayerArrayOffset = RuntimeAccess::OffsetAny({ "AGameStateBase::PlayerArray", "GameStateBase::PlayerArray", "AGameState::PlayerArray", "GameState::PlayerArray" });
		Symbols.levelActorsOffset = RuntimeAccess::OffsetAny({ "ULevel::Actors", "Level::Actors", "Engine.Level::Actors" });
		Symbols.actorRootComponentOffset = RuntimeAccess::OffsetAny({ "AActor::RootComponent", "Actor::RootComponent", "Engine.Actor::RootComponent" });
		Symbols.primitiveBoundsOffset = RuntimeAccess::OffsetAny({ "UPrimitiveComponent::Bounds", "PrimitiveComponent::Bounds", "Engine.PrimitiveComponent::Bounds" });
		Symbols.primitiveBoundsSize = RuntimePropertySizeAny(Db, { "UPrimitiveComponent::Bounds", "PrimitiveComponent::Bounds", "Engine.PrimitiveComponent::Bounds" });
		Symbols.engineConsoleClassOffset = RuntimeAccess::OffsetAny({ "UEngine::ConsoleClass", "Engine::ConsoleClass", "Engine.Engine::ConsoleClass" });
		Symbols.engineGameViewportOffset = RuntimeAccess::OffsetAny({ "UGameEngine::GameViewport", "GameEngine::GameViewport", "Engine.GameEngine::GameViewport", "UEngine::GameViewport", "Engine::GameViewport" });
		Symbols.gameViewportConsoleOffset = RuntimeAccess::OffsetAny({ "UGameViewportClient::ViewportConsole", "GameViewportClient::ViewportConsole", "Engine.GameViewportClient::ViewportConsole" });
		Symbols.consoleTargetPlayerOffset = RuntimeAccess::OffsetAny({ "UConsole::ConsoleTargetPlayer", "Console::ConsoleTargetPlayer", "Engine.Console::ConsoleTargetPlayer" });
		Symbols.inputSettingsConsoleKeyOffset = RuntimeAccess::OffsetAny({ "UInputSettings::ConsoleKey", "InputSettings::ConsoleKey", "Engine.InputSettings::ConsoleKey" });
		Symbols.inputSettingsConsoleKeysOffset = RuntimeAccess::OffsetAny({ "UInputSettings::ConsoleKeys", "InputSettings::ConsoleKeys", "Engine.InputSettings::ConsoleKeys" });
		Symbols.keyNameOffset = RuntimeAccess::OffsetAny({ "FKey::KeyName", "Key::KeyName", "InputCore.Key::KeyName" });

		if (Symbols.engineConsoleClassOffset < 0)
			Symbols.engineConsoleClassOffset = 0xF0;
		if (Symbols.engineGameViewportOffset < 0)
			Symbols.engineGameViewportOffset = 0x780;
		if (Symbols.gameViewportConsoleOffset < 0)
			Symbols.gameViewportConsoleOffset = 0x40;
		if (Symbols.consoleTargetPlayerOffset < 0)
			Symbols.consoleTargetPlayerOffset = 0x38;
		if (Symbols.localPlayerViewportClientOffset < 0)
			Symbols.localPlayerViewportClientOffset = 0x70;
		if (Symbols.inputSettingsConsoleKeyOffset < 0)
			Symbols.inputSettingsConsoleKeyOffset = 0x118;
		if (Symbols.inputSettingsConsoleKeysOffset < 0)
			Symbols.inputSettingsConsoleKeysOffset = 0x130;
		if (Symbols.keyNameOffset < 0)
			Symbols.keyNameOffset = 0;

		Symbols.getActorLocation = RuntimeFunctionAny(Db, { "AActor::K2_GetActorLocation", "AActor::GetActorLocation", "Actor::K2_GetActorLocation", "Actor::GetActorLocation" });
		Symbols.getActorBounds = RuntimeFunctionAny(Db, { "AActor::GetActorBounds", "AActor::K2_GetActorBounds", "Actor::GetActorBounds", "Actor::K2_GetActorBounds" });
		Symbols.getComponentLocation = RuntimeFunctionAny(Db, { "USceneComponent::K2_GetComponentLocation", "USceneComponent::GetComponentLocation", "SceneComponent::K2_GetComponentLocation", "SceneComponent::GetComponentLocation" });
		Symbols.getPawn = RuntimeFunctionAny(Db, { "APlayerController::K2_GetPawn", "APlayerController::GetPawn", "PlayerController::K2_GetPawn", "PlayerController::GetPawn" });
		Symbols.projectWorldLocationToScreen = RuntimeFunctionAny(Db, { "APlayerController::ProjectWorldLocationToScreen", "PlayerController::ProjectWorldLocationToScreen" });
		Symbols.getPlayerViewPoint = RuntimeFunctionAny(Db, { "APlayerController::GetPlayerViewPoint", "PlayerController::GetPlayerViewPoint" });
		Symbols.getControlRotation = RuntimeFunctionAny(Db, { "APlayerController::GetControlRotation", "PlayerController::GetControlRotation" });
		Symbols.getControllerActorRotation = RuntimeFunctionAny(Db, { "APlayerController::K2_GetActorRotation", "APlayerController::GetActorRotation", "PlayerController::K2_GetActorRotation", "PlayerController::GetActorRotation" });
		Symbols.getCameraLocation = RuntimeFunctionAny(Db, { "APlayerCameraManager::GetCameraLocation", "APlayerCameraManager::K2_GetActorLocation", "APlayerCameraManager::GetActorLocation", "PlayerCameraManager::GetCameraLocation" });
		Symbols.getCameraRotation = RuntimeFunctionAny(Db, { "APlayerCameraManager::GetCameraRotation", "APlayerCameraManager::K2_GetActorRotation", "APlayerCameraManager::GetActorRotation", "PlayerCameraManager::GetCameraRotation" });
		Symbols.getCameraFov = RuntimeFunctionAny(Db, { "APlayerCameraManager::GetFOVAngle", "APlayerCameraManager::GetCameraFOV", "APlayerCameraManager::GetCameraFov", "PlayerCameraManager::GetFOVAngle" });
		Symbols.spawnObject = RuntimeFunctionAny(Db, { "UGameplayStatics::SpawnObject", "GameplayStatics::SpawnObject" });
		Symbols.convStringToName = RuntimeFunctionAny(Db, { "UKismetStringLibrary::Conv_StringToName", "KismetStringLibrary::Conv_StringToName" });
		Symbols.setConsoleTarget = RuntimeFunctionAny(Db, { "UGameViewportClient::SetConsoleTarget", "GameViewportClient::SetConsoleTarget" });
		Symbols.consoleKey = RuntimeFunctionAny(Db, { "APlayerController::ConsoleKey", "PlayerController::ConsoleKey" });
		Symbols.sendToConsole = RuntimeFunctionAny(Db, { "APlayerController::SendToConsole", "PlayerController::SendToConsole" });
		Symbols.executeConsoleCommand = RuntimeFunctionAny(Db, { "UKismetSystemLibrary::ExecuteConsoleCommand", "KismetSystemLibrary::ExecuteConsoleCommand" });

		const bool hasBoneSource = Symbols.skinnedMeshComponentClass != 0 || Symbols.skeletalMeshComponentClass != 0;
		if (hasBoneSource)
		{
			Symbols.getNumBones = RuntimeFunctionAny(Db, { "USkinnedMeshComponent::GetNumBones", "USkeletalMeshComponent::GetNumBones", "SkinnedMeshComponent::GetNumBones", "SkeletalMeshComponent::GetNumBones" });
			Symbols.getBoneName = RuntimeFunctionAny(Db, { "USkinnedMeshComponent::GetBoneName", "USkeletalMeshComponent::GetBoneName", "SkinnedMeshComponent::GetBoneName", "SkeletalMeshComponent::GetBoneName" });
			Symbols.getBoneLocation = RuntimeFunctionAny(Db, { "USkinnedMeshComponent::GetBoneLocation", "USkeletalMeshComponent::GetBoneLocation", "SkinnedMeshComponent::GetBoneLocation", "SkeletalMeshComponent::GetBoneLocation" });
		}

		Symbols.ready = Symbols.actorClass != 0
			&& (Symbols.getActorLocation.address != 0 || (Symbols.actorRootComponentOffset >= 0 && Symbols.getComponentLocation.address != 0));

		if (Symbols.ready)
			std::cerr << "[Overlay] Runtime symbols ready\n";

		return Symbols.ready;
	}

	bool ResolveSymbols(UnrealSymbols& Symbols)
	{
		if (Symbols.Ready)
			return true;

		if (!ResolveOverlaySymbolsFromRuntimeDB(gRuntimeSymbols))
			return false;

		Symbols.ActorClass = ClassFromAddress(gRuntimeSymbols.actorClass);
		Symbols.WorldClass = ClassFromAddress(gRuntimeSymbols.worldClass);
		Symbols.LevelClass = ClassFromAddress(gRuntimeSymbols.levelClass);
		Symbols.PawnClass = ClassFromAddress(gRuntimeSymbols.pawnClass);
		Symbols.CharacterClass = ClassFromAddress(gRuntimeSymbols.characterClass);
		Symbols.SceneComponentClass = ClassFromAddress(gRuntimeSymbols.sceneComponentClass);
		Symbols.PrimitiveComponentClass = ClassFromAddress(gRuntimeSymbols.primitiveComponentClass);
		Symbols.SkinnedMeshComponentClass = ClassFromAddress(gRuntimeSymbols.skinnedMeshComponentClass);
		Symbols.SkeletalMeshComponentClass = ClassFromAddress(gRuntimeSymbols.skeletalMeshComponentClass);
		Symbols.PlayerControllerClass = ClassFromAddress(gRuntimeSymbols.playerControllerClass);
		Symbols.PlayerCameraManagerClass = ClassFromAddress(gRuntimeSymbols.playerCameraManagerClass);
		Symbols.GameInstanceClass = ClassFromAddress(gRuntimeSymbols.gameInstanceClass);
		Symbols.LocalPlayerClass = ClassFromAddress(gRuntimeSymbols.localPlayerClass);
		Symbols.GameStateBaseClass = ClassFromAddress(gRuntimeSymbols.gameStateBaseClass);
		Symbols.PlayerStateClass = ClassFromAddress(gRuntimeSymbols.playerStateClass);

		Symbols.GetActorLocation = FunctionFromRuntime(gRuntimeSymbols.getActorLocation);
		Symbols.GetActorBounds = FunctionFromRuntime(gRuntimeSymbols.getActorBounds);
		Symbols.GetPawn = FunctionFromRuntime(gRuntimeSymbols.getPawn);
		Symbols.GetComponentLocation = FunctionFromRuntime(gRuntimeSymbols.getComponentLocation);
		Symbols.ProjectWorldLocationToScreen = FunctionFromRuntime(gRuntimeSymbols.projectWorldLocationToScreen);
		Symbols.GetNumBones = FunctionFromRuntime(gRuntimeSymbols.getNumBones);
		Symbols.GetBoneName = FunctionFromRuntime(gRuntimeSymbols.getBoneName);
		Symbols.GetBoneLocation = FunctionFromRuntime(gRuntimeSymbols.getBoneLocation);
		Symbols.GetPlayerViewPoint = FunctionFromRuntime(gRuntimeSymbols.getPlayerViewPoint);
		Symbols.GetControlRotation = FunctionFromRuntime(gRuntimeSymbols.getControlRotation);
		Symbols.GetControllerActorRotation = FunctionFromRuntime(gRuntimeSymbols.getControllerActorRotation);
		Symbols.GetCameraLocation = FunctionFromRuntime(gRuntimeSymbols.getCameraLocation);
		Symbols.GetCameraRotation = FunctionFromRuntime(gRuntimeSymbols.getCameraRotation);
		Symbols.GetCameraFov = FunctionFromRuntime(gRuntimeSymbols.getCameraFov);

		Symbols.Ready = gRuntimeSymbols.ready;
		return Symbols.Ready;
	}

	EObjectFlags ActorSkipFlags()
	{
		return EObjectFlags::ClassDefaultObject
			| EObjectFlags::ArchetypeObject
			| EObjectFlags::BeginDestroyed
			| EObjectFlags::FinishDestroyed;
	}

	UEObject FindFirstObjectOfClass(UEClass Class)
	{
		if (!Class)
			return {};

		const EObjectFlags SkipFlags = ActorSkipFlags();
		for (UEObject Object : ObjectArray())
		{
			if (!Object || Object.HasAnyFlags(SkipFlags))
				continue;

			if (Object.IsA(Class))
				return Object;
		}

		return {};
	}

	UEObject FindFirstObjectOfClassCached(UEClass Class, DWORD MaxAgeMs = 750)
	{
		if (!Class)
			return {};

		const uintptr_t ClassAddress = reinterpret_cast<uintptr_t>(Class.GetAddress());
		const DWORD Now = GetTickCount();
		{
			std::scoped_lock Lock(gClassObjectCacheMutex);
			auto It = gClassObjectCache.find(ClassAddress);
			if (It != gClassObjectCache.end()
				&& It->second.Address != 0
				&& (MaxAgeMs == 0 || Now - It->second.Tick <= MaxAgeMs)
				&& IsReadableObject(reinterpret_cast<void*>(It->second.Address)))
			{
				UEObject Cached(reinterpret_cast<void*>(It->second.Address));
				if (Cached && Cached.IsA(Class))
					return Cached;
			}
		}

		UEObject Found = FindFirstObjectOfClass(Class);
		{
			std::scoped_lock Lock(gClassObjectCacheMutex);
			CachedClassObject& Cache = gClassObjectCache[ClassAddress];
			Cache.Address = Found ? reinterpret_cast<uintptr_t>(Found.GetAddress()) : 0;
			Cache.Tick = Now;
		}

		return Found;
	}

	void SetConsoleUnlockStatus(bool Unlocked, const std::string& Status, UEObject Engine = {}, UEObject GameViewport = {}, UEObject Console = {})
	{
		gConsoleState.attempted = true;
		gConsoleState.unlocked = Unlocked;
		gConsoleState.status = Status;
		gConsoleState.engine = Engine ? reinterpret_cast<uintptr_t>(Engine.GetAddress()) : 0;
		gConsoleState.gameViewport = GameViewport ? reinterpret_cast<uintptr_t>(GameViewport.GetAddress()) : 0;
		gConsoleState.console = Console ? reinterpret_cast<uintptr_t>(Console.GetAddress()) : 0;
		gConsoleState.tick = GetTickCount();
	}

	UEObject FindEngineObject()
	{
		if (gRuntimeSymbols.gameEngineClass)
		{
			UEObject Engine = FindFirstObjectOfClassCached(ClassFromAddress(gRuntimeSymbols.gameEngineClass), 5000);
			if (Engine)
				return Engine;
		}

		if (gRuntimeSymbols.engineClass)
			return FindFirstObjectOfClassCached(ClassFromAddress(gRuntimeSymbols.engineClass), 5000);

		return {};
	}

	bool ProcessEventWithRuntimeFlags(UEObject Target, UEFunction Function, void* Params, std::string* OutFailure)
	{
		if (!Target || !Function)
		{
			if (OutFailure)
				*OutFailure = "ProcessEvent target or function missing";
			return false;
		}

		uint8* FunctionAddress = static_cast<uint8*>(Function.GetAddress());
		uint8* FlagsAddress = FunctionAddress ? FunctionAddress + Off::UFunction::FunctionFlags : nullptr;
		if (!IsReadableRange(FlagsAddress, sizeof(uint32_t)))
		{
			if (OutFailure)
				*OutFailure = "FunctionFlags address is not readable";
			return false;
		}

		uint32_t* FunctionFlags = reinterpret_cast<uint32_t*>(FlagsAddress);
		const uint32_t PreviousFlags = *FunctionFlags;
		*FunctionFlags = PreviousFlags | 0x400u;
		Target.ProcessEvent(Function, Params);
		*FunctionFlags = PreviousFlags;
		return true;
	}

	bool CallRuntimeSpawnObject(UEObject ObjectClass, UEObject Outer, UEObject& OutObject, std::string* OutFailure = nullptr)
	{
		if (!ObjectClass || !Outer || gRuntimeSymbols.spawnObject.address == 0 || gRuntimeSymbols.gameplayStaticsClass == 0)
		{
			if (OutFailure)
				*OutFailure = "SpawnObject metadata or input object missing";
			return false;
		}

		UEClass GameplayStaticsClass = ClassFromAddress(gRuntimeSymbols.gameplayStaticsClass);
		UEObject GameplayStaticsDefault = GameplayStaticsClass ? GameplayStaticsClass.GetDefaultObject() : UEObject();
		UEFunction SpawnFunction = FunctionFromRuntime(gRuntimeSymbols.spawnObject);
		if (!GameplayStaticsDefault || !SpawnFunction)
		{
			if (OutFailure)
				*OutFailure = "UGameplayStatics default object or function wrapper missing";
			return false;
		}

		const RuntimePropertyInfo* ObjectClassParam = FindRuntimeParam(gRuntimeSymbols.spawnObject, { "ObjectClass", "Class", "ObjectType" });
		const RuntimePropertyInfo* OuterParam = FindRuntimeParam(gRuntimeSymbols.spawnObject, { "Outer", "Owner", "OuterObject" });
		if (!ObjectClassParam || !OuterParam)
		{
			if (OutFailure)
				*OutFailure = "SpawnObject parameter match failed: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.spawnObject);
			return false;
		}

		std::vector<uint8> Params = MakeRuntimeParamBuffer(gRuntimeSymbols.spawnObject);
		if (!WriteRuntimeObjectParam(Params, ObjectClassParam, ObjectClass)
			|| !WriteRuntimeObjectParam(Params, OuterParam, Outer))
		{
			if (OutFailure)
				*OutFailure = "SpawnObject parameter write failed: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.spawnObject);
			return false;
		}

		if (!ProcessEventWithRuntimeFlags(GameplayStaticsDefault, SpawnFunction, Params.data(), OutFailure))
			return false;

		if (!ReadRuntimeObjectReturn(Params, gRuntimeSymbols.spawnObject, OutObject))
		{
			if (OutFailure)
				*OutFailure = "SpawnObject returned null or unreadable object: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.spawnObject);
			return false;
		}

		return true;
	}

	bool CallRuntimeSetConsoleTarget(UEObject GameViewport, int32_t PlayerIndex, std::string& OutNote)
	{
		if (!GameViewport || gRuntimeSymbols.setConsoleTarget.address == 0)
		{
			OutNote = "SetConsoleTarget unavailable";
			return false;
		}

		UEFunction Function = FunctionFromRuntime(gRuntimeSymbols.setConsoleTarget);
		if (!Function)
		{
			OutNote = "SetConsoleTarget function wrapper missing";
			return false;
		}

		const RuntimePropertyInfo* PlayerIndexParam = FindRuntimeParam(gRuntimeSymbols.setConsoleTarget, { "PlayerIndex", "Index" });
		if (!PlayerIndexParam)
		{
			OutNote = "SetConsoleTarget parameter match failed: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.setConsoleTarget);
			return false;
		}

		std::vector<uint8> Params = MakeRuntimeParamBuffer(gRuntimeSymbols.setConsoleTarget);
		if (!WriteRuntimeIntParam(Params, PlayerIndexParam, PlayerIndex))
		{
			OutNote = "SetConsoleTarget parameter write failed";
			return false;
		}

		if (!ProcessEventWithRuntimeFlags(GameViewport, Function, Params.data(), &OutNote))
			return false;

		OutNote = "SetConsoleTarget(0) called";
		return true;
	}

	bool MakeRuntimeNameFromString(const wchar_t* Text, RawNameValue& OutName)
	{
		if (!Text || gRuntimeSymbols.convStringToName.address == 0 || gRuntimeSymbols.kismetStringLibraryClass == 0)
			return false;

		UEClass StringLibraryClass = ClassFromAddress(gRuntimeSymbols.kismetStringLibraryClass);
		UEObject StringLibraryDefault = StringLibraryClass ? StringLibraryClass.GetDefaultObject() : UEObject();
		UEFunction ConvFunction = FunctionFromRuntime(gRuntimeSymbols.convStringToName);
		if (!StringLibraryDefault || !ConvFunction)
			return false;

		const RuntimePropertyInfo* StringParam = FindRuntimeParam(gRuntimeSymbols.convStringToName, { "InString", "String", "SourceString" });
		if (!StringParam)
			return false;

		std::vector<uint8> Params = MakeRuntimeParamBuffer(gRuntimeSymbols.convStringToName);
		if (!WriteRuntimeStringParam(Params, StringParam, Text))
			return false;

		if (!ProcessEventWithRuntimeFlags(StringLibraryDefault, ConvFunction, Params.data()))
			return false;

		return ReadRuntimeNameReturn(Params, gRuntimeSymbols.convStringToName, OutName);
	}

	bool TrySetConsoleKeyBinding(const wchar_t* KeyName, std::string& OutNote)
	{
		if (gRuntimeSymbols.inputSettingsClass == 0
			|| gRuntimeSymbols.keyNameOffset < 0)
		{
			OutNote = "console key binding metadata missing";
			return false;
		}

		RawNameValue KeyNameValue;
		if (!MakeRuntimeNameFromString(KeyName, KeyNameValue) || KeyNameValue.Size <= 0)
		{
			OutNote = "could not create FName for console key";
			return false;
		}

		UEClass InputSettingsClass = ClassFromAddress(gRuntimeSymbols.inputSettingsClass);
		UEObject InputSettingsDefault = InputSettingsClass ? InputSettingsClass.GetDefaultObject() : UEObject();
		if (!InputSettingsDefault)
		{
			OutNote = "UInputSettings default object missing";
			return false;
		}

		int32_t KeyStride = 0;
		if (RuntimeSDK::IsReady())
		{
			const RuntimeDatabase& Db = RuntimeSDK::GetDatabase();
			if (const RuntimeStructInfo* KeyStruct = Db.FindStruct("FKey"))
				KeyStride = KeyStruct->size;
			else if (const RuntimeStructInfo* KeyStruct = Db.FindStruct("Key"))
				KeyStride = KeyStruct->size;
			else if (const RuntimeStructInfo* KeyStruct = Db.FindStruct("InputCore.Key"))
				KeyStride = KeyStruct->size;
		}

		if (KeyStride <= 0)
			KeyStride = gRuntimeSymbols.keyNameOffset + KeyNameValue.Size;

		auto WriteKeyName = [&](uint8* KeyBase, const char* TargetName) -> bool
		{
			const int32_t RequiredSize = std::max(KeyStride, gRuntimeSymbols.keyNameOffset + KeyNameValue.Size);
			if (!IsReadableRange(KeyBase, static_cast<size_t>(RequiredSize)))
			{
				OutNote = std::string(TargetName) + " is not readable";
				return false;
			}

			uint8* KeyNameAddress = KeyBase + gRuntimeSymbols.keyNameOffset;
			if (!IsReadableRange(KeyNameAddress, static_cast<size_t>(KeyNameValue.Size)))
			{
				OutNote = std::string(TargetName) + ".KeyName is not readable";
				return false;
			}

			std::memcpy(KeyNameAddress, KeyNameValue.Bytes.data(), static_cast<size_t>(KeyNameValue.Size));
			OutNote = std::string(TargetName) + " set";
			return true;
		};

		RawTArrayView ConsoleKeys;
		if (gRuntimeSymbols.inputSettingsConsoleKeysOffset >= 0
			&& ReadArrayPropertyAtOffset(InputSettingsDefault, gRuntimeSymbols.inputSettingsConsoleKeysOffset, ConsoleKeys)
			&& ConsoleKeys.Num > 0
			&& ConsoleKeys.Data)
		{
			if (WriteKeyName(static_cast<uint8*>(ConsoleKeys.Data), "ConsoleKeys[0]"))
				return true;
		}

		if (gRuntimeSymbols.inputSettingsConsoleKeyOffset >= 0)
		{
			uint8* LegacyKey = static_cast<uint8*>(InputSettingsDefault.GetAddress()) + gRuntimeSymbols.inputSettingsConsoleKeyOffset;
			if (WriteKeyName(LegacyKey, "ConsoleKey"))
				return true;
		}

		if (OutNote.empty())
			OutNote = "ConsoleKeys array is empty and ConsoleKey fallback failed";
		return false;
	}

	bool WriteRuntimeKeyParam(std::vector<uint8>& Params, const RuntimePropertyInfo* Property, const RawNameValue& KeyNameValue, std::string& OutNote)
	{
		if (!Property || KeyNameValue.Size <= 0 || !EnsureRuntimeParamSize(Params, *Property))
		{
			OutNote = "FKey parameter metadata missing";
			return false;
		}

		const int32_t KeyNameOffset = std::max(gRuntimeSymbols.keyNameOffset, 0);
		if (Property->size < KeyNameOffset + KeyNameValue.Size)
		{
			OutNote = "FKey parameter is too small";
			return false;
		}

		std::memcpy(Params.data() + Property->offset + KeyNameOffset, KeyNameValue.Bytes.data(), static_cast<size_t>(KeyNameValue.Size));
		return true;
	}

	UEObject FindConsoleLocalPlayer(UEObject* OutWorld = nullptr, UEObject* OutGameInstance = nullptr, UEObject* OutPlayerController = nullptr)
	{
		UEObject World = ReadGWorldObject();
		if (OutWorld)
			*OutWorld = World;

		UEObject GameInstance;
		if (World)
			ReadObjectPropertyAtOffset(World, gRuntimeSymbols.worldGameInstanceOffset, GameInstance);
		if (OutGameInstance)
			*OutGameInstance = GameInstance;

		if (GameInstance && gRuntimeSymbols.gameInstanceLocalPlayersOffset >= 0)
		{
			std::vector<UEObject> LocalPlayers;
			if (ReadObjectArrayPropertyAtOffset(GameInstance, gRuntimeSymbols.gameInstanceLocalPlayersOffset, LocalPlayers, 8))
			{
				for (UEObject LocalPlayer : LocalPlayers)
				{
					UEObject PlayerController;
					ReadObjectPropertyAtOffset(LocalPlayer, gRuntimeSymbols.localPlayerControllerOffset, PlayerController);
					if (OutPlayerController && PlayerController)
						*OutPlayerController = PlayerController;
					if (LocalPlayer)
						return LocalPlayer;
				}
			}
		}

		return {};
	}

	UEObject FindConsolePlayerController()
	{
		UEObject PlayerController;
		FindConsoleLocalPlayer(nullptr, nullptr, &PlayerController);
		if (PlayerController)
			return PlayerController;

		if (gRuntimeSymbols.playerControllerClass)
			return FindFirstObjectOfClassCached(ClassFromAddress(gRuntimeSymbols.playerControllerClass), 1000);

		return {};
	}

	UEObject ResolveConsoleGameViewport(UEObject Engine, UEObject LocalPlayer, std::string& OutSource)
	{
		UEObject GameViewport;
		if (LocalPlayer && ReadObjectPropertyAtOffset(LocalPlayer, gRuntimeSymbols.localPlayerViewportClientOffset, GameViewport))
		{
			OutSource = "LocalPlayer.ViewportClient";
			return GameViewport;
		}

		if (Engine && ReadObjectPropertyAtOffset(Engine, gRuntimeSymbols.engineGameViewportOffset, GameViewport))
		{
			OutSource = "Engine.GameViewport";
			return GameViewport;
		}

		if (Engine && gRuntimeSymbols.engineGameViewportOffset != 0x780 && ReadObjectPropertyAtOffset(Engine, 0x780, GameViewport))
		{
			OutSource = "UE4.27 Engine.GameViewport offset 0x780";
			return GameViewport;
		}

		if (gRuntimeSymbols.gameViewportClientClass)
		{
			GameViewport = FindFirstObjectOfClassCached(ClassFromAddress(gRuntimeSymbols.gameViewportClientClass), 5000);
			if (GameViewport)
			{
				OutSource = "live GameViewportClient fallback";
				return GameViewport;
			}
		}

		OutSource = "missing";
		return {};
	}

	bool AttachConsoleTargetPlayer(UEObject Console, UEObject LocalPlayer, std::string& OutNote)
	{
		if (!Console)
		{
			OutNote = "Console missing";
			return false;
		}

		if (!LocalPlayer)
		{
			OutNote = "LocalPlayer missing";
			return false;
		}

		if (gRuntimeSymbols.consoleTargetPlayerOffset < 0)
		{
			OutNote = "ConsoleTargetPlayer offset missing";
			return false;
		}

		if (!WriteObjectPropertyAtOffset(Console, gRuntimeSymbols.consoleTargetPlayerOffset, LocalPlayer))
		{
			OutNote = "ConsoleTargetPlayer write failed";
			return false;
		}

		OutNote = "ConsoleTargetPlayer assigned";
		return true;
	}

	bool EnsureConsoleViewportBinding(UEObject GameViewport, UEObject Console, UEObject LocalPlayer, std::string& OutNote)
	{
		std::vector<std::string> Notes;
		bool bOk = true;

		if (!GameViewport || !Console)
		{
			OutNote = "viewport or console missing";
			return false;
		}

		UEObject CurrentConsole;
		if (!ReadObjectPropertyAtOffset(GameViewport, gRuntimeSymbols.gameViewportConsoleOffset, CurrentConsole)
			|| CurrentConsole.GetAddress() != Console.GetAddress())
		{
			if (WriteObjectPropertyAtOffset(GameViewport, gRuntimeSymbols.gameViewportConsoleOffset, Console))
				Notes.emplace_back("ViewportConsole assigned");
			else
			{
				Notes.emplace_back("ViewportConsole write failed");
				bOk = false;
			}
		}
		else
		{
			Notes.emplace_back("ViewportConsole already set");
		}

		std::string TargetPlayerNote;
		if (AttachConsoleTargetPlayer(Console, LocalPlayer, TargetPlayerNote))
			Notes.push_back(TargetPlayerNote);
		else
		{
			Notes.push_back(TargetPlayerNote);
			bOk = false;
		}

		std::string SetTargetNote;
		if (CallRuntimeSetConsoleTarget(GameViewport, 0, SetTargetNote))
			Notes.push_back(SetTargetNote);
		else if (!SetTargetNote.empty())
			Notes.push_back(SetTargetNote);

		OutNote.clear();
		for (size_t Index = 0; Index < Notes.size(); ++Index)
		{
			if (Index != 0)
				OutNote += "; ";
			OutNote += Notes[Index];
		}
		return bOk;
	}

	bool CallRuntimeConsoleKey(UEObject PlayerController, const wchar_t* KeyName, std::string& OutNote)
	{
		if (!PlayerController)
		{
			OutNote = "PlayerController missing";
			return false;
		}

		if (gRuntimeSymbols.consoleKey.address == 0)
		{
			OutNote = "APlayerController::ConsoleKey missing";
			return false;
		}

		UEFunction Function = FunctionFromRuntime(gRuntimeSymbols.consoleKey);
		if (!Function)
		{
			OutNote = "ConsoleKey function wrapper missing";
			return false;
		}

		RawNameValue KeyNameValue;
		if (!MakeRuntimeNameFromString(KeyName, KeyNameValue) || KeyNameValue.Size <= 0)
		{
			OutNote = "could not create FName for ConsoleKey";
			return false;
		}

		const RuntimePropertyInfo* KeyParam = FindRuntimeParam(gRuntimeSymbols.consoleKey, { "Key", "InKey" });
		if (!KeyParam)
		{
			OutNote = "ConsoleKey parameter match failed: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.consoleKey);
			return false;
		}

		std::vector<uint8> Params = MakeRuntimeParamBuffer(gRuntimeSymbols.consoleKey);
		if (!WriteRuntimeKeyParam(Params, KeyParam, KeyNameValue, OutNote))
			return false;

		if (!ProcessEventWithRuntimeFlags(PlayerController, Function, Params.data(), &OutNote))
			return false;

		OutNote = "PlayerController.ConsoleKey(F2) called";
		return true;
	}

	std::wstring Utf8ToWide(const char* Text)
	{
		if (!Text || Text[0] == '\0')
			return {};

		const int Required = MultiByteToWideChar(CP_UTF8, 0, Text, -1, nullptr, 0);
		if (Required <= 1)
			return {};

		std::wstring Wide(static_cast<size_t>(Required), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Text, -1, Wide.data(), Required);
		Wide.resize(static_cast<size_t>(Required - 1));
		return Wide;
	}

	bool CallRuntimeSendToConsole(UEObject PlayerController, const wchar_t* Command, std::string& OutNote)
	{
		if (!PlayerController)
		{
			OutNote = "PlayerController missing";
			return false;
		}

		if (!Command || Command[0] == L'\0' || gRuntimeSymbols.sendToConsole.address == 0)
		{
			OutNote = "SendToConsole command or function missing";
			return false;
		}

		UEFunction Function = FunctionFromRuntime(gRuntimeSymbols.sendToConsole);
		if (!Function)
		{
			OutNote = "SendToConsole function wrapper missing";
			return false;
		}

		const RuntimePropertyInfo* CommandParam = FindRuntimeParam(gRuntimeSymbols.sendToConsole, { "Command", "Cmd" });
		if (!CommandParam)
		{
			OutNote = "SendToConsole parameter match failed: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.sendToConsole);
			return false;
		}

		std::vector<uint8> Params = MakeRuntimeParamBuffer(gRuntimeSymbols.sendToConsole);
		if (!WriteRuntimeStringParam(Params, CommandParam, Command))
		{
			OutNote = "SendToConsole parameter write failed";
			return false;
		}

		if (!ProcessEventWithRuntimeFlags(PlayerController, Function, Params.data(), &OutNote))
			return false;

		OutNote = "PlayerController.SendToConsole called";
		return true;
	}

	bool CallRuntimeExecuteConsoleCommand(UEObject World, UEObject PlayerController, const wchar_t* Command, std::string& OutNote)
	{
		UEObject ContextObject = World ? World : PlayerController;
		if (!ContextObject || !Command || Command[0] == L'\0'
			|| gRuntimeSymbols.executeConsoleCommand.address == 0
			|| gRuntimeSymbols.kismetSystemLibraryClass == 0)
		{
			OutNote = "ExecuteConsoleCommand metadata or input missing";
			return false;
		}

		UEClass SystemLibraryClass = ClassFromAddress(gRuntimeSymbols.kismetSystemLibraryClass);
		UEObject SystemLibraryDefault = SystemLibraryClass ? SystemLibraryClass.GetDefaultObject() : UEObject();
		UEFunction Function = FunctionFromRuntime(gRuntimeSymbols.executeConsoleCommand);
		if (!SystemLibraryDefault || !Function)
		{
			OutNote = "UKismetSystemLibrary default object or function wrapper missing";
			return false;
		}

		const RuntimePropertyInfo* WorldParam = FindRuntimeParam(gRuntimeSymbols.executeConsoleCommand, { "WorldContextObject", "ContextObject", "World" });
		const RuntimePropertyInfo* CommandParam = FindRuntimeParam(gRuntimeSymbols.executeConsoleCommand, { "Command", "Cmd" });
		const RuntimePropertyInfo* PlayerParam = FindRuntimeParam(gRuntimeSymbols.executeConsoleCommand, { "SpecificPlayer", "PlayerController", "Player" });
		if (!WorldParam || !CommandParam)
		{
			OutNote = "ExecuteConsoleCommand parameter match failed: " + DescribeRuntimeFunctionParams(gRuntimeSymbols.executeConsoleCommand);
			return false;
		}

		std::vector<uint8> Params = MakeRuntimeParamBuffer(gRuntimeSymbols.executeConsoleCommand);
		if (!WriteRuntimeObjectParam(Params, WorldParam, ContextObject)
			|| !WriteRuntimeStringParam(Params, CommandParam, Command))
		{
			OutNote = "ExecuteConsoleCommand parameter write failed";
			return false;
		}

		if (PlayerParam)
			WriteRuntimeObjectParam(Params, PlayerParam, PlayerController);

		if (!ProcessEventWithRuntimeFlags(SystemLibraryDefault, Function, Params.data(), &OutNote))
			return false;

		OutNote = "UKismetSystemLibrary.ExecuteConsoleCommand called";
		return true;
	}

	bool RunRuntimeConsoleCommand(const char* Command)
	{
		if (!RuntimeSDK::IsReady() || !ResolveOverlaySymbolsFromRuntimeDB(gRuntimeSymbols))
		{
			SetConsoleUnlockStatus(false, "RuntimeSDK symbols are not ready");
			return false;
		}

		const std::wstring WideCommand = Utf8ToWide(Command);
		if (WideCommand.empty())
		{
			gConsoleState.status = "Console command is empty";
			return false;
		}

		UEObject World;
		UEObject GameInstance;
		UEObject PlayerController;
		FindConsoleLocalPlayer(&World, &GameInstance, &PlayerController);
		if (!PlayerController)
			PlayerController = FindConsolePlayerController();

		std::string ExecuteNote;
		if (CallRuntimeExecuteConsoleCommand(World, PlayerController, WideCommand.c_str(), ExecuteNote))
		{
			gConsoleState.status = ExecuteNote + ": " + Command;
			std::cerr << "[Overlay] Console command routed through ExecuteConsoleCommand: " << Command << "\n";
			return true;
		}

		std::string SendNote;
		if (CallRuntimeSendToConsole(PlayerController, WideCommand.c_str(), SendNote))
		{
			gConsoleState.status = SendNote + ": " + Command;
			std::cerr << "[Overlay] Console command routed through SendToConsole: " << Command << "\n";
			return true;
		}

		gConsoleState.status = "Console command failed: " + ExecuteNote + "; " + SendNote;
		std::cerr << "[Overlay] Console command failed: " << gConsoleState.status << "\n";
		return false;
	}

	UEObject FindExistingConsoleObject()
	{
		if (!gRuntimeSymbols.consoleClass)
			return {};

		UEClass ConsoleClass = ClassFromAddress(gRuntimeSymbols.consoleClass);
		if (!ConsoleClass)
			return {};

		return FindFirstObjectOfClassCached(ConsoleClass, 0);
	}

	bool UnlockUnrealConsole()
	{
		if (!RuntimeSDK::IsReady() || !ResolveOverlaySymbolsFromRuntimeDB(gRuntimeSymbols))
		{
			SetConsoleUnlockStatus(false, "RuntimeSDK symbols are not ready");
			std::cerr << "[Overlay] Console unlock failed: runtime symbols not ready\n";
			return false;
		}

		UEObject World;
		UEObject GameInstance;
		UEObject PlayerController;
		UEObject LocalPlayer = FindConsoleLocalPlayer(&World, &GameInstance, &PlayerController);

		UEObject Engine = FindEngineObject();
		if (!Engine && !gRuntimeSymbols.consoleClass)
		{
			SetConsoleUnlockStatus(false, "UEngine object not found and UConsole class fallback unavailable");
			std::cerr << "[Overlay] Console unlock failed: engine object not found and UConsole class fallback unavailable\n";
			return false;
		}

		UEObject ConsoleClass;
		std::string ConsoleClassSource = "Engine.ConsoleClass";
		if (!Engine || !ReadObjectPropertyAtOffset(Engine, gRuntimeSymbols.engineConsoleClassOffset, ConsoleClass))
		{
			if (gRuntimeSymbols.consoleClass && IsReadableObject(reinterpret_cast<void*>(gRuntimeSymbols.consoleClass)))
			{
				ConsoleClass = UEObject(reinterpret_cast<void*>(gRuntimeSymbols.consoleClass));
				ConsoleClassSource = "UConsole class fallback";
				std::cerr << "[Overlay] ConsoleClass property unavailable at offset 0x"
					<< std::hex << gRuntimeSymbols.engineConsoleClassOffset << std::dec
					<< "; using UConsole class fallback\n";
			}
			else
			{
				SetConsoleUnlockStatus(false, "Engine.ConsoleClass missing and UConsole fallback unavailable", Engine);
				std::cerr << "[Overlay] Console unlock failed: Engine.ConsoleClass missing and UConsole fallback unavailable"
					<< " offset=0x" << std::hex << gRuntimeSymbols.engineConsoleClassOffset << std::dec << "\n";
				return false;
			}
		}

		std::string ViewportSource;
		UEObject GameViewport = ResolveConsoleGameViewport(Engine, LocalPlayer, ViewportSource);
		if (!GameViewport)
		{
			SetConsoleUnlockStatus(false, "GameViewport missing", Engine);
			std::cerr << "[Overlay] Console unlock failed: GameViewport missing"
				<< " metadataOffset=0x" << std::hex << gRuntimeSymbols.engineGameViewportOffset
				<< " sdkOffset=0x780 localPlayerViewport=0x" << gRuntimeSymbols.localPlayerViewportClientOffset
				<< std::dec << "\n";
			return false;
		}

		UEObject ExistingConsole;
		if (ReadObjectPropertyAtOffset(GameViewport, gRuntimeSymbols.gameViewportConsoleOffset, ExistingConsole) && ExistingConsole)
		{
			std::string KeyNote;
			std::string BindingNote;
			TrySetConsoleKeyBinding(L"F2", KeyNote);
			EnsureConsoleViewportBinding(GameViewport, ExistingConsole, LocalPlayer, BindingNote);
			SetConsoleUnlockStatus(true, "ViewportConsole already set; " + KeyNote + "; " + BindingNote + "; " + ConsoleClassSource + "; viewport=" + ViewportSource, Engine, GameViewport, ExistingConsole);
			std::cerr << "[Overlay] Console already unlocked\n";
			return true;
		}

		UEObject NewConsole;
		std::string SpawnFailure;
		if (!CallRuntimeSpawnObject(ConsoleClass, GameViewport, NewConsole, &SpawnFailure))
		{
			NewConsole = FindExistingConsoleObject();
			if (NewConsole)
			{
				std::cerr << "[Overlay] SpawnObject failed; using existing UConsole object fallback: "
					<< SpawnFailure << "\n";
			}
			else
			{
				SetConsoleUnlockStatus(false, "UGameplayStatics::SpawnObject failed: " + SpawnFailure, Engine, GameViewport);
				std::cerr << "[Overlay] Console unlock failed: SpawnObject failed: " << SpawnFailure << "\n";
				return false;
			}
		}

		std::string BindingNote;
		if (!EnsureConsoleViewportBinding(GameViewport, NewConsole, LocalPlayer, BindingNote))
		{
			SetConsoleUnlockStatus(false, "ViewportConsole binding failed: " + BindingNote, Engine, GameViewport, NewConsole);
			std::cerr << "[Overlay] Console unlock failed: " << BindingNote << "\n";
			return false;
		}

		std::string KeyNote;
		TrySetConsoleKeyBinding(L"F2", KeyNote);
		SetConsoleUnlockStatus(true, BindingNote + "; " + KeyNote + "; " + ConsoleClassSource + "; viewport=" + ViewportSource, Engine, GameViewport, NewConsole);
		std::cerr << "[Overlay] Console unlocked\n";
		return true;
	}

	bool TriggerUnrealConsoleHotkey()
	{
		const DWORD Now = GetTickCount();
		if (gLastConsoleHotkeyTick != 0 && Now - gLastConsoleHotkeyTick < 250)
			return true;
		gLastConsoleHotkeyTick = Now;

		if (!UnlockUnrealConsole())
			return false;

		UEObject PlayerController = FindConsolePlayerController();
		std::string ConsoleKeyNote;
		if (!CallRuntimeConsoleKey(PlayerController, L"F2", ConsoleKeyNote))
		{
			gConsoleState.status += "; ConsoleKey failed: " + ConsoleKeyNote;
			std::cerr << "[Overlay] Console hotkey failed: " << ConsoleKeyNote << "\n";
			return false;
		}

		gConsoleState.status += "; " + ConsoleKeyNote;
		std::cerr << "[Overlay] Console hotkey routed through PlayerController.ConsoleKey(F2)\n";
		return true;
	}

	bool PostConsoleKeyToTargetWindow()
	{
		HWND Target = gTargetWindow;
		if (!Target || !IsWindow(Target))
			Target = gWindow;
		if (!Target || !IsWindow(Target))
			Target = FindMainProcessWindow();
		if (!Target || !IsWindow(Target))
			return false;

		const UINT ScanCode = MapVirtualKeyA(VK_F2, MAPVK_VK_TO_VSC);
		const LPARAM KeyDown = 1 | (static_cast<LPARAM>(ScanCode) << 16);
		const LPARAM KeyUp = 1 | (static_cast<LPARAM>(ScanCode) << 16) | (1L << 30) | (1L << 31);
		PostMessageA(Target, WM_KEYDOWN, VK_F2, KeyDown);
		PostMessageA(Target, WM_KEYUP, VK_F2, KeyUp);
		return true;
	}

	ActorFilterReason GetActorFilterReason(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		if (Config.OnlyWithLocation && !Actor.HasLocation)
			return ActorFilterReason::MissingLocation;

		if (Config.HideLocalPlayer && Actor.IsLocalPlayer)
			return ActorFilterReason::LocalPlayer;

		if (Config.ClassExcludeFilter[0] != '\0' && ActorClassMatchesTokens(Actor, Config.ClassExcludeFilter))
			return ActorFilterReason::ClassExcludeFilter;

		const bool HasClassFilter = Config.EnableClassFilter && Config.ClassFilter[0] != '\0';
		if (HasClassFilter && !ActorClassMatchesTokens(Actor, Config.ClassFilter))
			return ActorFilterReason::ClassFilter;

		const std::string LikelyClassLock = ShouldUseLikelyClassLock(Config)
			? GetLikelyClassLockFilter()
			: std::string();

		if (!HasClassFilter && Config.HideBots && Actor.IsBot)
			return ActorFilterReason::Bot;

		if (!HasClassFilter && Config.HideNPCs && Actor.IsNPC)
			return ActorFilterReason::NPC;

		if (!HasClassFilter && Config.HideCivilians && Actor.IsCivilian)
			return ActorFilterReason::Civilian;

		if (!HasClassFilter && Config.HideAI && Actor.IsAI)
			return ActorFilterReason::AI;

		if (!HasClassFilter && Config.HideCameras && Actor.IsCameraActor)
			return ActorFilterReason::Camera;

		if (!HasClassFilter && Config.HideItems && Actor.IsItem)
			return ActorFilterReason::Item;

		if (!HasClassFilter && Config.HideWeapons && Actor.IsWeapon)
			return ActorFilterReason::Weapon;

		if (!HasClassFilter && Config.HideVehicles && Actor.IsVehicle)
			return ActorFilterReason::Vehicle;

		if (!HasClassFilter && Config.HideObjectives && Actor.IsObjective)
			return ActorFilterReason::Objective;

		if (!HasClassFilter && Config.HideEnvironmentActors && Actor.IsEnvironment)
			return ActorFilterReason::Environment;

		if (!HasClassFilter && Config.TargetMode == 1)
		{
			if (!Actor.IsLikelyPlayer)
				return ActorFilterReason::TargetMode;
			if (!LikelyClassLock.empty() && !ActorClassMatchesTokens(Actor, LikelyClassLock.c_str()))
			{
				const bool HasAuthoritativeSignal = Actor.IsRuntimePlayer
					|| Actor.HasPlayerState
					|| Actor.IsGameEnemy
					|| Actor.IsGamePlayer
					|| Actor.IsGameCharacter;
				const bool HasStrongScore = Actor.PlayerScore >= (Config.LikelyPlayerScoreThreshold + 20);
				if (!HasAuthoritativeSignal && !HasStrongScore)
					return ActorFilterReason::ClassFilter;
			}
		}

		if (!HasClassFilter && Config.TargetMode == 2 && !(Actor.IsPawn || Actor.IsCharacter))
			return ActorFilterReason::TargetMode;

		if (!HasClassFilter && Config.TargetMode == 3 && !Actor.IsBot)
			return ActorFilterReason::TargetMode;

		if (!HasClassFilter && Config.TargetMode == 4 && !(Actor.IsNPC || Actor.IsAI || Actor.IsBot || Actor.IsCivilian))
			return ActorFilterReason::TargetMode;

		if (!HasClassFilter && Config.TargetMode == 5 && !Actor.IsCivilian)
			return ActorFilterReason::TargetMode;

		if (Config.MaxDistanceMeters > 0.0f && Actor.HasDistance && Actor.DistanceMeters > Config.MaxDistanceMeters)
			return ActorFilterReason::Distance;

		if (Config.ExcludeFilter[0] != '\0' && ActorTextMatchesTokens(Actor, Config.ExcludeFilter))
			return ActorFilterReason::ExcludeFilter;

		if (HasClassFilter || Config.Filter[0] == '\0')
			return ActorFilterReason::None;

		return ActorTextMatchesTokens(Actor, Config.Filter) ? ActorFilterReason::None : ActorFilterReason::IncludeFilter;
	}

	bool ShouldKeepActor(const ActorDebugInfo& Actor, const OverlayConfig& Config)
	{
		return GetActorFilterReason(Actor, Config) == ActorFilterReason::None;
	}

	void UpdateLikelyPlayerClassLockFromActors(const std::vector<ActorDebugInfo>& Actors, const OverlayConfig& Config)
	{
		if (!ShouldUseLikelyClassLock(Config))
		{
			ClearLikelyClassLock();
			return;
		}

		struct ClassCandidate
		{
			std::string Name;
			std::string Path;
			int Count = 0;
			int Score = 0;
		};

		std::unordered_map<std::string, ClassCandidate> Candidates;
		for (const ActorDebugInfo& Actor : Actors)
		{
			if (!Actor.IsLikelyPlayer || Actor.IsLocalPlayer || Actor.IsEnvironment || Actor.ClassName.empty())
				continue;

			const std::string Key = Actor.ClassPath.empty() ? Actor.ClassName : Actor.ClassPath;
			ClassCandidate& Candidate = Candidates[Key];
			if (Candidate.Name.empty())
			{
				Candidate.Name = Actor.ClassName;
				Candidate.Path = Actor.ClassPath;
			}

			Candidate.Count++;
			Candidate.Score += 10;
			if (Actor.IsRuntimePlayer)
				Candidate.Score += 18;
			if (Actor.HasPlayerState)
				Candidate.Score += 14;
			if (Actor.IsCharacter)
				Candidate.Score += 8;
			else if (Actor.IsPawn)
				Candidate.Score += 5;
			if (Actor.IsInView)
				Candidate.Score += 5;
			if (Actor.HasBox)
				Candidate.Score += 3;
			if (Actor.HasSkeleton)
				Candidate.Score += 2;
		}

		std::vector<ClassCandidate> Ranked;
		Ranked.reserve(Candidates.size());
		for (auto& Pair : Candidates)
		{
			if (Pair.second.Count >= std::max(1, Config.LikelyClassLockMinActors))
				Ranked.push_back(std::move(Pair.second));
		}

		if (Ranked.empty())
		{
			ClearLikelyClassLock();
			return;
		}

		std::sort(Ranked.begin(), Ranked.end(), [](const ClassCandidate& A, const ClassCandidate& B)
		{
			if (A.Score != B.Score)
				return A.Score > B.Score;
			if (A.Count != B.Count)
				return A.Count > B.Count;
			return A.Name < B.Name;
		});

		std::string NextFilter;
		int ClassCount = 0;
		const int MaxClasses = std::clamp(Config.LikelyClassLockMaxClasses, 1, 16);
		for (const ClassCandidate& Candidate : Ranked)
		{
			if (ClassCount >= MaxClasses)
				break;
			if (Candidate.Name.empty())
				continue;

			const size_t ExtraSize = Candidate.Name.size() + (NextFilter.empty() ? 0 : 1);
			if (NextFilter.size() + ExtraSize >= 240)
				break;

			if (!NextFilter.empty())
				NextFilter.push_back(',');
			NextFilter += Candidate.Name;
			ClassCount++;
		}

		std::scoped_lock Lock(gLikelyClassLockMutex);
		gLikelyClassLockFilter = std::move(NextFilter);
		gLikelyClassLockClassCount = ClassCount;
		gLikelyClassLockTick = GetTickCount();
	}

	void CountFilteredActor(CaptureStats& Stats, ActorFilterReason Reason)
	{
		if (Reason == ActorFilterReason::None)
			return;

		Stats.FilteredActors++;
		switch (Reason)
		{
		case ActorFilterReason::MissingLocation:
			Stats.FilteredMissingLocation++;
			break;
		case ActorFilterReason::LocalPlayer:
			Stats.FilteredLocalPlayer++;
			break;
		case ActorFilterReason::Environment:
			Stats.FilteredEnvironment++;
			break;
		case ActorFilterReason::Bot:
			Stats.FilteredBot++;
			break;
		case ActorFilterReason::NPC:
			Stats.FilteredNPC++;
			break;
		case ActorFilterReason::Civilian:
			Stats.FilteredCivilian++;
			break;
		case ActorFilterReason::AI:
			Stats.FilteredAI++;
			break;
		case ActorFilterReason::Camera:
			Stats.FilteredCamera++;
			break;
		case ActorFilterReason::Item:
			Stats.FilteredItem++;
			break;
		case ActorFilterReason::Weapon:
			Stats.FilteredWeapon++;
			break;
		case ActorFilterReason::Vehicle:
			Stats.FilteredVehicle++;
			break;
		case ActorFilterReason::Objective:
			Stats.FilteredObjective++;
			break;
		case ActorFilterReason::ClassFilter:
			Stats.FilteredClass++;
			break;
		case ActorFilterReason::ClassExcludeFilter:
			Stats.FilteredClassExclude++;
			break;
		case ActorFilterReason::TargetMode:
			Stats.FilteredTargetMode++;
			break;
		case ActorFilterReason::Distance:
			Stats.FilteredDistance++;
			break;
		case ActorFilterReason::ExcludeFilter:
			Stats.FilteredExclude++;
			break;
		case ActorFilterReason::IncludeFilter:
			Stats.FilteredInclude++;
			break;
		case ActorFilterReason::NotInView:
			Stats.FilteredNotInView++;
			break;
		default:
			break;
		}
	}

	std::vector<std::string> EnumerateModuleNames()
	{
		std::vector<std::string> Modules;

		HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
		if (Snapshot == INVALID_HANDLE_VALUE)
			return Modules;

		MODULEENTRY32 Entry = {};
		Entry.dwSize = sizeof(Entry);
		if (Module32First(Snapshot, &Entry))
		{
			do
			{
#ifdef UNICODE
				char NarrowName[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, Entry.szModule, -1, NarrowName, sizeof(NarrowName), nullptr, nullptr);
				Modules.emplace_back(NarrowName);
#else
				Modules.emplace_back(Entry.szModule);
#endif
			} while (Module32Next(Snapshot, &Entry));
		}

		CloseHandle(Snapshot);
		return Modules;
	}

	std::string DetectLoadedRhiModules()
	{
		std::vector<std::string> Matches;
		for (const std::string& ModuleName : EnumerateModuleNames())
		{
			const std::string Lower = ToLower(ModuleName);
			if (Lower.find("d3d11rhi") != std::string::npos
				|| Lower.find("d3d12rhi") != std::string::npos
				|| Lower.find("vulkanrhi") != std::string::npos
				|| Lower.find("opengldrv") != std::string::npos
				|| Lower.find("d3d11.dll") != std::string::npos
				|| Lower.find("d3d12.dll") != std::string::npos
				|| Lower.find("vulkan-1.dll") != std::string::npos
				|| Lower.find("opengl32.dll") != std::string::npos)
			{
				Matches.push_back(ModuleName);
			}
		}

		if (Matches.empty())
			return "none";

		std::ostringstream Stream;
		for (size_t Index = 0; Index < Matches.size(); ++Index)
		{
			if (Index != 0)
				Stream << ", ";

			Stream << Matches[Index];
		}

		return Stream.str();
	}

	bool HasStreamlineOrFrameGenModule()
	{
		for (const std::string& ModuleName : EnumerateModuleNames())
		{
			const std::string Lower = ToLower(ModuleName);
			if (Lower.find("streamline") != std::string::npos
				|| Lower.find("sl.interposer") != std::string::npos
				|| Lower.find("sl_interposer") != std::string::npos
				|| Lower.find("dlssg") != std::string::npos
				|| Lower.find("nvngx") != std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	void AddUniqueActor(std::vector<UEObject>& Actors, std::unordered_set<uintptr_t>& Seen, UEObject Actor)
	{
		if (!Actor)
			return;

		const uintptr_t Address = reinterpret_cast<uintptr_t>(Actor.GetAddress());
		if (Address == 0 || Seen.contains(Address))
			return;

		Seen.insert(Address);
		Actors.push_back(Actor);
	}

	std::vector<UEObject> GetWorldActorCandidates(CaptureStats& Stats)
	{
		std::vector<UEObject> Actors;
		std::unordered_set<uintptr_t> SeenActors;
		std::unordered_set<uintptr_t> SeenLevels;

		UEObject World = ReadGWorldObject();
		if (!World)
			return Actors;

		Stats.HasWorld = true;
		Stats.WorldAddress = reinterpret_cast<uintptr_t>(World.GetAddress());
		Stats.WorldCount = 1;

		auto AddLevel = [&](UEObject Level)
		{
			if (!Level)
				return;

			const uintptr_t Address = reinterpret_cast<uintptr_t>(Level.GetAddress());
			if (Address == 0 || SeenLevels.contains(Address))
				return;

			SeenLevels.insert(Address);
			Stats.LevelCount++;

			std::vector<UEObject> LevelActors;
			if (!ReadLevelActors(Level, LevelActors, Stats))
				return;

			for (UEObject Actor : LevelActors)
				AddUniqueActor(Actors, SeenActors, Actor);
		};

		UEObject PersistentLevel;
		if (ReadObjectPropertyAtOffset(World, gRuntimeSymbols.worldPersistentLevelOffset, PersistentLevel))
			AddLevel(PersistentLevel);

		RawTArrayView Levels;
		if (ReadArrayPropertyAtOffset(World, gRuntimeSymbols.worldLevelsOffset, Levels))
		{
			void** RawLevelData = static_cast<void**>(Levels.Data);
			for (int32 Index = 0; Index < Levels.Num; ++Index)
			{
				void* LevelPointer = RawLevelData[Index];
				if (IsReadableObject(LevelPointer))
					AddLevel(UEObject(LevelPointer));
			}
		}

		return Actors;
	}

	std::vector<UEObject> GetGObjectsActorCandidates(CaptureStats& Stats)
	{
		std::vector<UEObject> Actors;
		const EObjectFlags SkipFlags = ActorSkipFlags();

		for (UEObject Object : ObjectArray())
		{
			if (!gRunning)
				break;

			Stats.ScannedObjects++;

			if (!Object || Object.HasAnyFlags(SkipFlags))
				continue;

			if (Object.IsA(gSymbols.ActorClass))
				Actors.push_back(Object);
		}

		return Actors;
	}

	std::vector<UEObject> GetActorCandidates(const OverlayConfig& Config, CaptureStats& Stats)
	{
		std::vector<UEObject> Actors;
		std::unordered_set<uintptr_t> Seen;
		const ActorCaptureSource Source = static_cast<ActorCaptureSource>(std::clamp(Config.ActorSource, 0, 3));

		auto AppendUnique = [&](const std::vector<UEObject>& SourceActors)
		{
			for (UEObject Actor : SourceActors)
				AddUniqueActor(Actors, Seen, Actor);
		};

		if (Source == ActorCaptureSource::WorldLevels || Source == ActorCaptureSource::Auto || Source == ActorCaptureSource::Both)
		{
			std::vector<UEObject> WorldActors = GetWorldActorCandidates(Stats);
			if (!WorldActors.empty())
			{
				Stats.UsedWorldActors = true;
				AppendUnique(WorldActors);
			}

			if (Source == ActorCaptureSource::Auto && !Actors.empty())
			{
				Stats.ActorSource = "World levels";
				return Actors;
			}
		}

		if (Source == ActorCaptureSource::GObjects || Source == ActorCaptureSource::Auto || Source == ActorCaptureSource::Both)
		{
			std::vector<UEObject> GObjectActors = GetGObjectsActorCandidates(Stats);
			if (!GObjectActors.empty())
			{
				Stats.UsedGObjects = true;
				AppendUnique(GObjectActors);
			}
		}

		if (Source == ActorCaptureSource::Both && Stats.UsedWorldActors && Stats.UsedGObjects)
			Stats.ActorSource = "World + GObjects";
		else if (Stats.UsedWorldActors)
			Stats.ActorSource = "World levels";
		else if (Stats.UsedGObjects)
			Stats.ActorSource = "GObjects";
		else
			Stats.ActorSource = ActorSourceName(Source);

		return Actors;
	}

	void AddRuntimeActor(RuntimePlayerContext& Context, UEObject Actor)
	{
		if (!Actor || !IsReadableObject(Actor.GetAddress()))
			return;

		if (gSymbols.ActorClass && !Actor.IsA(gSymbols.ActorClass))
			return;

		const uintptr_t Address = reinterpret_cast<uintptr_t>(Actor.GetAddress());
		if (Address != 0)
			Context.RuntimeActors.insert(Address);
	}

	void AddRuntimeActorForPlayerState(RuntimePlayerContext& Context, UEObject Actor, UEObject PlayerState)
	{
		if (!Actor || !PlayerState)
			return;

		AddRuntimeActor(Context, Actor);

		const uintptr_t ActorAddress = reinterpret_cast<uintptr_t>(Actor.GetAddress());
		const uintptr_t PlayerStateAddress = reinterpret_cast<uintptr_t>(PlayerState.GetAddress());
		if (ActorAddress != 0 && PlayerStateAddress != 0)
			Context.ActorToPlayerState[ActorAddress] = PlayerStateAddress;
	}

	void ResolvePlayerStateRuntimeActors(RuntimePlayerContext& Context, UEObject PlayerState)
	{
		if (!PlayerState)
			return;

		UEObject PawnFromState;
		if (ReadObjectPropertyAtOffset(PlayerState, gRuntimeSymbols.playerStatePawnOffset, PawnFromState))
			AddRuntimeActorForPlayerState(Context, PawnFromState, PlayerState);

		UEObject OwnerOrController;
		if (ReadObjectPropertyAtOffset(PlayerState, gRuntimeSymbols.playerStateOwnerOffset, OwnerOrController))
		{
			UEObject PawnFromController;
			if (ReadObjectPropertyAtOffset(OwnerOrController, gRuntimeSymbols.playerControllerAcknowledgedPawnOffset, PawnFromController))
				AddRuntimeActorForPlayerState(Context, PawnFromController, PlayerState);
			if (!PawnFromController && gSymbols.GetPawn)
				CallNoArgObjectFunction(OwnerOrController, gSymbols.GetPawn, PawnFromController);
			AddRuntimeActorForPlayerState(Context, PawnFromController, PlayerState);
		}
	}

	RuntimePlayerContext BuildRuntimePlayerContext(const OverlayConfig& Config, CaptureStats& Stats)
	{
		RuntimePlayerContext Context;
		if (!Config.UseRuntimePlayerContext)
			return Context;

		Context.World = ReadGWorldObject();
		Context.HasWorld = static_cast<bool>(Context.World);
		if (Context.HasWorld)
		{
			Stats.HasWorld = true;
			Stats.WorldAddress = reinterpret_cast<uintptr_t>(Context.World.GetAddress());
		}

		if (Context.World)
		{
			ReadObjectPropertyAtOffset(Context.World, gRuntimeSymbols.worldGameInstanceOffset, Context.GameInstance);
			ReadObjectPropertyAtOffset(Context.World, gRuntimeSymbols.worldGameStateOffset, Context.GameState);
		}

		Context.HasGameInstance = static_cast<bool>(Context.GameInstance);
		Context.HasGameState = static_cast<bool>(Context.GameState);

		if (Context.GameInstance && gRuntimeSymbols.gameInstanceLocalPlayersOffset >= 0)
		{
			std::vector<UEObject> LocalPlayers;
			if (ReadObjectArrayPropertyAtOffset(Context.GameInstance, gRuntimeSymbols.gameInstanceLocalPlayersOffset, LocalPlayers, 16))
			{
				Context.LocalPlayers = static_cast<int32>(LocalPlayers.size());
				if (!LocalPlayers.empty())
					Context.LocalPlayer = LocalPlayers.front();
			}
		}

		Context.HasLocalPlayer = static_cast<bool>(Context.LocalPlayer);
		if (Context.LocalPlayer)
			ReadObjectPropertyAtOffset(Context.LocalPlayer, gRuntimeSymbols.localPlayerControllerOffset, Context.PlayerController);

		if (!Context.PlayerController)
			Context.PlayerController = FindFirstObjectOfClassCached(gSymbols.PlayerControllerClass);

		Context.HasPlayerController = static_cast<bool>(Context.PlayerController);
		if (Context.PlayerController)
		{
			ReadObjectPropertyAtOffset(Context.PlayerController, gRuntimeSymbols.playerControllerCameraManagerOffset, Context.CameraManager);
			ReadObjectPropertyAtOffset(Context.PlayerController, gRuntimeSymbols.playerControllerAcknowledgedPawnOffset, Context.LocalPawn);
			ReadObjectPropertyAtOffset(Context.PlayerController, gRuntimeSymbols.playerControllerCharacterOffset, Context.LocalCharacter);

			if (!Context.LocalPawn && gSymbols.GetPawn)
				CallNoArgObjectFunction(Context.PlayerController, gSymbols.GetPawn, Context.LocalPawn);
		}

		if (!Context.CameraManager)
			Context.CameraManager = FindFirstObjectOfClassCached(gSymbols.PlayerCameraManagerClass);

		Context.HasLocalPawn = static_cast<bool>(Context.LocalPawn || Context.LocalCharacter);
		AddRuntimeActor(Context, Context.LocalPawn);
		AddRuntimeActor(Context, Context.LocalCharacter);

		if (Context.GameState && Config.IncludeGameStatePlayers)
		{
			std::vector<UEObject> PlayerStates;
			if (ReadObjectArrayPropertyAtOffset(Context.GameState, gRuntimeSymbols.gameStatePlayerArrayOffset, PlayerStates, 1024))
			{
				for (UEObject PlayerState : PlayerStates)
				{
					const uintptr_t Address = reinterpret_cast<uintptr_t>(PlayerState.GetAddress());
					if (Address != 0)
					{
						Context.PlayerStates.insert(Address);
						ResolvePlayerStateRuntimeActors(Context, PlayerState);
					}
				}
			}
		}

		Context.PlayerStateCount = static_cast<int32>(Context.PlayerStates.size());
		Stats.HasRuntimeContext = Context.HasWorld || Context.HasPlayerController || Context.HasGameState;
		Stats.HasGameInstance = Context.HasGameInstance;
		Stats.HasGameState = Context.HasGameState;
		Stats.HasPlayerController = Context.HasPlayerController;
		Stats.HasLocalPawn = Context.HasLocalPawn;
		Stats.RuntimeLocalPlayers = Context.LocalPlayers;
		Stats.RuntimePlayerStates = Context.PlayerStateCount;
		return Context;
	}

	void ApplyRuntimePlayerContext(ActorDebugInfo& Info, UEObject Object, const RuntimePlayerContext& Context, CaptureStats& Stats)
	{
		if (!Object)
			return;

		const uintptr_t ActorAddress = reinterpret_cast<uintptr_t>(Object.GetAddress());
		const uintptr_t LocalPawnAddress = reinterpret_cast<uintptr_t>(Context.LocalPawn.GetAddress());
		const uintptr_t LocalCharacterAddress = reinterpret_cast<uintptr_t>(Context.LocalCharacter.GetAddress());
		Info.IsLocalPlayer = (ActorAddress != 0 && (ActorAddress == LocalPawnAddress || ActorAddress == LocalCharacterAddress));
		Info.IsRuntimePlayer = Context.RuntimeActors.contains(ActorAddress);

		auto RuntimePlayerStateIt = Context.ActorToPlayerState.find(ActorAddress);
		if (RuntimePlayerStateIt != Context.ActorToPlayerState.end())
		{
			Info.HasPlayerState = true;
			Info.PlayerStateAddress = RuntimePlayerStateIt->second;
			Info.IsRuntimePlayer = true;
		}

		UEObject PlayerState;
		if (ReadObjectPropertyAtOffset(Object, gRuntimeSymbols.pawnPlayerStateOffset, PlayerState))
		{
			const uintptr_t PlayerStateAddress = reinterpret_cast<uintptr_t>(PlayerState.GetAddress());
			if (PlayerStateAddress != 0)
			{
				const bool bAlreadyHadPlayerState = Info.HasPlayerState;
				Info.HasPlayerState = true;
				Info.PlayerStateAddress = PlayerStateAddress;
				if (!bAlreadyHadPlayerState)
					Stats.PlayerStateActors++;
				if (Context.PlayerStates.empty() || Context.PlayerStates.contains(PlayerStateAddress))
					Info.IsRuntimePlayer = true;
			}
		}

		if (Info.IsRuntimePlayer)
			Stats.RuntimeContextActors++;
	}

	void AppendRuntimePlayerCandidates(std::vector<UEObject>& Actors, const RuntimePlayerContext& Context, const OverlayConfig& Config)
	{
		if (!Config.UseRuntimePlayerContext || !Config.PreferRuntimePlayers || !gSymbols.ActorClass || gRuntimeSymbols.pawnPlayerStateOffset < 0)
			return;

		std::unordered_set<uintptr_t> Seen;
		Seen.reserve(Actors.size() + 32);
		for (UEObject Actor : Actors)
		{
			const uintptr_t Address = reinterpret_cast<uintptr_t>(Actor.GetAddress());
			if (Address != 0)
				Seen.insert(Address);
		}

		for (uintptr_t Address : Context.RuntimeActors)
		{
			if (Address == 0 || Seen.contains(Address) || !IsReadableObject(reinterpret_cast<void*>(Address)))
				continue;

			UEObject Actor(reinterpret_cast<void*>(Address));
			if (!Actor || !Actor.IsA(gSymbols.ActorClass))
				continue;

			Seen.insert(Address);
			Actors.push_back(Actor);
			if (static_cast<int>(Actors.size()) >= std::clamp(Config.MaxActors, 1, 4096))
				return;
		}

		const EObjectFlags SkipFlags = ActorSkipFlags();
		for (UEObject Object : ObjectArray())
		{
			if (!Object || Object.HasAnyFlags(SkipFlags) || !Object.IsA(gSymbols.ActorClass))
				continue;

			const uintptr_t Address = reinterpret_cast<uintptr_t>(Object.GetAddress());
			if (Address == 0 || Seen.contains(Address))
				continue;

			const bool IsPawnOrCharacter = (gSymbols.PawnClass && Object.IsA(gSymbols.PawnClass))
				|| (gSymbols.CharacterClass && Object.IsA(gSymbols.CharacterClass));
			if (!IsPawnOrCharacter)
				continue;

			UEObject PlayerState;
			if (!ReadObjectPropertyAtOffset(Object, gRuntimeSymbols.pawnPlayerStateOffset, PlayerState))
				continue;

			const uintptr_t PlayerStateAddress = reinterpret_cast<uintptr_t>(PlayerState.GetAddress());
			if (PlayerStateAddress == 0)
				continue;

			if (!Context.PlayerStates.empty() && !Context.PlayerStates.contains(PlayerStateAddress))
				continue;

			Seen.insert(Address);
			Actors.push_back(Object);
			if (static_cast<int>(Actors.size()) >= std::clamp(Config.MaxActors, 1, 4096))
				return;
		}
	}

	bool CaptureActors(bool Blocking = false)
	{
		std::unique_lock<std::mutex> CaptureLock(gCaptureMutex, std::defer_lock);
		if (Blocking)
			CaptureLock.lock();
		else if (!CaptureLock.try_lock())
			return false;

		OverlayConfig Config = GetConfigSnapshot();
		CaptureStats Stats;
		Stats.LastCaptureTick = GetTickCount();
		Stats.ObjectCount = ObjectArray::Num();
		Stats.RhiModules = DetectLoadedRhiModules();
		Stats.HasStreamline = HasStreamlineOrFrameGenModule();

		if (!ResolveSymbols(gSymbols))
		{
			Stats.Status = "Actor symbols are not ready";
			std::scoped_lock Lock(gActorMutex);
			gActors.clear();
			gFilteredActors.clear();
			gStats = std::move(Stats);
			return true;
		}

		Stats.SymbolsReady = true;

		RuntimePlayerContext RuntimeContext = BuildRuntimePlayerContext(Config, Stats);
		UEObject PlayerController = RuntimeContext.PlayerController ? RuntimeContext.PlayerController : FindFirstObjectOfClassCached(gSymbols.PlayerControllerClass);
		UEObject CameraManager = RuntimeContext.CameraManager ? RuntimeContext.CameraManager : FindFirstObjectOfClassCached(gSymbols.PlayerCameraManagerClass);
		UEObject LocalPawn = RuntimeContext.LocalPawn ? RuntimeContext.LocalPawn : RuntimeContext.LocalCharacter;
		if (!LocalPawn && PlayerController && gSymbols.GetPawn)
			CallNoArgObjectFunction(PlayerController, gSymbols.GetPawn, LocalPawn);

		Stats.HasPlayerController = static_cast<bool>(PlayerController);
		Stats.HasLocalPawn = static_cast<bool>(LocalPawn);
		Stats.HasProjection = PlayerController && gSymbols.ProjectWorldLocationToScreen;

		Vec3 CameraLocation;
		Vec3 CameraRotation;
		float CameraFov = 90.0f;

		if (PlayerController && CallViewPointFunction(PlayerController, gSymbols.GetPlayerViewPoint, CameraLocation, CameraRotation))
		{
			Stats.HasCameraLocation = true;
			Stats.HasCameraRotation = true;
			Stats.CameraLocationSource = "PlayerController.GetPlayerViewPoint()";
			Stats.CameraRotationSource = "PlayerController.GetPlayerViewPoint()";
		}

		if (!Stats.HasCameraLocation && CameraManager && CallNoArgVectorFunction(CameraManager, gSymbols.GetCameraLocation, CameraLocation))
		{
			Stats.HasCameraLocation = true;
			Stats.CameraLocationSource = "PlayerCameraManager.GetCameraLocation()";
		}
		else if (!Stats.HasCameraLocation && PlayerController && GetActorLocation(PlayerController, CameraLocation))
		{
			Stats.HasCameraLocation = true;
			Stats.CameraLocationSource = "PlayerController actor location";
		}

		if (!Stats.HasCameraRotation && CameraManager && CallNoArgVectorFunction(CameraManager, gSymbols.GetCameraRotation, CameraRotation))
		{
			Stats.HasCameraRotation = true;
			Stats.CameraRotationSource = "PlayerCameraManager.GetCameraRotation()";
		}
		else if (!Stats.HasCameraRotation && PlayerController && CallNoArgVectorFunction(PlayerController, gSymbols.GetControlRotation, CameraRotation))
		{
			Stats.HasCameraRotation = true;
			Stats.CameraRotationSource = "PlayerController.GetControlRotation()";
		}
		else if (!Stats.HasCameraRotation && PlayerController && CallNoArgVectorFunction(PlayerController, gSymbols.GetControllerActorRotation, CameraRotation))
		{
			Stats.HasCameraRotation = true;
			Stats.CameraRotationSource = "PlayerController.GetActorRotation()";
		}

		if (CameraManager && CallNoArgFloatFunction(CameraManager, gSymbols.GetCameraFov, CameraFov))
			Stats.HasCameraFov = true;

		float ViewWidth = 0.0f;
		float ViewHeight = 0.0f;
		RECT ProjectionRect = {};
		GetProjectionViewport(ProjectionRect, ViewWidth, ViewHeight);
		Stats.ProjectionLeft = ProjectionRect.left;
		Stats.ProjectionTop = ProjectionRect.top;
		Stats.ProjectionRight = ProjectionRect.right;
		Stats.ProjectionBottom = ProjectionRect.bottom;
		Stats.ProjectionWidth = ViewWidth;
		Stats.ProjectionHeight = ViewHeight;

		std::vector<ActorDebugInfo> Actors;
		Actors.reserve(static_cast<size_t>(std::clamp(Config.MaxActors, 1, 4096)));
		std::vector<ActorDebugInfo> LikelyClassCandidates;
		std::vector<ActorDebugInfo> FilteredActors;
		const int FilteredActorLimit = Config.EnableDeveloperOptions ? std::clamp(Config.DeveloperMaxRows, 10, 500) : 0;
		if (FilteredActorLimit > 0)
			FilteredActors.reserve(static_cast<size_t>(FilteredActorLimit));

		auto KeepFilteredActor = [&](ActorDebugInfo Info, ActorFilterReason Reason)
		{
			Info.FilterReason = Reason;
			CountFilteredActor(Stats, Reason);
			if (FilteredActorLimit > 0 && static_cast<int>(FilteredActors.size()) < FilteredActorLimit)
				FilteredActors.push_back(std::move(Info));
		};

		const EObjectFlags SkipFlags = ActorSkipFlags();
		std::vector<UEObject> Candidates = GetActorCandidates(Config, Stats);
		AppendRuntimePlayerCandidates(Candidates, RuntimeContext, Config);
		for (UEObject Object : Candidates)
		{
			if (!gRunning)
				break;

			if (!Object || Object.HasAnyFlags(SkipFlags))
				continue;

			if (!Object.IsA(gSymbols.ActorClass))
				continue;

			Stats.ActorCandidates++;

			ActorDebugInfo Info;
			Info.Address = reinterpret_cast<uintptr_t>(Object.GetAddress());
			Info.Index = Object.GetIndex();
			Info.Name = Object.GetName();
			UEClass ObjectClass = Object.GetClass();
			Info.ClassAddress = reinterpret_cast<uintptr_t>(ObjectClass.GetAddress());
			Info.ClassName = ObjectClass.GetName();
			Info.ClassPath = ObjectClass.GetPathName();
			Info.FullName = Object.GetFullName();
			Info.IsPawn = gSymbols.PawnClass && Object.IsA(gSymbols.PawnClass);
			Info.IsCharacter = gSymbols.CharacterClass && Object.IsA(gSymbols.CharacterClass);
			Info.IsGameCharacter = gRuntimeSymbols.crabCharacterClass && Object.IsA(ClassFromAddress(gRuntimeSymbols.crabCharacterClass));
			Info.IsGameEnemy = gRuntimeSymbols.crabEnemyClass && Object.IsA(ClassFromAddress(gRuntimeSymbols.crabEnemyClass));
			Info.IsGamePlayer = gRuntimeSymbols.crabPlayerCharacterClass && Object.IsA(ClassFromAddress(gRuntimeSymbols.crabPlayerCharacterClass));
			ApplyRuntimePlayerContext(Info, Object, RuntimeContext, Stats);
			if (!Info.IsLocalPlayer)
				Info.IsLocalPlayer = LocalPawn && Object.GetAddress() == LocalPawn.GetAddress();
			Info.IsBot = IsBotLikeActor(Info, Config);
			Info.IsNPC = IsNpcLikeActor(Info, Config);
			Info.IsCivilian = IsCivilianLikeActor(Info, Config);
			Info.IsAI = IsAiLikeActor(Info, Config);
			Info.IsCameraActor = IsCameraLikeActor(Info, Config);
			Info.IsItem = IsItemLikeActor(Info, Config);
			Info.IsWeapon = IsWeaponLikeActor(Info, Config);
			Info.IsVehicle = IsVehicleLikeActor(Info, Config);
			Info.IsObjective = IsObjectiveLikeActor(Info, Config);
			UpdateLikelyPlayerScore(Info, Config);
			Info.IsEnvironment = IsEnvironmentLikeActor(Info, Config);

			if (Info.IsBot)
				Stats.BotActors++;
			if (Info.IsNPC)
				Stats.NpcActors++;
			if (Info.IsCivilian)
				Stats.CivilianActors++;
			if (Info.IsAI)
				Stats.AiActors++;
			if (Info.IsCameraActor)
				Stats.CameraActors++;
			if (Info.IsItem)
				Stats.ItemActors++;
			if (Info.IsWeapon)
				Stats.WeaponActors++;
			if (Info.IsVehicle)
				Stats.VehicleActors++;
			if (Info.IsObjective)
				Stats.ObjectiveActors++;

			if (Info.IsLikelyPlayer && !Info.IsLocalPlayer && !Info.IsEnvironment && !Info.ClassName.empty())
				LikelyClassCandidates.push_back(Info);

			std::vector<PositionCandidate> PositionCandidates;
			Info.HasLocation = GetActorLocation(Object, Info.Location, &Info.LocationSource);
			if (Info.HasLocation)
			{
				Stats.LocatedActors++;
				AddPositionCandidate(PositionCandidates, Info.Location, Info.LocationSource.empty() ? "actor location" : Info.LocationSource);
			}

			const size_t CandidateCountBeforeReflection = PositionCandidates.size();
			if (!Info.HasLocation || Config.ProbeReflectedPositionsOnLocatedActors || Config.EnableDeveloperOptions)
				CollectReflectedPositionCandidates(Object, Config, PositionCandidates, &Stats);
			if (PositionCandidates.size() > CandidateCountBeforeReflection)
			{
				Info.LastReflectedPositionTick = Stats.LastCaptureTick;
				Stats.ReflectedPositionHits++;
			}
			Info.PositionCandidateCount = static_cast<int>(PositionCandidates.size());

			if (Info.HasLocation && Stats.HasCameraLocation)
			{
				Info.DistanceMeters = Distance(Info.Location, CameraLocation) / 100.0f;
				Info.HasDistance = true;
			}

			ActorFilterReason FilterReason = GetActorFilterReason(Info, Config);
			if (FilterReason != ActorFilterReason::None)
			{
				KeepFilteredActor(std::move(Info), FilterReason);
				continue;
			}

			Info.HasBounds = ReadBestActorBounds(Object, Config, Info.BoundsOrigin, Info.BoundsExtent, Info.SphereRadius);
			if (Info.HasBounds)
			{
				Stats.BoundedActors++;
				UpdateBoundsOffset(Info);
				AddPositionCandidate(PositionCandidates, Info.BoundsOrigin, "bounds origin");
				Info.PositionCandidateCount = static_cast<int>(PositionCandidates.size());
			}

			const int ProjectionRoute = std::clamp(Config.ProjectionRoute, 0, 2);
			const bool NativeProjectionAvailable = ProjectionRoute != 2 && Stats.HasProjection;
			const bool FallbackProjectionAvailable = ProjectionRoute != 1 && Config.UseProjectionFallback && Stats.HasCameraLocation && Stats.HasCameraRotation;
			const bool CanProject = NativeProjectionAvailable || FallbackProjectionAvailable;
			if (Info.HasLocation && CanProject)
			{
				Info.HasScreen = ProjectBestPositionCandidate(PositionCandidates, PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
					CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Info);
				if (Info.HasScreen)
				{
					Stats.ProjectedActors++;
					if (Stats.HasCameraLocation)
					{
						Info.DistanceMeters = Distance(Info.Location, CameraLocation) / 100.0f;
						Info.HasDistance = true;
					}
				}
				else
					Stats.ProjectionFailures++;

				if (!Info.HasBox)
				{
					if (Info.HasBounds)
					{
						Vec2 BoundsMin;
						Vec2 BoundsMax;
						if (ProjectBoundsToScreenBoxAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
							CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight,
							Info.BoundsOrigin, Info.BoundsExtent, BoundsMin, BoundsMax))
						{
							Info.BoxMin = BoundsMin;
							Info.BoxMax = BoundsMax;
							Info.HasBox = true;
						}
					}
				}

				if (!Info.HasBox)
				{
					Vec3 Origin = Info.HasBounds ? Info.BoundsOrigin : Info.Location;
					const double HalfHeight = Info.HasBounds
						? std::clamp(std::max(Info.BoundsExtent.Z, 1.0), 10.0, static_cast<double>(std::max(Config.FallbackHalfHeight * 2.0f, 20.0f)))
						: static_cast<double>(Config.FallbackHalfHeight);
					Vec3 Top = { Origin.X, Origin.Y, Origin.Z + HalfHeight };
					Vec3 Bottom = { Origin.X, Origin.Y, Origin.Z - HalfHeight };
					Info.HasBox = ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
						CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Top, Info.ScreenTop)
						&& ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
							CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Bottom, Info.ScreenBottom);

					if (Info.HasBox)
					{
						Info.HasBox = SetVerticalScreenBoxFromTopBottom(Info, Config, ViewWidth, ViewHeight);
					}
				}

				CaptureSkeleton(Object, Config, Stats, PlayerController, gSymbols.ProjectWorldLocationToScreen,
					CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Info);

				if (SetScreenBoxFromSkeleton(Info, Config, ViewWidth, ViewHeight))
					Info.HasBox = true;

				if (Info.HasBox)
					Stats.BoxedActors++;

				Info.IsInView = ActorProjectionInView(Info, ViewWidth, ViewHeight);
				if (Info.IsInView)
					Stats.InViewActors++;
			}
			else if (Info.HasLocation)
			{
				Info.ProjectionFailure = Stats.HasProjection
					? "projection skipped"
					: "projection unavailable; missing player controller or camera fallback";
			}

			UpdateLikelyPlayerScore(Info, Config);

			if (Config.OnlyInView && !Info.IsInView)
			{
				KeepFilteredActor(std::move(Info), ActorFilterReason::NotInView);
				continue;
			}

			Actors.push_back(std::move(Info));
			if (static_cast<int>(Actors.size()) >= Config.MaxActors)
				break;
		}

		std::sort(Actors.begin(), Actors.end(), [](const ActorDebugInfo& A, const ActorDebugInfo& B)
		{
			return A.DistanceMeters < B.DistanceMeters;
		});

		UpdateLikelyPlayerClassLockFromActors(LikelyClassCandidates.empty() ? Actors : LikelyClassCandidates, Config);
		CopyLikelyClassLockToStats(Stats);
		Stats.CapturedActors = static_cast<int32>(Actors.size());
		Stats.Status = "Capturing actors";

		std::scoped_lock Lock(gActorMutex);
		gActors = std::move(Actors);
		gFilteredActors = std::move(FilteredActors);
		gStats = std::move(Stats);
		return true;
	}

	void CaptureActorsForRenderFrame(const OverlayConfig& Config);

	void CaptureThreadProc()
	{
		DWORD LastDiscoveryTick = 0;

		while (gRunning)
		{
			const OverlayConfig Config = GetConfigSnapshot();
			const DWORD Now = GetTickCount();
			const int DiscoveryMs = std::clamp(Config.RefreshMs, 250, 10000);

			if (LastDiscoveryTick == 0 || Now - LastDiscoveryTick >= static_cast<DWORD>(DiscoveryMs))
			{
				if (CaptureActors(false))
					LastDiscoveryTick = Now;
			}
			else
			{
				CaptureActorsForRenderFrame(Config);
			}

			const int SleepMs = Config.FastOverlayMode
				? std::clamp(Config.FrameCaptureMinMs, 8, 100)
				: 25;
			Sleep(SleepMs);
		}
	}

	RenderBackend DetectRendererBackend()
	{
		if (GetModuleHandleA("d3d12.dll"))
			return RenderBackend::D3D12;

		if (GetModuleHandleA("d3d11.dll"))
			return RenderBackend::D3D11;

		if (GetModuleHandleA("vulkan-1.dll"))
			return RenderBackend::Vulkan;

		if (GetModuleHandleA("opengl32.dll"))
			return RenderBackend::OpenGL;

		return RenderBackend::Unknown;
	}

	RenderBackend DetectSwapChainBackend(IDXGISwapChain* SwapChain)
	{
		if (!SwapChain)
			return RenderBackend::Unknown;

		ID3D12Device* D3D12Device = nullptr;
		if (SUCCEEDED(SwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&D3D12Device))) && D3D12Device)
		{
			D3D12Device->Release();
			return RenderBackend::D3D12;
		}

		ID3D11Device* D3D11Device = nullptr;
		if (SUCCEEDED(SwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&D3D11Device))) && D3D11Device)
		{
			D3D11Device->Release();
			return RenderBackend::D3D11;
		}

		return RenderBackend::Unknown;
	}

	template<typename T>
	void ReleaseCom(T*& Object)
	{
		if (Object)
		{
			Object->Release();
			Object = nullptr;
		}
	}

	RenderBackend WaitForRendererBackend()
	{
		for (int Attempt = 0; Attempt < 100 && !gShutdownRequested; ++Attempt)
		{
			RenderBackend Backend = DetectRendererBackend();
			if (Backend != RenderBackend::Unknown)
				return Backend;

			Sleep(100);
		}

		return RenderBackend::Unknown;
	}

	void ReleaseD3D11RenderTarget()
	{
		if (gRenderTargetView)
		{
			gRenderTargetView->Release();
			gRenderTargetView = nullptr;
		}
	}

	void ReleaseExternalRenderTarget()
	{
		ReleaseCom(gExternalRenderTargetView);
	}

	void CreateD3D11RenderTarget(IDXGISwapChain* SwapChain)
	{
		ID3D11Texture2D* BackBuffer = nullptr;
		if (SUCCEEDED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&BackBuffer))) && BackBuffer)
		{
			gDevice->CreateRenderTargetView(BackBuffer, nullptr, &gRenderTargetView);
			BackBuffer->Release();
		}
	}

	void CreateExternalRenderTarget()
	{
		ID3D11Texture2D* BackBuffer = nullptr;
		if (gExternalSwapChain && SUCCEEDED(gExternalSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&BackBuffer))) && BackBuffer)
		{
			gExternalDevice->CreateRenderTargetView(BackBuffer, nullptr, &gExternalRenderTargetView);
			BackBuffer->Release();
		}
	}

	void ReleaseExternalComposition()
	{
		ReleaseCom(gCompositionVisual);
		ReleaseCom(gCompositionTarget);
		ReleaseCom(gCompositionDevice);
	}

	bool CreateExternalCompositionSwapChain(int Width, int Height)
	{
		const D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
		HRESULT Result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			FeatureLevels, static_cast<UINT>(sizeof(FeatureLevels) / sizeof(FeatureLevels[0])), D3D11_SDK_VERSION,
			&gExternalDevice, &FeatureLevel, &gExternalDeviceContext);

		if (Result == E_INVALIDARG)
		{
			Result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				FeatureLevels + 1, static_cast<UINT>((sizeof(FeatureLevels) / sizeof(FeatureLevels[0])) - 1), D3D11_SDK_VERSION,
				&gExternalDevice, &FeatureLevel, &gExternalDeviceContext);
		}

		if (FAILED(Result))
		{
			return false;
		}

		IDXGIDevice* DxgiDevice = nullptr;
		IDXGIAdapter* Adapter = nullptr;
		IDXGIFactory2* Factory = nullptr;
		IDXGISwapChain1* SwapChain = nullptr;

		auto CleanupLocals = [&]()
		{
			ReleaseCom(SwapChain);
			ReleaseCom(Factory);
			ReleaseCom(Adapter);
			ReleaseCom(DxgiDevice);
		};

		auto Fail = [&]() -> bool
		{
			CleanupLocals();
			ReleaseExternalComposition();
			ReleaseCom(gExternalSwapChain);
			ReleaseCom(gExternalDeviceContext);
			ReleaseCom(gExternalDevice);
			return false;
		};

		if (FAILED(gExternalDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice))) || !DxgiDevice)
			return Fail();

		if (FAILED(DxgiDevice->GetAdapter(&Adapter)) || !Adapter)
			return Fail();

		if (FAILED(Adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&Factory))) || !Factory)
			return Fail();

		DXGI_SWAP_CHAIN_DESC1 SwapChainDesc = {};
		SwapChainDesc.Width = static_cast<UINT>(Width);
		SwapChainDesc.Height = static_cast<UINT>(Height);
		SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		SwapChainDesc.SampleDesc.Count = 1;
		SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		SwapChainDesc.BufferCount = 2;
		SwapChainDesc.Scaling = DXGI_SCALING_STRETCH;
		SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		SwapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

		if (FAILED(Factory->CreateSwapChainForComposition(gExternalDevice, &SwapChainDesc, nullptr, &SwapChain)) || !SwapChain)
			return Fail();

		if (FAILED(DCompositionCreateDevice(DxgiDevice, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&gCompositionDevice))) || !gCompositionDevice)
			return Fail();

		if (FAILED(gCompositionDevice->CreateTargetForHwnd(gExternalWindow, TRUE, &gCompositionTarget)) || !gCompositionTarget)
			return Fail();

		if (FAILED(gCompositionDevice->CreateVisual(&gCompositionVisual)) || !gCompositionVisual)
			return Fail();

		if (FAILED(gCompositionVisual->SetContent(SwapChain))
			|| FAILED(gCompositionTarget->SetRoot(gCompositionVisual))
			|| FAILED(gCompositionDevice->Commit()))
			return Fail();

		gExternalSwapChain = SwapChain;
		SwapChain = nullptr;
		CleanupLocals();
		return true;
	}

	LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
	{
		if ((Msg == WM_KEYDOWN || Msg == WM_SYSKEYDOWN) && wParam == VK_F2 && (lParam & (1L << 30)) == 0)
		{
			TriggerUnrealConsoleHotkey();
			// Let the game's original window proc see the physical F2 as well. Some UE builds
			// only show the viewport console through the normal input routing path.
			if (gOriginalWndProc)
				return CallWindowProc(gOriginalWndProc, hWnd, Msg, wParam, lParam);
		}

		if (gMenuOpen && ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam))
			return true;

		return CallWindowProc(gOriginalWndProc, hWnd, Msg, wParam, lParam);
	}

	LRESULT CALLBACK ExternalOverlayWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
	{
		if ((Msg == WM_KEYDOWN || Msg == WM_SYSKEYDOWN) && wParam == VK_F2 && (lParam & (1L << 30)) == 0)
		{
			const bool PassThrough = !gMenuOpen || gExternalInputPassthrough.load();
			TriggerUnrealConsoleHotkey();
			if (!PassThrough && PostConsoleKeyToTargetWindow())
				std::cerr << "[Overlay] Console F2 posted to target window\n";
			return 0;
		}

		if (gMenuOpen && ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam))
			return true;

		const bool PassThrough = !gMenuOpen || gExternalInputPassthrough.load();
		if (Msg == WM_MOUSEACTIVATE && PassThrough)
			return MA_NOACTIVATE;

		if (Msg == WM_NCHITTEST && PassThrough)
			return HTTRANSPARENT;

		if (Msg == WM_DESTROY)
			return 0;

		return DefWindowProc(hWnd, Msg, wParam, lParam);
	}

	void ApplyImGuiStyle()
	{
		ImGuiStyle& Style = ImGui::GetStyle();
		Style.WindowRounding = 6.0f;
		Style.ChildRounding = 4.0f;
		Style.FrameRounding = 4.0f;
		Style.PopupRounding = 4.0f;
		Style.ScrollbarRounding = 6.0f;
		Style.GrabRounding = 4.0f;
		Style.WindowBorderSize = 1.0f;
		Style.FrameBorderSize = 0.0f;
		Style.WindowPadding = ImVec2(12.0f, 10.0f);
		Style.FramePadding = ImVec2(8.0f, 5.0f);

		ImVec4* Colors = Style.Colors;
		Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.09f, 0.94f);
		Colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.11f, 0.88f);
		Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.24f, 0.80f);
		Colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
		Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.20f, 0.19f, 1.00f);
		Colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.26f, 0.23f, 1.00f);
		Colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.09f, 1.00f);
		Colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.11f, 0.11f, 1.00f);
		Colors[ImGuiCol_CheckMark] = ImVec4(0.12f, 0.82f, 0.58f, 1.00f);
		Colors[ImGuiCol_SliderGrab] = ImVec4(0.12f, 0.82f, 0.58f, 1.00f);
		Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.72f, 0.25f, 1.00f);
		Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.15f, 0.15f, 1.00f);
		Colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.24f, 0.22f, 1.00f);
		Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.82f, 0.58f, 1.00f);
		Colors[ImGuiCol_Header] = ImVec4(0.14f, 0.18f, 0.17f, 1.00f);
		Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.25f, 0.22f, 1.00f);
		Colors[ImGuiCol_HeaderActive] = ImVec4(0.12f, 0.82f, 0.58f, 1.00f);
		Colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.12f, 0.13f, 1.00f);
		Colors[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.25f, 0.22f, 1.00f);
		Colors[ImGuiCol_TabSelected] = ImVec4(0.14f, 0.20f, 0.18f, 1.00f);
	}

	void DrawActorOverlay(const std::vector<ActorDebugInfo>& Actors, const OverlayConfig& Config);
	void DrawDeveloperPreviewOverlay(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors, const OverlayConfig& Config);
	void DrawCrosshairOverlay(const OverlayConfig& Config);
	void CopyActorsAndStats(std::vector<ActorDebugInfo>& OutActors, CaptureStats& OutStats);
	void CopyState(std::vector<ActorDebugInfo>& OutActors, std::vector<ActorDebugInfo>& OutFilteredActors, CaptureStats& OutStats);
	void DrawMenu(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors, const CaptureStats& Stats);

	void EnsureImGuiContext()
	{
		if (ImGui::GetCurrentContext())
			return;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& IO = ImGui::GetIO();
		IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		IO.IniFilename = nullptr;

		ApplyImGuiStyle();
	}

	void InstallOverlayWndProc(HWND Window)
	{
		if (!Window || gOriginalWndProc)
			return;

		gWindow = Window;
		gOriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWndProc)));
	}

	HWND FindMainProcessWindow()
	{
		struct SearchState
		{
			DWORD ProcessId = 0;
			HWND Window = nullptr;
			LONG Area = 0;
		} State;

		State.ProcessId = GetCurrentProcessId();

		EnumWindows([](HWND Window, LPARAM Param) -> BOOL
		{
			auto* State = reinterpret_cast<SearchState*>(Param);
			DWORD WindowProcessId = 0;
			GetWindowThreadProcessId(Window, &WindowProcessId);

			if (WindowProcessId != State->ProcessId || !IsWindowVisible(Window) || GetWindow(Window, GW_OWNER))
				return TRUE;

			char ClassName[128] = {};
			GetClassNameA(Window, ClassName, sizeof(ClassName));
			if (std::strcmp(ClassName, "ConsoleWindowClass") == 0)
				return TRUE;

			RECT Rect = {};
			GetWindowRect(Window, &Rect);
			const LONG Area = std::max<LONG>(0, Rect.right - Rect.left) * std::max<LONG>(0, Rect.bottom - Rect.top);
			if (Area > State->Area)
			{
				State->Area = Area;
				State->Window = Window;
			}

			return TRUE;
		}, reinterpret_cast<LPARAM>(&State));

		return State.Window;
	}

	bool GetTargetClientRect(RECT& OutRect)
	{
		if (!gTargetWindow || !IsWindow(gTargetWindow))
			gTargetWindow = FindMainProcessWindow();

		if (!gTargetWindow)
			return false;

		RECT Client = {};
		if (!GetClientRect(gTargetWindow, &Client))
			return false;

		POINT TopLeft = { Client.left, Client.top };
		POINT BottomRight = { Client.right, Client.bottom };
		ClientToScreen(gTargetWindow, &TopLeft);
		ClientToScreen(gTargetWindow, &BottomRight);

		OutRect.left = TopLeft.x;
		OutRect.top = TopLeft.y;
		OutRect.right = BottomRight.x;
		OutRect.bottom = BottomRight.y;
		return (OutRect.right - OutRect.left) > 0 && (OutRect.bottom - OutRect.top) > 0;
	}

	bool ExternalShouldPassThrough()
	{
		return !gMenuOpen || gExternalInputPassthrough.load();
	}

	void FocusWindowBestEffort(HWND Window)
	{
		if (!Window)
			return;

		const DWORD CurrentThread = GetCurrentThreadId();
		const DWORD ForegroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
		const bool Attach = ForegroundThread != 0 && ForegroundThread != CurrentThread;
		if (Attach)
			AttachThreadInput(CurrentThread, ForegroundThread, TRUE);

		BringWindowToTop(Window);
		SetForegroundWindow(Window);
		SetActiveWindow(Window);
		SetFocus(Window);

		if (Attach)
			AttachThreadInput(CurrentThread, ForegroundThread, FALSE);
	}

	void UpdateExternalClickThrough(bool Force = false)
	{
		if (!gExternalWindow)
			return;

		const bool PassThrough = ExternalShouldPassThrough();
		if (!Force && gExternalInputModeApplied && gLastExternalMenuOpen == gMenuOpen && gLastExternalInputPassthrough == PassThrough)
			return;

		LONG_PTR Style = GetWindowLongPtr(gExternalWindow, GWL_EXSTYLE);
		if (PassThrough)
			Style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
		else
			Style &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);

		SetWindowLongPtr(gExternalWindow, GWL_EXSTYLE, Style);
		const UINT Flags = SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | (PassThrough ? SWP_NOACTIVATE : 0);
		SetWindowPos(gExternalWindow, HWND_TOPMOST, 0, 0, 0, 0, Flags);

		if (PassThrough)
			FocusWindowBestEffort(gTargetWindow);
		else
			FocusWindowBestEffort(gExternalWindow);

		gLastExternalMenuOpen = gMenuOpen;
		gLastExternalInputPassthrough = PassThrough;
		gExternalInputModeApplied = true;
	}

	void ProcessOverlayHotkeys()
	{
		if (GetAsyncKeyState(VK_F2) & 1)
			TriggerUnrealConsoleHotkey();

		if (GetAsyncKeyState(VK_F4) & 1)
		{
			gMenuOpen = !gMenuOpen;
			if (gMenuOpen)
				gExternalInputPassthrough = false;

			if (gExternalOverlay)
				UpdateExternalClickThrough(true);
		}

		if (GetAsyncKeyState(VK_F8) & 1)
		{
			gExternalInputPassthrough = !gExternalInputPassthrough.load();
			if (gExternalOverlay)
				UpdateExternalClickThrough(true);
		}

		if (GetAsyncKeyState(VK_F7) & 1)
		{
			std::scoped_lock Lock(gConfigMutex);
			gConfig.Enabled = !gConfig.Enabled;
		}
	}

	bool ShouldCaptureForRenderFrame(const OverlayConfig& Config)
	{
		return Config.Enabled && (Config.FastOverlayMode || Config.CaptureOnRenderFrame);
	}

	void CaptureActorsForRenderFrame(const OverlayConfig& Config)
	{
		if (!ShouldCaptureForRenderFrame(Config))
			return;

		const DWORD Now = GetTickCount();
		const DWORD Last = gLastRenderFrameCaptureTick.load();
		const DWORD MinMs = static_cast<DWORD>(Config.FastOverlayMode
			? std::clamp(Config.FrameCaptureMinMs, 8, 100)
			: std::clamp(Config.FrameCaptureMinMs, 0, 100));
		if (Last != 0 && MinMs > 0 && Now - Last < MinMs)
			return;

		std::unique_lock<std::mutex> CaptureLock(gCaptureMutex, std::defer_lock);
		if (!CaptureLock.try_lock())
		{
			gRenderFrameCaptureSkips.fetch_add(1);
			return;
		}

		std::vector<ActorDebugInfo> Actors;
		CaptureStats Stats;
		CopyActorsAndStats(Actors, Stats);
		if (Actors.empty())
		{
			gLastRenderFrameCaptureTick.store(Now);
			gRenderFrameCapturePasses.fetch_add(1);
			return;
		}

		if (!ResolveSymbols(gSymbols))
		{
			gRenderFrameCaptureSkips.fetch_add(1);
			return;
		}

		RuntimePlayerContext RuntimeContext = BuildRuntimePlayerContext(Config, Stats);
		UEObject PlayerController = RuntimeContext.PlayerController ? RuntimeContext.PlayerController : FindFirstObjectOfClassCached(gSymbols.PlayerControllerClass);
		UEObject CameraManager = RuntimeContext.CameraManager ? RuntimeContext.CameraManager : FindFirstObjectOfClassCached(gSymbols.PlayerCameraManagerClass);
		UEObject LocalPawn = RuntimeContext.LocalPawn ? RuntimeContext.LocalPawn : RuntimeContext.LocalCharacter;
		Stats.HasPlayerController = static_cast<bool>(PlayerController);
		Stats.HasLocalPawn = static_cast<bool>(LocalPawn);
		Stats.HasProjection = PlayerController && gSymbols.ProjectWorldLocationToScreen;

		Vec3 CameraLocation;
		Vec3 CameraRotation;
		float CameraFov = 90.0f;
		Stats.HasCameraLocation = false;
		Stats.HasCameraRotation = false;
		Stats.HasCameraFov = false;
		Stats.CameraLocationSource = "missing";
		Stats.CameraRotationSource = "missing";
		Stats.UsedProjectionFallback = false;
		Stats.UsedDesktopProjection = false;

		if (PlayerController && CallViewPointFunction(PlayerController, gSymbols.GetPlayerViewPoint, CameraLocation, CameraRotation))
		{
			Stats.HasCameraLocation = true;
			Stats.HasCameraRotation = true;
			Stats.CameraLocationSource = "PlayerController.GetPlayerViewPoint()";
			Stats.CameraRotationSource = "PlayerController.GetPlayerViewPoint()";
		}

		if (!Stats.HasCameraLocation && CameraManager && CallNoArgVectorFunction(CameraManager, gSymbols.GetCameraLocation, CameraLocation))
		{
			Stats.HasCameraLocation = true;
			Stats.CameraLocationSource = "PlayerCameraManager.GetCameraLocation()";
		}
		else if (!Stats.HasCameraLocation && PlayerController && GetActorLocation(PlayerController, CameraLocation))
		{
			Stats.HasCameraLocation = true;
			Stats.CameraLocationSource = "PlayerController actor location";
		}

		if (!Stats.HasCameraRotation && CameraManager && CallNoArgVectorFunction(CameraManager, gSymbols.GetCameraRotation, CameraRotation))
		{
			Stats.HasCameraRotation = true;
			Stats.CameraRotationSource = "PlayerCameraManager.GetCameraRotation()";
		}
		else if (!Stats.HasCameraRotation && PlayerController && CallNoArgVectorFunction(PlayerController, gSymbols.GetControlRotation, CameraRotation))
		{
			Stats.HasCameraRotation = true;
			Stats.CameraRotationSource = "PlayerController.GetControlRotation()";
		}
		else if (!Stats.HasCameraRotation && PlayerController && CallNoArgVectorFunction(PlayerController, gSymbols.GetControllerActorRotation, CameraRotation))
		{
			Stats.HasCameraRotation = true;
			Stats.CameraRotationSource = "PlayerController.GetActorRotation()";
		}

		if (CameraManager && CallNoArgFloatFunction(CameraManager, gSymbols.GetCameraFov, CameraFov))
			Stats.HasCameraFov = true;

		float ViewWidth = 0.0f;
		float ViewHeight = 0.0f;
		RECT ProjectionRect = {};
		GetProjectionViewport(ProjectionRect, ViewWidth, ViewHeight);
		Stats.ProjectionLeft = ProjectionRect.left;
		Stats.ProjectionTop = ProjectionRect.top;
		Stats.ProjectionRight = ProjectionRect.right;
		Stats.ProjectionBottom = ProjectionRect.bottom;
		Stats.ProjectionWidth = ViewWidth;
		Stats.ProjectionHeight = ViewHeight;

		Stats.LocatedActors = 0;
		Stats.ProjectedActors = 0;
		Stats.ProjectionFailures = 0;
		Stats.ProjectionCandidateAttempts = 0;
		Stats.ReflectedPositionHits = 0;
		Stats.NativeProjectionAttempts = 0;
		Stats.NativeProjectionSuccesses = 0;
		Stats.NativeProjectionFailures = 0;
		Stats.FallbackProjectionAttempts = 0;
		Stats.FallbackProjectionSuccesses = 0;
		Stats.FallbackProjectionFailures = 0;
		Stats.InViewActors = 0;
		Stats.BoxedActors = 0;
		Stats.SkeletonActors = 0;
		Stats.SkeletonBones = 0;
		Stats.SkeletonSegments = 0;

		const int ProjectionRoute = std::clamp(Config.ProjectionRoute, 0, 2);
		const bool NativeProjectionAvailable = ProjectionRoute != 2 && Stats.HasProjection;
		const bool FallbackProjectionAvailable = ProjectionRoute != 1 && Config.UseProjectionFallback && Stats.HasCameraLocation && Stats.HasCameraRotation;
		const bool CanProject = NativeProjectionAvailable || FallbackProjectionAvailable;
		const int MaxLiveActors = Config.FastOverlayMode
			? std::clamp(Config.FrameProjectionMaxActors, 16, 512)
			: std::clamp(Config.FrameProjectionMaxActors, 16, 4096);
		const DWORD LastSkeleton = gLastFrameSkeletonTick.load();
		const DWORD SkeletonMinMs = static_cast<DWORD>(Config.FastOverlayMode
			? std::clamp(Config.FrameSkeletonMinMs, 125, 1000)
			: std::clamp(Config.FrameSkeletonMinMs, 0, 1000));
		const bool RefreshSkeletons = Config.DrawSkeletons && (LastSkeleton == 0 || SkeletonMinMs == 0 || Now - LastSkeleton >= SkeletonMinMs);

		int ProcessedActors = 0;
		const EObjectFlags SkipFlags = ActorSkipFlags();
		for (ActorDebugInfo& Info : Actors)
		{
			Info.HasScreen = false;
			Info.HasBox = false;
			Info.IsInView = false;
			Info.ProjectionAttemptCount = 0;
			Info.ProjectionFailure.clear();

			if (ProcessedActors >= MaxLiveActors)
			{
				Info.HasSkeleton = false;
				continue;
			}

			UEObject Object(reinterpret_cast<void*>(Info.Address));
			if (!IsReadableObject(Object.GetAddress()) || Object.HasAnyFlags(SkipFlags))
			{
				Info.HasLocation = false;
				Info.HasDistance = false;
				Info.HasSkeleton = false;
				continue;
			}

			ProcessedActors++;
			ApplyRuntimePlayerContext(Info, Object, RuntimeContext, Stats);
			UpdateLikelyPlayerScore(Info, Config);

			const Vec3 PreviousLocation = Info.Location;
			std::vector<PositionCandidate> PositionCandidates;
			Info.HasLocation = GetActorLocation(Object, Info.Location, &Info.LocationSource);
			if (Info.HasLocation)
			{
				Stats.LocatedActors++;
				AddPositionCandidate(PositionCandidates, Info.Location, Info.LocationSource.empty() ? "actor location" : Info.LocationSource);
			}
			else if (Config.UseReflectedPositionFallback)
			{
				const DWORD ReflectionDelay = static_cast<DWORD>(std::clamp(Config.ReflectedPositionRefreshMs, 0, 1000));
				const bool CanRefreshReflection = !Config.ThrottleLiveReflectionFallback
					|| Info.LastReflectedPositionTick == 0
					|| ReflectionDelay == 0
					|| Now - Info.LastReflectedPositionTick >= ReflectionDelay;

				if (CanRefreshReflection)
				{
					const size_t BeforeReflection = PositionCandidates.size();
					CollectReflectedPositionCandidates(Object, Config, PositionCandidates, &Stats);
					if (PositionCandidates.size() > BeforeReflection)
					{
						Info.LastReflectedPositionTick = Now;
						Stats.ReflectedPositionHits++;
					}
				}
				else if (IsSaneWorldPosition(PreviousLocation))
				{
					AddPositionCandidate(PositionCandidates, PreviousLocation, "cached reflected position");
				}

				Info.HasLocation = !PositionCandidates.empty();
				if (Info.HasLocation)
				{
					Info.Location = PositionCandidates.front().Location;
					Info.LocationSource = PositionCandidates.front().Source;
					Stats.LocatedActors++;
				}
			}

			if (Info.HasLocation && Info.HasBounds)
				AddPositionCandidate(PositionCandidates, BoundsOriginForCurrentLocation(Info), "live bounds origin");

			Info.PositionCandidateCount = static_cast<int>(PositionCandidates.size());
			if (!Info.HasLocation)
			{
				Info.HasDistance = false;
				Info.HasSkeleton = false;
				continue;
			}

			if (Stats.HasCameraLocation)
			{
				Info.DistanceMeters = Distance(Info.Location, CameraLocation) / 100.0f;
				Info.HasDistance = true;
			}
			else
			{
				Info.HasDistance = false;
			}

			const Vec3 LocationDelta =
			{
				Info.Location.X - PreviousLocation.X,
				Info.Location.Y - PreviousLocation.Y,
				Info.Location.Z - PreviousLocation.Z
			};

			if (!CanProject)
			{
				Info.ProjectionFailure = Stats.HasProjection
					? "projection skipped"
					: "projection unavailable; missing player controller or camera fallback";
				Stats.ProjectionFailures++;
				Info.HasSkeleton = false;
				continue;
			}

			Info.HasScreen = ProjectBestPositionCandidate(PositionCandidates, PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
				CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Info);
			if (Info.HasScreen)
				Stats.ProjectedActors++;
			else
				Stats.ProjectionFailures++;

			if (Info.HasScreen)
			{
				if (!Info.HasBox)
				{
					if (Info.HasBounds)
					{
						Vec2 BoundsMin;
						Vec2 BoundsMax;
						if (ProjectBoundsToScreenBoxAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
							CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight,
							BoundsOriginForCurrentLocation(Info), Info.BoundsExtent, BoundsMin, BoundsMax))
						{
							Info.BoxMin = BoundsMin;
							Info.BoxMax = BoundsMax;
							Info.HasBox = true;
						}
					}
				}

				if (!Info.HasBox)
				{
					const double HalfHeight = Info.HasBounds
						? std::clamp(std::max(Info.BoundsExtent.Z, 1.0), 10.0, static_cast<double>(std::max(Config.FallbackHalfHeight * 2.0f, 20.0f)))
						: static_cast<double>(Config.FallbackHalfHeight);
					const Vec3 Origin = Info.HasBounds ? BoundsOriginForCurrentLocation(Info) : Info.Location;
					Vec3 Top = { Origin.X, Origin.Y, Origin.Z + HalfHeight };
					Vec3 Bottom = { Origin.X, Origin.Y, Origin.Z - HalfHeight };
					Info.HasBox = ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
						CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Top, Info.ScreenTop)
						&& ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
							CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Bottom, Info.ScreenBottom);

					if (Info.HasBox)
						Info.HasBox = SetVerticalScreenBoxFromTopBottom(Info, Config, ViewWidth, ViewHeight);
				}

				if (RefreshSkeletons)
					CaptureSkeleton(Object, Config, Stats, PlayerController, gSymbols.ProjectWorldLocationToScreen,
						CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Info);
				else if (Info.HasSkeleton)
					ReprojectCachedSkeleton(Info, Config, Stats, PlayerController, gSymbols.ProjectWorldLocationToScreen,
						CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, LocationDelta);

				if (SetScreenBoxFromSkeleton(Info, Config, ViewWidth, ViewHeight))
					Info.HasBox = true;

				if (Info.HasBox)
					Stats.BoxedActors++;

				Info.IsInView = ActorProjectionInView(Info, ViewWidth, ViewHeight);
				if (Info.IsInView)
					Stats.InViewActors++;
			}
			else
			{
				Info.HasSkeleton = false;
			}
		}

		Stats.FrameProcessedActors = ProcessedActors;

		if (RefreshSkeletons)
			gLastFrameSkeletonTick.store(Now);

		std::sort(Actors.begin(), Actors.end(), [](const ActorDebugInfo& A, const ActorDebugInfo& B)
		{
			return A.DistanceMeters < B.DistanceMeters;
		});

		UpdateLikelyPlayerClassLockFromActors(Actors, Config);
		CopyLikelyClassLockToStats(Stats);
		Stats.CapturedActors = static_cast<int32>(Actors.size());
		Stats.Status = "Live background projection";

		{
			std::scoped_lock Lock(gActorMutex);
			gActors = std::move(Actors);
			gStats = std::move(Stats);
		}

		gLastRenderFrameCaptureTick.store(Now);
		gRenderFrameCapturePasses.fetch_add(1);
	}

	void DrawOverlayUi()
	{
		const OverlayConfig Config = GetConfigSnapshot();

		std::vector<ActorDebugInfo> Actors;
		std::vector<ActorDebugInfo> FilteredActors;
		CaptureStats Stats;
		CopyState(Actors, FilteredActors, Stats);

		ImGui::GetIO().MouseDrawCursor = gMenuOpen;
		DrawCrosshairOverlay(Config);
		DrawActorOverlay(Actors, Config);
		DrawDeveloperPreviewOverlay(Actors, FilteredActors, Config);
		DrawMenu(Actors, FilteredActors, Stats);
	}

	bool InitializeD3D11ImGui(IDXGISwapChain* SwapChain)
	{
		if (gImGuiInitialized)
			return true;

		if (FAILED(SwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&gDevice))) || !gDevice)
			return false;

		gDevice->GetImmediateContext(&gDeviceContext);

		DXGI_SWAP_CHAIN_DESC Desc = {};
		SwapChain->GetDesc(&Desc);
		gWindow = Desc.OutputWindow;

		CreateD3D11RenderTarget(SwapChain);

		EnsureImGuiContext();
		ImGui_ImplWin32_Init(gWindow);
		ImGui_ImplDX11_Init(gDevice, gDeviceContext);
		InstallOverlayWndProc(gWindow);
		gImGuiInitialized = true;
		SetStatus("ImGui overlay ready (D3D11)");
		return true;
	}

	void D3D12SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* OutCpu, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpu)
	{
		for (size_t Index = 0; Index < gD3D12SrvDescriptorUsed.size(); ++Index)
		{
			if (gD3D12SrvDescriptorUsed[Index])
				continue;

			gD3D12SrvDescriptorUsed[Index] = true;
			*OutCpu = gD3D12SrvHeap->GetCPUDescriptorHandleForHeapStart();
			*OutGpu = gD3D12SrvHeap->GetGPUDescriptorHandleForHeapStart();
			OutCpu->ptr += Index * gD3D12SrvDescriptorSize;
			OutGpu->ptr += Index * gD3D12SrvDescriptorSize;
			return;
		}

		*OutCpu = {};
		*OutGpu = {};
	}

	void D3D12SrvDescriptorFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE Cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
	{
		if (!gD3D12SrvHeap || !Cpu.ptr)
			return;

		const SIZE_T Start = gD3D12SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
		const SIZE_T Offset = Cpu.ptr - Start;
		const size_t Index = static_cast<size_t>(Offset / gD3D12SrvDescriptorSize);
		if (Index < gD3D12SrvDescriptorUsed.size())
			gD3D12SrvDescriptorUsed[Index] = false;
	}

	void ReleaseD3D12RenderTargets()
	{
		for (ID3D12Resource*& RenderTarget : gD3D12RenderTargets)
			ReleaseCom(RenderTarget);

		gD3D12RenderTargets.clear();
		gD3D12RtvHandles.clear();
	}

	bool CreateD3D12RenderTargets(IDXGISwapChain* SwapChain)
	{
		if (!gD3D12Device || !gD3D12RtvHeap || !SwapChain || gD3D12BufferCount == 0)
			return false;

		gD3D12RenderTargets.assign(gD3D12BufferCount, nullptr);
		gD3D12RtvHandles.assign(gD3D12BufferCount, {});

		D3D12_CPU_DESCRIPTOR_HANDLE Handle = gD3D12RtvHeap->GetCPUDescriptorHandleForHeapStart();
		for (UINT Index = 0; Index < gD3D12BufferCount; ++Index)
		{
			ID3D12Resource* BackBuffer = nullptr;
			if (FAILED(SwapChain->GetBuffer(Index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&BackBuffer))) || !BackBuffer)
				return false;

			gD3D12RenderTargets[Index] = BackBuffer;
			gD3D12RtvHandles[Index] = Handle;
			gD3D12Device->CreateRenderTargetView(BackBuffer, nullptr, Handle);
			Handle.ptr += gD3D12RtvDescriptorSize;
		}

		return true;
	}

	void WaitForD3D12Gpu()
	{
		if (!gD3D12CommandQueue || !gD3D12Fence || !gD3D12FenceEvent)
			return;

		const UINT64 FenceValue = ++gD3D12FenceValue;
		if (FAILED(gD3D12CommandQueue->Signal(gD3D12Fence, FenceValue)))
			return;

		if (gD3D12Fence->GetCompletedValue() < FenceValue)
		{
			gD3D12Fence->SetEventOnCompletion(FenceValue, gD3D12FenceEvent);
			WaitForSingleObject(gD3D12FenceEvent, 1000);
		}
	}

	void WaitForD3D12Frame(UINT BackBufferIndex)
	{
		if (!gD3D12Fence || !gD3D12FenceEvent || BackBufferIndex >= gD3D12FrameFenceValues.size())
			return;

		const UINT64 FenceValue = gD3D12FrameFenceValues[BackBufferIndex];
		if (FenceValue == 0 || gD3D12Fence->GetCompletedValue() >= FenceValue)
			return;

		gD3D12Fence->SetEventOnCompletion(FenceValue, gD3D12FenceEvent);
		WaitForSingleObject(gD3D12FenceEvent, 2);
	}

	void SignalD3D12Frame(UINT BackBufferIndex)
	{
		if (!gD3D12CommandQueue || !gD3D12Fence || BackBufferIndex >= gD3D12FrameFenceValues.size())
			return;

		const UINT64 FenceValue = ++gD3D12FenceValue;
		if (SUCCEEDED(gD3D12CommandQueue->Signal(gD3D12Fence, FenceValue)))
			gD3D12FrameFenceValues[BackBufferIndex] = FenceValue;
	}

	bool InitializeD3D12ImGui(IDXGISwapChain* SwapChain)
	{
		if (gImGuiInitialized)
			return true;

		if (!gD3D12CommandQueue)
		{
			SetStatus("D3D12 detected, waiting for command queue");
			return false;
		}

		IDXGISwapChain3* SwapChain3 = nullptr;
		if (FAILED(SwapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&SwapChain3))) || !SwapChain3)
			return false;

		if (FAILED(SwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&gD3D12Device))) || !gD3D12Device)
		{
			SwapChain3->Release();
			return false;
		}

		DXGI_SWAP_CHAIN_DESC Desc = {};
		SwapChain->GetDesc(&Desc);
		gWindow = Desc.OutputWindow;
		gD3D12BufferCount = std::max<UINT>(Desc.BufferCount, 2);
		gD3D12RtvFormat = Desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : Desc.BufferDesc.Format;

		D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
		RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		RtvHeapDesc.NumDescriptors = gD3D12BufferCount;
		if (FAILED(gD3D12Device->CreateDescriptorHeap(&RtvHeapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&gD3D12RtvHeap))))
		{
			SwapChain3->Release();
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC SrvHeapDesc = {};
		SrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		SrvHeapDesc.NumDescriptors = 64;
		SrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(gD3D12Device->CreateDescriptorHeap(&SrvHeapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&gD3D12SrvHeap))))
		{
			SwapChain3->Release();
			return false;
		}

		gD3D12RtvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		gD3D12SrvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		gD3D12SrvDescriptorUsed.assign(SrvHeapDesc.NumDescriptors, false);

		gD3D12CommandAllocators.assign(gD3D12BufferCount, nullptr);
		gD3D12FrameFenceValues.assign(gD3D12BufferCount, 0);
		for (UINT Index = 0; Index < gD3D12BufferCount; ++Index)
		{
			if (FAILED(gD3D12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&gD3D12CommandAllocators[Index]))))
			{
				SwapChain3->Release();
				return false;
			}
		}

		if (FAILED(gD3D12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gD3D12CommandAllocators[0], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&gD3D12CommandList))))
		{
			SwapChain3->Release();
			return false;
		}
		gD3D12CommandList->Close();

		if (FAILED(gD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gD3D12Fence))))
		{
			SwapChain3->Release();
			return false;
		}
		gD3D12FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		if (!CreateD3D12RenderTargets(SwapChain))
		{
			SwapChain3->Release();
			return false;
		}

		EnsureImGuiContext();
		ImGui_ImplWin32_Init(gWindow);

		ImGui_ImplDX12_InitInfo InitInfo;
		InitInfo.Device = gD3D12Device;
		InitInfo.CommandQueue = gD3D12CommandQueue;
		InitInfo.NumFramesInFlight = static_cast<int>(gD3D12BufferCount);
		InitInfo.RTVFormat = gD3D12RtvFormat;
		InitInfo.SrvDescriptorHeap = gD3D12SrvHeap;
		InitInfo.SrvDescriptorAllocFn = D3D12SrvDescriptorAlloc;
		InitInfo.SrvDescriptorFreeFn = D3D12SrvDescriptorFree;

		if (!ImGui_ImplDX12_Init(&InitInfo))
		{
			SwapChain3->Release();
			return false;
		}

		InstallOverlayWndProc(gWindow);
		gImGuiInitialized = true;
		SwapChain3->Release();
		SetStatus("ImGui overlay ready (D3D12)");
		return true;
	}

	void RenderD3D12DrawData(IDXGISwapChain* SwapChain)
	{
		IDXGISwapChain3* SwapChain3 = nullptr;
		if (FAILED(SwapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&SwapChain3))) || !SwapChain3)
			return;

		const UINT BackBufferIndex = SwapChain3->GetCurrentBackBufferIndex();
		if (BackBufferIndex >= gD3D12CommandAllocators.size() || BackBufferIndex >= gD3D12RenderTargets.size())
		{
			SwapChain3->Release();
			return;
		}

		WaitForD3D12Frame(BackBufferIndex);

		ID3D12CommandAllocator* Allocator = gD3D12CommandAllocators[BackBufferIndex];
		Allocator->Reset();
		gD3D12CommandList->Reset(Allocator, nullptr);

		D3D12_RESOURCE_BARRIER Barrier = {};
		Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		Barrier.Transition.pResource = gD3D12RenderTargets[BackBufferIndex];
		Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		gD3D12CommandList->ResourceBarrier(1, &Barrier);

		gD3D12CommandList->OMSetRenderTargets(1, &gD3D12RtvHandles[BackBufferIndex], FALSE, nullptr);
		ID3D12DescriptorHeap* Heaps[] = { gD3D12SrvHeap };
		gD3D12CommandList->SetDescriptorHeaps(1, Heaps);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gD3D12CommandList);

		std::swap(Barrier.Transition.StateBefore, Barrier.Transition.StateAfter);
		gD3D12CommandList->ResourceBarrier(1, &Barrier);
		gD3D12CommandList->Close();

		ID3D12CommandList* CommandLists[] = { gD3D12CommandList };
		gD3D12CommandQueue->ExecuteCommandLists(1, CommandLists);
		SignalD3D12Frame(BackBufferIndex);

		SwapChain3->Release();
	}

	bool WriteAbsoluteJump(void* Source, void* Destination)
	{
		if (!Source || !Destination)
			return false;

		DWORD OldProtect = 0;
		if (!VirtualProtect(Source, AbsoluteJumpSize, PAGE_EXECUTE_READWRITE, &OldProtect))
			return false;

#if defined(_WIN64)
		uint8 Patch[AbsoluteJumpSize] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
		*reinterpret_cast<uint64_t*>(Patch + 6) = reinterpret_cast<uint64_t>(Destination);
#else
		uint8 Patch[AbsoluteJumpSize] = {};
		Patch[0] = 0xE9;
		*reinterpret_cast<int32_t*>(Patch + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(Destination) - reinterpret_cast<uintptr_t>(Source) - 5);
		memset(Patch + 5, 0x90, AbsoluteJumpSize - 5);
#endif

		memcpy(Source, Patch, sizeof(Patch));
		FlushInstructionCache(GetCurrentProcess(), Source, sizeof(Patch));

		DWORD Ignored = 0;
		VirtualProtect(Source, AbsoluteJumpSize, OldProtect, &Ignored);
		return true;
	}

	bool InstallInlineHook(void* Target, void* Hook, void** Trampoline, uint8* OriginalBytes)
	{
		if (!Target || !Hook || !Trampoline || !OriginalBytes)
			return false;

		memcpy(OriginalBytes, Target, AbsoluteJumpSize);

		uint8* Stub = static_cast<uint8*>(VirtualAlloc(nullptr, AbsoluteJumpSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!Stub)
			return false;

		memcpy(Stub, OriginalBytes, AbsoluteJumpSize);
		if (!WriteAbsoluteJump(Stub + AbsoluteJumpSize, static_cast<uint8*>(Target) + AbsoluteJumpSize))
		{
			VirtualFree(Stub, 0, MEM_RELEASE);
			return false;
		}

		if (!WriteAbsoluteJump(Target, Hook))
		{
			VirtualFree(Stub, 0, MEM_RELEASE);
			return false;
		}

		*Trampoline = Stub;
		return true;
	}

	bool RestoreInlineHook(void* Target, void* Trampoline, const uint8* OriginalBytes)
	{
		if (!Target || !OriginalBytes)
			return false;

		DWORD OldProtect = 0;
		if (!VirtualProtect(Target, AbsoluteJumpSize, PAGE_EXECUTE_READWRITE, &OldProtect))
			return false;

		memcpy(Target, OriginalBytes, AbsoluteJumpSize);
		FlushInstructionCache(GetCurrentProcess(), Target, AbsoluteJumpSize);

		DWORD Ignored = 0;
		VirtualProtect(Target, AbsoluteJumpSize, OldProtect, &Ignored);

		if (Trampoline)
			VirtualFree(Trampoline, 0, MEM_RELEASE);

		return true;
	}

	bool InitializeOpenGLImGui(HDC DeviceContext)
	{
		if (gImGuiInitialized)
			return true;

		if (!DeviceContext || !wglGetCurrentContext())
			return false;

		gWindow = WindowFromDC(DeviceContext);
		if (!gWindow)
			return false;

		EnsureImGuiContext();
		ImGui_ImplWin32_Init(gWindow);
		if (!ImGui_ImplOpenGL3_Init(nullptr))
			return false;

		InstallOverlayWndProc(gWindow);
		gImGuiInitialized = true;
		SetStatus("ImGui overlay ready (OpenGL)");
		return true;
	}

	bool InitializeExternalOverlay()
	{
		if (gImGuiInitialized)
			return true;

		gTargetWindow = FindMainProcessWindow();
		if (!gTargetWindow)
			return false;

		RECT TargetRect = {};
		if (!GetTargetClientRect(TargetRect))
			return false;

		WNDCLASSEXA WindowClass = {};
		WindowClass.cbSize = sizeof(WindowClass);
		WindowClass.style = CS_HREDRAW | CS_VREDRAW;
		WindowClass.lpfnWndProc = ExternalOverlayWndProc;
		WindowClass.hInstance = GetModuleHandle(nullptr);
		WindowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
		WindowClass.lpszClassName = "Dumper7ExternalOverlay";
		RegisterClassExA(&WindowClass);

		const int Width = TargetRect.right - TargetRect.left;
		const int Height = TargetRect.bottom - TargetRect.top;
		DWORD ExternalStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
		if (ExternalShouldPassThrough())
			ExternalStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

		gExternalWindow = CreateWindowExA(
			ExternalStyle,
			WindowClass.lpszClassName,
			"Dumper-7 Overlay",
			WS_POPUP,
			TargetRect.left,
			TargetRect.top,
			Width,
			Height,
			nullptr,
			nullptr,
			WindowClass.hInstance,
			nullptr);

		if (!gExternalWindow)
			return false;

		ShowWindow(gExternalWindow, ExternalShouldPassThrough() ? SW_SHOWNOACTIVATE : SW_SHOW);
		SetWindowPos(gExternalWindow, HWND_TOPMOST, TargetRect.left, TargetRect.top, Width, Height,
			(ExternalShouldPassThrough() ? SWP_NOACTIVATE : 0) | SWP_SHOWWINDOW);

		if (!CreateExternalCompositionSwapChain(Width, Height))
		{
			DestroyWindow(gExternalWindow);
			gExternalWindow = nullptr;
			return false;
		}

		CreateExternalRenderTarget();
		EnsureImGuiContext();
		ImGui_ImplWin32_Init(gExternalWindow);
		ImGui_ImplDX11_Init(gExternalDevice, gExternalDeviceContext);

		gExternalOverlay = true;
		gImGuiInitialized = true;
		UpdateExternalClickThrough(true);
		SetStatus("ImGui overlay ready (external transparent mode)");
		return true;
	}

	void ExternalOverlayRenderLoop()
	{
		if (!InitializeExternalOverlay())
		{
			SetStatus("Failed to initialize external overlay");
			gRunning = false;
			return;
		}

		while (gRunning)
		{
			MSG Message = {};
			while (PeekMessage(&Message, gExternalWindow, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&Message);
				DispatchMessage(&Message);
			}

			RECT TargetRect = {};
			if (GetTargetClientRect(TargetRect))
			{
				const int Width = TargetRect.right - TargetRect.left;
				const int Height = TargetRect.bottom - TargetRect.top;
				RECT OverlayRect = {};
				GetWindowRect(gExternalWindow, &OverlayRect);

				if (OverlayRect.left != TargetRect.left || OverlayRect.top != TargetRect.top
					|| (OverlayRect.right - OverlayRect.left) != Width
					|| (OverlayRect.bottom - OverlayRect.top) != Height)
				{
					ReleaseExternalRenderTarget();
					gExternalSwapChain->ResizeBuffers(0, static_cast<UINT>(Width), static_cast<UINT>(Height), DXGI_FORMAT_UNKNOWN, 0);
					CreateExternalRenderTarget();
					if (gCompositionDevice)
						gCompositionDevice->Commit();
					SetWindowPos(gExternalWindow, HWND_TOPMOST, TargetRect.left, TargetRect.top, Width, Height, SWP_NOACTIVATE);
				}
			}

			ProcessOverlayHotkeys();
			UpdateExternalClickThrough();

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			DrawOverlayUi();
			ImGui::Render();

			const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			gExternalDeviceContext->OMSetRenderTargets(1, &gExternalRenderTargetView, nullptr);
			gExternalDeviceContext->ClearRenderTargetView(gExternalRenderTargetView, ClearColor);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			gExternalSwapChain->Present(0, 0);
			Sleep(1);
		}
	}

	bool StartExternalOverlay()
	{
		gExternalOverlay = true;
		gHookInstalled = true;
		gExternalRenderThread = std::thread(ExternalOverlayRenderLoop);
		return true;
	}

	void DrawOutlinedText(ImDrawList* DrawList, const ImVec2& Position, ImU32 Color, const char* Text)
	{
		const ImU32 Shadow = IM_COL32(0, 0, 0, 210);
		DrawList->AddText(ImVec2(Position.x + 1.0f, Position.y + 1.0f), Shadow, Text);
		DrawList->AddText(Position, Color, Text);
	}

	ImVec2 GetLineOrigin(const OverlayConfig& Config, const ImVec2& DisplaySize)
	{
		switch (Config.LineOrigin)
		{
		case 0:
			return ImVec2(DisplaySize.x * 0.5f, 0.0f);
		case 1:
			return ImVec2(DisplaySize.x * 0.5f, DisplaySize.y * 0.5f);
		default:
			return ImVec2(DisplaySize.x * 0.5f, DisplaySize.y);
		}
	}

	bool IsOnScreen(const Vec2& Screen, const ImVec2& DisplaySize)
	{
		return Screen.X >= 0.0f && Screen.Y >= 0.0f && Screen.X <= DisplaySize.x && Screen.Y <= DisplaySize.y;
	}

	void DrawCrosshairOverlay(const OverlayConfig& Config)
	{
		if (!Config.DrawCrosshair)
			return;

		const ImGuiIO& IO = ImGui::GetIO();
		const ImVec2 Center(IO.DisplaySize.x * 0.5f, IO.DisplaySize.y * 0.5f);
		const float Gap = std::max(Config.CrosshairGap, 0.0f);
		const float Size = std::max(Config.CrosshairSize, 1.0f);
		const float Thickness = std::max(Config.CrosshairThickness, 0.5f);
		const ImU32 Color = ImGui::ColorConvertFloat4ToU32(Config.CrosshairColor);
		const ImU32 Outline = IM_COL32(0, 0, 0, 190);
		ImDrawList* DrawList = ImGui::GetForegroundDrawList();

		auto Line = [&](ImVec2 A, ImVec2 B)
		{
			DrawList->AddLine(A, B, Outline, Thickness + 2.0f);
			DrawList->AddLine(A, B, Color, Thickness);
		};

		Line(ImVec2(Center.x - Gap - Size, Center.y), ImVec2(Center.x - Gap, Center.y));
		Line(ImVec2(Center.x + Gap, Center.y), ImVec2(Center.x + Gap + Size, Center.y));
		Line(ImVec2(Center.x, Center.y - Gap - Size), ImVec2(Center.x, Center.y - Gap));
		Line(ImVec2(Center.x, Center.y + Gap), ImVec2(Center.x, Center.y + Gap + Size));
	}

	void DrawActorOverlay(const std::vector<ActorDebugInfo>& Actors, const OverlayConfig& Config)
	{
		if (!Config.Enabled)
			return;

		ImDrawList* DrawList = ImGui::GetForegroundDrawList();
		const ImGuiIO& IO = ImGui::GetIO();
		const ImVec2 DisplaySize = IO.DisplaySize;
		const ImVec2 LineOrigin = GetLineOrigin(Config, DisplaySize);
		const ImU32 BoxColor = ImGui::ColorConvertFloat4ToU32(Config.BoxColor);
		const ImU32 LineColor = ImGui::ColorConvertFloat4ToU32(Config.LineColor);
		const ImU32 TextColor = ImGui::ColorConvertFloat4ToU32(Config.TextColor);
		const ImU32 BoundsColor = ImGui::ColorConvertFloat4ToU32(Config.BoundsColor);
		const ImU32 TargetPreviewColor = ImGui::ColorConvertFloat4ToU32(Config.TargetPreviewColor);
		const ImU32 SkeletonColor = ImGui::ColorConvertFloat4ToU32(Config.SkeletonColor);
		const ImU32 SkeletonOutline = IM_COL32(0, 0, 0, 210);
		const ImVec2 TargetPreviewOrigin(DisplaySize.x * 0.5f, DisplaySize.y * 0.5f);
		const ActorDebugInfo* TargetPreviewActor = nullptr;
		float TargetPreviewBestDistanceSq = Config.TargetPreviewRadius * Config.TargetPreviewRadius;

		for (const ActorDebugInfo& Actor : Actors)
		{
			if (!Actor.HasScreen)
				continue;

			if (Config.TargetMode == 1 && (!Actor.IsLikelyPlayer || Actor.IsLocalPlayer))
				continue;

			if (Config.OnlyOnScreen && !ActorProjectionInView(Actor, DisplaySize.x, DisplaySize.y))
				continue;

			const ImVec2 Screen(Actor.Screen.X, Actor.Screen.Y);
			if (Config.DrawTargetPreview && Actor.IsLikelyPlayer && !Actor.IsLocalPlayer)
			{
				const float DeltaX = Screen.x - TargetPreviewOrigin.x;
				const float DeltaY = Screen.y - TargetPreviewOrigin.y;
				const float DistanceSq = (DeltaX * DeltaX) + (DeltaY * DeltaY);
				if (DistanceSq <= TargetPreviewBestDistanceSq)
				{
					TargetPreviewBestDistanceSq = DistanceSq;
					TargetPreviewActor = &Actor;
				}
			}

			float BoxHeight = 0.0f;
			float BoxWidth = 0.0f;
			float BoxTop = Screen.y - Config.FallbackHalfHeight;
			float BoxBottom = Screen.y + Config.FallbackHalfHeight;
			float Left = Screen.x - Config.FallbackHalfWidth;
			float Right = Screen.x + Config.FallbackHalfWidth;

			const bool UseActorBox = Actor.HasBox && IsSaneScreenBox(Actor.BoxMin, Actor.BoxMax, Config, DisplaySize.x, DisplaySize.y);
			if (UseActorBox)
			{
				Left = std::min(Actor.BoxMin.X, Actor.BoxMax.X);
				Right = std::max(Actor.BoxMin.X, Actor.BoxMax.X);
				BoxTop = std::min(Actor.BoxMin.Y, Actor.BoxMax.Y);
				BoxBottom = std::max(Actor.BoxMin.Y, Actor.BoxMax.Y);
				const float Padding = std::max(Config.BoxPaddingPixels, 0.0f);
				Left -= Padding;
				Right += Padding;
				BoxTop -= Padding;
				BoxBottom += Padding;
				BoxHeight = std::max(BoxBottom - BoxTop, 1.0f);
				BoxWidth = std::max(Right - Left, 1.0f);
			}
			else
			{
				BoxHeight = Config.FallbackHalfHeight * 2.0f;
				BoxWidth = Config.FallbackHalfWidth * 2.0f;
			}

			if (Config.MinBoxHeightPixels > 0.0f && BoxHeight < Config.MinBoxHeightPixels)
			{
				const float CenterY = (BoxTop + BoxBottom) * 0.5f;
				const float HalfHeight = Config.MinBoxHeightPixels * 0.5f;
				BoxTop = CenterY - HalfHeight;
				BoxBottom = CenterY + HalfHeight;
				BoxHeight = Config.MinBoxHeightPixels;
			}

			if (BoxWidth < 4.0f)
			{
				Left = Screen.x - 2.0f;
				Right = Screen.x + 2.0f;
				BoxWidth = 4.0f;
			}

			ImVec2 LineTarget = Screen;
			if (UseActorBox)
			{
				if (Config.LineTarget == 1)
					LineTarget = ImVec2((Left + Right) * 0.5f, (BoxTop + BoxBottom) * 0.5f);
				else if (Config.LineTarget == 2)
					LineTarget = ImVec2((Left + Right) * 0.5f, BoxBottom);
			}

			if (Config.DrawLines)
				DrawList->AddLine(LineOrigin, LineTarget, LineColor, Config.LineThickness);

			if (Config.DrawSkeletons && Actor.HasSkeleton)
			{
				const float SkeletonThickness = std::max(Config.SkeletonThickness, 0.5f);
				for (const SkeletonSegment& Segment : Actor.SkeletonSegments)
				{
					if (Segment.A < 0 || Segment.B < 0
						|| Segment.A >= static_cast<int32>(Actor.SkeletonBones.size())
						|| Segment.B >= static_cast<int32>(Actor.SkeletonBones.size()))
					{
						continue;
					}

					const SkeletonBonePoint& A = Actor.SkeletonBones[Segment.A];
					const SkeletonBonePoint& B = Actor.SkeletonBones[Segment.B];
					if (!A.HasScreen || !B.HasScreen)
						continue;

					const ImVec2 PointA(A.Screen.X, A.Screen.Y);
					const ImVec2 PointB(B.Screen.X, B.Screen.Y);
					DrawList->AddLine(PointA, PointB, SkeletonOutline, SkeletonThickness + 2.0f);
					DrawList->AddLine(PointA, PointB, SkeletonColor, SkeletonThickness);
				}

				const float PointRadius = std::max(Config.SkeletonPointRadius, 0.0f);
				for (const SkeletonBonePoint& Bone : Actor.SkeletonBones)
				{
					if (!Bone.HasScreen)
						continue;

					const ImVec2 Point(Bone.Screen.X, Bone.Screen.Y);
					if (PointRadius > 0.0f)
					{
						DrawList->AddCircleFilled(Point, PointRadius + 1.0f, SkeletonOutline, 10);
						DrawList->AddCircleFilled(Point, PointRadius, SkeletonColor, 10);
					}

					if (Config.DrawSkeletonBoneIds || Config.DrawSkeletonBoneNames)
					{
						char BoneLabel[128] = {};
						if (Config.DrawSkeletonBoneIds && Config.DrawSkeletonBoneNames)
							std::snprintf(BoneLabel, sizeof(BoneLabel), "%d %s", Bone.Index, Bone.Name.c_str());
						else if (Config.DrawSkeletonBoneIds)
							std::snprintf(BoneLabel, sizeof(BoneLabel), "%d", Bone.Index);
						else
							std::snprintf(BoneLabel, sizeof(BoneLabel), "%s", Bone.Name.c_str());

						DrawOutlinedText(DrawList, ImVec2(Point.x + 4.0f, Point.y + 2.0f), SkeletonColor, BoneLabel);
					}
				}
			}

			if (Config.DrawBoxes)
				DrawList->AddRect(ImVec2(Left, BoxTop), ImVec2(Right, BoxBottom), BoxColor, 0.0f, 0, Config.BoxThickness);

			if (Config.DrawBounds && Actor.HasBounds)
			{
				const float Radius = std::clamp(BoxWidth * 0.22f, 3.0f, 24.0f);
				DrawList->AddCircle(Screen, Radius, BoundsColor, 20, 1.25f);
			}

			if (Config.DrawCenterDot)
				DrawList->AddCircleFilled(Screen, 2.5f, BoxColor, 12);

			if (Config.DrawNames || Config.DrawDistance)
			{
				char Label[256] = {};
				if (Config.DrawNames && Config.DrawDistance && Actor.HasDistance)
					std::snprintf(Label, sizeof(Label), "%s [%.1fm]", Actor.Name.c_str(), Actor.DistanceMeters);
				else if (Config.DrawNames)
					std::snprintf(Label, sizeof(Label), "%s", Actor.Name.c_str());
				else if (Actor.HasDistance)
					std::snprintf(Label, sizeof(Label), "%.1fm", Actor.DistanceMeters);

				if (Label[0] != '\0')
				{
					const ImVec2 TextSize = ImGui::CalcTextSize(Label);
					DrawOutlinedText(DrawList, ImVec2(Screen.x - (TextSize.x * 0.5f), BoxTop - TextSize.y - 3.0f), TextColor, Label);
				}
			}
		}

		if (Config.DrawTargetPreview)
		{
			DrawList->AddCircle(TargetPreviewOrigin, Config.TargetPreviewRadius, TargetPreviewColor, 96, 1.0f);
			if (TargetPreviewActor)
			{
				const ImVec2 TargetPoint(TargetPreviewActor->Screen.X, TargetPreviewActor->Screen.Y);
				DrawList->AddLine(TargetPreviewOrigin, TargetPoint, TargetPreviewColor, Config.TargetPreviewLineThickness);
				DrawList->AddCircle(TargetPoint, 7.0f, TargetPreviewColor, 24, Config.TargetPreviewLineThickness);

				char Label[128] = {};
				std::snprintf(Label, sizeof(Label), "target preview: %s (%d)", TargetPreviewActor->Name.c_str(), TargetPreviewActor->PlayerScore);
				DrawOutlinedText(DrawList, ImVec2(TargetPoint.x + 8.0f, TargetPoint.y + 8.0f), TargetPreviewColor, Label);
			}
		}
	}

	void DrawDeveloperPreviewActor(ImDrawList* DrawList, const ActorDebugInfo& Actor, const OverlayConfig& Config, const ImVec2& DisplaySize, const ImVec2& LineOrigin)
	{
		if (!Actor.HasScreen)
			return;
		if (!ActorClassMatchesTokens(Actor, Config.DeveloperPreviewClassFilter))
			return;
		if (Config.OnlyOnScreen && !ActorProjectionInView(Actor, DisplaySize.x, DisplaySize.y))
			return;

		const ImU32 PreviewLineColor = IM_COL32(255, 90, 220, 210);
		const ImU32 PreviewBoxColor = IM_COL32(255, 90, 220, 230);
		const ImU32 PreviewTextColor = IM_COL32(255, 220, 250, 235);
		const ImVec2 Screen(Actor.Screen.X, Actor.Screen.Y);

		float Left = Screen.x - Config.FallbackHalfWidth;
		float Right = Screen.x + Config.FallbackHalfWidth;
		float BoxTop = Screen.y - Config.FallbackHalfHeight;
		float BoxBottom = Screen.y + Config.FallbackHalfHeight;

		if (Actor.HasBox)
		{
			Left = std::min(Actor.BoxMin.X, Actor.BoxMax.X);
			Right = std::max(Actor.BoxMin.X, Actor.BoxMax.X);
			BoxTop = std::min(Actor.BoxMin.Y, Actor.BoxMax.Y);
			BoxBottom = std::max(Actor.BoxMin.Y, Actor.BoxMax.Y);
		}

		const ImVec2 BoxCenter((Left + Right) * 0.5f, (BoxTop + BoxBottom) * 0.5f);
		if (Config.DeveloperPreviewDrawLines)
			DrawList->AddLine(LineOrigin, BoxCenter, PreviewLineColor, std::max(Config.LineThickness, 1.0f));

		if (Config.DeveloperPreviewDrawBoxes)
			DrawList->AddRect(ImVec2(Left, BoxTop), ImVec2(Right, BoxBottom), PreviewBoxColor, 0.0f, 0, std::max(Config.BoxThickness, 1.0f));

		char Label[192] = {};
		std::snprintf(Label, sizeof(Label), "dev: %s", Actor.ClassName.empty() ? Actor.Name.c_str() : Actor.ClassName.c_str());
		DrawOutlinedText(DrawList, ImVec2(BoxCenter.x + 6.0f, BoxTop - 14.0f), PreviewTextColor, Label);
	}

	void DrawDeveloperPreviewOverlay(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors, const OverlayConfig& Config)
	{
		if (!Config.EnableDeveloperOptions || !Config.DeveloperPreviewEnabled || Config.DeveloperPreviewClassFilter[0] == '\0')
			return;

		ImDrawList* DrawList = ImGui::GetForegroundDrawList();
		const ImGuiIO& IO = ImGui::GetIO();
		const ImVec2 DisplaySize = IO.DisplaySize;
		const ImVec2 LineOrigin = GetLineOrigin(Config, DisplaySize);

		for (const ActorDebugInfo& Actor : Actors)
			DrawDeveloperPreviewActor(DrawList, Actor, Config, DisplaySize, LineOrigin);
		for (const ActorDebugInfo& Actor : FilteredActors)
			DrawDeveloperPreviewActor(DrawList, Actor, Config, DisplaySize, LineOrigin);
	}

	void CopyActorsAndStats(std::vector<ActorDebugInfo>& OutActors, CaptureStats& OutStats)
	{
		std::scoped_lock Lock(gActorMutex);
		OutActors = gActors;
		OutStats = gStats;
	}

	void CopyState(std::vector<ActorDebugInfo>& OutActors, std::vector<ActorDebugInfo>& OutFilteredActors, CaptureStats& OutStats)
	{
		std::scoped_lock Lock(gActorMutex);
		OutActors = gActors;
		OutFilteredActors = gFilteredActors;
		OutStats = gStats;
	}

	struct ActorClassSummary
	{
		std::string Name;
		std::string Path;
		uintptr_t ClassAddress = 0;
		uintptr_t SampleActorAddress = 0;
		int Count = 0;
		int Kept = 0;
		int Filtered = 0;
		int InView = 0;
		int Boxed = 0;
		int Skeletons = 0;
	};

	std::string ClassSummaryKey(const ActorDebugInfo& Actor)
	{
		if (!Actor.ClassPath.empty() && Actor.ClassPath != "None")
			return Actor.ClassPath;
		if (!Actor.ClassName.empty())
			return Actor.ClassName;
		char Buffer[32] = {};
		std::snprintf(Buffer, sizeof(Buffer), "0x%p", reinterpret_cast<void*>(Actor.ClassAddress));
		return Buffer;
	}

	bool ClassSummaryMatchesProbe(const ActorClassSummary& Summary, const char* Probe)
	{
		if (!Probe || Probe[0] == '\0')
			return true;

		return MatchesTokenListNoCase(Summary.Name, Probe)
			|| MatchesTokenListNoCase(Summary.Path, Probe);
	}

	std::vector<ActorClassSummary> BuildActorClassSummaries(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors)
	{
		std::vector<ActorClassSummary> Classes;
		std::unordered_map<std::string, size_t> IndexByKey;

		auto AddActor = [&](const ActorDebugInfo& Actor, bool bFiltered)
		{
			const std::string Key = ClassSummaryKey(Actor);
			auto It = IndexByKey.find(Key);
			if (It == IndexByKey.end())
			{
				ActorClassSummary Summary;
				Summary.Name = Actor.ClassName.empty() ? Key : Actor.ClassName;
				Summary.Path = Actor.ClassPath;
				Summary.ClassAddress = Actor.ClassAddress;
				Summary.SampleActorAddress = Actor.Address;
				It = IndexByKey.emplace(Key, Classes.size()).first;
				Classes.push_back(std::move(Summary));
			}

			ActorClassSummary& Summary = Classes[It->second];
			Summary.Count++;
			if (bFiltered)
				Summary.Filtered++;
			else
				Summary.Kept++;
			if (Actor.IsInView)
				Summary.InView++;
			if (Actor.HasBox)
				Summary.Boxed++;
			if (Actor.HasSkeleton)
				Summary.Skeletons++;
			if (Summary.SampleActorAddress == 0)
				Summary.SampleActorAddress = Actor.Address;
			if (Summary.ClassAddress == 0)
				Summary.ClassAddress = Actor.ClassAddress;
			if (Summary.Path.empty())
				Summary.Path = Actor.ClassPath;
		};

		for (const ActorDebugInfo& Actor : Actors)
			AddActor(Actor, false);
		for (const ActorDebugInfo& Actor : FilteredActors)
			AddActor(Actor, true);

		std::sort(Classes.begin(), Classes.end(), [](const ActorClassSummary& A, const ActorClassSummary& B)
		{
			if (A.Count != B.Count)
				return A.Count > B.Count;
			return A.Name < B.Name;
		});

		return Classes;
	}

	void FocusClassForOverlay(const ActorClassSummary& Summary)
	{
		if (Summary.Name.empty())
			return;

		gSelectedActorAddress = Summary.SampleActorAddress;
		std::scoped_lock Lock(gConfigMutex);
		gConfig.EnableClassFilter = true;
		gConfig.Enabled = true;
		gConfig.DrawLines = true;
		gConfig.DrawBoxes = true;
		std::snprintf(gConfig.ClassFilter, sizeof(gConfig.ClassFilter), "%s", Summary.Name.c_str());
	}

	void FocusActorClassForOverlay(const ActorDebugInfo& Actor)
	{
		if (Actor.ClassName.empty())
			return;

		gSelectedActorAddress = Actor.Address;
		std::scoped_lock Lock(gConfigMutex);
		gConfig.EnableClassFilter = true;
		gConfig.Enabled = true;
		gConfig.DrawLines = true;
		gConfig.DrawBoxes = true;
		std::snprintf(gConfig.ClassFilter, sizeof(gConfig.ClassFilter), "%s", Actor.ClassName.c_str());
	}

	void FocusClassForDeveloperPreview(const ActorClassSummary& Summary)
	{
		if (Summary.Name.empty())
			return;

		gSelectedActorAddress = Summary.SampleActorAddress;
		std::scoped_lock Lock(gConfigMutex);
		gConfig.EnableDeveloperOptions = true;
		gConfig.DeveloperPreviewEnabled = true;
		std::snprintf(gConfig.DeveloperPreviewClassFilter, sizeof(gConfig.DeveloperPreviewClassFilter), "%s", Summary.Name.c_str());
	}

	void FocusActorClassForDeveloperPreview(const ActorDebugInfo& Actor)
	{
		if (Actor.ClassName.empty())
			return;

		gSelectedActorAddress = Actor.Address;
		std::scoped_lock Lock(gConfigMutex);
		gConfig.EnableDeveloperOptions = true;
		gConfig.DeveloperPreviewEnabled = true;
		std::snprintf(gConfig.DeveloperPreviewClassFilter, sizeof(gConfig.DeveloperPreviewClassFilter), "%s", Actor.ClassName.c_str());
	}

	void UpdateClassAutoCycle(const std::vector<ActorClassSummary>& Classes)
	{
		const OverlayConfig Config = GetConfigSnapshot();
		if (!Config.EnableDeveloperOptions || !Config.DeveloperPreviewEnabled || !Config.DeveloperAutoCycleClasses || Classes.empty())
			return;

		const DWORD Now = GetTickCount();
		const DWORD Delay = static_cast<DWORD>(std::clamp(Config.DeveloperClassCycleMs, 250, 10000));
		if (gClassCycleIndex >= 0 && gLastClassCycleTick != 0 && Now - gLastClassCycleTick < Delay)
			return;

		gClassCycleIndex = (gClassCycleIndex + 1) % static_cast<int>(Classes.size());
		gLastClassCycleTick = Now;
		FocusClassForDeveloperPreview(Classes[gClassCycleIndex]);
	}

	void DrawActorTable(const std::vector<ActorDebugInfo>& Actors)
	{
		const ImGuiTableFlags Flags = ImGuiTableFlags_Resizable
			| ImGuiTableFlags_RowBg
			| ImGuiTableFlags_BordersOuter
			| ImGuiTableFlags_BordersV
			| ImGuiTableFlags_ScrollY;

		if (!ImGui::BeginTable("##actors", 9, Flags, ImVec2(0.0f, 280.0f)))
			return;

		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 76.0f);
		ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 58.0f);
		ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 78.0f);
		ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 190.0f);
		ImGui::TableSetupColumn("Extent", ImGuiTableColumnFlags_WidthFixed, 160.0f);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableHeadersRow();

		ImGuiListClipper Clipper;
		Clipper.Begin(static_cast<int>(Actors.size()));
		while (Clipper.Step())
		{
			for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				const ActorDebugInfo& Actor = Actors[Row];
				ImGui::PushID(static_cast<int>(Actor.Index));
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				const bool Selected = gSelectedActorAddress == Actor.Address;
				if (ImGui::Selectable(Actor.Name.c_str(), Selected, ImGuiSelectableFlags_SpanAllColumns))
					gSelectedActorAddress = Actor.Address;

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Actor.ClassName.c_str());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(ActorKindText(Actor));

				ImGui::TableNextColumn();
				ImGui::Text("%d", Actor.PlayerScore);

				ImGui::TableNextColumn();
				const std::string FlagsText = ActorFlagText(Actor);
				ImGui::TextUnformatted(FlagsText.c_str());

				ImGui::TableNextColumn();
				if (Actor.HasDistance)
					ImGui::Text("%.1fm", Actor.DistanceMeters);
				else
					ImGui::TextUnformatted("-");

				ImGui::TableNextColumn();
				if (Actor.HasLocation)
					ImGui::Text("%.1f, %.1f, %.1f", Actor.Location.X, Actor.Location.Y, Actor.Location.Z);
				else
					ImGui::TextUnformatted("-");

				ImGui::TableNextColumn();
				if (Actor.HasBounds)
					ImGui::Text("%.1f, %.1f, %.1f", Actor.BoundsExtent.X, Actor.BoundsExtent.Y, Actor.BoundsExtent.Z);
				else
					ImGui::TextUnformatted("-");

				ImGui::TableNextColumn();
				ImGui::Text("0x%p", reinterpret_cast<void*>(Actor.Address));
				ImGui::PopID();
			}
		}

		ImGui::EndTable();
	}

	const ActorDebugInfo* FindSelectedActor(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors)
	{
		auto It = std::find_if(Actors.begin(), Actors.end(), [](const ActorDebugInfo& Actor)
		{
			return Actor.Address == gSelectedActorAddress;
		});

		if (It != Actors.end())
			return &*It;

		auto FilteredIt = std::find_if(FilteredActors.begin(), FilteredActors.end(), [](const ActorDebugInfo& Actor)
		{
			return Actor.Address == gSelectedActorAddress;
		});

		if (FilteredIt != FilteredActors.end())
			return &*FilteredIt;

		return nullptr;
	}

	bool ResolveActorClass(const ActorDebugInfo& Actor, UEClass& OutClass)
	{
		if (Actor.ClassAddress != 0 && IsReadableObject(reinterpret_cast<void*>(Actor.ClassAddress)))
		{
			OutClass = UEClass(reinterpret_cast<void*>(Actor.ClassAddress));
			return static_cast<bool>(OutClass);
		}

		if (Actor.Address != 0 && IsReadableObject(reinterpret_cast<void*>(Actor.Address)))
		{
			UEObject Object(reinterpret_cast<void*>(Actor.Address));
			OutClass = Object.GetClass();
			return static_cast<bool>(OutClass);
		}

		return false;
	}

	const char* PropertyKindName(const UEProperty& Property)
	{
		if (Property.IsA(EClassCastFlags::BoolProperty)) return "bool";
		if (Property.IsA(EClassCastFlags::Int8Property)) return "int8";
		if (Property.IsA(EClassCastFlags::Int16Property)) return "int16";
		if (Property.IsA(EClassCastFlags::IntProperty)) return "int32";
		if (Property.IsA(EClassCastFlags::Int64Property)) return "int64";
		if (Property.IsA(EClassCastFlags::ByteProperty)) return "byte";
		if (Property.IsA(EClassCastFlags::UInt16Property)) return "uint16";
		if (Property.IsA(EClassCastFlags::UInt32Property)) return "uint32";
		if (Property.IsA(EClassCastFlags::UInt64Property)) return "uint64";
		if (Property.IsA(EClassCastFlags::FloatProperty)) return "float";
		if (Property.IsA(EClassCastFlags::DoubleProperty)) return "double";
		if (Property.IsA(EClassCastFlags::NameProperty)) return "name";
		if (Property.IsA(EClassCastFlags::StrProperty)) return "string";
		if (Property.IsA(EClassCastFlags::AnsiStrProperty)) return "ansi string";
		if (Property.IsA(EClassCastFlags::Utf8StrProperty)) return "utf8 string";
		if (Property.IsA(EClassCastFlags::TextProperty)) return "text";
		if (Property.IsA(EClassCastFlags::EnumProperty)) return "enum";
		if (Property.IsA(EClassCastFlags::StructProperty)) return "struct";
		if (Property.IsA(EClassCastFlags::ObjectPropertyBase) || Property.IsA(EClassCastFlags::ObjectProperty)) return "object";
		if (Property.IsA(EClassCastFlags::ClassProperty) || Property.IsA(EClassCastFlags::SoftClassProperty)) return "class";
		if (Property.IsA(EClassCastFlags::ArrayProperty)) return "array";
		if (Property.IsA(EClassCastFlags::MapProperty)) return "map";
		if (Property.IsA(EClassCastFlags::SetProperty)) return "set";
		if (Property.IsA(EClassCastFlags::DelegateProperty) || Property.IsA(EClassCastFlags::MulticastDelegateProperty)) return "delegate";
		if (Property.IsA(EClassCastFlags::FieldPathProperty)) return "field path";
		if (Property.IsA(EClassCastFlags::OptionalProperty)) return "optional";
		return "unknown";
	}

	void DrawClassReflection(const ActorDebugInfo& Actor)
	{
		UEClass Class;
		if (!ResolveActorClass(Actor, Class))
		{
			ImGui::TextUnformatted("Reflection: class pointer is no longer readable");
			return;
		}

		OverlayConfig Config = GetConfigSnapshot();
		if (ImGui::Button("Preview selected class"))
			FocusActorClassForDeveloperPreview(Actor);
		ImGui::SameLine();
		if (ImGui::Button("Use selected class as main filter"))
			FocusActorClassForOverlay(Actor);
		ImGui::SameLine();
		bool bShowInheritedMembers = Config.DeveloperShowInheritedMembers;
		if (ImGui::Checkbox("Inherited members", &bShowInheritedMembers))
		{
			std::scoped_lock Lock(gConfigMutex);
			gConfig.DeveloperShowInheritedMembers = bShowInheritedMembers;
			Config.DeveloperShowInheritedMembers = bShowInheritedMembers;
		}

		ImGui::Text("Class address: 0x%p", Class.GetAddress());
		ImGui::TextWrapped("Class path: %s", Class.GetPathName().c_str());

		if (!ImGui::TreeNode("Fields / Methods"))
			return;

		int DrawnStructs = 0;
		for (UEStruct Struct = Class; Struct; Struct = Config.DeveloperShowInheritedMembers ? Struct.GetSuper() : UEStruct(nullptr))
		{
			if (++DrawnStructs > 32)
				break;

			const std::string StructName = Struct.GetName();
			const std::vector<UEProperty> Properties = Struct.GetProperties();
			const std::vector<UEFunction> Functions = Struct.GetFunctions();
			const bool Open = ImGui::TreeNode(Struct.GetAddress(), "%s  (%zu fields, %zu methods)",
				StructName.c_str(), Properties.size(), Functions.size());
			if (!Open)
				continue;

			const int MaxRows = std::max(10, Config.DeveloperMaxRows);
			if (ImGui::BeginTable("##reflection_fields", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 150.0f)))
			{
				ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 86.0f);
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 190.0f);
				ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 54.0f);
				ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 160.0f);
				ImGui::TableHeadersRow();

				int Rows = 0;
				for (const UEProperty& Property : Properties)
				{
					if (Rows++ >= MaxRows)
						break;

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Property.GetName().c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(PropertyKindName(Property));
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Property.GetCppType().c_str());
					ImGui::TableNextColumn();
					ImGui::Text("0x%X", Property.GetOffset());
					ImGui::TableNextColumn();
					ImGui::Text("%d", Property.GetSize());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Property.StringifyFlags().c_str());
				}

				ImGui::EndTable();
			}

			if (ImGui::BeginTable("##reflection_methods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 150.0f)))
			{
				ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Params", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableSetupColumn("Exec", ImGuiTableColumnFlags_WidthFixed, 120.0f);
				ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 220.0f);
				ImGui::TableHeadersRow();

				int Rows = 0;
				for (const UEFunction& Function : Functions)
				{
					if (Rows++ >= MaxRows)
						break;

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Function.GetName().c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%zu", Function.GetProperties().size());
					ImGui::TableNextColumn();
					ImGui::Text("0x%p", Function.GetExecFunction());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Function.StringifyFlags().c_str());
				}

				ImGui::EndTable();
			}

			ImGui::TreePop();

			if (!Config.DeveloperShowInheritedMembers)
				break;
		}

		ImGui::TreePop();
	}

	void DrawSelectedActor(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors)
	{
		const ActorDebugInfo* SelectedActor = FindSelectedActor(Actors, FilteredActors);
		if (!SelectedActor)
			return;

		const ActorDebugInfo& Actor = *SelectedActor;
		ImGui::SeparatorText("Selected Actor");
		ImGui::TextWrapped("%s", Actor.FullName.c_str());
		ImGui::Text("Index: %d", Actor.Index);
		ImGui::Text("Kind: %s  Pawn: %s  Character: %s  Local: %s",
			ActorKindText(Actor),
			Actor.IsPawn ? "yes" : "no",
			Actor.IsCharacter ? "yes" : "no",
			Actor.IsLocalPlayer ? "yes" : "no");
		const std::string FlagsText = ActorFlagText(Actor);
		ImGui::TextWrapped("Flags: %s", FlagsText.c_str());
		ImGui::Text("Environment: %s  Filter: %s",
			Actor.IsEnvironment ? "yes" : "no",
			FilterReasonName(Actor.FilterReason));
		ImGui::Text("Likely-player score: %d  Likely: %s",
			Actor.PlayerScore,
			Actor.IsLikelyPlayer ? "yes" : "no");
		ImGui::TextWrapped("Score reasons: %s", Actor.PlayerScoreReasons.c_str());
		ImGui::Text("In view: %s", Actor.IsInView ? "yes" : "no");
		ImGui::Text("Address: 0x%p", reinterpret_cast<void*>(Actor.Address));
		if (Actor.HasPlayerState)
			ImGui::Text("PlayerState: 0x%p", reinterpret_cast<void*>(Actor.PlayerStateAddress));
		if (Actor.HasLocation)
		{
			ImGui::Text("Location: %.3f, %.3f, %.3f", Actor.Location.X, Actor.Location.Y, Actor.Location.Z);
			ImGui::TextWrapped("Location source: %s", Actor.LocationSource.empty() ? "unknown" : Actor.LocationSource.c_str());
		}
		ImGui::Text("Position candidates: %d  Projection attempts: %d", Actor.PositionCandidateCount, Actor.ProjectionAttemptCount);
		if (!Actor.ProjectionSource.empty())
			ImGui::TextWrapped("Projection source: %s", Actor.ProjectionSource.c_str());
		if (!Actor.ProjectionFailure.empty())
			ImGui::TextWrapped("Projection failure: %s", Actor.ProjectionFailure.c_str());
		if (Actor.HasBounds)
		{
			ImGui::Text("Bounds origin: %.3f, %.3f, %.3f", Actor.BoundsOrigin.X, Actor.BoundsOrigin.Y, Actor.BoundsOrigin.Z);
			ImGui::Text("Bounds extent: %.3f, %.3f, %.3f", Actor.BoundsExtent.X, Actor.BoundsExtent.Y, Actor.BoundsExtent.Z);
			ImGui::Text("Sphere radius: %.3f", Actor.SphereRadius);
		}
		if (Actor.HasScreen)
			ImGui::Text("Screen: %.1f, %.1f", Actor.Screen.X, Actor.Screen.Y);
		if (Actor.HasBox)
			ImGui::Text("Screen box: %.1f, %.1f -> %.1f, %.1f", Actor.BoxMin.X, Actor.BoxMin.Y, Actor.BoxMax.X, Actor.BoxMax.Y);
		if (Actor.HasSkeleton)
		{
			ImGui::Text("Skeleton: %d bones  %d lines", static_cast<int>(Actor.SkeletonBones.size()), static_cast<int>(Actor.SkeletonSegments.size()));
			ImGui::TextWrapped("Skeleton source: %s", Actor.SkeletonSource.c_str());
		}
		DrawClassReflection(Actor);
	}

	bool ActorPassesDeveloperProbe(const ActorDebugInfo& Actor, const char* Probe)
	{
		return !Probe || Probe[0] == '\0' || ActorTextMatchesTokens(Actor, Probe);
	}

	void DrawDeveloperActorTable(const char* TableId, const std::vector<ActorDebugInfo>& Actors, bool ShowFilterReason, const char* Probe, int MaxRows, float Height)
	{
		const ImGuiTableFlags Flags = ImGuiTableFlags_Resizable
			| ImGuiTableFlags_RowBg
			| ImGuiTableFlags_BordersOuter
			| ImGuiTableFlags_BordersV
			| ImGuiTableFlags_ScrollY;
		const int ColumnCount = ShowFilterReason ? 12 : 11;
		if (!ImGui::BeginTable(TableId, ColumnCount, Flags, ImVec2(0.0f, Height)))
			return;

		if (ShowFilterReason)
			ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 92.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 58.0f);
		ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 170.0f);
		ImGui::TableSetupColumn("Screen", ImGuiTableColumnFlags_WidthFixed, 96.0f);
		ImGui::TableSetupColumn("Proj", ImGuiTableColumnFlags_WidthFixed, 64.0f);
		ImGui::TableSetupColumn("Skel", ImGuiTableColumnFlags_WidthFixed, 58.0f);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableHeadersRow();

		int RowsDrawn = 0;
		for (const ActorDebugInfo& Actor : Actors)
		{
			if (MaxRows > 0 && RowsDrawn >= MaxRows)
				break;
			if (!ActorPassesDeveloperProbe(Actor, Probe))
				continue;

			ImGui::PushID(static_cast<int>(Actor.Index));
			ImGui::TableNextRow();

			if (ShowFilterReason)
			{
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(FilterReasonName(Actor.FilterReason));
			}

			ImGui::TableNextColumn();
			const bool Selected = gSelectedActorAddress == Actor.Address;
			if (ImGui::Selectable(Actor.Name.c_str(), Selected, ImGuiSelectableFlags_SpanAllColumns))
				gSelectedActorAddress = Actor.Address;

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Actor.ClassName.c_str());

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(ActorKindText(Actor));

			ImGui::TableNextColumn();
			ImGui::Text("%d", Actor.PlayerScore);

			ImGui::TableNextColumn();
			const std::string FlagsText = ActorFlagText(Actor);
			ImGui::TextUnformatted(FlagsText.c_str());

			ImGui::TableNextColumn();
			if (Actor.HasDistance)
				ImGui::Text("%.1fm", Actor.DistanceMeters);
			else
				ImGui::TextUnformatted("-");

			ImGui::TableNextColumn();
			if (Actor.HasLocation)
				ImGui::Text("%.1f, %.1f, %.1f", Actor.Location.X, Actor.Location.Y, Actor.Location.Z);
			else
				ImGui::TextUnformatted("-");

			ImGui::TableNextColumn();
			if (Actor.HasScreen)
				ImGui::Text("%.0f, %.0f", Actor.Screen.X, Actor.Screen.Y);
			else
				ImGui::TextUnformatted("-");

			ImGui::TableNextColumn();
			const char* ProjectionState = Actor.HasBox ? "box" : (Actor.HasScreen ? "point" : (Actor.ProjectionFailure.empty() ? "-" : "fail"));
			ImGui::TextUnformatted(ProjectionState);
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::Text("Candidates: %d", Actor.PositionCandidateCount);
				ImGui::Text("Attempts: %d", Actor.ProjectionAttemptCount);
				if (!Actor.ProjectionSource.empty())
					ImGui::TextWrapped("Source: %s", Actor.ProjectionSource.c_str());
				if (!Actor.ProjectionFailure.empty())
					ImGui::TextWrapped("Failure: %s", Actor.ProjectionFailure.c_str());
				ImGui::EndTooltip();
			}

			ImGui::TableNextColumn();
			if (Actor.HasSkeleton)
				ImGui::Text("%d/%d", static_cast<int>(Actor.SkeletonBones.size()), static_cast<int>(Actor.SkeletonSegments.size()));
			else
				ImGui::TextUnformatted("-");
			if (Actor.HasSkeleton && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Actor.SkeletonSource.c_str());

			ImGui::TableNextColumn();
			ImGui::Text("0x%p", reinterpret_cast<void*>(Actor.Address));
			ImGui::PopID();
			RowsDrawn++;
		}

		ImGui::EndTable();
	}

	void DrawDeveloperClassBrowser(const std::vector<ActorClassSummary>& Classes, const OverlayConfig& Config, float Height)
	{
		const ImGuiTableFlags Flags = ImGuiTableFlags_Resizable
			| ImGuiTableFlags_RowBg
			| ImGuiTableFlags_BordersOuter
			| ImGuiTableFlags_BordersV
			| ImGuiTableFlags_ScrollY;

		if (!ImGui::BeginTable("##developer_classes", 8, Flags, ImVec2(0.0f, Height)))
			return;

		ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 56.0f);
		ImGui::TableSetupColumn("Kept", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Filt", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Box", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Skel", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Class Ptr", ImGuiTableColumnFlags_WidthFixed, 118.0f);
		ImGui::TableHeadersRow();

		const int MaxRows = std::max(10, Config.DeveloperMaxRows);
		int RowsDrawn = 0;
		for (const ActorClassSummary& Summary : Classes)
		{
			if (RowsDrawn >= MaxRows)
				break;
			if (!ClassSummaryMatchesProbe(Summary, Config.DeveloperProbeFilter))
				continue;

			const bool Active = Config.DeveloperPreviewEnabled && Config.DeveloperPreviewClassFilter[0] != '\0'
				&& (MatchesTokenListNoCase(Summary.Name, Config.DeveloperPreviewClassFilter) || MatchesTokenListNoCase(Summary.Path, Config.DeveloperPreviewClassFilter));

			ImGui::PushID(static_cast<int>(Summary.ClassAddress ^ Summary.SampleActorAddress));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(Summary.Name.c_str(), Active, ImGuiSelectableFlags_SpanAllColumns))
				FocusClassForDeveloperPreview(Summary);
			if (Active && Config.DeveloperAutoCycleClasses)
				ImGui::SetScrollHereY(0.5f);
			if (!Summary.Path.empty() && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Summary.Path.c_str());

			ImGui::TableNextColumn();
			ImGui::Text("%d", Summary.Count);
			ImGui::TableNextColumn();
			ImGui::Text("%d", Summary.Kept);
			ImGui::TableNextColumn();
			ImGui::Text("%d", Summary.Filtered);
			ImGui::TableNextColumn();
			ImGui::Text("%d", Summary.InView);
			ImGui::TableNextColumn();
			ImGui::Text("%d", Summary.Boxed);
			ImGui::TableNextColumn();
			ImGui::Text("%d", Summary.Skeletons);
			ImGui::TableNextColumn();
			ImGui::Text("0x%p", reinterpret_cast<void*>(Summary.ClassAddress));
			ImGui::PopID();
			RowsDrawn++;
		}

		ImGui::EndTable();
	}

	void DrawTypedConfigEditor()
	{
		if (!ImGui::TreeNode("Live Typed Overlay Variables"))
			return;

		ImGui::TextWrapped("These are project-owned overlay/debug variables. Edits apply immediately; use Save settings to persist them.");
		if (ImGui::SmallButton("Save typed values"))
			SaveOverlayConfig();
		ImGui::SameLine();
		if (ImGui::SmallButton("Reload typed values"))
			LoadOverlayConfig();

		std::scoped_lock Lock(gConfigMutex);
		const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable;
		if (ImGui::BeginTable("##typed_config_editor", 3, Flags, ImVec2(0.0f, 0.0f)))
		{
			ImGui::TableSetupColumn("Variable", ImGuiTableColumnFlags_WidthFixed, 190.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Live value", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			auto BeginRow = [](const char* Name, const char* Type)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Name);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Type);
				ImGui::TableNextColumn();
				ImGui::PushID(Name);
			};
			auto EndRow = []()
			{
				ImGui::PopID();
			};
			auto BoolRow = [&](const char* Name, bool& Value)
			{
				BeginRow(Name, "bool");
				ImGui::Checkbox("##value", &Value);
				EndRow();
			};
			auto IntRow = [&](const char* Name, int& Value, int Min, int Max)
			{
				BeginRow(Name, "int");
				ImGui::SliderInt("##value", &Value, Min, Max);
				EndRow();
			};
			auto FloatRow = [&](const char* Name, float& Value, float Min, float Max, const char* Format)
			{
				BeginRow(Name, "float");
				ImGui::SliderFloat("##value", &Value, Min, Max, Format);
				EndRow();
			};
			auto ColorRow = [&](const char* Name, ImVec4& Value)
			{
				BeginRow(Name, "color4");
				ImGui::ColorEdit4("##value", &Value.x, ImGuiColorEditFlags_NoInputs);
				EndRow();
			};
			auto TextRow = [&](const char* Name, char* Value, size_t Size)
			{
				BeginRow(Name, "text");
				ImGui::InputText("##value", Value, Size);
				EndRow();
			};

			BoolRow("Enabled", gConfig.Enabled);
			BoolRow("DrawLines", gConfig.DrawLines);
			BoolRow("DrawBoxes", gConfig.DrawBoxes);
			BoolRow("DrawNames", gConfig.DrawNames);
			BoolRow("DrawDistance", gConfig.DrawDistance);
			BoolRow("DrawBounds", gConfig.DrawBounds);
			BoolRow("DrawCrosshair", gConfig.DrawCrosshair);
			BoolRow("DrawTargetPreview", gConfig.DrawTargetPreview);
			BoolRow("DrawSkeletons", gConfig.DrawSkeletons);
			BoolRow("DrawSkeletonBoneIds", gConfig.DrawSkeletonBoneIds);
			BoolRow("DrawSkeletonBoneNames", gConfig.DrawSkeletonBoneNames);
			BoolRow("UseReflectedPositionFallback", gConfig.UseReflectedPositionFallback);
			BoolRow("FastOverlayMode", gConfig.FastOverlayMode);
			BoolRow("ProbeReflectedPositionsOnLocatedActors", gConfig.ProbeReflectedPositionsOnLocatedActors);
			BoolRow("ThrottleLiveReflectionFallback", gConfig.ThrottleLiveReflectionFallback);
			BoolRow("CaptureOnRenderFrame", gConfig.CaptureOnRenderFrame);
			BoolRow("OnlyOnScreen", gConfig.OnlyOnScreen);
			BoolRow("OnlyWithLocation", gConfig.OnlyWithLocation);
			BoolRow("OnlyInView", gConfig.OnlyInView);
			BoolRow("HideEnvironmentActors", gConfig.HideEnvironmentActors);
			BoolRow("HideLocalPlayer", gConfig.HideLocalPlayer);
			BoolRow("LockLikelyPlayerClasses", gConfig.LockLikelyPlayerClasses);
			BoolRow("EnableDeveloperOptions", gConfig.EnableDeveloperOptions);
			BoolRow("DeveloperAutoCycleClasses", gConfig.DeveloperAutoCycleClasses);
			BoolRow("DeveloperPreviewEnabled", gConfig.DeveloperPreviewEnabled);
			BoolRow("DeveloperPreviewDrawLines", gConfig.DeveloperPreviewDrawLines);
			BoolRow("DeveloperPreviewDrawBoxes", gConfig.DeveloperPreviewDrawBoxes);
			IntRow("ActorSource", gConfig.ActorSource, 0, 3);
			IntRow("TargetMode", gConfig.TargetMode, 0, 6);
			IntRow("ProjectionRoute", gConfig.ProjectionRoute, 0, 2);
			IntRow("LikelyPlayerScoreThreshold", gConfig.LikelyPlayerScoreThreshold, 0, 100);
			IntRow("LikelyClassLockMinActors", gConfig.LikelyClassLockMinActors, 1, 32);
			IntRow("LikelyClassLockMaxClasses", gConfig.LikelyClassLockMaxClasses, 1, 16);
			IntRow("PositionProbeMaxFields", gConfig.PositionProbeMaxFields, 8, 500);
			IntRow("SkeletonMaxBones", gConfig.SkeletonMaxBones, 4, 256);
			IntRow("RefreshMs", gConfig.RefreshMs, 250, 10000);
			IntRow("FrameCaptureMinMs", gConfig.FrameCaptureMinMs, 0, 100);
			IntRow("FrameProjectionMaxActors", gConfig.FrameProjectionMaxActors, 16, 4096);
			IntRow("FrameSkeletonMinMs", gConfig.FrameSkeletonMinMs, 0, 1000);
			IntRow("ReflectedPositionRefreshMs", gConfig.ReflectedPositionRefreshMs, 0, 1000);
			IntRow("MaxActors", gConfig.MaxActors, 1, 4096);
			IntRow("DeveloperMaxRows", gConfig.DeveloperMaxRows, 10, 500);
			FloatRow("MaxDistanceMeters", gConfig.MaxDistanceMeters, 0.0f, 5000.0f, "%.0f");
			FloatRow("LineThickness", gConfig.LineThickness, 0.5f, 12.0f, "%.1f");
			FloatRow("BoxThickness", gConfig.BoxThickness, 0.5f, 12.0f, "%.1f");
			FloatRow("TargetPreviewRadius", gConfig.TargetPreviewRadius, 25.0f, 2000.0f, "%.0f");
			FloatRow("TargetPreviewLineThickness", gConfig.TargetPreviewLineThickness, 0.5f, 12.0f, "%.1f");
			FloatRow("SkeletonThickness", gConfig.SkeletonThickness, 0.5f, 12.0f, "%.1f");
			FloatRow("SkeletonPointRadius", gConfig.SkeletonPointRadius, 0.0f, 12.0f, "%.1f");
			FloatRow("ProjectionOffsetX", gConfig.ProjectionOffsetX, -4000.0f, 4000.0f, "%.1f");
			FloatRow("ProjectionOffsetY", gConfig.ProjectionOffsetY, -4000.0f, 4000.0f, "%.1f");
			FloatRow("ProjectionScaleX", gConfig.ProjectionScaleX, 0.10f, 4.0f, "%.3f");
			FloatRow("ProjectionScaleY", gConfig.ProjectionScaleY, 0.10f, 4.0f, "%.3f");
			ColorRow("TargetPreviewColor", gConfig.TargetPreviewColor);
			ColorRow("SkeletonColor", gConfig.SkeletonColor);
			TextRow("PlayerFilter", gConfig.PlayerFilter, sizeof(gConfig.PlayerFilter));
			TextRow("NonPlayerFilter", gConfig.NonPlayerFilter, sizeof(gConfig.NonPlayerFilter));
			TextRow("PositionFieldFilter", gConfig.PositionFieldFilter, sizeof(gConfig.PositionFieldFilter));
			TextRow("ClassFilter", gConfig.ClassFilter, sizeof(gConfig.ClassFilter));
			TextRow("DeveloperPreviewClassFilter", gConfig.DeveloperPreviewClassFilter, sizeof(gConfig.DeveloperPreviewClassFilter));
			TextRow("ExcludeFilter", gConfig.ExcludeFilter, sizeof(gConfig.ExcludeFilter));

			ImGui::EndTable();
		}

		ImGui::TreePop();
	}

	void DrawFeaturePlaceholderStateEditor()
	{
		if (!ImGui::TreeNode("Feature Placeholder Variables"))
			return;

		ImGui::TextWrapped("Kio-style global state bag for the Aim, ESP, Exploits, and Misc tabs. These placeholders are UI state only unless explicitly wired into the safe overlay renderer.");

		const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable;
		if (ImGui::BeginTable("##feature_placeholder_editor", 3, Flags, ImVec2(0.0f, 0.0f)))
		{
			ImGui::TableSetupColumn("Variable", ImGuiTableColumnFlags_WidthFixed, 220.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			auto BeginRow = [](const char* Name, const char* Type)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Name);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Type);
				ImGui::TableNextColumn();
				ImGui::PushID(Name);
			};
			auto EndRow = []()
			{
				ImGui::PopID();
			};
			auto BoolRow = [&](const char* Name, bool& Value)
			{
				BeginRow(Name, "bool");
				ImGui::Checkbox("##value", &Value);
				EndRow();
			};
			auto IntRow = [&](const char* Name, int& Value, int Min, int Max)
			{
				BeginRow(Name, "int");
				ImGui::SliderInt("##value", &Value, Min, Max);
				EndRow();
			};
			auto FloatRow = [&](const char* Name, float& Value, float Min, float Max, const char* Format)
			{
				BeginRow(Name, "float");
				ImGui::SliderFloat("##value", &Value, Min, Max, Format);
				EndRow();
			};
			auto ColorRow = [&](const char* Name, ImVec4& Value)
			{
				BeginRow(Name, "color4");
				ImGui::ColorEdit4("##value", &Value.x, ImGuiColorEditFlags_NoInputs);
				EndRow();
			};
			auto TextRow = [&](const char* Name, char* Value, size_t Size)
			{
				BeginRow(Name, "text");
				ImGui::InputText("##value", Value, Size);
				EndRow();
			};

			BoolRow("bEnableAimbot", gFeatureState.bEnableAimbot);
			BoolRow("bAimbotFovCheck", gFeatureState.bAimbotFovCheck);
			BoolRow("bRainbowAimbotTargetColor", gFeatureState.bRainbowAimbotTargetColor);
			FloatRow("fAimbotFov", gFeatureState.fAimbotFov, 0.1f, 800.0f, "%.1f");
			FloatRow("fAimbotSmoothness", gFeatureState.fAimbotSmoothness, 0.0f, 30.0f, "%.2f");
			FloatRow("fHeadPosOffset", gFeatureState.fHeadPosOffset, -10.0f, 30.0f, "%.1f");
			FloatRow("fFeetPosOffset", gFeatureState.fFeetPosOffset, -10.0f, 30.0f, "%.1f");
			IntRow("nAimTargetBone", gFeatureState.nAimTargetBone, 0, 128);
			IntRow("nAimActivationKey", gFeatureState.nAimActivationKey, 1, 255);
			ColorRow("cAimbotTargetColor", gFeatureState.cAimbotTargetColor);

			BoolRow("bPlayersSnapline", gFeatureState.bPlayersSnapline);
			BoolRow("bPlayersBox", gFeatureState.bPlayersBox);
			BoolRow("bPlayersBoxFilled", gFeatureState.bPlayersBoxFilled);
			BoolRow("bPlayersBox3D", gFeatureState.bPlayersBox3D);
			BoolRow("bPlayerSkeleton", gFeatureState.bPlayerSkeleton);
			BoolRow("bPlayersHealth", gFeatureState.bPlayersHealth);
			BoolRow("bBotChecker", gFeatureState.bBotChecker);
			BoolRow("bPlayerChams", gFeatureState.bPlayerChams);
			IntRow("nPlayersSnaplineType", gFeatureState.nPlayersSnaplineType, 0, 2);
			IntRow("nBoxStyle", gFeatureState.nBoxStyle, 0, 3);
			FloatRow("fBoxThickness", gFeatureState.fBoxThickness, 0.5f, 12.0f, "%.1f");
			FloatRow("fSnaplineThickness", gFeatureState.fSnaplineThickness, 0.5f, 12.0f, "%.1f");
			FloatRow("fSkeletonThickness", gFeatureState.fSkeletonThickness, 0.5f, 12.0f, "%.1f");
			ColorRow("cPlayersSnaplineColor", gFeatureState.cPlayersSnaplineColor);
			ColorRow("cPlayersBoxColor", gFeatureState.cPlayersBoxColor);
			ColorRow("cPlayerSkeletonColor", gFeatureState.cPlayerSkeletonColor);
			ColorRow("cTargetNotVisibleColor", gFeatureState.cTargetNotVisibleColor);

			BoolRow("bGodMode", gFeatureState.bGodMode);
			BoolRow("bNoClip", gFeatureState.bNoClip);
			BoolRow("bFly", gFeatureState.bFly);
			BoolRow("bNoGravity", gFeatureState.bNoGravity);
			BoolRow("bTimeScaleChanger", gFeatureState.bTimeScaleChanger);
			BoolRow("bSpeedHack", gFeatureState.bSpeedHack);
			BoolRow("bNoRecoil", gFeatureState.bNoRecoil);
			BoolRow("bNoSpread", gFeatureState.bNoSpread);
			BoolRow("bRapidFire", gFeatureState.bRapidFire);
			BoolRow("bOneShot", gFeatureState.bOneShot);
			BoolRow("bInfiniteAmmo", gFeatureState.bInfiniteAmmo);
			BoolRow("bKillAll", gFeatureState.bKillAll);
			FloatRow("fTimeScale", gFeatureState.fTimeScale, 0.1f, 10000.0f, "%.2f");
			FloatRow("fSpeedValue", gFeatureState.fSpeedValue, 0.1f, 10000.0f, "%.2f");
			FloatRow("fProjectileScale", gFeatureState.fProjectileScale, 0.1f, 50.0f, "%.2f");
			IntRow("nExploitProfile", gFeatureState.nExploitProfile, 0, 3);

			BoolRow("bWatermark", gFeatureState.bWatermark);
			BoolRow("bShowMouse", gFeatureState.bShowMouse);
			BoolRow("bRainbowMouse", gFeatureState.bRainbowMouse);
			BoolRow("bCrosshair", gFeatureState.bCrosshair);
			BoolRow("bRainbowCrosshair", gFeatureState.bRainbowCrosshair);
			BoolRow("bCameraFovChanger", gFeatureState.bCameraFovChanger);
			BoolRow("bShowInspector", gFeatureState.bShowInspector);
			BoolRow("bUpdateTargets", gFeatureState.bUpdateTargets);
			BoolRow("bUpdateTargetsInDifferentThread", gFeatureState.bUpdateTargetsInDifferentThread);
			FloatRow("fCrosshairSize", gFeatureState.fCrosshairSize, 0.1f, 40.0f, "%.1f");
			FloatRow("fCameraCustomFov", gFeatureState.fCameraCustomFov, 0.1f, 300.0f, "%.1f");
			IntRow("nMouseType", gFeatureState.nMouseType, 0, 1);
			IntRow("nCrosshairType", gFeatureState.nCrosshairType, 0, 1);
			IntRow("nTargetFetch", gFeatureState.nTargetFetch, 0, 2);
			ColorRow("cMouseColor", gFeatureState.cMouseColor);
			ColorRow("cCrosshairColor", gFeatureState.cCrosshairColor);
			TextRow("szAimTargetGroup", gFeatureState.szAimTargetGroup, sizeof(gFeatureState.szAimTargetGroup));
			TextRow("szEspFilterProfile", gFeatureState.szEspFilterProfile, sizeof(gFeatureState.szEspFilterProfile));
			TextRow("szMiscProfileName", gFeatureState.szMiscProfileName, sizeof(gFeatureState.szMiscProfileName));
			TextRow("szDeveloperNote", gFeatureState.szDeveloperNote, sizeof(gFeatureState.szDeveloperNote));

			ImGui::EndTable();
		}

		ImGui::TreePop();
	}

	void DrawOverlaySettingsPanel()
	{
		if (ImGui::Button("Save settings"))
			SaveOverlayConfig();
		ImGui::SameLine();
		if (ImGui::Button("Reload settings"))
			LoadOverlayConfig();
		ImGui::SameLine();
		if (ImGui::Button("Reset projection auto"))
		{
			std::scoped_lock Lock(gConfigMutex);
			gConfig.ProjectionRoute = 0;
			gConfig.ProjectionSpace = 0;
			gConfig.ProjectionOffsetX = 0.0f;
			gConfig.ProjectionOffsetY = 0.0f;
			gConfig.ProjectionScaleX = 1.0f;
			gConfig.ProjectionScaleY = 1.0f;
		}

		std::scoped_lock Lock(gConfigMutex);
		ImGui::Checkbox("Enabled", &gConfig.Enabled);
		ImGui::SameLine();
		ImGui::Checkbox("Only on screen", &gConfig.OnlyOnScreen);
		ImGui::SameLine();
		ImGui::Checkbox("Require location", &gConfig.OnlyWithLocation);
		ImGui::SameLine();
		ImGui::Checkbox("Only in view", &gConfig.OnlyInView);

		if (ImGui::CollapsingHeader("Drawing", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Lines", &gConfig.DrawLines);
			ImGui::SameLine();
			ImGui::Checkbox("Boxes", &gConfig.DrawBoxes);
			ImGui::SameLine();
			ImGui::Checkbox("Names", &gConfig.DrawNames);
			ImGui::SameLine();
			ImGui::Checkbox("Distance", &gConfig.DrawDistance);
			ImGui::Checkbox("Skeletons", &gConfig.DrawSkeletons);
			ImGui::SameLine();
			ImGui::Checkbox("Bone IDs", &gConfig.DrawSkeletonBoneIds);
			ImGui::SameLine();
			ImGui::Checkbox("Bone names", &gConfig.DrawSkeletonBoneNames);
			ImGui::Checkbox("Bounds", &gConfig.DrawBounds);
			ImGui::SameLine();
			ImGui::Checkbox("Actor center dots", &gConfig.DrawCenterDot);
			ImGui::SameLine();
			ImGui::Checkbox("Crosshair", &gConfig.DrawCrosshair);
			ImGui::SameLine();
			ImGui::Checkbox("Target preview", &gConfig.DrawTargetPreview);

			ImGui::Combo("Line origin", &gConfig.LineOrigin, "Top\0Center\0Bottom\0");
			ImGui::Combo("Line target", &gConfig.LineTarget, "Actor location\0Box center\0Box bottom\0");
			ImGui::SliderFloat("Line thickness", &gConfig.LineThickness, 0.5f, 6.0f, "%.1f");
			ImGui::SliderFloat("Box thickness", &gConfig.BoxThickness, 0.5f, 6.0f, "%.1f");
			ImGui::SliderFloat("Skeleton thickness", &gConfig.SkeletonThickness, 0.5f, 6.0f, "%.1f");
			ImGui::SliderFloat("Skeleton point radius", &gConfig.SkeletonPointRadius, 0.0f, 8.0f, "%.1f");
			ImGui::SliderInt("Skeleton max bones", &gConfig.SkeletonMaxBones, 4, 128);
		}

		if (ImGui::CollapsingHeader("Capture And Projection", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Fast overlay mode", &gConfig.FastOverlayMode);
			ImGui::SameLine();
			ImGui::Checkbox("Throttle reflection fallback", &gConfig.ThrottleLiveReflectionFallback);
			ImGui::Checkbox("Probe reflected positions on located actors", &gConfig.ProbeReflectedPositionsOnLocatedActors);
			ImGui::SliderInt("Reflection refresh ms", &gConfig.ReflectedPositionRefreshMs, 0, 250);
			ImGui::Combo("Renderer route", &gConfig.RendererRoute, "Auto\0Internal only\0External only\0");
			ImGui::Combo("Actor source", &gConfig.ActorSource, "Auto\0World levels\0GObjects\0World + GObjects\0");
			ImGui::Combo("Projection route", &gConfig.ProjectionRoute, "Auto\0Native only\0Fallback only\0");
			ImGui::Combo("Projection space", &gConfig.ProjectionSpace, "Auto\0Viewport\0Desktop\0");
			ImGui::Checkbox("Projection fallback", &gConfig.UseProjectionFallback);
			ImGui::SameLine();
			ImGui::Checkbox("Reflected position fallback", &gConfig.UseReflectedPositionFallback);
			gConfig.CaptureOnRenderFrame = false;
			ImGui::BeginDisabled(true);
			ImGui::Checkbox("Frame-synced capture", &gConfig.CaptureOnRenderFrame);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("render path is cache-only");
			ImGui::SliderInt("Frame min ms", &gConfig.FrameCaptureMinMs, 0, 100);
			ImGui::SliderInt("Frame actor cap", &gConfig.FrameProjectionMaxActors, 16, 512);
			ImGui::SliderInt("Skeleton refresh ms", &gConfig.FrameSkeletonMinMs, 750, 2000);
			ImGui::SliderInt("Position probe fields", &gConfig.PositionProbeMaxFields, 8, 240);
			ImGui::InputTextWithHint("Position fields", "field tokens used to infer live actor position", gConfig.PositionFieldFilter, sizeof(gConfig.PositionFieldFilter));
			ImGui::Combo("Bounds source", &gConfig.BoundsMode, "Auto\0Actor\0Root component\0Fallback\0");
			ImGui::SliderInt("Discovery refresh ms", &gConfig.RefreshMs, 250, 3000);
			ImGui::SliderInt("Max actors", &gConfig.MaxActors, 1, 2048);
			ImGui::SliderFloat("Max distance meters", &gConfig.MaxDistanceMeters, 0.0f, 5000.0f, gConfig.MaxDistanceMeters <= 0.0f ? "disabled" : "%.0f");
		}

		if (ImGui::CollapsingHeader("Sizing"))
		{
			ImGui::SliderFloat("Box width ratio", &gConfig.BoxWidthRatio, 0.15f, 1.00f, "%.2f");
			ImGui::Checkbox("Clamp large boxes", &gConfig.ClampLargeBoxes);
			ImGui::SameLine();
			ImGui::SliderFloat("Max screen box", &gConfig.MaxBoxScreenFraction, 0.20f, 1.00f, "%.2fx");
			ImGui::SliderFloat("Box padding", &gConfig.BoxPaddingPixels, 0.0f, 20.0f, "%.0f px");
			ImGui::SliderFloat("Min box height", &gConfig.MinBoxHeightPixels, 0.0f, 80.0f, "%.0f px");
			ImGui::SliderFloat("Fallback half height", &gConfig.FallbackHalfHeight, 10.0f, 250.0f, "%.0f");
			ImGui::SliderFloat("Fallback half width", &gConfig.FallbackHalfWidth, 5.0f, 160.0f, "%.0f");
			ImGui::SliderFloat("Crosshair size", &gConfig.CrosshairSize, 1.0f, 40.0f, "%.0f px");
			ImGui::SliderFloat("Crosshair gap", &gConfig.CrosshairGap, 0.0f, 25.0f, "%.0f px");
			ImGui::SliderFloat("Crosshair thickness", &gConfig.CrosshairThickness, 0.5f, 6.0f, "%.1f");
			ImGui::SliderFloat("Target preview radius", &gConfig.TargetPreviewRadius, 25.0f, 1000.0f, "%.0f px");
			ImGui::SliderFloat("Target preview line", &gConfig.TargetPreviewLineThickness, 0.5f, 6.0f, "%.1f");
		}

		if (ImGui::CollapsingHeader("Filtering", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Combo("Target mode", &gConfig.TargetMode, "All actors\0Likely players\0Pawn / Character\0Bots\0NPC / AI\0Civilians\0Custom filters\0");
			ImGui::Checkbox("Lock likely classes", &gConfig.LockLikelyPlayerClasses);
			ImGui::SameLine();
			ImGui::SliderInt("Class lock max", &gConfig.LikelyClassLockMaxClasses, 1, 8);
			ImGui::Checkbox("Hide environment", &gConfig.HideEnvironmentActors);
			ImGui::SameLine();
			ImGui::Checkbox("Hide local player", &gConfig.HideLocalPlayer);
			ImGui::Checkbox("Class filter", &gConfig.EnableClassFilter);
			ImGui::SameLine();
			if (ImGui::Button("Clear class filter"))
			{
				gConfig.EnableClassFilter = false;
				gConfig.ClassFilter[0] = '\0';
			}
			ImGui::InputTextWithHint("Include filters", "comma-separated actor, class, or path tokens", gConfig.Filter, sizeof(gConfig.Filter));
			ImGui::InputTextWithHint("Exclude filters", "comma-separated actor, class, or path tokens", gConfig.ExcludeFilter, sizeof(gConfig.ExcludeFilter));
			ImGui::InputTextWithHint("Class include", "comma-separated class tokens", gConfig.ClassFilter, sizeof(gConfig.ClassFilter));
			ImGui::InputTextWithHint("Class exclude", "comma-separated class tokens", gConfig.ClassExcludeFilter, sizeof(gConfig.ClassExcludeFilter));
			if (ImGui::TreeNode("Category filters"))
			{
				ImGui::Checkbox("Hide bots", &gConfig.HideBots);
				ImGui::SameLine();
				ImGui::Checkbox("Hide NPCs", &gConfig.HideNPCs);
				ImGui::SameLine();
				ImGui::Checkbox("Hide civilians", &gConfig.HideCivilians);
				ImGui::SameLine();
				ImGui::Checkbox("Hide AI", &gConfig.HideAI);
				ImGui::Checkbox("Hide cameras", &gConfig.HideCameras);
				ImGui::SameLine();
				ImGui::Checkbox("Hide items", &gConfig.HideItems);
				ImGui::SameLine();
				ImGui::Checkbox("Hide weapons", &gConfig.HideWeapons);
				ImGui::SameLine();
				ImGui::Checkbox("Hide vehicles", &gConfig.HideVehicles);
				ImGui::SameLine();
				ImGui::Checkbox("Hide objectives", &gConfig.HideObjectives);
				ImGui::TreePop();
			}
		}

		if (ImGui::CollapsingHeader("Colors"))
		{
			ImGui::ColorEdit4("Box", &gConfig.BoxColor.x, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Line", &gConfig.LineColor.x, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Skeleton", &gConfig.SkeletonColor.x, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Text", &gConfig.TextColor.x, ImGuiColorEditFlags_NoInputs);
			ImGui::ColorEdit4("Bounds", &gConfig.BoundsColor.x, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Crosshair", &gConfig.CrosshairColor.x, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Target", &gConfig.TargetPreviewColor.x, ImGuiColorEditFlags_NoInputs);
		}

		if (ImGui::CollapsingHeader("Manual Projection Calibration"))
		{
			ImGui::TextDisabled("For diagnostics only. Auto projection should handle normal cases.");
			ImGui::DragFloat("Projection offset X", &gConfig.ProjectionOffsetX, 0.25f, -4000.0f, 4000.0f, "%.1f px");
			ImGui::DragFloat("Projection offset Y", &gConfig.ProjectionOffsetY, 0.25f, -4000.0f, 4000.0f, "%.1f px");
			ImGui::DragFloat("Projection scale X", &gConfig.ProjectionScaleX, 0.001f, 0.10f, 4.0f, "%.3f");
			ImGui::DragFloat("Projection scale Y", &gConfig.ProjectionScaleY, 0.001f, 0.10f, 4.0f, "%.3f");
		}
	}

	void DrawMenu(const std::vector<ActorDebugInfo>& Actors, const std::vector<ActorDebugInfo>& FilteredActors, const CaptureStats& Stats)
	{
		if (!gMenuOpen)
			return;

		ImGui::SetNextWindowSize(ImVec2(780.0f, 560.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Dumper-7 Debug Overlay", &gMenuOpen, ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		static std::vector<ActorClassSummary> CachedClassSummaries;
		static DWORD LastClassSummaryBuildTick = 0;
		static DWORD LastClassSummaryCaptureTick = 0;
		static size_t LastClassSummaryActorCount = 0;
		static size_t LastClassSummaryFilteredCount = 0;
		const OverlayConfig MenuConfigSnapshot = GetConfigSnapshot();
		const DWORD Now = GetTickCount();
		const DWORD ClassSummaryRefreshMs = MenuConfigSnapshot.FastOverlayMode ? 500u : 0u;
		const bool bClassSummaryStale = CachedClassSummaries.empty()
			|| Stats.LastCaptureTick != LastClassSummaryCaptureTick
			|| Actors.size() != LastClassSummaryActorCount
			|| FilteredActors.size() != LastClassSummaryFilteredCount
			|| ClassSummaryRefreshMs == 0
			|| Now - LastClassSummaryBuildTick >= ClassSummaryRefreshMs;
		if (bClassSummaryStale)
		{
			CachedClassSummaries = BuildActorClassSummaries(Actors, FilteredActors);
			LastClassSummaryBuildTick = Now;
			LastClassSummaryCaptureTick = Stats.LastCaptureTick;
			LastClassSummaryActorCount = Actors.size();
			LastClassSummaryFilteredCount = FilteredActors.size();
		}
		const std::vector<ActorClassSummary>& ClassSummaries = CachedClassSummaries;

		if (ImGui::BeginTabBar("##tabs"))
		{
			if (ImGui::BeginTabItem("Aim"))
			{
				ImGui::SeparatorText("Aim");
				ImGui::TextDisabled("UI-only placeholder state. Target diagnostics can be previewed, but no gameplay automation is wired here.");
				ImGui::InputTextWithHint("Target group", "label only", gFeatureState.szAimTargetGroup, sizeof(gFeatureState.szAimTargetGroup));
				ImGui::Checkbox("Enable Aimbot", &gFeatureState.bEnableAimbot);
				ImGui::SameLine();
				ImGui::Checkbox("FOV Check", &gFeatureState.bAimbotFovCheck);
				ImGui::SliderFloat("Aimbot FOV", &gFeatureState.fAimbotFov, 0.1f, 800.0f, "%.1f");
				ImGui::SliderFloat("Smoothness", &gFeatureState.fAimbotSmoothness, 0.0f, 30.0f, "%.2f");
				ImGui::SliderFloat("Head offset", &gFeatureState.fHeadPosOffset, -10.0f, 30.0f, "%.1f");
				ImGui::SliderFloat("Feet offset", &gFeatureState.fFeetPosOffset, -10.0f, 30.0f, "%.1f");
				ImGui::SliderInt("Target bone preset", &gFeatureState.nAimTargetBone, 0, 8);
				ImGui::SliderInt("Activation key", &gFeatureState.nAimActivationKey, 1, 255);
				ImGui::ColorEdit4("Target color", &gFeatureState.cAimbotTargetColor.x, ImGuiColorEditFlags_NoInputs);
				ImGui::SameLine();
				ImGui::Checkbox("Rainbow target", &gFeatureState.bRainbowAimbotTargetColor);
				ImGui::SeparatorText("Safe Preview Overlay");
				ImGui::Checkbox("Crosshair", &gConfig.DrawCrosshair);
				gFeatureState.bCrosshair = gConfig.DrawCrosshair;
				ImGui::SameLine();
				ImGui::Checkbox("Target preview", &gConfig.DrawTargetPreview);
				gFeatureState.bTargetPreview = gConfig.DrawTargetPreview;
				ImGui::SliderFloat("Preview radius", &gConfig.TargetPreviewRadius, 25.0f, 1000.0f, "%.0f px");
				ImGui::SliderFloat("Preview line", &gConfig.TargetPreviewLineThickness, 0.5f, 6.0f, "%.1f");
				ImGui::SeparatorText("Current Runtime Context");
				ImGui::Text("Controller: %s  Camera: %s  Local pawn: %s",
					Stats.HasPlayerController ? "yes" : "no",
					Stats.HasCameraLocation ? "yes" : "no",
					Stats.HasLocalPawn ? "yes" : "no");
				ImGui::TextWrapped("Camera sources: location=%s  rotation=%s",
					Stats.CameraLocationSource.c_str(),
					Stats.CameraRotationSource.c_str());
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("ESP"))
			{
				DrawOverlaySettingsPanel();
				ImGui::SeparatorText("Main Runtime ESP");
				ImGui::InputTextWithHint("ESP profile", "label only", gFeatureState.szEspFilterProfile, sizeof(gFeatureState.szEspFilterProfile));
				ImGui::Checkbox("Players Snapline", &gConfig.DrawLines);
				gFeatureState.bPlayersSnapline = gConfig.DrawLines;
				ImGui::SameLine();
				ImGui::ColorEdit4("##PlayersSnaplineColor", &gConfig.LineColor.x, ImGuiColorEditFlags_NoInputs);
				gFeatureState.cPlayersSnaplineColor = gConfig.LineColor;
				ImGui::SameLine();
				ImGui::Checkbox("Rainbow snapline", &gFeatureState.bRainbowPlayersSnapline);
				ImGui::SliderInt("Snapline type", &gFeatureState.nPlayersSnaplineType, 0, 2);
				ImGui::Checkbox("Players Box", &gConfig.DrawBoxes);
				gFeatureState.bPlayersBox = gConfig.DrawBoxes;
				ImGui::SameLine();
				ImGui::ColorEdit4("##PlayersBoxColor", &gConfig.BoxColor.x, ImGuiColorEditFlags_NoInputs);
				gFeatureState.cPlayersBoxColor = gConfig.BoxColor;
				ImGui::SameLine();
				ImGui::Checkbox("Box filled", &gFeatureState.bPlayersBoxFilled);
				ImGui::Checkbox("Players Box 3D", &gFeatureState.bPlayersBox3D);
				ImGui::SameLine();
				ImGui::SliderInt("Box style", &gFeatureState.nBoxStyle, 0, 3);
				ImGui::Checkbox("Players Skeleton", &gConfig.DrawSkeletons);
				gFeatureState.bPlayerSkeleton = gConfig.DrawSkeletons;
				ImGui::SameLine();
				ImGui::ColorEdit4("##PlayerSkeletonColor", &gConfig.SkeletonColor.x, ImGuiColorEditFlags_NoInputs);
				gFeatureState.cPlayerSkeletonColor = gConfig.SkeletonColor;
				ImGui::SameLine();
				ImGui::Checkbox("Rainbow skeleton", &gFeatureState.bRainbowPlayerSkeleton);
				ImGui::Checkbox("Players Health", &gFeatureState.bPlayersHealth);
				ImGui::SameLine();
				ImGui::Checkbox("Bot checker", &gFeatureState.bBotChecker);
				ImGui::SameLine();
				ImGui::Checkbox("Bot text", &gFeatureState.bBotCheckerText);
				ImGui::Checkbox("Names", &gConfig.DrawNames);
				gFeatureState.bShowNames = gConfig.DrawNames;
				ImGui::SameLine();
				ImGui::Checkbox("Distance", &gConfig.DrawDistance);
				gFeatureState.bShowDistance = gConfig.DrawDistance;
				ImGui::SliderFloat("Snapline thickness", &gConfig.LineThickness, 0.5f, 6.0f, "%.1f");
				gFeatureState.fSnaplineThickness = gConfig.LineThickness;
				ImGui::SliderFloat("Box thickness", &gConfig.BoxThickness, 0.5f, 6.0f, "%.1f");
				gFeatureState.fBoxThickness = gConfig.BoxThickness;
				ImGui::SliderFloat("Skeleton thickness", &gConfig.SkeletonThickness, 0.5f, 6.0f, "%.1f");
				gFeatureState.fSkeletonThickness = gConfig.SkeletonThickness;
				ImGui::Text("Captured: %d  Candidates: %d  Scanned: %d  Objects: %d",
					Stats.CapturedActors, Stats.ActorCandidates, Stats.ScannedObjects, Stats.ObjectCount);
				{
					const DWORD Now = GetTickCount();
					const DWORD LastCapture = Stats.LastCaptureTick;
					const DWORD LastFrameCapture = gLastRenderFrameCaptureTick.load();
					ImGui::Text("Discovery age: %lu ms  Frame projections: %llu  skipped: %llu  frame age: %lu ms",
						LastCapture ? static_cast<unsigned long>(Now - LastCapture) : 0ul,
						static_cast<unsigned long long>(gRenderFrameCapturePasses.load()),
						static_cast<unsigned long long>(gRenderFrameCaptureSkips.load()),
						LastFrameCapture ? static_cast<unsigned long>(Now - LastFrameCapture) : 0ul);
				}
				ImGui::Text("Located: %d  Projected: %d  Boxes: %d  Projection failures: %d",
					Stats.LocatedActors, Stats.ProjectedActors, Stats.BoxedActors, Stats.ProjectionFailures);
				ImGui::Text("Skeletons: %d actors  %d bones  %d lines",
					Stats.SkeletonActors, Stats.SkeletonBones, Stats.SkeletonSegments);
				ImGui::Text("Projection candidate attempts: %d  Reflected position hits: %d",
					Stats.ProjectionCandidateAttempts, Stats.ReflectedPositionHits);
				ImGui::Text("Live processed: %d  Position cache: %d hit / %d miss",
					Stats.FrameProcessedActors,
					Stats.PositionProbeCacheHits,
					Stats.PositionProbeCacheMisses);
				ImGui::Text("Projection route: %s  Native %d/%d  Fallback %d/%d",
					ProjectionRouteName(GetConfigSnapshot().ProjectionRoute),
					Stats.NativeProjectionSuccesses,
					Stats.NativeProjectionAttempts,
					Stats.FallbackProjectionSuccesses,
					Stats.FallbackProjectionAttempts);
				ImGui::TextWrapped("Likely class lock: %s", Stats.LikelyClassLock.c_str());
				ImGui::Text("In view: %d  Filtered: %d  Environment filtered: %d",
					Stats.InViewActors, Stats.FilteredActors, Stats.FilteredEnvironment);
				ImGui::Text("Bot: %d  NPC: %d  Civilian: %d  AI: %d  Camera: %d",
					Stats.BotActors, Stats.NpcActors, Stats.CivilianActors, Stats.AiActors, Stats.CameraActors);
				ImGui::Text("Item: %d  Weapon: %d  Vehicle: %d  Objective: %d",
					Stats.ItemActors, Stats.WeaponActors, Stats.VehicleActors, Stats.ObjectiveActors);
				ImGui::Text("World bounds: %d  Bounds source: %s",
					Stats.BoundedActors, BoundsModeName(GetConfigSnapshot().BoundsMode));
				ImGui::Text("Source: %s  World: %s  Levels: %d  Level actor slots: %d",
					Stats.ActorSource.c_str(),
					Stats.HasWorld ? "yes" : "no",
					Stats.LevelCount,
					Stats.LevelActorSlots);
				ImGui::Text("Controller: %s  Camera: %s  Projection: %s",
					Stats.HasPlayerController ? "yes" : "no",
					Stats.HasCameraLocation ? "yes" : "no",
					Stats.HasProjection ? "native" : (Stats.UsedProjectionFallback ? "fallback" : "no"));
				ImGui::Text("Camera rotation: %s  FOV: %s  Input: %s",
					Stats.HasCameraRotation ? "yes" : "no",
					Stats.HasCameraFov ? "yes" : "default",
					ExternalShouldPassThrough() ? "pass-through" : "menu");
				ImGui::TextWrapped("Camera sources: location=%s  rotation=%s",
					Stats.CameraLocationSource.c_str(),
					Stats.CameraRotationSource.c_str());
				ImGui::Text("Projection space: %s%s",
					ProjectionSpaceName(GetConfigSnapshot().ProjectionSpace),
					Stats.UsedDesktopProjection ? " (desktop adjusted)" : "");
				ImGui::Text("Projection rect: %ld,%ld -> %ld,%ld  %.0fx%.0f",
					Stats.ProjectionLeft,
					Stats.ProjectionTop,
					Stats.ProjectionRight,
					Stats.ProjectionBottom,
					Stats.ProjectionWidth,
					Stats.ProjectionHeight);
				{
					const OverlayConfig ProjectionConfig = GetConfigSnapshot();
					ImGui::Text("Calibration: offset %.1f, %.1f  scale %.3f, %.3f",
						ProjectionConfig.ProjectionOffsetX,
						ProjectionConfig.ProjectionOffsetY,
						ProjectionConfig.ProjectionScaleX,
						ProjectionConfig.ProjectionScaleY);
				}
				DrawActorTable(Actors);
				DrawSelectedActor(Actors, FilteredActors);
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Exploits"))
			{
				ImGui::SeparatorText("Exploits");
				ImGui::TextDisabled("UI-only placeholder state. These values are stored for layout/config work and are not connected to runtime memory writes.");
				ImGui::Combo("Exploit profile", &gFeatureState.nExploitProfile, "Disabled\0QA Sandbox\0Movement Lab\0Weapon Lab\0");
				ImGui::Checkbox("GodMode", &gFeatureState.bGodMode);
				ImGui::SameLine();
				ImGui::Checkbox("NoClip", &gFeatureState.bNoClip);
				ImGui::SameLine();
				ImGui::Checkbox("Fly", &gFeatureState.bFly);
				ImGui::SameLine();
				ImGui::Checkbox("NoGravity", &gFeatureState.bNoGravity);
				ImGui::Checkbox("Time Dilation", &gFeatureState.bTimeScaleChanger);
				ImGui::SameLine();
				ImGui::SliderFloat("Time scale", &gFeatureState.fTimeScale, 0.1f, 10000.0f, "%.2f");
				if (ImGui::Button("Reset time scale"))
					gFeatureState.fTimeScale = 1.0f;
				ImGui::Checkbox("Speed Hack", &gFeatureState.bSpeedHack);
				ImGui::SameLine();
				ImGui::SliderFloat("Speed value", &gFeatureState.fSpeedValue, 0.1f, 10000.0f, "%.2f");
				ImGui::Checkbox("No Recoil", &gFeatureState.bNoRecoil);
				ImGui::SameLine();
				ImGui::Checkbox("No Spread", &gFeatureState.bNoSpread);
				ImGui::SameLine();
				ImGui::Checkbox("Rapid Fire", &gFeatureState.bRapidFire);
				ImGui::Checkbox("One Shot", &gFeatureState.bOneShot);
				ImGui::SameLine();
				ImGui::Checkbox("Infinite Ammo", &gFeatureState.bInfiniteAmmo);
				ImGui::SameLine();
				ImGui::Checkbox("Kill All", &gFeatureState.bKillAll);
				ImGui::SliderFloat("Projectile scale", &gFeatureState.fProjectileScale, 0.1f, 50.0f, "%.2f");
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Settings"))
			{
				OverlayConfig ConfigSnapshot = GetConfigSnapshot();
				ImGui::Text("Captured: %d  Filtered: %d  Filter cache: %d",
					Stats.CapturedActors, Stats.FilteredActors, static_cast<int>(FilteredActors.size()));
				{
					const DWORD Now = GetTickCount();
					const DWORD LastCapture = Stats.LastCaptureTick;
					const DWORD LastFrameCapture = gLastRenderFrameCaptureTick.load();
					ImGui::Text("Live projection: %s  discovery age %lu ms  frame age %lu ms  cap %d",
						ConfigSnapshot.CaptureOnRenderFrame ? "frame-synced" : "background",
						LastCapture ? static_cast<unsigned long>(Now - LastCapture) : 0ul,
						LastFrameCapture ? static_cast<unsigned long>(Now - LastFrameCapture) : 0ul,
						ConfigSnapshot.FrameProjectionMaxActors);
				}
				ImGui::Text("Source: %s  Objects: %d  Candidates: %d",
					Stats.ActorSource.c_str(), Stats.ObjectCount, Stats.ActorCandidates);
				ImGui::Text("Projection route: %s  Native %d/%d  Fallback %d/%d",
					ProjectionRouteName(ConfigSnapshot.ProjectionRoute),
					Stats.NativeProjectionSuccesses,
					Stats.NativeProjectionAttempts,
					Stats.FallbackProjectionSuccesses,
					Stats.FallbackProjectionAttempts);
				ImGui::TextWrapped("Camera sources: location=%s  rotation=%s",
					Stats.CameraLocationSource.c_str(),
					Stats.CameraRotationSource.c_str());

				{
					std::scoped_lock Lock(gConfigMutex);
					ImGui::Checkbox("Enable Developer Options", &gConfig.EnableDeveloperOptions);
					ImGui::SameLine();
					ImGui::SliderInt("Max dev rows", &gConfig.DeveloperMaxRows, 10, 500);
					ImGui::InputTextWithHint("Probe filter", "actor, class, or path tokens", gConfig.DeveloperProbeFilter, sizeof(gConfig.DeveloperProbeFilter));

					ImGui::SeparatorText("Developer Class Preview");
					ImGui::Checkbox("Enable dev preview", &gConfig.DeveloperPreviewEnabled);
					ImGui::SameLine();
					ImGui::Checkbox("Auto-cycle classes", &gConfig.DeveloperAutoCycleClasses);
					ImGui::SameLine();
					ImGui::SliderInt("Cycle ms", &gConfig.DeveloperClassCycleMs, 250, 5000);
					ImGui::Checkbox("Preview lines", &gConfig.DeveloperPreviewDrawLines);
					ImGui::SameLine();
					ImGui::Checkbox("Preview boxes", &gConfig.DeveloperPreviewDrawBoxes);
					if (ImGui::Button("Clear dev preview"))
					{
						gConfig.DeveloperAutoCycleClasses = false;
						gConfig.DeveloperPreviewClassFilter[0] = '\0';
					}
					ImGui::InputTextWithHint("Preview class", "click a class below or type class tokens", gConfig.DeveloperPreviewClassFilter, sizeof(gConfig.DeveloperPreviewClassFilter));
					ImGui::SeparatorText("Main Class Filter");
					ImGui::Checkbox("Enable main class filter", &gConfig.EnableClassFilter);
					ImGui::SameLine();
					if (ImGui::Button("Clear main class filter"))
					{
						gConfig.EnableClassFilter = false;
						gConfig.ClassFilter[0] = '\0';
					}
					ImGui::InputTextWithHint("Main class", "main ESP include class tokens", gConfig.ClassFilter, sizeof(gConfig.ClassFilter));
					ImGui::InputTextWithHint("Excluded classes", "comma-separated class tokens to never draw", gConfig.ClassExcludeFilter, sizeof(gConfig.ClassExcludeFilter));

					ImGui::SeparatorText("Environment Classifier");
					ImGui::Checkbox("Hide environment", &gConfig.HideEnvironmentActors);
					ImGui::SameLine();
					if (ImGui::Button("Reset environment tokens"))
						std::snprintf(gConfig.EnvironmentFilter, sizeof(gConfig.EnvironmentFilter), "%s", DefaultEnvironmentTokens());
					ImGui::InputTextMultiline("Environment tokens", gConfig.EnvironmentFilter, sizeof(gConfig.EnvironmentFilter),
						ImVec2(0.0f, ImGui::GetTextLineHeight() * 4.0f));

					ImGui::SeparatorText("Actor Classifiers");
					ImGui::Checkbox("Hide bots", &gConfig.HideBots);
					ImGui::SameLine();
					ImGui::Checkbox("Hide NPCs", &gConfig.HideNPCs);
					ImGui::SameLine();
					ImGui::Checkbox("Hide civilians", &gConfig.HideCivilians);
					ImGui::SameLine();
					ImGui::Checkbox("Hide AI", &gConfig.HideAI);
					ImGui::Checkbox("Hide cameras", &gConfig.HideCameras);
					ImGui::SameLine();
					ImGui::Checkbox("Hide items", &gConfig.HideItems);
					ImGui::SameLine();
					ImGui::Checkbox("Hide weapons", &gConfig.HideWeapons);
					ImGui::SameLine();
					ImGui::Checkbox("Hide vehicles", &gConfig.HideVehicles);
					ImGui::SameLine();
					ImGui::Checkbox("Hide objectives", &gConfig.HideObjectives);
					if (ImGui::Button("Reset actor classifier tokens"))
					{
						std::snprintf(gConfig.BotFilter, sizeof(gConfig.BotFilter), "%s", DefaultBotTokens());
						std::snprintf(gConfig.NpcFilter, sizeof(gConfig.NpcFilter), "%s", DefaultNpcTokens());
						std::snprintf(gConfig.CivilianFilter, sizeof(gConfig.CivilianFilter), "%s", DefaultCivilianTokens());
						std::snprintf(gConfig.AiFilter, sizeof(gConfig.AiFilter), "%s", DefaultAiTokens());
						std::snprintf(gConfig.CameraFilter, sizeof(gConfig.CameraFilter), "%s", DefaultCameraTokens());
						std::snprintf(gConfig.ItemFilter, sizeof(gConfig.ItemFilter), "%s", DefaultItemTokens());
						std::snprintf(gConfig.WeaponFilter, sizeof(gConfig.WeaponFilter), "%s", DefaultWeaponTokens());
						std::snprintf(gConfig.VehicleFilter, sizeof(gConfig.VehicleFilter), "%s", DefaultVehicleTokens());
						std::snprintf(gConfig.ObjectiveFilter, sizeof(gConfig.ObjectiveFilter), "%s", DefaultObjectiveTokens());
					}
					ImGui::SeparatorText("Likely Player Scoring");
					ImGui::Checkbox("Use runtime player context", &gConfig.UseRuntimePlayerContext);
					ImGui::SameLine();
					ImGui::Checkbox("Prefer runtime players", &gConfig.PreferRuntimePlayers);
					ImGui::SameLine();
					ImGui::Checkbox("GameState PlayerArray", &gConfig.IncludeGameStatePlayers);
					ImGui::SliderInt("Likely threshold", &gConfig.LikelyPlayerScoreThreshold, 0, 100);
					ImGui::SameLine();
					ImGui::Checkbox("Lock likely classes", &gConfig.LockLikelyPlayerClasses);
					ImGui::SliderInt("Lock min actors", &gConfig.LikelyClassLockMinActors, 1, 8);
					ImGui::SameLine();
					ImGui::SliderInt("Lock max classes", &gConfig.LikelyClassLockMaxClasses, 1, 8);
					ImGui::TextWrapped("Likely class lock: %s", Stats.LikelyClassLock.c_str());
					if (ImGui::Button("Clear likely class lock"))
						ClearLikelyClassLock();
					ImGui::SameLine();
					if (ImGui::Button("Reset player tokens"))
					{
						std::snprintf(gConfig.PlayerFilter, sizeof(gConfig.PlayerFilter), "%s", DefaultPlayerTokens());
						std::snprintf(gConfig.NonPlayerFilter, sizeof(gConfig.NonPlayerFilter), "%s", DefaultNonPlayerTokens());
					}
					ImGui::InputTextWithHint("Player tokens", "positive actor/class/path tokens", gConfig.PlayerFilter, sizeof(gConfig.PlayerFilter));
					ImGui::InputTextWithHint("Non-player tokens", "negative actor/class/path tokens", gConfig.NonPlayerFilter, sizeof(gConfig.NonPlayerFilter));
					ImGui::SeparatorText("Position Inference");
					ImGui::Checkbox("Use reflected position fallback", &gConfig.UseReflectedPositionFallback);
					ImGui::SameLine();
					ImGui::SliderInt("Position probe fields", &gConfig.PositionProbeMaxFields, 8, 240);
					ImGui::Combo("Projection route", &gConfig.ProjectionRoute, "Auto\0Native only\0Fallback only\0");
					ImGui::SameLine();
					if (ImGui::Button("Reset position tokens"))
						std::snprintf(gConfig.PositionFieldFilter, sizeof(gConfig.PositionFieldFilter), "%s", DefaultPositionFieldTokens());
					ImGui::InputTextWithHint("Position field tokens", "fields/method hints used to infer actor position", gConfig.PositionFieldFilter, sizeof(gConfig.PositionFieldFilter));
					ImGui::InputTextWithHint("Bot tokens", "comma-separated bot actor, class, or path tokens", gConfig.BotFilter, sizeof(gConfig.BotFilter));
					ImGui::InputTextWithHint("NPC tokens", "comma-separated NPC actor, class, or path tokens", gConfig.NpcFilter, sizeof(gConfig.NpcFilter));
					ImGui::InputTextWithHint("Civilian tokens", "comma-separated civilian actor, class, or path tokens", gConfig.CivilianFilter, sizeof(gConfig.CivilianFilter));
					ImGui::InputTextWithHint("AI tokens", "comma-separated AI actor, class, or path tokens", gConfig.AiFilter, sizeof(gConfig.AiFilter));
					ImGui::InputTextWithHint("Camera tokens", "comma-separated camera actor, class, or path tokens", gConfig.CameraFilter, sizeof(gConfig.CameraFilter));
					ImGui::InputTextWithHint("Item tokens", "comma-separated item actor, class, or path tokens", gConfig.ItemFilter, sizeof(gConfig.ItemFilter));
					ImGui::InputTextWithHint("Weapon tokens", "comma-separated weapon actor, class, or path tokens", gConfig.WeaponFilter, sizeof(gConfig.WeaponFilter));
					ImGui::InputTextWithHint("Vehicle tokens", "comma-separated vehicle actor, class, or path tokens", gConfig.VehicleFilter, sizeof(gConfig.VehicleFilter));
					ImGui::InputTextWithHint("Objective tokens", "comma-separated objective actor, class, or path tokens", gConfig.ObjectiveFilter, sizeof(gConfig.ObjectiveFilter));
					ConfigSnapshot = gConfig;
				}

				UpdateClassAutoCycle(ClassSummaries);
				DrawTypedConfigEditor();
				DrawFeaturePlaceholderStateEditor();

				ImGui::SeparatorText("Class Browser");
				ImGui::Text("Classes: %d  Dev preview: %s",
					static_cast<int>(ClassSummaries.size()),
					(ConfigSnapshot.DeveloperPreviewEnabled && ConfigSnapshot.DeveloperPreviewClassFilter[0] != '\0') ? ConfigSnapshot.DeveloperPreviewClassFilter : "disabled");
				DrawDeveloperClassBrowser(ClassSummaries, ConfigSnapshot, 155.0f);

				ImGui::SeparatorText("Filter Counts");
				ImGui::Text("No location: %d  Local player: %d  Environment: %d  Target mode: %d",
					Stats.FilteredMissingLocation, Stats.FilteredLocalPlayer, Stats.FilteredEnvironment, Stats.FilteredTargetMode);
				ImGui::Text("Distance: %d  Exclude: %d  Include: %d  Not in view: %d",
					Stats.FilteredDistance, Stats.FilteredExclude, Stats.FilteredInclude, Stats.FilteredNotInView);
				ImGui::Text("Class filter: %d  Class exclude: %d",
					Stats.FilteredClass, Stats.FilteredClassExclude);
				ImGui::Text("Filtered Bot: %d  NPC: %d  Civilian: %d  AI: %d  Camera: %d",
					Stats.FilteredBot, Stats.FilteredNPC, Stats.FilteredCivilian, Stats.FilteredAI, Stats.FilteredCamera);
				ImGui::Text("Filtered Item: %d  Weapon: %d  Vehicle: %d  Objective: %d",
					Stats.FilteredItem, Stats.FilteredWeapon, Stats.FilteredVehicle, Stats.FilteredObjective);
				ImGui::Text("Detected Bot: %d  NPC: %d  Civilian: %d  AI: %d  Camera: %d",
					Stats.BotActors, Stats.NpcActors, Stats.CivilianActors, Stats.AiActors, Stats.CameraActors);
				ImGui::Text("Detected Item: %d  Weapon: %d  Vehicle: %d  Objective: %d",
					Stats.ItemActors, Stats.WeaponActors, Stats.VehicleActors, Stats.ObjectiveActors);
				ImGui::Text("Runtime context: %s  Local players: %d  PlayerStates: %d  Runtime actors: %d  PlayerState actors: %d",
					Stats.HasRuntimeContext ? "yes" : "no",
					Stats.RuntimeLocalPlayers,
					Stats.RuntimePlayerStates,
					Stats.RuntimeContextActors,
					Stats.PlayerStateActors);

				if (ImGui::BeginTabBar("##developer_actor_tabs"))
				{
					if (ImGui::BeginTabItem("Kept"))
					{
						DrawDeveloperActorTable("##developer_kept", Actors, false,
							ConfigSnapshot.DeveloperProbeFilter, ConfigSnapshot.DeveloperMaxRows, 185.0f);
						ImGui::EndTabItem();
					}

					if (ImGui::BeginTabItem("Filtered"))
					{
						DrawDeveloperActorTable("##developer_filtered", FilteredActors, true,
							ConfigSnapshot.DeveloperProbeFilter, ConfigSnapshot.DeveloperMaxRows, 185.0f);
						ImGui::EndTabItem();
					}

					ImGui::EndTabBar();
				}

				DrawSelectedActor(Actors, FilteredActors);
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Misc"))
			{
				ImGui::SeparatorText("Misc");
				ImGui::InputTextWithHint("Misc profile", "label only", gFeatureState.szMiscProfileName, sizeof(gFeatureState.szMiscProfileName));
				ImGui::Checkbox("Show Watermark", &gFeatureState.bWatermark);
				ImGui::Checkbox("Draw mouse", &gFeatureState.bShowMouse);
				ImGui::SameLine();
				ImGui::ColorEdit4("##MouseColor", &gFeatureState.cMouseColor.x, ImGuiColorEditFlags_NoInputs);
				ImGui::SameLine();
				ImGui::Checkbox("Rainbow mouse", &gFeatureState.bRainbowMouse);
				ImGui::SliderInt("Mouse type", &gFeatureState.nMouseType, 0, 1);
				ImGui::Checkbox("Crosshair", &gConfig.DrawCrosshair);
				gFeatureState.bCrosshair = gConfig.DrawCrosshair;
				ImGui::SameLine();
				ImGui::ColorEdit4("##CrosshairColor", &gConfig.CrosshairColor.x, ImGuiColorEditFlags_NoInputs);
				gFeatureState.cCrosshairColor = gConfig.CrosshairColor;
				ImGui::SameLine();
				ImGui::Checkbox("Rainbow crosshair", &gFeatureState.bRainbowCrosshair);
				ImGui::SliderFloat("Crosshair size", &gConfig.CrosshairSize, 1.0f, 40.0f, "%.0f px");
				gFeatureState.fCrosshairSize = gConfig.CrosshairSize;
				ImGui::SliderInt("Crosshair type", &gFeatureState.nCrosshairType, 0, 1);
				ImGui::Checkbox("Camera FOV Changer", &gFeatureState.bCameraFovChanger);
				ImGui::SameLine();
				ImGui::SliderFloat("Camera custom FOV", &gFeatureState.fCameraCustomFov, 0.1f, 300.0f, "%.1f");
				ImGui::Checkbox("Show Inspector", &gFeatureState.bShowInspector);
				ImGui::SameLine();
				ImGui::Checkbox("Update targets", &gFeatureState.bUpdateTargets);
				ImGui::Checkbox("Update targets in different thread", &gFeatureState.bUpdateTargetsInDifferentThread);
				ImGui::SliderInt("Target fetch", &gFeatureState.nTargetFetch, 0, 2);
				ImGui::InputTextWithHint("Developer note", "label only", gFeatureState.szDeveloperNote, sizeof(gFeatureState.szDeveloperNote));
				ImGui::SeparatorText("Runtime Status");
				ImGui::Text("Status: %s", Stats.Status.c_str());
				ImGui::Text("Renderer: %s%s", BackendName(gBackend), gExternalOverlay ? " (external overlay)" : "");
				ImGui::Text("Renderer route: %s", RendererRouteName(GetConfigSnapshot().RendererRoute));
				ImGui::Text("External input: %s  F4 menu  F8 pass-through", ExternalShouldPassThrough() ? "pass-through" : "menu");
				ImGui::Text("Streamline/DLSSG: %s", Stats.HasStreamline ? "detected" : "not detected");
				ImGui::TextWrapped("RHI modules: %s", Stats.RhiModules.c_str());
				ImGui::Text("Game: %s", Settings::Generator::GameName.empty() ? "(auto)" : Settings::Generator::GameName.c_str());
				ImGui::Text("Version: %s", Settings::Generator::GameVersion.empty() ? "(auto)" : Settings::Generator::GameVersion.c_str());
				ImGui::Text("SDK path: %s", Settings::Generator::SDKGenerationPath.c_str());
				ImGui::SeparatorText("Unreal Console");
				ImGui::Text("Console: %s", gConsoleState.unlocked ? "unlocked" : (gConsoleState.attempted ? "not unlocked" : "not attempted"));
				ImGui::TextWrapped("Console status: %s", gConsoleState.status.c_str());
				ImGui::Text("Engine: 0x%p  Viewport: 0x%p  Console: 0x%p",
					reinterpret_cast<void*>(gConsoleState.engine),
					reinterpret_cast<void*>(gConsoleState.gameViewport),
					reinterpret_cast<void*>(gConsoleState.console));
				if (ImGui::Button("Unlock UE console"))
					UnlockUnrealConsole();
				ImGui::SameLine();
				if (ImGui::Button("Route F2 ConsoleKey"))
					TriggerUnrealConsoleHotkey();
				ImGui::SameLine();
				if (ImGui::Button("Post F2 to game"))
				{
					if (PostConsoleKeyToTargetWindow())
						gConsoleState.status = "F2 posted to game window";
					else
						gConsoleState.status = "Could not find target window for F2";
				}
				const int PresetCount = static_cast<int>(sizeof(kConsoleCommandPresets) / sizeof(kConsoleCommandPresets[0]));
				const bool PresetIndexValid = gConsoleCommandPresetIndex >= 0 && gConsoleCommandPresetIndex < PresetCount;
				const char* PresetPreview = PresetIndexValid ? kConsoleCommandPresets[gConsoleCommandPresetIndex].label : "Select command";
				if (ImGui::BeginCombo("Command preset", PresetPreview))
				{
					for (int PresetIndex = 0; PresetIndex < PresetCount; ++PresetIndex)
					{
						const ConsoleCommandPreset& Preset = kConsoleCommandPresets[PresetIndex];
						const bool bSelected = PresetIndex == gConsoleCommandPresetIndex;
						if (ImGui::Selectable(Preset.label, bSelected))
						{
							gConsoleCommandPresetIndex = PresetIndex;
							std::snprintf(gConsoleCommandBuffer, sizeof(gConsoleCommandBuffer), "%s", Preset.command);
							gConsoleState.status = std::string("Preset loaded: ") + Preset.command;
						}
						if (ImGui::IsItemHovered() && Preset.note)
							ImGui::SetTooltip("%s", Preset.note);
						if (bSelected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::InputTextWithHint("Console command", "stat fps", gConsoleCommandBuffer, sizeof(gConsoleCommandBuffer));
				ImGui::SameLine();
				if (ImGui::Button("Run command"))
					RunRuntimeConsoleCommand(gConsoleCommandBuffer);
				ImGui::TextDisabled("F2 now uses three routes: unlock/bind, PlayerController.ConsoleKey, and the game's own window input path.");
				ImGui::SeparatorText("Symbols");
				ImGui::Text("Actor class: %s", gSymbols.ActorClass ? "yes" : "no");
				ImGui::Text("Pawn class: %s", gSymbols.PawnClass ? "yes" : "no");
				ImGui::Text("Character class: %s", gSymbols.CharacterClass ? "yes" : "no");
				ImGui::Text("Primitive component class: %s", gSymbols.PrimitiveComponentClass ? "yes" : "no");
				ImGui::Text("Component bounds property: %s", gRuntimeSymbols.primitiveBoundsOffset >= 0 ? "yes" : "no");
				ImGui::Text("World class: %s", gSymbols.WorldClass ? "yes" : "no");
				ImGui::Text("PersistentLevel property: %s", gRuntimeSymbols.worldPersistentLevelOffset >= 0 ? "yes" : "no");
				ImGui::Text("Levels property: %s", gRuntimeSymbols.worldLevelsOffset >= 0 ? "yes" : "no");
				ImGui::Text("OwningGameInstance property: %s", gRuntimeSymbols.worldGameInstanceOffset >= 0 ? "yes" : "no");
				ImGui::Text("World GameState property: %s", gRuntimeSymbols.worldGameStateOffset >= 0 ? "yes" : "no");
				ImGui::Text("Console symbols: engine=%s viewport=%s console=%s spawn=%s",
					gRuntimeSymbols.engineClass ? "yes" : "no",
					gRuntimeSymbols.engineGameViewportOffset >= 0 ? "yes" : "no",
					(gRuntimeSymbols.gameViewportConsoleOffset >= 0 && gRuntimeSymbols.consoleClass) ? "yes" : "partial",
					gRuntimeSymbols.spawnObject.address ? "yes" : "no");
				ImGui::Text("ConsoleKey function: %s", gRuntimeSymbols.consoleKey.address ? "yes" : "no");
				ImGui::Text("Console command functions: SendToConsole=%s ExecuteConsoleCommand=%s",
					gRuntimeSymbols.sendToConsole.address ? "yes" : "no",
					gRuntimeSymbols.executeConsoleCommand.address ? "yes" : "no");
				ImGui::Text("Console links: ViewportConsole=%s ConsoleTargetPlayer=%s LocalPlayer.ViewportClient=%s",
					gRuntimeSymbols.gameViewportConsoleOffset >= 0 ? "yes" : "no",
					gRuntimeSymbols.consoleTargetPlayerOffset >= 0 ? "yes" : "no",
					gRuntimeSymbols.localPlayerViewportClientOffset >= 0 ? "yes" : "no");
				ImGui::Text("GameInstance class/property: %s / %s",
					gSymbols.GameInstanceClass ? "yes" : "no",
					gRuntimeSymbols.gameInstanceLocalPlayersOffset >= 0 ? "LocalPlayers" : "missing");
				ImGui::Text("LocalPlayer class/controller: %s / %s",
					gSymbols.LocalPlayerClass ? "yes" : "no",
					gRuntimeSymbols.localPlayerControllerOffset >= 0 ? "PlayerController" : "missing");
				ImGui::Text("GameState class PlayerArray: %s / %s",
					gSymbols.GameStateBaseClass ? "yes" : "no",
					gRuntimeSymbols.gameStatePlayerArrayOffset >= 0 ? "yes" : "no");
				ImGui::Text("PlayerState class: %s  Pawn.PlayerState: %s",
					gSymbols.PlayerStateClass ? "yes" : "no",
					gRuntimeSymbols.pawnPlayerStateOffset >= 0 ? "yes" : "no");
				ImGui::Text("Crab classes: CrabC=%s Enemy=%s Player=%s",
					gRuntimeSymbols.crabCharacterClass ? "yes" : "no",
					gRuntimeSymbols.crabEnemyClass ? "yes" : "no",
					gRuntimeSymbols.crabPlayerCharacterClass ? "yes" : "no");
				ImGui::Text("PlayerState pawn/owner links: pawn=%s owner=%s",
					gRuntimeSymbols.playerStatePawnOffset >= 0 ? "yes" : "no",
					gRuntimeSymbols.playerStateOwnerOffset >= 0 ? "yes" : "no");
				ImGui::Text("Actor location: %s", gSymbols.GetActorLocation ? "actor function" : (gSymbols.GetComponentLocation ? "root component fallback" : "missing"));
				ImGui::Text("Actor bounds: %s", gSymbols.GetActorBounds ? "yes" : "no");
				ImGui::Text("Controller pawn: %s", gSymbols.GetPawn ? "yes" : "no");
				ImGui::Text("Controller properties: pawn=%s character=%s camera=%s",
					gRuntimeSymbols.playerControllerAcknowledgedPawnOffset >= 0 ? "yes" : "no",
					gRuntimeSymbols.playerControllerCharacterOffset >= 0 ? "yes" : "no",
					gRuntimeSymbols.playerControllerCameraManagerOffset >= 0 ? "yes" : "no");
				ImGui::Text("Projection: %s", gSymbols.ProjectWorldLocationToScreen ? "yes" : "no");
				ImGui::Text("Skinned mesh class: %s", gSymbols.SkinnedMeshComponentClass ? "yes" : "no");
				ImGui::Text("Skeletal mesh class: %s", gSymbols.SkeletalMeshComponentClass ? "yes" : "no");
				ImGui::Text("Bone functions: count=%s name=%s location=%s",
					gSymbols.GetNumBones ? "yes" : "no",
					gSymbols.GetBoneName ? "yes" : "no",
					gSymbols.GetBoneLocation ? "yes" : "no");
				ImGui::Text("Controller viewpoint: %s", gSymbols.GetPlayerViewPoint ? "yes" : "no");
				ImGui::Text("Control rotation fallback: %s", gSymbols.GetControlRotation ? "yes" : "no");
				ImGui::Text("Camera rotation: %s", gSymbols.GetCameraRotation ? "yes" : "no");
				ImGui::Text("Camera FOV: %s", gSymbols.GetCameraFov ? "yes" : "no");
				ImGui::SeparatorText("Offsets");
				ImGui::Text("GObjects: 0x%X", Off::InSDK::ObjArray::GObjects);
				ImGui::Text("GWorld: 0x%X", Off::InSDK::World::GWorld);
				ImGui::Text("Current world: 0x%p", reinterpret_cast<void*>(Stats.WorldAddress));
				ImGui::Text("ProcessEvent index: 0x%X", Off::InSDK::ProcessEvent::PEIndex);
				ImGui::Text("ProcessEvent offset: 0x%X", Off::InSDK::ProcessEvent::PEOffset);
				ImGui::Text("ULevel::Actors: 0x%X", Off::InSDK::ULevel::Actors);
				ImGui::Text("FVector: %s", Settings::Internal::bUseLargeWorldCoordinates ? "double" : "float");
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::End();
	}

	void RenderD3D11Frame(IDXGISwapChain* SwapChain)
	{
		if (!InitializeD3D11ImGui(SwapChain))
			return;

		ProcessOverlayHotkeys();

		if (!gRenderTargetView)
			CreateD3D11RenderTarget(SwapChain);

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		DrawOverlayUi();

		ImGui::Render();
		gDeviceContext->OMSetRenderTargets(1, &gRenderTargetView, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	void RenderD3D12Frame(IDXGISwapChain* SwapChain)
	{
		if (!InitializeD3D12ImGui(SwapChain))
			return;

		ProcessOverlayHotkeys();

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		DrawOverlayUi();

		ImGui::Render();
		RenderD3D12DrawData(SwapChain);
	}

	void RenderOpenGLFrame(HDC DeviceContext)
	{
		if (!InitializeOpenGLImGui(DeviceContext))
			return;

		ProcessOverlayHotkeys();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		DrawOverlayUi();

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	}

	HRESULT __stdcall HookedPresent(IDXGISwapChain* SwapChain, UINT SyncInterval, UINT Flags)
	{
		if (!gShutdownRequested)
		{
			const RenderBackend SwapChainBackend = DetectSwapChainBackend(SwapChain);
			if (!gImGuiInitialized && (SwapChainBackend == RenderBackend::D3D11 || SwapChainBackend == RenderBackend::D3D12))
				gBackend = SwapChainBackend;

			if (gBackend == RenderBackend::D3D12)
				RenderD3D12Frame(SwapChain);
			else
				RenderD3D11Frame(SwapChain);
		}

		return gOriginalPresent(SwapChain, SyncInterval, Flags);
	}

	void __stdcall HookedExecuteCommandLists(ID3D12CommandQueue* Queue, UINT NumCommandLists, ID3D12CommandList* const* CommandLists)
	{
		if (Queue && Queue != gD3D12CommandQueue)
		{
			Queue->AddRef();
			ReleaseCom(gD3D12CommandQueue);
			gD3D12CommandQueue = Queue;
		}

		gOriginalExecuteCommandLists(Queue, NumCommandLists, CommandLists);
	}

	BOOL WINAPI HookedSwapBuffers(HDC DeviceContext)
	{
		if (!gShutdownRequested)
			RenderOpenGLFrame(DeviceContext);

		return gOriginalSwapBuffers(DeviceContext);
	}

	HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* SwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
	{
		if (gBackend == RenderBackend::D3D12)
		{
			WaitForD3D12Gpu();
			ReleaseD3D12RenderTargets();
		}
		else
		{
			ReleaseD3D11RenderTarget();
		}

		const HRESULT Result = gOriginalResizeBuffers(SwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
		if (SUCCEEDED(Result) && gImGuiInitialized)
		{
			if (gBackend == RenderBackend::D3D12)
				CreateD3D12RenderTargets(SwapChain);
			else if (gDevice)
				CreateD3D11RenderTarget(SwapChain);
		}
		return Result;
	}

	bool InstallDXGIHook()
	{
		WNDCLASSEXA WindowClass = {};
		WindowClass.cbSize = sizeof(WindowClass);
		WindowClass.style = CS_HREDRAW | CS_VREDRAW;
		WindowClass.lpfnWndProc = DefWindowProcA;
		WindowClass.hInstance = GetModuleHandle(nullptr);
		WindowClass.lpszClassName = "Dumper7DebugOverlayDummy";

		RegisterClassExA(&WindowClass);
		HWND DummyWindow = CreateWindowA(WindowClass.lpszClassName, "Dumper7DebugOverlayDummy", WS_OVERLAPPEDWINDOW,
			0, 0, 100, 100, nullptr, nullptr, WindowClass.hInstance, nullptr);

		if (!DummyWindow)
			return false;

		DXGI_SWAP_CHAIN_DESC SwapChainDesc = {};
		SwapChainDesc.BufferCount = 1;
		SwapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		SwapChainDesc.OutputWindow = DummyWindow;
		SwapChainDesc.SampleDesc.Count = 1;
		SwapChainDesc.Windowed = TRUE;
		SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		IDXGISwapChain* SwapChain = nullptr;
		ID3D11Device* Device = nullptr;
		ID3D11DeviceContext* Context = nullptr;
		D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
		const D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

		const HRESULT Result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			FeatureLevels, static_cast<UINT>(sizeof(FeatureLevels) / sizeof(FeatureLevels[0])), D3D11_SDK_VERSION,
			&SwapChainDesc, &SwapChain, &Device, &FeatureLevel, &Context);

		if (FAILED(Result) || !SwapChain)
		{
			if (Context) Context->Release();
			if (Device) Device->Release();
			DestroyWindow(DummyWindow);
			UnregisterClassA(WindowClass.lpszClassName, WindowClass.hInstance);
			return false;
		}

		gSwapChainVTable = *reinterpret_cast<void***>(SwapChain);
		const bool PresentPatched = PatchVTable(gSwapChainVTable, PresentVTableIndex, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&gOriginalPresent));
		const bool ResizePatched = PatchVTable(gSwapChainVTable, ResizeBuffersVTableIndex, reinterpret_cast<void*>(&HookedResizeBuffers), reinterpret_cast<void**>(&gOriginalResizeBuffers));

		Context->Release();
		Device->Release();
		SwapChain->Release();
		DestroyWindow(DummyWindow);
		UnregisterClassA(WindowClass.lpszClassName, WindowClass.hInstance);

		if (!PresentPatched || !ResizePatched)
		{
			if (PresentPatched)
				RestoreVTable(gSwapChainVTable, PresentVTableIndex, reinterpret_cast<void*>(gOriginalPresent));
			if (ResizePatched)
				RestoreVTable(gSwapChainVTable, ResizeBuffersVTableIndex, reinterpret_cast<void*>(gOriginalResizeBuffers));
			gOriginalPresent = nullptr;
			gOriginalResizeBuffers = nullptr;
			gSwapChainVTable = nullptr;
			return false;
		}

		gHookInstalled = true;
		return true;
	}

	bool InstallD3D12CommandQueueHook()
	{
		ID3D12Device* Device = nullptr;
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&Device))) || !Device)
			return false;

		D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
		QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

		ID3D12CommandQueue* Queue = nullptr;
		if (FAILED(Device->CreateCommandQueue(&QueueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&Queue))) || !Queue)
		{
			Device->Release();
			return false;
		}

		gD3D12CommandQueueVTable = *reinterpret_cast<void***>(Queue);
		const bool Patched = PatchVTable(gD3D12CommandQueueVTable, ExecuteCommandListsVTableIndex, reinterpret_cast<void*>(&HookedExecuteCommandLists), reinterpret_cast<void**>(&gOriginalExecuteCommandLists));

		Queue->Release();
		Device->Release();

		if (!Patched)
		{
			gD3D12CommandQueueVTable = nullptr;
			gOriginalExecuteCommandLists = nullptr;
			return false;
		}

		return true;
	}

	void UninstallD3D12CommandQueueHook()
	{
		if (!gD3D12CommandQueueVTable || !gOriginalExecuteCommandLists)
			return;

		RestoreVTable(gD3D12CommandQueueVTable, ExecuteCommandListsVTableIndex, reinterpret_cast<void*>(gOriginalExecuteCommandLists));
		gD3D12CommandQueueVTable = nullptr;
		gOriginalExecuteCommandLists = nullptr;
	}

	bool InstallOpenGLHook()
	{
		HMODULE Gdi32 = GetModuleHandleA("gdi32.dll");
		if (!Gdi32)
			Gdi32 = LoadLibraryA("gdi32.dll");

		void* SwapBuffersAddress = Gdi32 ? reinterpret_cast<void*>(GetProcAddress(Gdi32, "SwapBuffers")) : nullptr;
		if (!SwapBuffersAddress)
			return false;

		if (!InstallInlineHook(SwapBuffersAddress, reinterpret_cast<void*>(&HookedSwapBuffers), &gSwapBuffersTrampoline, gSwapBuffersOriginalBytes))
			return false;

		gOriginalSwapBuffers = reinterpret_cast<SwapBuffersFn>(gSwapBuffersTrampoline);
		gHookInstalled = true;
		return true;
	}

	void UninstallOpenGLHook()
	{
		if (!gSwapBuffersTrampoline)
			return;

		HMODULE Gdi32 = GetModuleHandleA("gdi32.dll");
		void* SwapBuffersAddress = Gdi32 ? reinterpret_cast<void*>(GetProcAddress(Gdi32, "SwapBuffers")) : nullptr;
		RestoreInlineHook(SwapBuffersAddress, gSwapBuffersTrampoline, gSwapBuffersOriginalBytes);
		gSwapBuffersTrampoline = nullptr;
		gOriginalSwapBuffers = nullptr;
		gHookInstalled = false;
	}

	void UninstallDXGIHook()
	{
		if (!gHookInstalled)
			return;

		RestoreVTable(gSwapChainVTable, PresentVTableIndex, reinterpret_cast<void*>(gOriginalPresent));
		RestoreVTable(gSwapChainVTable, ResizeBuffersVTableIndex, reinterpret_cast<void*>(gOriginalResizeBuffers));
		gHookInstalled = false;
	}

	void ShutdownImGui()
	{
		if (!gImGuiInitialized)
			return;

		if (gOriginalWndProc && gWindow)
		{
			SetWindowLongPtr(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gOriginalWndProc));
			gOriginalWndProc = nullptr;
		}

		if (gExternalOverlay)
			ImGui_ImplDX11_Shutdown();
		else if (gBackend == RenderBackend::D3D12)
			ImGui_ImplDX12_Shutdown();
		else if (gBackend == RenderBackend::OpenGL)
			ImGui_ImplOpenGL3_Shutdown();
		else
			ImGui_ImplDX11_Shutdown();

		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		gImGuiInitialized = false;

		ReleaseD3D11RenderTarget();
		ReleaseExternalRenderTarget();
		ReleaseD3D12RenderTargets();
		ReleaseExternalComposition();

		if (gDeviceContext)
		{
			gDeviceContext->Release();
			gDeviceContext = nullptr;
		}

		if (gDevice)
		{
			gDevice->Release();
			gDevice = nullptr;
		}

		ReleaseCom(gExternalDeviceContext);
		ReleaseCom(gExternalDevice);
		ReleaseCom(gExternalSwapChain);
		if (gExternalWindow)
		{
			DestroyWindow(gExternalWindow);
			gExternalWindow = nullptr;
		}

		for (ID3D12CommandAllocator*& Allocator : gD3D12CommandAllocators)
			ReleaseCom(Allocator);
		gD3D12CommandAllocators.clear();
		gD3D12FrameFenceValues.clear();
		ReleaseCom(gD3D12CommandList);
		ReleaseCom(gD3D12Fence);
		ReleaseCom(gD3D12SrvHeap);
		ReleaseCom(gD3D12RtvHeap);
		ReleaseCom(gD3D12Device);
		if (gD3D12FenceEvent)
		{
			CloseHandle(gD3D12FenceEvent);
			gD3D12FenceEvent = nullptr;
		}
		gD3D12SrvDescriptorUsed.clear();
	}
}

namespace DebugOverlay
{
	bool Start()
	{
		if (gRunning)
			return true;

		gShutdownRequested = false;
		LoadOverlayConfig();

		if (!RuntimeSDK::IsReady() || !ResolveOverlaySymbolsFromRuntimeDB(gRuntimeSymbols))
		{
			SetStatus("RuntimeSDK is not ready");
			return false;
		}

		{
			std::scoped_lock Lock(gConfigMutex);
			gConfig.CaptureOnRenderFrame = false;
			gConfig.FastOverlayMode = true;
			gConfig.ProbeReflectedPositionsOnLocatedActors = false;
			gConfig.ThrottleLiveReflectionFallback = true;
			gConfig.DeveloperAutoCycleClasses = gConfig.EnableDeveloperOptions && gConfig.DeveloperAutoCycleClasses;
			gConfig.DeveloperPreviewEnabled = gConfig.EnableDeveloperOptions && gConfig.DeveloperPreviewEnabled;
			if (!gConfig.EnableDeveloperOptions)
			{
				gConfig.EnableClassFilter = false;
				gConfig.LockLikelyPlayerClasses = false;
				gConfig.ClassFilter[0] = '\0';
				gConfig.ClassExcludeFilter[0] = '\0';
				gConfig.DeveloperPreviewClassFilter[0] = '\0';
			}
			gConfig.HideLocalPlayer = true;
			gConfig.HideEnvironmentActors = true;
			gConfig.OnlyOnScreen = true;
			gConfig.OnlyWithLocation = true;
			EnsureRuntimeTokenDefaults(gConfig);
			gConfig.DrawSkeletonBoneIds = false;
			gConfig.DrawSkeletonBoneNames = false;
			gConfig.FrameProjectionMaxActors = std::min(gConfig.FrameProjectionMaxActors, 64);
			gConfig.FrameSkeletonMinMs = std::max(gConfig.FrameSkeletonMinMs, 1000);
			gConfig.ReflectedPositionRefreshMs = std::max(gConfig.ReflectedPositionRefreshMs, 1000);
			gConfig.RefreshMs = std::max(gConfig.RefreshMs, 1000);
			gConfig.MaxActors = std::min(gConfig.MaxActors, 256);
			gConfig.MaxBoxScreenFraction = std::min(gConfig.MaxBoxScreenFraction, 0.40f);
		}
		ClearLikelyClassLock();
		std::cerr << "[Overlay] Render path is cache-only\n";

		gBackend = WaitForRendererBackend();

		bool bHooked = false;
		const OverlayConfig Config = GetConfigSnapshot();
		const bool bStreamlineNeedsExternal = gBackend == RenderBackend::D3D12
			&& Config.ExternalOverlayOnStreamline
			&& HasStreamlineOrFrameGenModule();
		const bool bUseExternalOverlay = Config.RendererRoute == 2
			|| (Config.RendererRoute == 0 && bStreamlineNeedsExternal);

		if (bUseExternalOverlay)
		{
			gRunning = true;
			bHooked = StartExternalOverlay();
		}
		else if (gBackend == RenderBackend::D3D12)
		{
			bHooked = InstallDXGIHook();
			if (bHooked)
				InstallD3D12CommandQueueHook();
		}
		else if (gBackend == RenderBackend::D3D11)
		{
			bHooked = InstallDXGIHook();
		}
		else if (gBackend == RenderBackend::OpenGL)
		{
			bHooked = InstallOpenGLHook();
		}
		else if (gBackend == RenderBackend::Vulkan)
		{
			SetStatus("Vulkan detected, no Vulkan hook installed yet");
			return false;
		}

		if (!bHooked)
		{
			SetStatus(std::string("Failed to install render hook for ") + BackendName(gBackend));
			return false;
		}

		gRunning = true;
		gCaptureThread = std::thread(CaptureThreadProc);
		std::cerr << "[Overlay] Background capture started\n";
		SetStatus(std::string("Render backend detected: ") + BackendName(gBackend) + (gExternalOverlay ? " (external overlay)" : " (internal overlay)"));
		return true;
	}

	void Shutdown()
	{
		if (!gRunning && !gHookInstalled && !gImGuiInitialized)
			return;

		gShutdownRequested = true;
		gRunning = false;

		if (gCaptureThread.joinable())
			gCaptureThread.join();

		if (gExternalRenderThread.joinable())
			gExternalRenderThread.join();

		SaveOverlayConfig();
		ShutdownImGui();
		UninstallD3D12CommandQueueHook();
		if (gExternalOverlay)
		{
			gHookInstalled = false;
		}
		else if (gBackend == RenderBackend::OpenGL)
			UninstallOpenGLHook();
		else
			UninstallDXGIHook();
		ReleaseCom(gD3D12CommandQueue);
		gExternalOverlay = false;

		std::scoped_lock Lock(gActorMutex);
		gActors.clear();
		gFilteredActors.clear();
		gStats = {};
		{
			std::scoped_lock CacheLock(gClassObjectCacheMutex);
			gClassObjectCache.clear();
		}
		{
			std::scoped_lock CacheLock(gPositionProbeCacheMutex);
			gPositionProbeCache.clear();
		}
		{
			std::scoped_lock CacheLock(gMeshProbeCacheMutex);
			gMeshProbeCache.clear();
		}
	}

	bool IsRunning()
	{
		return gRunning;
	}
}
