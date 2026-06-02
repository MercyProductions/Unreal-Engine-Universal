#include <Windows.h>
#include <iostream>
#include <chrono>
#include <exception>
#include <fstream>
#include <string>
#include <cstdio>

#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"

#include "Generators/Generator.h"
#include "DebugOverlay.h"
#include "RuntimeSDK/RuntimeSDK.h"

enum class EFortToastType : uint8
{
        Default                        = 0,
        Subdued                        = 1,
        Impactful                      = 2,
        EFortToastType_MAX             = 3,
};

void LogDumperTrace(const std::string& Message);

template<typename GeneratorType>
bool RunGeneratorSafely(const char* Name)
{
	LogDumperTrace(std::string(Name) + " generation begin");
	try
	{
		Generator::Generate<GeneratorType>();
		LogDumperTrace(std::string(Name) + " generation finished");
		return true;
	}
	catch (const std::exception& Exception)
	{
		std::cerr << Name << " generation failed: " << Exception.what() << "\n";
		LogDumperTrace(std::string(Name) + " generation failed: " + Exception.what());
	}
	catch (...)
	{
		std::cerr << Name << " generation failed: unknown exception\n";
		LogDumperTrace(std::string(Name) + " generation failed: unknown exception");
	}

	return false;
}

void LogDumperTrace(const std::string& Message)
{
	try
	{
		SYSTEMTIME Now = {};
		GetLocalTime(&Now);

		std::ofstream Log("C:/Dumper-7/DumperMainTrace.log", std::ios::app);
		if (!Log)
			return;

		char Timestamp[64] = {};
		sprintf_s(Timestamp, "%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu",
			Now.wYear, Now.wMonth, Now.wDay, Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);

		Log << '[' << Timestamp << "] " << Message << '\n';
	}
	catch (...)
	{
	}
}

int LogDumperStructuredException(DWORD Code)
{
	char Message[128] = {};
	sprintf_s(Message, "Main thread structured exception: 0x%08lX", Code);
	LogDumperTrace(Message);
	return EXCEPTION_EXECUTE_HANDLER;
}

void NotifyGenerationFinished()
{
	std::cerr << "\n\n========================================\n";
	std::cerr << "GENERATION COMPLETE - SAFE TO MOVE FORWARD\n";
	std::cerr << "========================================\n\n";
	LogDumperTrace("Generation complete notification emitted");

	if (!Settings::Config::BeepWhenGenerationFinished)
		return;

	Beep(880, 180);
	Sleep(80);
	Beep(988, 180);
	Sleep(80);
	Beep(1175, 260);
	MessageBeep(MB_ICONINFORMATION);
}

void UnloadDumper(HMODULE Module, FILE* ConsoleInput)
{
	LogDumperTrace("Unload begin");

	try
	{
		LogDumperTrace("DebugOverlay::Shutdown begin");
		DebugOverlay::Shutdown();
		LogDumperTrace("DebugOverlay::Shutdown finished");
	}
	catch (const std::exception& Exception)
	{
		LogDumperTrace(std::string("DebugOverlay::Shutdown threw: ") + Exception.what());
	}
	catch (...)
	{
		LogDumperTrace("DebugOverlay::Shutdown threw: unknown exception");
	}

	try
	{
		LogDumperTrace("RuntimeSDK::Shutdown begin");
		RuntimeSDK::Shutdown();
		LogDumperTrace("RuntimeSDK::Shutdown finished");
	}
	catch (const std::exception& Exception)
	{
		LogDumperTrace(std::string("RuntimeSDK::Shutdown threw: ") + Exception.what());
	}
	catch (...)
	{
		LogDumperTrace("RuntimeSDK::Shutdown threw: unknown exception");
	}

	fclose(stderr);
	if (ConsoleInput)
	{
		fclose(ConsoleInput);
	}
	FreeConsole();

	LogDumperTrace("Calling FreeLibraryAndExitThread");
	FreeLibraryAndExitThread(Module, 0);
}

DWORD MainThreadBody(HMODULE Module)
{
	AllocConsole();
	FILE* Dummy = nullptr;
	freopen_s(&Dummy, "CONOUT$", "w", stderr);
	freopen_s(&Dummy, "CONIN$", "r", stdin);

	std::cerr << "Initializing [Dumper-7]\n";
	LogDumperTrace("Main thread begin");

	Settings::Config::Load();
	LogDumperTrace("Config loaded");
	Settings::Config::DelayDumperStart();
	LogDumperTrace("Delay complete");
	
	std::cerr << "Started Generation [Dumper-7]!\n";
	auto DumpStartTime = std::chrono::high_resolution_clock::now();

	LogDumperTrace("Generator::InitEngineCore begin");
	Generator::InitEngineCore();
	LogDumperTrace("Generator::InitEngineCore finished");
	LogDumperTrace("Generator::InitInternal begin");
	Generator::InitInternal();
	LogDumperTrace("Generator::InitInternal finished");

	bool bRuntimeSDKReady = false;
	if (Settings::Config::StartDebugOverlay)
	{
		try
		{
			LogDumperTrace("RuntimeSDK::Initialize begin");
			bRuntimeSDKReady = RuntimeSDK::Initialize();
			LogDumperTrace(std::string("RuntimeSDK::Initialize finished: ") + (bRuntimeSDKReady ? "ready" : "not ready"));
		}
		catch (const std::exception& Exception)
		{
			std::cerr << "[RuntimeSDK] Initialization threw: " << Exception.what() << "\n";
			LogDumperTrace(std::string("RuntimeSDK::Initialize threw: ") + Exception.what());
		}
		catch (...)
		{
			std::cerr << "[RuntimeSDK] Initialization threw an unknown exception\n";
			LogDumperTrace("RuntimeSDK::Initialize threw: unknown exception");
		}
	}
	else
	{
		std::cerr << "[RuntimeSDK] Skipped because StartDebugOverlay=0.\n";
		LogDumperTrace("RuntimeSDK::Initialize skipped because StartDebugOverlay=0");
	}

	if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
	{
		LogDumperTrace("Game name/version lookup begin");
		// Only Possible in Main()
		FString Name;
		FString Version;
		UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
		UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
		UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

		Kismet.ProcessEvent(GetGameName, &Name);
		Kismet.ProcessEvent(GetEngineVersion, &Version);

		Settings::Generator::GameName = Name.ToString();
		Settings::Generator::GameVersion = Version.ToString();
		LogDumperTrace("Game name/version lookup finished");
	}

	std::cerr << "GameName: " << Settings::Generator::GameName << "\n";
	std::cerr << "GameVersion: " << Settings::Generator::GameVersion << "\n\n";

	std::cerr << "FolderName: " << (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) << "\n\n";

	if (Settings::Config::StartDebugOverlay)
	{
		if (bRuntimeSDKReady && DebugOverlay::Start())
			std::cerr << "Debug overlay started. Press F4 for the ImGui menu and F7 to toggle actor drawing.\n\n";
		else if (!bRuntimeSDKReady)
			std::cerr << "Debug overlay not started because RuntimeSDK validation did not pass.\n\n";
		else
			std::cerr << "Debug overlay failed to start. The dumper will continue without ImGui.\n\n";
	}
	else
	{
		std::cerr << "Debug overlay not started because StartDebugOverlay=0.\n\n";
	}

	RunGeneratorSafely<CppGenerator>("C++ SDK");

	if (Settings::Config::GenerateMappings)
		RunGeneratorSafely<MappingGenerator>("Mapping");
	else
		std::cerr << "Mapping generation skipped. Set GenerateMappings=1 in Dumper-7.ini to enable it.\n";

	if (Settings::Config::GenerateIDAMappings)
		RunGeneratorSafely<IDAMappingGenerator>("IDA mapping");
	else
		std::cerr << "IDA mapping generation skipped. Set GenerateIDAMappings=1 in Dumper-7.ini to enable it.\n";

	if (Settings::Config::GenerateDumpspace)
		RunGeneratorSafely<DumpspaceGenerator>("Dumpspace");
	else
		std::cerr << "Dumpspace generation skipped. Set GenerateDumpspace=1 in Dumper-7.ini to enable it.\n";

	auto DumpFinishTime = std::chrono::high_resolution_clock::now();

	std::chrono::duration<double, std::milli> DumpTime = DumpFinishTime - DumpStartTime;

	std::cerr << "\n\nGenerating SDK took (" << DumpTime.count() << "ms)\n\n\n";
	NotifyGenerationFinished();

	if (Settings::Config::AutoUnloadAfterGeneration)
	{
		std::cerr << "\n\nGeneration finished. Auto-unloading Dumper-7.\n\n\n";
		UnloadDumper(Module, Dummy);
	}

	std::cerr << "\n\nPress F6 to unload\n\n\n";
	LogDumperTrace("Main thread idle; waiting for F6");

	while (true)
	{
		if (GetAsyncKeyState(VK_F6) & 1)
		{
			UnloadDumper(Module, Dummy);
		}

		Sleep(100);
	}

	return 0;
}

DWORD MainThreadImpl(HMODULE Module)
{
	try
	{
		return MainThreadBody(Module);
	}
	catch (const std::exception& Exception)
	{
		LogDumperTrace(std::string("Main thread C++ exception: ") + Exception.what());
	}
	catch (...)
	{
		LogDumperTrace("Main thread C++ exception: unknown exception");
	}

	return 0;
}

DWORD MainThread(HMODULE Module)
{
	__try
	{
		return MainThreadImpl(Module);
	}
	__except (LogDumperStructuredException(GetExceptionCode()))
	{
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
		break;
	}

	return TRUE;
}
