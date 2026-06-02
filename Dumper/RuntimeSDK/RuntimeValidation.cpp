#include "RuntimeSDK/RuntimeValidation.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace
{
	bool ValidateRequiredProperty(const RuntimeDatabase& db, const char* name)
	{
		const RuntimePropertyInfo* property = db.FindProperty(name);
		if (!property)
		{
			std::cerr << "[RuntimeSDK] Missing property: " << name << "\n";
			return false;
		}

		if (property->offset < 0)
		{
			std::cerr << "[RuntimeSDK] Invalid offset: " << name << "\n";
			return false;
		}

		return true;
	}

	bool ValidateRequiredPropertyAny(const RuntimeDatabase& db, const char* displayName, std::initializer_list<const char*> names)
	{
		for (const char* name : names)
		{
			const RuntimePropertyInfo* property = db.FindProperty(name);
			if (!property)
				continue;

			if (property->offset < 0)
			{
				std::cerr << "[RuntimeSDK] Invalid offset: " << displayName << "\n";
				return false;
			}

			return true;
		}

		std::cerr << "[RuntimeSDK] Missing property: " << displayName << "\n";
		return false;
	}

	bool ValidateOptionalPropertyAny(const RuntimeDatabase& db, const char* displayName, const char* fallbackNote, std::initializer_list<const char*> names)
	{
		for (const char* name : names)
		{
			const RuntimePropertyInfo* property = db.FindProperty(name);
			if (!property)
				continue;

			if (property->offset < 0)
			{
				std::cerr << "[RuntimeSDK] Optional property invalid: " << displayName
					<< " (" << fallbackNote << ")\n";
				return false;
			}

			return true;
		}

		std::cerr << "[RuntimeSDK] Optional property missing: " << displayName
			<< " (" << fallbackNote << ")\n";
		return false;
	}

	bool ValidateRequiredFunction(const RuntimeDatabase& db, const char* name)
	{
		const RuntimeFunctionInfo* function = db.FindFunction(name);
		if (!function)
		{
			std::cerr << "[RuntimeSDK] Missing function: " << name << "\n";
			return false;
		}

		if (function->address == 0 && function->execOffset == 0)
		{
			std::cerr << "[RuntimeSDK] Invalid function: " << name << "\n";
			return false;
		}

		return true;
	}

	bool ValidateRequiredFunctionAny(const RuntimeDatabase& db, const char* displayName, std::initializer_list<const char*> names)
	{
		for (const char* name : names)
		{
			const RuntimeFunctionInfo* function = db.FindFunction(name);
			if (!function)
				continue;

			if (function->address == 0 && function->execOffset == 0)
			{
				std::cerr << "[RuntimeSDK] Invalid function: " << displayName << "\n";
				return false;
			}

			return true;
		}

		std::cerr << "[RuntimeSDK] Missing function: " << displayName << "\n";
		return false;
	}

	bool ValidateOptionalFunctionAny(const RuntimeDatabase& db, const char* displayName, const char* fallbackNote, std::initializer_list<const char*> names)
	{
		for (const char* name : names)
		{
			const RuntimeFunctionInfo* function = db.FindFunction(name);
			if (!function)
				continue;

			if (function->address == 0 && function->execOffset == 0)
			{
				std::cerr << "[RuntimeSDK] Optional function invalid: " << displayName
					<< " (" << fallbackNote << ")\n";
				return false;
			}

			return true;
		}

		std::cerr << "[RuntimeSDK] Optional function missing: " << displayName
			<< " (" << fallbackNote << ")\n";
		return false;
	}

	bool ValidateRequiredClass(const RuntimeDatabase& db, const char* name)
	{
		if (db.HasStruct(name))
			return true;

		std::cerr << "[RuntimeSDK] Missing class: " << name << "\n";
		return false;
	}

	bool ValidatePropertyBounds(const RuntimeDatabase& db, const RuntimeStructInfo& owner)
	{
		bool valid = true;
		if (owner.size <= 0)
			return true;

		for (const RuntimePropertyInfo& property : owner.properties)
		{
			if (property.offset < 0)
			{
				std::cerr << "[RuntimeSDK] Invalid offset: " << property.fullName << "\n";
				valid = false;
				continue;
			}

			const int32_t size = property.size * std::max(property.arrayDim, 1);
			if (size < 0 || size > 0x100000)
			{
				std::cerr << "[RuntimeSDK] Invalid property size: " << property.fullName << "\n";
				valid = false;
				continue;
			}

			if (size > 0 && property.offset + size > owner.size)
			{
				std::cerr << "[RuntimeSDK] Property outside owner size: " << property.fullName
					<< " offset=0x" << std::hex << property.offset
					<< " size=0x" << size
					<< " owner=0x" << owner.size << std::dec << "\n";
				valid = false;
			}
		}

		return valid;
	}

	bool HasAnyStruct(const RuntimeDatabase& db, std::initializer_list<const char*> names)
	{
		for (const char* name : names)
		{
			if (db.HasStruct(name))
				return true;
		}

		return false;
	}

	bool ValidateLegacyRuntime(const RuntimeDatabase& db)
	{
		const RuntimeGlobalOffsets& globals = db.Globals();
		const std::string generationName = globals.engineGenerationName.empty()
			? "legacy Unreal Engine"
			: globals.engineGenerationName;

		std::cerr << "[RuntimeSDK] Legacy Unreal generation detected: " << generationName << "\n";

		bool metadataValid = true;
		if (globals.gObjects < 0)
		{
			std::cerr << "[RuntimeSDK] Missing global: GObjects\n";
			metadataValid = false;
		}

		if (globals.gNames < 0)
		{
			std::cerr << "[RuntimeSDK] Missing global: GNames\n";
			metadataValid = false;
		}

		if (globals.processEvent < 0 && globals.processEventIndex < 0)
		{
			std::cerr << "[RuntimeSDK] Missing global: ProcessEvent\n";
			metadataValid = false;
		}

		if (!HasAnyStruct(db, { "UObject", "Object", "Core.Object" }))
			std::cerr << "[RuntimeSDK] Legacy core class not indexed yet: UObject/Object\n";
		if (!HasAnyStruct(db, { "UClass", "Class", "Core.Class" }))
			std::cerr << "[RuntimeSDK] Legacy core class not indexed yet: UClass/Class\n";

		if (!metadataValid)
			std::cerr << "[RuntimeSDK] Legacy metadata validation completed with missing entries\n";
		else
			std::cerr << "[RuntimeSDK] Legacy metadata validation passed\n";

		std::cerr << "[RuntimeSDK] UE1/UE2/UE3 runtime profile is recognized, but the UE4+ overlay path is disabled for this engine generation\n";
		std::cerr << "[RuntimeSDK] Legacy object/name/property layout resolver must be selected before starting overlay features\n";

		return false;
	}
}

bool RuntimeValidation::Validate(const RuntimeDatabase& db)
{
	bool valid = true;
	bool optionalComplete = true;

	const RuntimeGlobalOffsets& globals = db.Globals();
	if (globals.legacyRuntime)
		return ValidateLegacyRuntime(db);

	if (globals.gObjects < 0)
	{
		std::cerr << "[RuntimeSDK] Missing global: GObjects\n";
		valid = false;
	}

	if (globals.gWorld < 0)
	{
		std::cerr << "[RuntimeSDK] Missing global: GWorld\n";
		valid = false;
	}

	if (globals.processEvent < 0 && globals.processEventIndex < 0)
	{
		std::cerr << "[RuntimeSDK] Missing global: ProcessEvent\n";
		valid = false;
	}

	if (globals.uLevelActors < 0)
	{
		std::cerr << "[RuntimeSDK] Missing property: ULevel::Actors\n";
		valid = false;
	}

	constexpr const char* requiredClasses[] = {
		"UWorld",
		"ULevel",
		"UGameInstance",
		"ULocalPlayer",
		"AActor",
		"APawn",
		"APlayerController",
		"APlayerCameraManager",
		"USceneComponent",
		"UPrimitiveComponent",
		"UEngine",
		"UGameViewportClient",
		"UConsole"
	};

	for (const char* name : requiredClasses)
		valid &= ValidateRequiredClass(db, name);

	valid &= ValidateRequiredProperty(db, "UWorld::PersistentLevel");
	valid &= ValidateRequiredPropertyAny(db, "UWorld::Levels", { "UWorld::Levels", "UWorld::StreamingLevels" });
	valid &= ValidateRequiredPropertyAny(db, "UWorld::OwningGameInstance", { "UWorld::OwningGameInstance", "UWorld::GameInstance" });
	valid &= ValidateRequiredPropertyAny(db, "UWorld::GameState", { "UWorld::GameState", "UWorld::AuthorityGameState" });
	valid &= ValidateRequiredProperty(db, "UGameInstance::LocalPlayers");
	valid &= ValidateRequiredProperty(db, "ULocalPlayer::PlayerController");
	valid &= ValidateRequiredPropertyAny(db, "APlayerController::AcknowledgedPawn", { "APlayerController::AcknowledgedPawn", "APlayerController::Pawn" });
	valid &= ValidateRequiredPropertyAny(db, "APlayerController::PlayerCameraManager", { "APlayerController::PlayerCameraManager", "APlayerController::CameraManager" });
	valid &= ValidateRequiredProperty(db, "APawn::PlayerState");
	valid &= ValidateRequiredProperty(db, "AActor::RootComponent");
	optionalComplete &= ValidateOptionalPropertyAny(db, "UPrimitiveComponent::Bounds", "root-component bounds fallback disabled; AActor::GetActorBounds remains available", { "UPrimitiveComponent::Bounds", "USceneComponent::Bounds" });
	if (globals.uLevelActors < 0)
		std::cerr << "[RuntimeSDK] Invalid offset: ULevel::Actors\n";
	valid &= globals.uLevelActors >= 0;

	valid &= ValidateRequiredFunctionAny(db, "AActor::K2_GetActorLocation", { "AActor::K2_GetActorLocation", "AActor::GetActorLocation" });
	valid &= ValidateRequiredFunctionAny(db, "AActor::GetActorBounds", { "AActor::GetActorBounds", "AActor::K2_GetActorBounds" });
	valid &= ValidateRequiredFunctionAny(db, "USceneComponent::K2_GetComponentLocation", { "USceneComponent::K2_GetComponentLocation", "USceneComponent::GetComponentLocation" });
	valid &= ValidateRequiredFunction(db, "APlayerController::ProjectWorldLocationToScreen");
	optionalComplete &= ValidateOptionalFunctionAny(db, "APlayerController::GetPlayerViewPoint", "camera manager location/rotation fallback will be used", { "APlayerController::GetPlayerViewPoint", "AController::GetPlayerViewPoint" });
	valid &= ValidateRequiredFunctionAny(db, "APlayerCameraManager::GetCameraLocation", { "APlayerCameraManager::GetCameraLocation", "APlayerCameraManager::K2_GetActorLocation", "APlayerCameraManager::GetActorLocation" });
	valid &= ValidateRequiredFunctionAny(db, "APlayerCameraManager::GetCameraRotation", { "APlayerCameraManager::GetCameraRotation", "APlayerCameraManager::K2_GetActorRotation", "APlayerCameraManager::GetActorRotation" });
	valid &= ValidateRequiredFunctionAny(db, "APlayerCameraManager::GetFOVAngle", { "APlayerCameraManager::GetFOVAngle", "APlayerCameraManager::GetCameraFOV", "APlayerCameraManager::GetCameraFov" });

	optionalComplete &= ValidateOptionalPropertyAny(db, "UEngine::ConsoleClass", "UConsole class fallback will be used", { "UEngine::ConsoleClass", "Engine::ConsoleClass" });
	optionalComplete &= ValidateOptionalPropertyAny(db, "UEngine::GameViewport", "live GameViewportClient fallback will be used", { "UEngine::GameViewport", "UGameEngine::GameViewport", "Engine::GameViewport" });
	optionalComplete &= ValidateOptionalPropertyAny(db, "UGameViewportClient::ViewportConsole", "UE4.27 SDK offset fallback will be used", { "UGameViewportClient::ViewportConsole", "GameViewportClient::ViewportConsole" });
	optionalComplete &= ValidateOptionalPropertyAny(db, "ULocalPlayer::ViewportClient", "Engine.GameViewport/live viewport fallback will be used", { "ULocalPlayer::ViewportClient", "LocalPlayer::ViewportClient" });
	optionalComplete &= ValidateOptionalPropertyAny(db, "UConsole::ConsoleTargetPlayer", "SetConsoleTarget/ViewportConsole assignment can still be attempted", { "UConsole::ConsoleTargetPlayer", "Console::ConsoleTargetPlayer" });
	optionalComplete &= ValidateOptionalPropertyAny(db, "UInputSettings::ConsoleKeys", "legacy ConsoleKey fallback will be used", { "UInputSettings::ConsoleKeys", "InputSettings::ConsoleKeys" });
	optionalComplete &= ValidateOptionalFunctionAny(db, "UGameplayStatics::SpawnObject", "existing UConsole fallback will be attempted", { "UGameplayStatics::SpawnObject", "GameplayStatics::SpawnObject" });
	optionalComplete &= ValidateOptionalFunctionAny(db, "UGameViewportClient::SetConsoleTarget", "ViewportConsole assignment can still succeed", { "UGameViewportClient::SetConsoleTarget", "GameViewportClient::SetConsoleTarget" });
	optionalComplete &= ValidateOptionalFunctionAny(db, "APlayerController::ConsoleKey", "F2 can still rely on normal input routing", { "APlayerController::ConsoleKey", "PlayerController::ConsoleKey" });
	optionalComplete &= ValidateOptionalFunctionAny(db, "APlayerController::SendToConsole", "UKismetSystemLibrary::ExecuteConsoleCommand fallback will be used", { "APlayerController::SendToConsole", "PlayerController::SendToConsole" });
	optionalComplete &= ValidateOptionalFunctionAny(db, "UKismetSystemLibrary::ExecuteConsoleCommand", "SendToConsole fallback will be used", { "UKismetSystemLibrary::ExecuteConsoleCommand", "KismetSystemLibrary::ExecuteConsoleCommand" });

	constexpr const char* boundsOwners[] = {
		"UWorld",
		"UGameInstance",
		"ULocalPlayer",
		"APlayerController",
		"APawn",
		"AActor",
		"UPrimitiveComponent",
		"ULevel"
	};

	for (const char* ownerName : boundsOwners)
	{
		if (const RuntimeStructInfo* owner = db.FindStruct(ownerName))
			valid &= ValidatePropertyBounds(db, *owner);
	}

	if (valid)
	{
		std::cerr << "[RuntimeSDK] Validation passed\n";
		if (!optionalComplete)
			std::cerr << "[RuntimeSDK] Validation passed with optional fallbacks disabled\n";
	}
	else
	{
		std::cerr << "[RuntimeSDK] Validation completed with missing or invalid entries\n";
	}

	return valid;
}
