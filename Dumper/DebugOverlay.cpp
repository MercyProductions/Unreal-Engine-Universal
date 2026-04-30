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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
		float SphereRadius = 0.0f;
		float DistanceMeters = 0.0f;
		bool HasLocation = false;
		bool HasBounds = false;
		bool HasDistance = false;
		bool HasScreen = false;
		bool HasBox = false;
		bool IsPawn = false;
		bool IsCharacter = false;
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
		ActorFilterReason FilterReason = ActorFilterReason::None;
		Vec2 Screen;
		Vec2 ScreenTop;
		Vec2 ScreenBottom;
		Vec2 BoxMin;
		Vec2 BoxMax;
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
		bool UsedDesktopProjection = false;
		bool SymbolsReady = false;
		bool HasPlayerController = false;
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
		int32 WorldCount = 0;
		int32 LevelCount = 0;
		int32 LevelActorSlots = 0;
		std::string ActorSource = "Auto";
		std::string RhiModules = "none";
		DWORD LastCaptureTick = 0;
		std::string Status = "Waiting for Unreal symbols";
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
		bool EnableClassFilter = false;
		bool DeveloperAutoCycleClasses = false;
		bool DeveloperShowInheritedMembers = true;
		bool EnableDeveloperOptions = false;
		int ActorSource = static_cast<int>(ActorCaptureSource::Auto);
		int TargetMode = 1;
		int ProjectionSpace = 0;
		int BoundsMode = 0;
		int RendererRoute = 0;
		int LineOrigin = 2;
		int LineTarget = 1;
		int RefreshMs = 350;
		int MaxActors = 256;
		int DeveloperClassCycleMs = 900;
		float MaxDistanceMeters = 0.0f;
		float CrosshairSize = 9.0f;
		float CrosshairGap = 4.0f;
		float CrosshairThickness = 1.5f;
		float LineThickness = 1.5f;
		float BoxThickness = 1.5f;
		float BoxWidthRatio = 0.45f;
		float MaxBoxScreenFraction = 1.30f;
		float BoxPaddingPixels = 2.0f;
		float MinBoxHeightPixels = 8.0f;
		float FallbackHalfHeight = 90.0f;
		float FallbackHalfWidth = 35.0f;
		char Filter[128] = {};
		char ExcludeFilter[128] = {};
		char EnvironmentFilter[512] = {};
		char BotFilter[256] = {};
		char NpcFilter[256] = {};
		char CivilianFilter[256] = {};
		char AiFilter[256] = {};
		char CameraFilter[256] = {};
		char ItemFilter[256] = {};
		char WeaponFilter[256] = {};
		char VehicleFilter[256] = {};
		char ObjectiveFilter[256] = {};
		char ClassFilter[256] = {};
		char ClassExcludeFilter[256] = {};
		char DeveloperProbeFilter[128] = {};
		int DeveloperMaxRows = 80;
		ImVec4 BoxColor = ImVec4(0.12f, 0.82f, 0.58f, 1.0f);
		ImVec4 LineColor = ImVec4(1.0f, 0.72f, 0.25f, 1.0f);
		ImVec4 TextColor = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);
		ImVec4 BoundsColor = ImVec4(0.42f, 0.68f, 1.0f, 1.0f);
		ImVec4 CrosshairColor = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);
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
		UEClass PlayerControllerClass;
		UEClass PlayerCameraManagerClass;
		UEProperty PersistentLevelProperty;
		UEProperty LevelsProperty;
		UEProperty RootComponentProperty;
		UEProperty ComponentBoundsProperty;
		UEFunction GetActorLocation;
		UEFunction GetActorBounds;
		UEFunction GetPawn;
		UEFunction GetComponentLocation;
		UEFunction ProjectWorldLocationToScreen;
		UEFunction GetCameraLocation;
		UEFunction GetCameraRotation;
		UEFunction GetCameraFov;
		bool Ready = false;
	};

	struct RawTArrayView
	{
		void* Data = nullptr;
		int32 Num = 0;
		int32 Max = 0;
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

	std::vector<ActorDebugInfo> gActors;
	std::vector<ActorDebugInfo> gFilteredActors;
	CaptureStats gStats;
	OverlayConfig gConfig;
	UnrealSymbols gSymbols;
	DWORD gLastClassCycleTick = 0;
	int gClassCycleIndex = -1;

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
		if (Actor.IsLocalPlayer || Actor.IsPawn || Actor.IsCharacter || Actor.IsLikelyPlayer
			|| Actor.IsBot || Actor.IsNPC || Actor.IsCivilian || Actor.IsAI)
			return false;

		return ActorTextMatchesTokens(Actor, Config.ExcludeFilter)
			|| ActorTextMatchesTokens(Actor, Config.EnvironmentFilter);
	}

	bool IsPlayerLikeActor(const ActorDebugInfo& Actor)
	{
		static constexpr const char* PlayerLikeTokens =
			"player,pawn,character,bot,npc,hero,unit";
		static constexpr const char* NonVisualTokens =
			"controller,camera,manager,gamemode,gamestate,playerstate";

		return Actor.IsPawn
			|| Actor.IsCharacter
			|| Actor.IsBot
			|| Actor.IsNPC
			|| Actor.IsCivilian
			|| Actor.IsAI
			|| (!ActorTextMatchesTokens(Actor, NonVisualTokens) && ActorTextMatchesTokens(Actor, PlayerLikeTokens));
	}

	const char* ActorKindText(const ActorDebugInfo& Actor)
	{
		if (Actor.IsLocalPlayer)
			return "Local";
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

		std::scoped_lock Lock(gConfigMutex);
		gConfig.Enabled = ReadConfigBool("Enabled", gConfig.Enabled);
		gConfig.DrawLines = ReadConfigBool("DrawLines", gConfig.DrawLines);
		gConfig.DrawBoxes = ReadConfigBool("DrawBoxes", gConfig.DrawBoxes);
		gConfig.DrawNames = ReadConfigBool("DrawNames", gConfig.DrawNames);
		gConfig.DrawDistance = ReadConfigBool("DrawDistance", gConfig.DrawDistance);
		gConfig.DrawBounds = ReadConfigBool("DrawBounds", gConfig.DrawBounds);
		gConfig.DrawCenterDot = ReadConfigBool("DrawCenterDot", gConfig.DrawCenterDot);
		gConfig.DrawCrosshair = ReadConfigBool("DrawCrosshair", gConfig.DrawCrosshair);
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
		gConfig.EnableClassFilter = ReadConfigBool("EnableClassFilter", gConfig.EnableClassFilter);
		gConfig.DeveloperAutoCycleClasses = ReadConfigBool("DeveloperAutoCycleClasses", gConfig.DeveloperAutoCycleClasses);
		gConfig.DeveloperShowInheritedMembers = ReadConfigBool("DeveloperShowInheritedMembers", gConfig.DeveloperShowInheritedMembers);
		gConfig.EnableDeveloperOptions = ReadConfigBool("EnableDeveloperOptions", gConfig.EnableDeveloperOptions);
		gConfig.ActorSource = std::clamp(ReadConfigInt("ActorSource", gConfig.ActorSource), 0, 3);
		gConfig.TargetMode = std::clamp(ReadConfigInt("TargetMode", gConfig.TargetMode), 0, 6);
		gConfig.ProjectionSpace = std::clamp(ReadConfigInt("ProjectionSpace", gConfig.ProjectionSpace), 0, 2);
		gConfig.BoundsMode = std::clamp(ReadConfigInt("BoundsMode", gConfig.BoundsMode), 0, 3);
		gConfig.RendererRoute = std::clamp(ReadConfigInt("RendererRoute", gConfig.RendererRoute), 0, 2);
		gConfig.LineOrigin = std::clamp(ReadConfigInt("LineOrigin", gConfig.LineOrigin), 0, 2);
		gConfig.LineTarget = std::clamp(ReadConfigInt("LineTarget", gConfig.LineTarget), 0, 2);
		gConfig.RefreshMs = std::clamp(ReadConfigInt("RefreshMs", gConfig.RefreshMs), 50, 5000);
		gConfig.MaxActors = std::clamp(ReadConfigInt("MaxActors", gConfig.MaxActors), 1, 4096);
		gConfig.DeveloperMaxRows = std::clamp(ReadConfigInt("DeveloperMaxRows", gConfig.DeveloperMaxRows), 10, 500);
		gConfig.DeveloperClassCycleMs = std::clamp(ReadConfigInt("DeveloperClassCycleMs", gConfig.DeveloperClassCycleMs), 250, 10000);
		gConfig.MaxDistanceMeters = std::max(0.0f, ReadConfigFloat("MaxDistanceMeters", gConfig.MaxDistanceMeters));
		gConfig.CrosshairSize = std::clamp(ReadConfigFloat("CrosshairSize", gConfig.CrosshairSize), 1.0f, 80.0f);
		gConfig.CrosshairGap = std::clamp(ReadConfigFloat("CrosshairGap", gConfig.CrosshairGap), 0.0f, 40.0f);
		gConfig.CrosshairThickness = std::clamp(ReadConfigFloat("CrosshairThickness", gConfig.CrosshairThickness), 0.5f, 12.0f);
		gConfig.LineThickness = std::clamp(ReadConfigFloat("LineThickness", gConfig.LineThickness), 0.5f, 12.0f);
		gConfig.BoxThickness = std::clamp(ReadConfigFloat("BoxThickness", gConfig.BoxThickness), 0.5f, 12.0f);
		gConfig.BoxWidthRatio = std::clamp(ReadConfigFloat("BoxWidthRatio", gConfig.BoxWidthRatio), 0.1f, 2.0f);
		gConfig.MaxBoxScreenFraction = std::clamp(ReadConfigFloat("MaxBoxScreenFraction", gConfig.MaxBoxScreenFraction), 0.15f, 4.0f);
		gConfig.BoxPaddingPixels = std::clamp(ReadConfigFloat("BoxPaddingPixels", gConfig.BoxPaddingPixels), 0.0f, 80.0f);
		gConfig.MinBoxHeightPixels = std::clamp(ReadConfigFloat("MinBoxHeightPixels", gConfig.MinBoxHeightPixels), 0.0f, 250.0f);
		gConfig.FallbackHalfHeight = std::clamp(ReadConfigFloat("FallbackHalfHeight", gConfig.FallbackHalfHeight), 5.0f, 500.0f);
		gConfig.FallbackHalfWidth = std::clamp(ReadConfigFloat("FallbackHalfWidth", gConfig.FallbackHalfWidth), 5.0f, 500.0f);
		gConfig.BoxColor = ReadConfigColor("BoxColor", gConfig.BoxColor);
		gConfig.LineColor = ReadConfigColor("LineColor", gConfig.LineColor);
		gConfig.TextColor = ReadConfigColor("TextColor", gConfig.TextColor);
		gConfig.BoundsColor = ReadConfigColor("BoundsColor", gConfig.BoundsColor);
		gConfig.CrosshairColor = ReadConfigColor("CrosshairColor", gConfig.CrosshairColor);
		GetPrivateProfileStringA("DebugOverlay", "Filter", gConfig.Filter, gConfig.Filter, sizeof(gConfig.Filter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ExcludeFilter", gConfig.ExcludeFilter, gConfig.ExcludeFilter, sizeof(gConfig.ExcludeFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "EnvironmentFilter", DefaultEnvironmentTokens(), gConfig.EnvironmentFilter, sizeof(gConfig.EnvironmentFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "BotFilter", DefaultBotTokens(), gConfig.BotFilter, sizeof(gConfig.BotFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "NpcFilter", DefaultNpcTokens(), gConfig.NpcFilter, sizeof(gConfig.NpcFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "CivilianFilter", DefaultCivilianTokens(), gConfig.CivilianFilter, sizeof(gConfig.CivilianFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "AiFilter", DefaultAiTokens(), gConfig.AiFilter, sizeof(gConfig.AiFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "CameraFilter", DefaultCameraTokens(), gConfig.CameraFilter, sizeof(gConfig.CameraFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ItemFilter", DefaultItemTokens(), gConfig.ItemFilter, sizeof(gConfig.ItemFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "WeaponFilter", DefaultWeaponTokens(), gConfig.WeaponFilter, sizeof(gConfig.WeaponFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "VehicleFilter", DefaultVehicleTokens(), gConfig.VehicleFilter, sizeof(gConfig.VehicleFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ObjectiveFilter", DefaultObjectiveTokens(), gConfig.ObjectiveFilter, sizeof(gConfig.ObjectiveFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ClassFilter", gConfig.ClassFilter, gConfig.ClassFilter, sizeof(gConfig.ClassFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "ClassExcludeFilter", gConfig.ClassExcludeFilter, gConfig.ClassExcludeFilter, sizeof(gConfig.ClassExcludeFilter), Settings::GlobalConfigPath);
		GetPrivateProfileStringA("DebugOverlay", "DeveloperProbeFilter", gConfig.DeveloperProbeFilter, gConfig.DeveloperProbeFilter, sizeof(gConfig.DeveloperProbeFilter), Settings::GlobalConfigPath);
	}

	void SaveOverlayConfig()
	{
		CreateDirectoryA("C:\\Dumper-7", nullptr);

		const OverlayConfig Config = GetConfigSnapshot();
		WriteConfigBool("Enabled", Config.Enabled);
		WriteConfigBool("DrawLines", Config.DrawLines);
		WriteConfigBool("DrawBoxes", Config.DrawBoxes);
		WriteConfigBool("DrawNames", Config.DrawNames);
		WriteConfigBool("DrawDistance", Config.DrawDistance);
		WriteConfigBool("DrawBounds", Config.DrawBounds);
		WriteConfigBool("DrawCenterDot", Config.DrawCenterDot);
		WriteConfigBool("DrawCrosshair", Config.DrawCrosshair);
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
	WriteConfigBool("EnableClassFilter", Config.EnableClassFilter);
	WriteConfigBool("DeveloperAutoCycleClasses", Config.DeveloperAutoCycleClasses);
	WriteConfigBool("DeveloperShowInheritedMembers", Config.DeveloperShowInheritedMembers);
	WriteConfigBool("EnableDeveloperOptions", Config.EnableDeveloperOptions);
		WriteConfigInt("ActorSource", Config.ActorSource);
		WriteConfigInt("TargetMode", Config.TargetMode);
		WriteConfigInt("ProjectionSpace", Config.ProjectionSpace);
		WriteConfigInt("BoundsMode", Config.BoundsMode);
		WriteConfigInt("RendererRoute", Config.RendererRoute);
		WriteConfigInt("LineOrigin", Config.LineOrigin);
		WriteConfigInt("LineTarget", Config.LineTarget);
		WriteConfigInt("RefreshMs", Config.RefreshMs);
	WriteConfigInt("MaxActors", Config.MaxActors);
	WriteConfigInt("DeveloperMaxRows", Config.DeveloperMaxRows);
	WriteConfigInt("DeveloperClassCycleMs", Config.DeveloperClassCycleMs);
	WriteConfigFloat("MaxDistanceMeters", Config.MaxDistanceMeters);
		WriteConfigFloat("CrosshairSize", Config.CrosshairSize);
		WriteConfigFloat("CrosshairGap", Config.CrosshairGap);
		WriteConfigFloat("CrosshairThickness", Config.CrosshairThickness);
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
	WritePrivateProfileStringA("DebugOverlay", "Filter", Config.Filter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ExcludeFilter", Config.ExcludeFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "EnvironmentFilter", Config.EnvironmentFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "BotFilter", Config.BotFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "NpcFilter", Config.NpcFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "CivilianFilter", Config.CivilianFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "AiFilter", Config.AiFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "CameraFilter", Config.CameraFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ItemFilter", Config.ItemFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "WeaponFilter", Config.WeaponFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "VehicleFilter", Config.VehicleFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ObjectiveFilter", Config.ObjectiveFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ClassFilter", Config.ClassFilter, Settings::GlobalConfigPath);
	WritePrivateProfileStringA("DebugOverlay", "ClassExcludeFilter", Config.ClassExcludeFilter, Settings::GlobalConfigPath);
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
		return MaxExtent >= 1.0 && MaxExtent <= 1000000.0;
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
		Object.ProcessEvent(Function, Params.data());

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
		Object.ProcessEvent(Function, Params.data());

		return ReadScalar(Params, ReturnProperty, OutValue);
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
		Object.ProcessEvent(Function, Params.data());

		return ReadObjectValue(Params, ReturnProperty, OutObject);
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
		WriteBool(Params, IncludeChildren, true);
		EnsureParamSize(Params, Origin);
		EnsureParamSize(Params, BoxExtent);
		EnsureParamSize(Params, SphereRadius);

		Actor.ProcessEvent(Function, Params.data());

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

		PlayerController.ProcessEvent(Function, Params.data());

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

	bool IsSaneScreenBox(const Vec2& MinScreen, const Vec2& MaxScreen, const OverlayConfig& Config, float ViewWidth, float ViewHeight)
	{
		if (!Config.ClampLargeBoxes)
			return true;

		if (!IsFiniteScreenPoint(MinScreen) || !IsFiniteScreenPoint(MaxScreen))
			return false;

		const float Width = std::abs(MaxScreen.X - MinScreen.X);
		const float Height = std::abs(MaxScreen.Y - MinScreen.Y);
		const float MaxSize = std::max(ViewWidth, ViewHeight) * std::max(Config.MaxBoxScreenFraction, 0.15f);
		return Width >= 1.0f && Height >= 1.0f && Width <= MaxSize && Height <= MaxSize;
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
		if (PlayerController && Function && ProjectWorldToScreen(PlayerController, Function, WorldLocation, OutScreen))
		{
			NormalizeProjectedPoint(OutScreen, Config, Stats, ProjectionRect, ViewWidth, ViewHeight);
			return true;
		}

		if (!Config.UseProjectionFallback || !Stats.HasCameraLocation || !Stats.HasCameraRotation)
			return false;

		if (ProjectWorldToScreenCameraFallback(WorldLocation, CameraLocation, CameraRotation, CameraFov, ViewWidth, ViewHeight, OutScreen))
		{
			Stats.UsedProjectionFallback = true;
			return true;
		}

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

	bool IsReadablePointer(const void* Pointer)
	{
		return Pointer && !Platform::IsBadReadPtr(Pointer);
	}

	bool IsReadableObject(void* Pointer)
	{
		if (!IsReadablePointer(Pointer))
			return false;

		void* Vft = *reinterpret_cast<void**>(Pointer);
		return IsReadablePointer(Vft);
	}

	bool ReadObjectProperty(UEObject Object, UEProperty Property, UEObject& OutObject)
	{
		if (!Object || !Property)
			return false;

		const int32 Offset = Property.GetOffset();
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

	bool ReadBoxSphereBoundsProperty(UEObject Object, UEProperty Property, Vec3& OutOrigin, Vec3& OutExtent, float& OutSphereRadius)
	{
		if (!Object || !Property)
			return false;

		const int32 Offset = Property.GetOffset();
		const int32 Size = Property.GetSize();
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
		if (!ReadObjectProperty(Actor, gSymbols.RootComponentProperty, RootComponent))
			return false;

		if (gSymbols.PrimitiveComponentClass && !RootComponent.IsA(gSymbols.PrimitiveComponentClass))
			return false;

		return ReadBoxSphereBoundsProperty(RootComponent, gSymbols.ComponentBoundsProperty, OutOrigin, OutExtent, OutSphereRadius);
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

		if (TryActorBounds())
			return true;

		return TryRootBounds();
	}

	bool ReadArrayProperty(UEObject Object, UEProperty Property, RawTArrayView& OutArray)
	{
		if (!Object || !Property)
			return false;

		const int32 Offset = Property.GetOffset();
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

	bool ReadLevelActors(UEObject Level, std::vector<UEObject>& OutActors, CaptureStats& Stats)
	{
		if (!Level || Off::InSDK::ULevel::Actors <= 0)
			return false;

		uint8* Address = static_cast<uint8*>(Level.GetAddress()) + Off::InSDK::ULevel::Actors;
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
		if (Off::InSDK::World::GWorld == 0)
			return {};

		const uintptr_t ModuleBase = Platform::GetModuleBase(Settings::General::DefaultModuleName);
		void** WorldAddress = reinterpret_cast<void**>(ModuleBase + Off::InSDK::World::GWorld);
		if (!IsReadablePointer(WorldAddress))
			return {};

		void* WorldPointer = *WorldAddress;
		if (!IsReadableObject(WorldPointer))
			return {};

		return UEObject(WorldPointer);
	}

	bool GetActorLocation(UEObject Actor, Vec3& OutLocation)
	{
		if (CallNoArgVectorFunction(Actor, gSymbols.GetActorLocation, OutLocation))
			return true;

		UEObject RootComponent;
		if (!ReadObjectProperty(Actor, gSymbols.RootComponentProperty, RootComponent))
			return false;

		return CallNoArgVectorFunction(RootComponent, gSymbols.GetComponentLocation, OutLocation);
	}

	bool ResolveSymbols(UnrealSymbols& Symbols)
	{
		if (Symbols.Ready)
			return true;

		Symbols.ActorClass = ObjectArray::FindClassFast("Actor");
		Symbols.WorldClass = ObjectArray::FindClassFast("World");
		Symbols.LevelClass = ObjectArray::FindClassFast("Level");
		Symbols.PawnClass = ObjectArray::FindClassFast("Pawn");
		Symbols.CharacterClass = ObjectArray::FindClassFast("Character");
		Symbols.SceneComponentClass = ObjectArray::FindClassFast("SceneComponent");
		Symbols.PrimitiveComponentClass = ObjectArray::FindClassFast("PrimitiveComponent");
		Symbols.PlayerControllerClass = ObjectArray::FindClassFast("PlayerController");
		Symbols.PlayerCameraManagerClass = ObjectArray::FindClassFast("PlayerCameraManager");

		if (!Symbols.ActorClass)
			return false;

		if (Symbols.WorldClass)
		{
			Symbols.PersistentLevelProperty = Symbols.WorldClass.FindMember("PersistentLevel");
			Symbols.LevelsProperty = Symbols.WorldClass.FindMember("Levels", EClassCastFlags::ArrayProperty);
		}

		Symbols.RootComponentProperty = Symbols.ActorClass.FindMember("RootComponent");
		Symbols.GetActorLocation = FindFirstFunction(Symbols.ActorClass, { "K2_GetActorLocation", "GetActorLocation" });
		Symbols.GetActorBounds = FindFirstFunction(Symbols.ActorClass, { "GetActorBounds", "K2_GetActorBounds" });

		if (Symbols.PrimitiveComponentClass)
			Symbols.ComponentBoundsProperty = Symbols.PrimitiveComponentClass.FindMember("Bounds");

		if (Symbols.SceneComponentClass)
			Symbols.GetComponentLocation = FindFirstFunction(Symbols.SceneComponentClass, { "K2_GetComponentLocation", "GetComponentLocation" });

		if (Symbols.PlayerControllerClass)
		{
			Symbols.ProjectWorldLocationToScreen = FindFirstFunction(Symbols.PlayerControllerClass, { "ProjectWorldLocationToScreen" });
			Symbols.GetPawn = FindFirstFunction(Symbols.PlayerControllerClass, { "K2_GetPawn", "GetPawn" });
		}

		if (Symbols.PlayerCameraManagerClass)
		{
			Symbols.GetCameraLocation = FindFirstFunction(Symbols.PlayerCameraManagerClass, { "GetCameraLocation", "K2_GetActorLocation", "GetActorLocation" });
			Symbols.GetCameraRotation = FindFirstFunction(Symbols.PlayerCameraManagerClass, { "GetCameraRotation", "K2_GetActorRotation", "GetActorRotation" });
			Symbols.GetCameraFov = FindFirstFunction(Symbols.PlayerCameraManagerClass, { "GetFOVAngle", "GetCameraFOV", "GetCameraFov" });
		}

		Symbols.Ready = static_cast<bool>(Symbols.GetActorLocation) || (Symbols.RootComponentProperty && Symbols.GetComponentLocation);
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

		if (!HasClassFilter && Config.TargetMode == 1 && !Actor.IsLikelyPlayer)
			return ActorFilterReason::TargetMode;

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
		if (ReadObjectProperty(World, gSymbols.PersistentLevelProperty, PersistentLevel))
			AddLevel(PersistentLevel);

		RawTArrayView Levels;
		if (ReadArrayProperty(World, gSymbols.LevelsProperty, Levels))
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

	void CaptureActors()
	{
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
			return;
		}

		Stats.SymbolsReady = true;

			UEObject PlayerController = FindFirstObjectOfClass(gSymbols.PlayerControllerClass);
			UEObject CameraManager = FindFirstObjectOfClass(gSymbols.PlayerCameraManagerClass);
			UEObject LocalPawn;
			if (PlayerController && gSymbols.GetPawn)
				CallNoArgObjectFunction(PlayerController, gSymbols.GetPawn, LocalPawn);

			Stats.HasPlayerController = static_cast<bool>(PlayerController);
		Stats.HasProjection = PlayerController && gSymbols.ProjectWorldLocationToScreen;

		Vec3 CameraLocation;
		Vec3 CameraRotation;
		float CameraFov = 90.0f;
		if (CameraManager && CallNoArgVectorFunction(CameraManager, gSymbols.GetCameraLocation, CameraLocation))
		{
			Stats.HasCameraLocation = true;
		}
		else if (PlayerController && GetActorLocation(PlayerController, CameraLocation))
		{
			Stats.HasCameraLocation = true;
		}

		if (CameraManager && CallNoArgVectorFunction(CameraManager, gSymbols.GetCameraRotation, CameraRotation))
			Stats.HasCameraRotation = true;

		if (CameraManager && CallNoArgFloatFunction(CameraManager, gSymbols.GetCameraFov, CameraFov))
			Stats.HasCameraFov = true;

		float ViewWidth = 0.0f;
		float ViewHeight = 0.0f;
		RECT ProjectionRect = {};
		GetProjectionViewport(ProjectionRect, ViewWidth, ViewHeight);

		std::vector<ActorDebugInfo> Actors;
		Actors.reserve(static_cast<size_t>(std::clamp(Config.MaxActors, 1, 4096)));
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
			Info.IsLikelyPlayer = IsPlayerLikeActor(Info);
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

			Info.HasLocation = GetActorLocation(Object, Info.Location);
			if (Info.HasLocation)
				Stats.LocatedActors++;

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
				Stats.BoundedActors++;

			const bool CanProject = Stats.HasProjection || (Config.UseProjectionFallback && Stats.HasCameraLocation && Stats.HasCameraRotation);
			if (Info.HasLocation && CanProject)
			{
				Info.HasScreen = ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
					CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Info.Location, Info.Screen);
				if (Info.HasScreen)
					Stats.ProjectedActors++;
				else
					Stats.ProjectionFailures++;

				if (Info.HasBounds)
					Info.HasBox = ProjectBoundsToScreenBoxAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
						CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Info.BoundsOrigin, Info.BoundsExtent, Info.BoxMin, Info.BoxMax);

				if (!Info.HasBox)
				{
					Vec3 Origin = Info.HasBounds ? Info.BoundsOrigin : Info.Location;
					const double HalfHeight = Info.HasBounds ? std::max(Info.BoundsExtent.Z, 1.0) : Config.FallbackHalfHeight;
					Vec3 Top = { Origin.X, Origin.Y, Origin.Z + HalfHeight };
					Vec3 Bottom = { Origin.X, Origin.Y, Origin.Z - HalfHeight };
					Info.HasBox = ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
						CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Top, Info.ScreenTop)
						&& ProjectWorldToScreenAny(PlayerController, gSymbols.ProjectWorldLocationToScreen, Config, Stats,
							CameraLocation, CameraRotation, CameraFov, ProjectionRect, ViewWidth, ViewHeight, Bottom, Info.ScreenBottom);

					if (Info.HasBox)
					{
						const float BoxTop = std::min(Info.ScreenTop.Y, Info.ScreenBottom.Y);
						const float BoxBottom = std::max(Info.ScreenTop.Y, Info.ScreenBottom.Y);
						const float BoxHeight = std::max(BoxBottom - BoxTop, 8.0f);
						const float BoxWidth = std::max(BoxHeight * Config.BoxWidthRatio, 4.0f);
						Info.BoxMin = { Info.Screen.X - (BoxWidth * 0.5f), BoxTop };
						Info.BoxMax = { Info.Screen.X + (BoxWidth * 0.5f), BoxBottom };
						if (!IsSaneScreenBox(Info.BoxMin, Info.BoxMax, Config, ViewWidth, ViewHeight))
							Info.HasBox = false;
					}
				}

				if (Info.HasBox)
					Stats.BoxedActors++;

				Info.IsInView = ActorProjectionInView(Info, ViewWidth, ViewHeight);
				if (Info.IsInView)
					Stats.InViewActors++;
			}

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

		Stats.CapturedActors = static_cast<int32>(Actors.size());
		Stats.Status = "Capturing actors";

		std::scoped_lock Lock(gActorMutex);
		gActors = std::move(Actors);
		gFilteredActors = std::move(FilteredActors);
		gStats = std::move(Stats);
	}

	void CaptureThreadProc()
	{
		while (gRunning)
		{
			CaptureActors();

			const OverlayConfig Config = GetConfigSnapshot();
			const int SleepMs = std::clamp(Config.RefreshMs, 50, 5000);
			for (int Remaining = SleepMs; gRunning && Remaining > 0; Remaining -= 25)
				Sleep(std::min(Remaining, 25));
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
		if (gMenuOpen && ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam))
			return true;

		return CallWindowProc(gOriginalWndProc, hWnd, Msg, wParam, lParam);
	}

	LRESULT CALLBACK ExternalOverlayWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
	{
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
	void DrawCrosshairOverlay(const OverlayConfig& Config);
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

	void DrawOverlayUi()
	{
		std::vector<ActorDebugInfo> Actors;
		std::vector<ActorDebugInfo> FilteredActors;
		CaptureStats Stats;
		CopyState(Actors, FilteredActors, Stats);
		const OverlayConfig Config = GetConfigSnapshot();

		ImGui::GetIO().MouseDrawCursor = gMenuOpen;
		DrawCrosshairOverlay(Config);
		DrawActorOverlay(Actors, Config);
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

		WaitForD3D12Gpu();

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
		WaitForD3D12Gpu();

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

		for (const ActorDebugInfo& Actor : Actors)
		{
			if (!Actor.HasScreen)
				continue;

			if (Config.OnlyOnScreen && !ActorProjectionInView(Actor, DisplaySize.x, DisplaySize.y))
				continue;

			const ImVec2 Screen(Actor.Screen.X, Actor.Screen.Y);

			float BoxHeight = 0.0f;
			float BoxWidth = 0.0f;
			float BoxTop = Screen.y - Config.FallbackHalfHeight;
			float BoxBottom = Screen.y + Config.FallbackHalfHeight;
			float Left = Screen.x - Config.FallbackHalfWidth;
			float Right = Screen.x + Config.FallbackHalfWidth;

			if (Actor.HasBox)
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
			if (Actor.HasBox)
			{
				if (Config.LineTarget == 1)
					LineTarget = ImVec2((Left + Right) * 0.5f, (BoxTop + BoxBottom) * 0.5f);
				else if (Config.LineTarget == 2)
					LineTarget = ImVec2((Left + Right) * 0.5f, BoxBottom);
			}

			if (Config.DrawLines)
				DrawList->AddLine(LineOrigin, LineTarget, LineColor, Config.LineThickness);

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

	void UpdateClassAutoCycle(const std::vector<ActorClassSummary>& Classes)
	{
		const OverlayConfig Config = GetConfigSnapshot();
		if (!Config.DeveloperAutoCycleClasses || Classes.empty())
			return;

		const DWORD Now = GetTickCount();
		const DWORD Delay = static_cast<DWORD>(std::clamp(Config.DeveloperClassCycleMs, 250, 10000));
		if (gClassCycleIndex >= 0 && gLastClassCycleTick != 0 && Now - gLastClassCycleTick < Delay)
			return;

		gClassCycleIndex = (gClassCycleIndex + 1) % static_cast<int>(Classes.size());
		gLastClassCycleTick = Now;
		FocusClassForOverlay(Classes[gClassCycleIndex]);
	}

	void DrawActorTable(const std::vector<ActorDebugInfo>& Actors)
	{
		const ImGuiTableFlags Flags = ImGuiTableFlags_Resizable
			| ImGuiTableFlags_RowBg
			| ImGuiTableFlags_BordersOuter
			| ImGuiTableFlags_BordersV
			| ImGuiTableFlags_ScrollY;

		if (!ImGui::BeginTable("##actors", 8, Flags, ImVec2(0.0f, 280.0f)))
			return;

		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 76.0f);
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

	void DrawClassReflection(const ActorDebugInfo& Actor)
	{
		UEClass Class;
		if (!ResolveActorClass(Actor, Class))
		{
			ImGui::TextUnformatted("Reflection: class pointer is no longer readable");
			return;
		}

		OverlayConfig Config = GetConfigSnapshot();
		if (ImGui::Button("Use selected class as overlay filter"))
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
			if (ImGui::BeginTable("##reflection_fields", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 150.0f)))
			{
				ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch);
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
		ImGui::Text("In view: %s", Actor.IsInView ? "yes" : "no");
		ImGui::Text("Address: 0x%p", reinterpret_cast<void*>(Actor.Address));
		if (Actor.HasLocation)
			ImGui::Text("Location: %.3f, %.3f, %.3f", Actor.Location.X, Actor.Location.Y, Actor.Location.Z);
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
		const int ColumnCount = ShowFilterReason ? 8 : 7;
		if (!ImGui::BeginTable(TableId, ColumnCount, Flags, ImVec2(0.0f, Height)))
			return;

		if (ShowFilterReason)
			ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 92.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 170.0f);
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

		if (!ImGui::BeginTable("##developer_classes", 7, Flags, ImVec2(0.0f, Height)))
			return;

		ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 56.0f);
		ImGui::TableSetupColumn("Kept", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Filt", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Box", ImGuiTableColumnFlags_WidthFixed, 52.0f);
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

			const bool Active = Config.EnableClassFilter && Config.ClassFilter[0] != '\0'
				&& (MatchesTokenListNoCase(Summary.Name, Config.ClassFilter) || MatchesTokenListNoCase(Summary.Path, Config.ClassFilter));

			ImGui::PushID(static_cast<int>(Summary.ClassAddress ^ Summary.SampleActorAddress));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(Summary.Name.c_str(), Active, ImGuiSelectableFlags_SpanAllColumns))
				FocusClassForOverlay(Summary);
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
			ImGui::Text("0x%p", reinterpret_cast<void*>(Summary.ClassAddress));
			ImGui::PopID();
			RowsDrawn++;
		}

		ImGui::EndTable();
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

		std::vector<ActorClassSummary> ClassSummaries = BuildActorClassSummaries(Actors, FilteredActors);
		UpdateClassAutoCycle(ClassSummaries);

		if (ImGui::BeginTabBar("##tabs"))
		{
			if (ImGui::BeginTabItem("Overlay"))
			{
				if (ImGui::Button("Save settings"))
					SaveOverlayConfig();
				ImGui::SameLine();
				if (ImGui::Button("Reload settings"))
					LoadOverlayConfig();

				{
					std::scoped_lock Lock(gConfigMutex);
					ImGui::Checkbox("Enabled", &gConfig.Enabled);
					ImGui::SameLine();
					ImGui::Checkbox("Only on screen", &gConfig.OnlyOnScreen);
					ImGui::SameLine();
					ImGui::Checkbox("Require location", &gConfig.OnlyWithLocation);
					ImGui::SameLine();
					ImGui::Checkbox("Only in view", &gConfig.OnlyInView);

					ImGui::SeparatorText("Drawing");
					ImGui::Checkbox("Lines", &gConfig.DrawLines);
					ImGui::SameLine();
					ImGui::Checkbox("Boxes", &gConfig.DrawBoxes);
					ImGui::SameLine();
					ImGui::Checkbox("Names", &gConfig.DrawNames);
					ImGui::SameLine();
					ImGui::Checkbox("Distance", &gConfig.DrawDistance);
					ImGui::SameLine();
					ImGui::Checkbox("Bounds", &gConfig.DrawBounds);
					ImGui::Checkbox("Actor center dots", &gConfig.DrawCenterDot);
					ImGui::SameLine();
					ImGui::Checkbox("Crosshair", &gConfig.DrawCrosshair);

					ImGui::Combo("Line origin", &gConfig.LineOrigin, "Top\0Center\0Bottom\0");
					ImGui::Combo("Line target", &gConfig.LineTarget, "Actor location\0Box center\0Box bottom\0");
					ImGui::SliderFloat("Crosshair size", &gConfig.CrosshairSize, 1.0f, 40.0f, "%.0f px");
					ImGui::SliderFloat("Crosshair gap", &gConfig.CrosshairGap, 0.0f, 25.0f, "%.0f px");
					ImGui::SliderFloat("Crosshair thickness", &gConfig.CrosshairThickness, 0.5f, 6.0f, "%.1f");
					ImGui::SliderFloat("Line thickness", &gConfig.LineThickness, 0.5f, 6.0f, "%.1f");
					ImGui::SliderFloat("Box thickness", &gConfig.BoxThickness, 0.5f, 6.0f, "%.1f");
					ImGui::SliderFloat("Box width ratio", &gConfig.BoxWidthRatio, 0.15f, 1.00f, "%.2f");
					ImGui::Checkbox("Clamp large boxes", &gConfig.ClampLargeBoxes);
					ImGui::SameLine();
					ImGui::SliderFloat("Max screen box", &gConfig.MaxBoxScreenFraction, 0.25f, 2.5f, "%.2fx");
					ImGui::SliderFloat("Box padding", &gConfig.BoxPaddingPixels, 0.0f, 20.0f, "%.0f px");
					ImGui::SliderFloat("Min box height", &gConfig.MinBoxHeightPixels, 0.0f, 80.0f, "%.0f px");
					ImGui::SliderFloat("Fallback half height", &gConfig.FallbackHalfHeight, 10.0f, 250.0f, "%.0f");
					ImGui::SliderFloat("Fallback half width", &gConfig.FallbackHalfWidth, 5.0f, 160.0f, "%.0f");

					ImGui::SeparatorText("Capture");
					ImGui::Combo("Renderer route", &gConfig.RendererRoute, "Auto\0Internal only\0External only\0");
					ImGui::Checkbox("Projection fallback", &gConfig.UseProjectionFallback);
					ImGui::Combo("Actor source", &gConfig.ActorSource, "Auto\0World levels\0GObjects\0World + GObjects\0");
					ImGui::Combo("Projection space", &gConfig.ProjectionSpace, "Auto\0Viewport\0Desktop\0");
					ImGui::Combo("Bounds source", &gConfig.BoundsMode, "Auto\0Actor\0Root component\0Fallback\0");
					ImGui::Combo("Target mode", &gConfig.TargetMode, "All actors\0Likely players\0Pawn / Character\0Bots\0NPC / AI\0Civilians\0Custom filters\0");
					ImGui::Checkbox("Hide environment", &gConfig.HideEnvironmentActors);
					ImGui::SameLine();
					ImGui::Checkbox("Hide local player", &gConfig.HideLocalPlayer);
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
					ImGui::InputTextWithHint("Include filters", "comma-separated actor, class, or path tokens", gConfig.Filter, sizeof(gConfig.Filter));
					ImGui::InputTextWithHint("Exclude filters", "comma-separated actor, class, or path tokens", gConfig.ExcludeFilter, sizeof(gConfig.ExcludeFilter));
					ImGui::Checkbox("Class filter", &gConfig.EnableClassFilter);
					ImGui::SameLine();
					if (ImGui::Button("Clear class filter"))
					{
						gConfig.EnableClassFilter = false;
						gConfig.ClassFilter[0] = '\0';
					}
					ImGui::InputTextWithHint("Class include", "comma-separated class tokens", gConfig.ClassFilter, sizeof(gConfig.ClassFilter));
					ImGui::InputTextWithHint("Class exclude", "comma-separated class tokens", gConfig.ClassExcludeFilter, sizeof(gConfig.ClassExcludeFilter));
					ImGui::SliderInt("Refresh ms", &gConfig.RefreshMs, 50, 2000);
					ImGui::SliderInt("Max actors", &gConfig.MaxActors, 1, 2048);
					ImGui::SliderFloat("Max distance meters", &gConfig.MaxDistanceMeters, 0.0f, 5000.0f, gConfig.MaxDistanceMeters <= 0.0f ? "disabled" : "%.0f");

					ImGui::SeparatorText("Colors");
					ImGui::ColorEdit4("Box", &gConfig.BoxColor.x, ImGuiColorEditFlags_NoInputs);
					ImGui::SameLine();
					ImGui::ColorEdit4("Line", &gConfig.LineColor.x, ImGuiColorEditFlags_NoInputs);
					ImGui::SameLine();
					ImGui::ColorEdit4("Text", &gConfig.TextColor.x, ImGuiColorEditFlags_NoInputs);
					ImGui::SameLine();
					ImGui::ColorEdit4("Bounds", &gConfig.BoundsColor.x, ImGuiColorEditFlags_NoInputs);
					ImGui::SameLine();
					ImGui::ColorEdit4("Crosshair", &gConfig.CrosshairColor.x, ImGuiColorEditFlags_NoInputs);
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Actors"))
			{
				ImGui::Text("Captured: %d  Candidates: %d  Scanned: %d  Objects: %d",
					Stats.CapturedActors, Stats.ActorCandidates, Stats.ScannedObjects, Stats.ObjectCount);
				ImGui::Text("Located: %d  Projected: %d  Boxes: %d  Projection failures: %d",
					Stats.LocatedActors, Stats.ProjectedActors, Stats.BoxedActors, Stats.ProjectionFailures);
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
				ImGui::Text("Projection space: %s%s",
					ProjectionSpaceName(GetConfigSnapshot().ProjectionSpace),
					Stats.UsedDesktopProjection ? " (desktop adjusted)" : "");
				DrawActorTable(Actors);
				DrawSelectedActor(Actors, FilteredActors);
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Developer"))
			{
				OverlayConfig ConfigSnapshot = GetConfigSnapshot();
				ImGui::Text("Captured: %d  Filtered: %d  Filter cache: %d",
					Stats.CapturedActors, Stats.FilteredActors, static_cast<int>(FilteredActors.size()));
				ImGui::Text("Source: %s  Objects: %d  Candidates: %d",
					Stats.ActorSource.c_str(), Stats.ObjectCount, Stats.ActorCandidates);

				{
					std::scoped_lock Lock(gConfigMutex);
					ImGui::Checkbox("Enable Developer Options", &gConfig.EnableDeveloperOptions);
					ImGui::SameLine();
					ImGui::SliderInt("Max dev rows", &gConfig.DeveloperMaxRows, 10, 500);
					ImGui::InputTextWithHint("Probe filter", "actor, class, or path tokens", gConfig.DeveloperProbeFilter, sizeof(gConfig.DeveloperProbeFilter));

					ImGui::SeparatorText("Class Overlay Filter");
					ImGui::Checkbox("Enable class filter", &gConfig.EnableClassFilter);
					ImGui::SameLine();
					ImGui::Checkbox("Auto-cycle classes", &gConfig.DeveloperAutoCycleClasses);
					ImGui::SameLine();
					ImGui::SliderInt("Cycle ms", &gConfig.DeveloperClassCycleMs, 250, 5000);
					if (ImGui::Button("Clear active class"))
					{
						gConfig.EnableClassFilter = false;
						gConfig.DeveloperAutoCycleClasses = false;
						gConfig.ClassFilter[0] = '\0';
					}
					ImGui::InputTextWithHint("Active class", "click a class below or type class tokens", gConfig.ClassFilter, sizeof(gConfig.ClassFilter));
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

				ImGui::SeparatorText("Class Browser");
				ImGui::Text("Classes: %d  Active class filter: %s",
					static_cast<int>(ClassSummaries.size()),
					(ConfigSnapshot.EnableClassFilter && ConfigSnapshot.ClassFilter[0] != '\0') ? ConfigSnapshot.ClassFilter : "disabled");
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

			if (ImGui::BeginTabItem("Dumper"))
			{
				ImGui::Text("Status: %s", Stats.Status.c_str());
				ImGui::Text("Renderer: %s%s", BackendName(gBackend), gExternalOverlay ? " (external overlay)" : "");
				ImGui::Text("Renderer route: %s", RendererRouteName(GetConfigSnapshot().RendererRoute));
				ImGui::Text("External input: %s  F4 menu  F8 pass-through", ExternalShouldPassThrough() ? "pass-through" : "menu");
				ImGui::Text("Streamline/DLSSG: %s", Stats.HasStreamline ? "detected" : "not detected");
				ImGui::TextWrapped("RHI modules: %s", Stats.RhiModules.c_str());
				ImGui::Text("Game: %s", Settings::Generator::GameName.empty() ? "(auto)" : Settings::Generator::GameName.c_str());
				ImGui::Text("Version: %s", Settings::Generator::GameVersion.empty() ? "(auto)" : Settings::Generator::GameVersion.c_str());
				ImGui::Text("SDK path: %s", Settings::Generator::SDKGenerationPath.c_str());
				ImGui::SeparatorText("Symbols");
				ImGui::Text("Actor class: %s", gSymbols.ActorClass ? "yes" : "no");
				ImGui::Text("Pawn class: %s", gSymbols.PawnClass ? "yes" : "no");
				ImGui::Text("Character class: %s", gSymbols.CharacterClass ? "yes" : "no");
				ImGui::Text("Primitive component class: %s", gSymbols.PrimitiveComponentClass ? "yes" : "no");
				ImGui::Text("Component bounds property: %s", gSymbols.ComponentBoundsProperty ? "yes" : "no");
				ImGui::Text("World class: %s", gSymbols.WorldClass ? "yes" : "no");
				ImGui::Text("PersistentLevel property: %s", gSymbols.PersistentLevelProperty ? "yes" : "no");
				ImGui::Text("Levels property: %s", gSymbols.LevelsProperty ? "yes" : "no");
				ImGui::Text("Actor location: %s", gSymbols.GetActorLocation ? "actor function" : (gSymbols.GetComponentLocation ? "root component fallback" : "missing"));
				ImGui::Text("Actor bounds: %s", gSymbols.GetActorBounds ? "yes" : "no");
				ImGui::Text("Controller pawn: %s", gSymbols.GetPawn ? "yes" : "no");
				ImGui::Text("Projection: %s", gSymbols.ProjectWorldLocationToScreen ? "yes" : "no");
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
	}

	bool IsRunning()
	{
		return gRunning;
	}
}
