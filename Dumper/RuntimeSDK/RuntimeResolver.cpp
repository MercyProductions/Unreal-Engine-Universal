#include "RuntimeSDK/RuntimeResolver.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"
#include "Managers/StructManager.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "Settings.h"
#include "Unreal/ObjectArray.h"
#include "Wrappers/EnumWrapper.h"
#include "Wrappers/MemberWrappers.h"
#include "Wrappers/StructWrapper.h"

namespace
{
	RuntimeEngineGeneration DetectGenerationFromVersionString(const std::string& version)
	{
		size_t pos = 0;
		while (pos < version.size() && std::isspace(static_cast<unsigned char>(version[pos])))
			pos++;

		if (pos >= version.size() || !std::isdigit(static_cast<unsigned char>(version[pos])))
			return RuntimeEngineGeneration::Unknown;

		const int major = version[pos] - '0';
		switch (major)
		{
		case 1:
			return RuntimeEngineGeneration::UnrealEngine1;
		case 2:
			return RuntimeEngineGeneration::UnrealEngine2;
		case 3:
			return RuntimeEngineGeneration::UnrealEngine3;
		case 4:
			return RuntimeEngineGeneration::UnrealEngine4;
		case 5:
			return RuntimeEngineGeneration::UnrealEngine5;
		case 6:
			return RuntimeEngineGeneration::UnrealEngine6;
		default:
			return RuntimeEngineGeneration::Unknown;
		}
	}

	const char* GenerationName(RuntimeEngineGeneration generation)
	{
		switch (generation)
		{
		case RuntimeEngineGeneration::UnrealEngine1:
			return "Unreal Engine 1";
		case RuntimeEngineGeneration::UnrealEngine2:
			return "Unreal Engine 2";
		case RuntimeEngineGeneration::UnrealEngine3:
			return "Unreal Engine 3";
		case RuntimeEngineGeneration::UnrealEngine4:
			return "Unreal Engine 4";
		case RuntimeEngineGeneration::UnrealEngine5:
			return "Unreal Engine 5";
		case RuntimeEngineGeneration::UnrealEngine6:
			return "Unreal Engine 6";
		default:
			return "Unknown Unreal Engine";
		}
	}

	std::string StructRuntimeName(const StructWrapper& Struct)
	{
		if (Struct.IsUnrealStruct() && Struct.GetUnrealStruct())
			return Struct.GetUnrealStruct().GetCppName();

		return Struct.GetName();
	}

	std::string FunctionOwnerName(const FunctionWrapper& Function)
	{
		if (!Function.GetUnrealFunction())
			return {};

		UEObject Outer = Function.GetUnrealFunction().GetOuter();
		if (Outer && Outer.IsA(EClassCastFlags::Struct))
			return Outer.Cast<UEStruct>().GetCppName();

		return Outer ? Outer.GetCppName() : std::string();
	}

	std::string NormalizeTypeName(const std::string& value)
	{
		std::string result = value;
		constexpr const char* classPrefix = "class ";
		constexpr const char* structPrefix = "struct ";

		if (result.rfind(classPrefix, 0) == 0)
			result.erase(0, std::strlen(classPrefix));
		else if (result.rfind(structPrefix, 0) == 0)
			result.erase(0, std::strlen(structPrefix));

		return result;
	}

	std::string PropertyCategory(UEProperty property)
	{
		if (!property)
			return "unknown";

		if (property.IsA(EClassCastFlags::BoolProperty))
			return "bool";
		if (property.IsA(EClassCastFlags::EnumProperty)
			|| (property.IsA(EClassCastFlags::ByteProperty) && property.Cast<UEByteProperty>().GetEnum()))
			return "enum";
		if (property.IsA(EClassCastFlags::ObjectProperty)
			|| property.IsA(EClassCastFlags::ClassProperty)
			|| property.IsA(EClassCastFlags::WeakObjectProperty)
			|| property.IsA(EClassCastFlags::LazyObjectProperty)
			|| property.IsA(EClassCastFlags::SoftObjectProperty)
			|| property.IsA(EClassCastFlags::SoftClassProperty)
			|| property.IsA(EClassCastFlags::InterfaceProperty))
			return "object";
		if (property.IsA(EClassCastFlags::StructProperty))
			return "struct";
		if (property.IsA(EClassCastFlags::ArrayProperty)
			|| property.IsA(EClassCastFlags::SetProperty))
			return "array";
		if (property.IsA(EClassCastFlags::MapProperty))
			return "map";
		if (property.IsA(EClassCastFlags::FloatProperty)
			|| property.IsA(EClassCastFlags::DoubleProperty))
			return "float";
		if (property.IsA(EClassCastFlags::Int8Property)
			|| property.IsA(EClassCastFlags::Int16Property)
			|| property.IsA(EClassCastFlags::IntProperty)
			|| property.IsA(EClassCastFlags::Int64Property)
			|| property.IsA(EClassCastFlags::UInt16Property)
			|| property.IsA(EClassCastFlags::UInt32Property)
			|| property.IsA(EClassCastFlags::UInt64Property)
			|| property.IsA(EClassCastFlags::ByteProperty))
			return "int";

		return "unknown";
	}

	std::string RuntimeTypeName(const PropertyWrapper& property)
	{
		if (!property.IsUnrealProperty())
			return NormalizeTypeName(property.GetType());

		return NormalizeTypeName(property.GetUnrealProperty().GetCppType());
	}

	RuntimePropertyInfo ConvertProperty(const PropertyWrapper& property, const std::string& ownerName)
	{
		RuntimePropertyInfo info;
		info.ownerName = ownerName;
		info.propertyName = property.GetName();
		info.fullName = ownerName.empty() ? info.propertyName : ownerName + "::" + info.propertyName;
		info.typeName = RuntimeTypeName(property);
		info.offset = property.GetOffset();
		info.size = property.GetSize();
		info.arrayDim = property.GetArrayDim();
		info.isBitfield = property.IsBitField();
		info.bitIndex = info.isBitfield ? property.GetBitIndex() : -1;
		info.bitMask = info.isBitfield ? property.GetFieldMask() : 0;
		info.propertyFlags = static_cast<uint64_t>(property.GetPropertyFlags());

		if (property.IsUnrealProperty())
		{
			UEProperty unrealProperty = property.GetUnrealProperty();
			info.category = PropertyCategory(unrealProperty);
			info.address = reinterpret_cast<uintptr_t>(unrealProperty.GetAddress());
		}
		else
		{
			info.category = "unknown";
		}

		return info;
	}

	RuntimeFunctionInfo ConvertFunction(const FunctionWrapper& function, const std::string& ownerName)
	{
		RuntimeFunctionInfo info;
		info.ownerName = ownerName.empty() ? FunctionOwnerName(function) : ownerName;
		info.functionName = function.GetName();
		info.fullName = info.ownerName.empty() ? info.functionName : info.ownerName + "::" + info.functionName;
		info.execOffset = function.GetExecFuncOffset();
		info.functionFlags = static_cast<uint32_t>(function.GetFunctionFlags());

		if (!function.IsPredefined())
		{
			if (UEFunction unrealFunction = function.GetUnrealFunction())
				info.address = reinterpret_cast<uintptr_t>(unrealFunction.GetAddress());

			StructWrapper functionAsStruct = function.AsStruct();
			MemberManager params = functionAsStruct.GetMembers();

			for (const PropertyWrapper& param : params.IterateMembers())
			{
				if (!param.HasPropertyFlags(EPropertyFlags::Parm))
					continue;

				RuntimePropertyInfo paramInfo = ConvertProperty(param, info.fullName);
				if (param.HasPropertyFlags(EPropertyFlags::ReturnParm))
				{
					info.returnValue = std::move(paramInfo);
					info.hasReturnValue = true;
					continue;
				}

				info.params.push_back(std::move(paramInfo));
			}
		}

		return info;
	}

	RuntimeStructInfo ConvertStruct(const StructWrapper& Struct)
	{
		RuntimeStructInfo info;
		info.name = StructRuntimeName(Struct);
		info.fullName = Struct.GetFullName();
		info.size = Struct.GetSize();
		info.alignment = Struct.GetAlignment();
		info.isClass = Struct.IsClass();
		info.isFunction = Struct.IsFunction();
		info.isInterface = Struct.IsInterface();
		info.isStruct = !info.isClass && !info.isFunction;

		if (Struct.IsUnrealStruct() && Struct.GetUnrealStruct())
			info.address = reinterpret_cast<uintptr_t>(Struct.GetUnrealStruct().GetAddress());

		StructWrapper Super = Struct.GetSuper();
		if (Super.IsValid())
			info.superName = StructRuntimeName(Super);

		MemberManager members = Struct.GetMembers();
		for (const PropertyWrapper& property : members.IterateMembers())
			info.properties.push_back(ConvertProperty(property, info.name));

		if (Struct.IsClass())
		{
			for (const FunctionWrapper& function : members.IterateFunctions())
				info.functions.push_back(ConvertFunction(function, info.name));
		}

		return info;
	}

	std::string EnumUnderlyingType(int32_t size)
	{
		switch (size)
		{
		case 1:
			return "uint8";
		case 2:
			return "uint16";
		case 4:
			return "uint32";
		case 8:
			return "uint64";
		default:
			return "uint8";
		}
	}

	RuntimeEnumInfo ConvertEnum(const EnumWrapper& Enum)
	{
		RuntimeEnumInfo info;
		info.name = Enum.GetUnrealEnum().GetEnumPrefixedName();
		info.fullName = Enum.GetFullName();
		info.underlyingType = EnumUnderlyingType(Enum.GetUnderlyingTypeSize());
		info.address = reinterpret_cast<uintptr_t>(Enum.GetUnrealEnum().GetAddress());

		for (const EnumCollisionInfo& value : Enum.GetMembers())
			info.values.emplace_back(value.GetUniqueName(), static_cast<int64_t>(value.GetValue()));

		return info;
	}

	void FillGlobalOffsets(RuntimeDatabase& db)
	{
		RuntimeGlobalOffsets& globals = db.Globals();
		globals.imageBase = Platform::GetModuleBase(Settings::General::DefaultModuleName);
		globals.engineGeneration = DetectGenerationFromVersionString(Settings::Generator::GameVersion);
		globals.usesFProperty = Settings::Internal::bUseFProperty;
		globals.usesFField = Settings::Internal::bUseFProperty;
		globals.gObjects = Off::InSDK::ObjArray::GObjects;
		globals.gNames = Off::InSDK::NameArray::GNames;
		globals.gWorld = Off::InSDK::World::GWorld;
		globals.processEvent = Off::InSDK::ProcessEvent::PEOffset;
		globals.processEventIndex = Off::InSDK::ProcessEvent::PEIndex;

		if (Off::InSDK::Name::bIsUsingAppendStringOverToString)
			globals.appendString = Off::InSDK::Name::AppendNameToString;
		else
			globals.toString = Off::InSDK::Name::AppendNameToString;

		globals.uLevelActors = Off::InSDK::ULevel::Actors;
	}

	void UpdateGenerationFromDatabase(RuntimeDatabase& db)
	{
		RuntimeGlobalOffsets& globals = db.Globals();
		RuntimeEngineGeneration generation = globals.engineGeneration;

		if (generation == RuntimeEngineGeneration::Unknown)
		{
			const bool hasUE5Hints = db.HasStruct("WorldPartition")
				|| db.HasStruct("MassEntitySubsystem")
				|| db.HasStruct("EnhancedInputLocalPlayerSubsystem");
			const bool hasUE4PlusProfile = db.HasStruct("UGameInstance")
				|| db.HasStruct("GameInstance")
				|| db.HasStruct("UKismetSystemLibrary")
				|| db.HasStruct("KismetSystemLibrary");
			const bool hasUE3Profile = db.HasStruct("GameViewportClient")
				&& db.HasStruct("PlayerController")
				&& !hasUE4PlusProfile;

			if (hasUE5Hints)
				generation = RuntimeEngineGeneration::UnrealEngine5;
			else if (hasUE4PlusProfile || globals.usesFProperty)
				generation = RuntimeEngineGeneration::UnrealEngine4;
			else if (hasUE3Profile)
				generation = RuntimeEngineGeneration::UnrealEngine3;
		}

		globals.engineGeneration = generation;
		globals.engineGenerationName = GenerationName(generation);
		globals.legacyRuntime = generation == RuntimeEngineGeneration::UnrealEngine1
			|| generation == RuntimeEngineGeneration::UnrealEngine2
			|| generation == RuntimeEngineGeneration::UnrealEngine3;
	}
}

bool RuntimeResolver::BuildFromReflection(RuntimeDatabase& db)
{
	db.Clear();
	FillGlobalOffsets(db);

	std::cerr << "[RuntimeSDK] Global offsets resolved\n";

	int32_t structCount = 0;
	int32_t enumCount = 0;
	int32_t functionCount = 0;
	int32_t propertyCount = 0;

	for (PackageInfoHandle package : PackageManager::IterateOverPackageInfos())
	{
		if (package.IsEmpty())
			continue;

		for (int32 enumIndex : package.GetEnums())
		{
			UEEnum unrealEnum = ObjectArray::GetByIndex<UEEnum>(enumIndex);
			if (!unrealEnum)
				continue;

			db.AddEnum(ConvertEnum(unrealEnum));
			enumCount++;
		}

		auto addStruct = [&](int32 index) -> void
		{
			UEStruct unrealStruct = ObjectArray::GetByIndex<UEStruct>(index);
			if (!unrealStruct)
				return;

			RuntimeStructInfo info = ConvertStruct(unrealStruct);
			propertyCount += static_cast<int32_t>(info.properties.size());
			functionCount += static_cast<int32_t>(info.functions.size());
			db.AddStruct(std::move(info));
			structCount++;
		};

		if (package.HasStructs())
			package.GetSortedStructs().VisitAllNodesWithCallback(addStruct);

		if (package.HasClasses())
			package.GetSortedClasses().VisitAllNodesWithCallback(addStruct);

		for (int32 functionIndex : package.GetFunctions())
		{
			UEFunction unrealFunction = ObjectArray::GetByIndex<UEFunction>(functionIndex);
			if (!unrealFunction)
				continue;

			db.AddFunction(ConvertFunction(FunctionWrapper(nullptr, unrealFunction), {}));
			functionCount++;
		}
	}

	std::cerr << "[RuntimeSDK] Classes resolved: " << structCount << "\n";
	std::cerr << "[RuntimeSDK] Properties resolved: " << propertyCount << "\n";
	std::cerr << "[RuntimeSDK] Functions resolved: " << functionCount << "\n";
	std::cerr << "[RuntimeSDK] Enums resolved: " << enumCount << "\n";
	UpdateGenerationFromDatabase(db);
	std::cerr << "[RuntimeSDK] Engine generation detected: " << db.Globals().engineGenerationName;
	if (db.Globals().legacyRuntime)
		std::cerr << " (legacy runtime profile)";
	std::cerr << "\n";

	return structCount > 0;
}
