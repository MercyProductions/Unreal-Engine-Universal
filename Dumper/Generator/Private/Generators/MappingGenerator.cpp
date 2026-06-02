
#include <iostream>
#include <string>
#include <system_error>

#include "Generators/MappingGenerator.h"
#include "Managers/PackageManager.h"
#include "Compression/zstd.h"

#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "../Settings.h"
#include "Utils.h"

namespace
{
	bool IsReadableRange(const void* Address, const size_t Bytes = sizeof(void*))
	{
		if (!Address)
			return false;

		const uintptr_t Start = reinterpret_cast<uintptr_t>(Address);
		const uintptr_t LastByte = Start + (Bytes > 0 ? Bytes - 1 : 0);

		if (LastByte < Start)
			return false;

		return !Platform::IsBadReadPtr(reinterpret_cast<const void*>(Start))
			&& !Platform::IsBadReadPtr(reinterpret_cast<const void*>(LastByte));
	}

	bool IsReadableAtOffset(const void* Base, const int32 Offset, const size_t Bytes = sizeof(void*))
	{
		if (!Base || Offset < 0)
			return false;

		const uintptr_t Address = reinterpret_cast<uintptr_t>(Base) + static_cast<uintptr_t>(Offset);

		if (Address < reinterpret_cast<uintptr_t>(Base))
			return false;

		return IsReadableRange(reinterpret_cast<const void*>(Address), Bytes);
	}

	void* TryReadPointerAtOffset(const void* Base, const int32 Offset)
	{
		if (!IsReadableAtOffset(Base, Offset, sizeof(void*)))
			return nullptr;

		return *reinterpret_cast<void* const*>(static_cast<const uint8*>(Base) + Offset);
	}

	bool IsReadableFFieldClass(UEFFieldClass FieldClass)
	{
		return IsReadableAtOffset(FieldClass.GetAddress(), Off::FFieldClass::CastFlags, sizeof(EClassCastFlags));
	}

	bool IsReadableUClass(UEClass Class)
	{
		return IsReadableAtOffset(Class.GetAddress(), Off::UClass::CastFlags, sizeof(EClassCastFlags));
	}

	bool IsReadableUObject(UEObject Object)
	{
		const void* Address = Object.GetAddress();
		const void* Class = TryReadPointerAtOffset(Address, Off::UObject::Class);

		return Class && IsReadableUClass(UEClass(const_cast<void*>(Class)));
	}

	bool IsReadableProperty(UEProperty Property)
	{
		const void* Address = Property.GetAddress();

		if (!IsReadableRange(Address))
			return false;

		const int32 ClassOffset = Settings::Internal::bUseFProperty ? Off::FField::Class : Off::UObject::Class;
		const void* Class = TryReadPointerAtOffset(Address, ClassOffset);

		if (!Class)
			return false;

		if (Settings::Internal::bUseFProperty)
			return IsReadableFFieldClass(UEFFieldClass(const_cast<void*>(Class)));

		return IsReadableUClass(UEClass(const_cast<void*>(Class)));
	}

	bool IsReadableWrapperProperty(const PropertyWrapper& Property)
	{
		return Property.IsUnrealProperty() && IsReadableProperty(Property.GetUnrealProperty());
	}

	UEEnum GetReadableBytePropertyEnum(UEProperty Property)
	{
		void* Enum = TryReadPointerAtOffset(Property.GetAddress(), Off::ByteProperty::Enum);
		UEEnum WrappedEnum(Enum);

		return IsReadableUObject(WrappedEnum) ? WrappedEnum : UEEnum(nullptr);
	}

	UEStruct GetReadableStructPropertyStruct(UEProperty Property)
	{
		void* Struct = TryReadPointerAtOffset(Property.GetAddress(), Off::StructProperty::Struct);
		UEStruct WrappedStruct(Struct);

		return IsReadableUObject(WrappedStruct) ? WrappedStruct : UEStruct(nullptr);
	}

	UEProperty GetReadableArrayInnerProperty(UEProperty Property)
	{
		UEProperty Inner(TryReadPointerAtOffset(Property.GetAddress(), Off::ArrayProperty::Inner));

		return IsReadableProperty(Inner) ? Inner : UEProperty(nullptr);
	}

	UEProperty GetReadableSetElementProperty(UEProperty Property)
	{
		UEProperty Element(TryReadPointerAtOffset(Property.GetAddress(), Off::SetProperty::ElementProp));

		return IsReadableProperty(Element) ? Element : UEProperty(nullptr);
	}

	UEProperty GetReadableOptionalValueProperty(UEProperty Property)
	{
		UEProperty Value(TryReadPointerAtOffset(Property.GetAddress(), Off::OptionalProperty::ValueProperty));

		return IsReadableProperty(Value) ? Value : UEProperty(nullptr);
	}

	bool GetReadableMapProperties(UEProperty Property, UEProperty& OutKey, UEProperty& OutValue)
	{
		if (!IsReadableAtOffset(Property.GetAddress(), Off::MapProperty::Base, sizeof(Off::MapProperty::UMapPropertyBase)))
			return false;

		const auto* MapBase = reinterpret_cast<const Off::MapProperty::UMapPropertyBase*>(static_cast<const uint8*>(Property.GetAddress()) + Off::MapProperty::Base);

		OutKey = UEProperty(MapBase->KeyProperty);
		OutValue = UEProperty(MapBase->ValueProperty);

		if (!IsReadableProperty(OutKey))
			OutKey = UEProperty(nullptr);

		if (!IsReadableProperty(OutValue))
			OutValue = UEProperty(nullptr);

		return OutKey || OutValue;
	}

	bool GetReadableEnumPropertyParts(UEProperty Property, UEProperty& OutUnderlayingProperty, UEEnum& OutEnum)
	{
		if (!IsReadableAtOffset(Property.GetAddress(), Off::EnumProperty::Base, sizeof(Off::EnumProperty::UEnumPropertyBase)))
			return false;

		const auto* EnumBase = reinterpret_cast<const Off::EnumProperty::UEnumPropertyBase*>(static_cast<const uint8*>(Property.GetAddress()) + Off::EnumProperty::Base);

		OutUnderlayingProperty = UEProperty(EnumBase->UnderlayingProperty);
		OutEnum = UEEnum(EnumBase->Enum);

		if (!IsReadableProperty(OutUnderlayingProperty))
			OutUnderlayingProperty = UEProperty(nullptr);

		if (!IsReadableUObject(OutEnum))
			OutEnum = UEEnum(nullptr);

		return OutUnderlayingProperty || OutEnum;
	}
}

EMappingsTypeFlags MappingGenerator::GetMappingType(UEProperty Property)
{
	if (!IsReadableProperty(Property))
		return EMappingsTypeFlags::Unknown;

	auto [Class, FieldClass] = Property.GetClass();

	EClassCastFlags Flags = EClassCastFlags::None;

	if (Class && IsReadableUClass(Class))
		Flags = Class.GetCastFlags();
	else if (FieldClass && IsReadableFFieldClass(FieldClass))
		Flags = FieldClass.GetCastFlags();
	else
		return EMappingsTypeFlags::Unknown;

	if (Flags & EClassCastFlags::ByteProperty)
	{
		return EMappingsTypeFlags::ByteProperty;
	}
	else if (Flags & EClassCastFlags::UInt16Property)
	{
		return EMappingsTypeFlags::UInt16Property;
	}
	else if (Flags & EClassCastFlags::UInt32Property)
	{
		return EMappingsTypeFlags::UInt32Property;
	}
	else if (Flags & EClassCastFlags::UInt64Property)
	{
		return EMappingsTypeFlags::UInt64Property;
	}
	else if (Flags & EClassCastFlags::Int8Property)
	{
		return EMappingsTypeFlags::Int8Property;
	}
	else if (Flags & EClassCastFlags::Int16Property)
	{
		return EMappingsTypeFlags::Int16Property;
	}
	else if (Flags & EClassCastFlags::IntProperty)
	{
		return EMappingsTypeFlags::IntProperty;
	}
	else if (Flags & EClassCastFlags::Int64Property)
	{
		return EMappingsTypeFlags::Int64Property;
	}
	else if (Flags & EClassCastFlags::FloatProperty)
	{
		return EMappingsTypeFlags::FloatProperty;
	}
	else if (Flags & EClassCastFlags::DoubleProperty)
	{
		return EMappingsTypeFlags::DoubleProperty;
	}
	else if ((Flags & EClassCastFlags::ObjectProperty) || (Flags & EClassCastFlags::ClassProperty))
	{
		return EMappingsTypeFlags::ObjectProperty;
	}
	else if (Flags & EClassCastFlags::NameProperty)
	{
		return EMappingsTypeFlags::NameProperty;
	}
	else if (Flags & EClassCastFlags::StrProperty)
	{
		return EMappingsTypeFlags::StrProperty;
	}
	else if (Flags & EClassCastFlags::TextProperty)
	{
		return EMappingsTypeFlags::TextProperty;
	}
	else if (Flags & EClassCastFlags::BoolProperty)
	{
		return EMappingsTypeFlags::BoolProperty;
	}
	else if (Flags & EClassCastFlags::StructProperty)
	{
		return EMappingsTypeFlags::StructProperty;
	}
	else if (Flags & EClassCastFlags::ArrayProperty)
	{
		return EMappingsTypeFlags::ArrayProperty;
	}
	else if (Flags & EClassCastFlags::WeakObjectProperty)
	{
		return EMappingsTypeFlags::WeakObjectProperty;
	}
	else if (Flags & EClassCastFlags::LazyObjectProperty)
	{
		return EMappingsTypeFlags::LazyObjectProperty;
	}
	else if ((Flags & EClassCastFlags::SoftObjectProperty) || (Flags & EClassCastFlags::SoftClassProperty))
	{
		return EMappingsTypeFlags::SoftObjectProperty;
	}
	else if (Flags & EClassCastFlags::MapProperty)
	{
		return EMappingsTypeFlags::MapProperty;
	}
	else if (Flags & EClassCastFlags::SetProperty)
	{
		return EMappingsTypeFlags::SetProperty;
	}
	else if (Flags & EClassCastFlags::EnumProperty)
	{
		return EMappingsTypeFlags::EnumProperty;
	}
	else if (Flags & EClassCastFlags::InterfaceProperty)
	{
		return EMappingsTypeFlags::InterfaceProperty;
	}
	else if (Flags & EClassCastFlags::FieldPathProperty)
	{
		return EMappingsTypeFlags::FieldPathProperty;
	}
	else if (Flags & EClassCastFlags::OptionalProperty)
	{
		return EMappingsTypeFlags::OptionalProperty;
	}
	else if (Flags & EClassCastFlags::MulticastDelegateProperty)
	{
		return EMappingsTypeFlags::MulticastDelegateProperty;
	}
	else if (Flags & EClassCastFlags::DelegateProperty)
	{
		return EMappingsTypeFlags::DelegateProperty;
	}
	else if (Flags & EClassCastFlags::Utf8StrProperty)
	{
		return EMappingsTypeFlags::Utf8StrProperty;
	}
	else if (Flags & EClassCastFlags::AnsiStrProperty)
	{
		return EMappingsTypeFlags::AnsiStrProperty;
	}
	else if (Flags & EClassCastFlags::ClassProperty)
	{
		return EMappingsTypeFlags::ClassProperty;
	}
	else if (Flags & EClassCastFlags::MulticastInlineDelegateProperty)
	{
		return EMappingsTypeFlags::MulticastInlineDelegateProperty;
	}
	else if (Flags & EClassCastFlags::SoftClassProperty)
	{
		return EMappingsTypeFlags::SoftClassProperty;
	}
	
	return EMappingsTypeFlags::Unknown;
}

int32 MappingGenerator::AddNameToData(std::stringstream& NameTable, const std::string& Name)
{
	if constexpr (Settings::MappingGenerator::bShouldCheckForDuplicatedNames)
	{
		static std::unordered_map<std::string, int32> NameMap;
		
		auto [It, bInserted] = NameMap.insert({ Name, static_cast<int32>(NameCounter) });

		/* The name didn't occure yet, write it to the NameTable */
		if (bInserted)
		{
			WriteToStream(NameTable, static_cast<uint16>(Name.length()));
			NameTable.write(Name.c_str(), Name.length());
			return static_cast<int32>(NameCounter++);
		}

		return It->second;
	}

	WriteToStream(NameTable, static_cast<uint16>(Name.length()));
	NameTable.write(Name.c_str(), Name.length());

	return static_cast<int32>(NameCounter++);
}

void MappingGenerator::GeneratePropertyType(UEProperty Property, std::stringstream& Data, std::stringstream& NameTable)
{
	if (!IsReadableProperty(Property))
	{
		WriteToStream(Data, static_cast<uint8>(EMappingsTypeFlags::Unknown));
		return;
	}

	EMappingsTypeFlags MappingType = GetMappingType(Property);

	if (MappingType == EMappingsTypeFlags::Unknown)
	{
		WriteToStream(Data, static_cast<uint8>(EMappingsTypeFlags::Unknown));
		return;
	}

	/* Serialize ByteProperty as an EnumProperty with 'UnderlayingType == uint8' if the inner enum is valid */
	UEEnum BytePropertyEnum = MappingType == EMappingsTypeFlags::ByteProperty ? GetReadableBytePropertyEnum(Property) : UEEnum(nullptr);
	const bool bIsFakeEnumProperty = MappingType == EMappingsTypeFlags::ByteProperty && BytePropertyEnum;

	if (MappingType == EMappingsTypeFlags::EnumProperty)
	{
		UEProperty UnderlayingProperty = nullptr;
		UEEnum Enum = nullptr;

		GetReadableEnumPropertyParts(Property, UnderlayingProperty, Enum);

		if (!Enum)
		{
			GeneratePropertyType(UnderlayingProperty, Data, NameTable);
			return;
		}

		WriteToStream(Data, static_cast<uint8>(EMappingsTypeFlags::EnumProperty));
		GeneratePropertyType(UnderlayingProperty, Data, NameTable);

		const int32 EnumNameIdx = AddNameToData(NameTable, Enum.GetName());
		WriteToStream(Data, EnumNameIdx);

		return;
	}

	if (MappingType == EMappingsTypeFlags::StructProperty)
	{
		UEStruct UnderlayingStruct = GetReadableStructPropertyStruct(Property);

		if (!UnderlayingStruct)
		{
			WriteToStream(Data, static_cast<uint8>(EMappingsTypeFlags::Unknown));
			return;
		}

		WriteToStream(Data, static_cast<uint8>(EMappingsTypeFlags::StructProperty));

		const int32 StructNameIdx = AddNameToData(NameTable, UnderlayingStruct.GetName());
		WriteToStream(Data, StructNameIdx);

		return;
	}

	WriteToStream(Data, static_cast<uint8>(!bIsFakeEnumProperty ? MappingType : EMappingsTypeFlags::EnumProperty));

	/* Write ByteProperty as the fake EnumProperty's underlaying type */
	if (bIsFakeEnumProperty)
		WriteToStream(Data, static_cast<uint8>(EMappingsTypeFlags::ByteProperty));

	if (bIsFakeEnumProperty)
	{
		const int32 EnumNameIdx = AddNameToData(NameTable, BytePropertyEnum.GetName());
		WriteToStream(Data, EnumNameIdx);
	}
	else if (MappingType == EMappingsTypeFlags::SetProperty)
	{
		GeneratePropertyType(GetReadableSetElementProperty(Property), Data, NameTable);
	}
	else if (MappingType == EMappingsTypeFlags::ArrayProperty)
	{
		GeneratePropertyType(GetReadableArrayInnerProperty(Property), Data, NameTable);
	}
	else if (MappingType == EMappingsTypeFlags::OptionalProperty)
	{
		GeneratePropertyType(GetReadableOptionalValueProperty(Property), Data, NameTable);
	}
	else if (MappingType == EMappingsTypeFlags::MapProperty)
	{
		UEProperty KeyProperty = nullptr;
		UEProperty ValueProperty = nullptr;

		GetReadableMapProperties(Property, KeyProperty, ValueProperty);

		GeneratePropertyType(KeyProperty, Data, NameTable);
		GeneratePropertyType(ValueProperty, Data, NameTable);
	}
}

void MappingGenerator::GeneratePropertyInfo(const PropertyWrapper& Property, std::stringstream& Data, std::stringstream& NameTable, int32& Index)
{
	if (!IsReadableWrapperProperty(Property))
	{
		std::cerr << "\nInvalid or unreadable Unreal property skipped by mapping generator.\n" << std::endl;
		return;
	}

	const int32 ArrayDim = Property.GetArrayDim();

	if (ArrayDim <= 0 || ArrayDim > 0xFF)
	{
		std::cerr << "\nUnreal property with invalid ArrayDim skipped by mapping generator.\n" << std::endl;
		return;
	}

	WriteToStream(Data, static_cast<uint16>(Index));
	WriteToStream(Data, static_cast<uint8>(ArrayDim));

	const int32 MemberNameIdx = AddNameToData(NameTable, Property.GetUnrealProperty().GetName());
	WriteToStream(Data, MemberNameIdx);

	GeneratePropertyType(Property.GetUnrealProperty(), Data, NameTable);

	Index += ArrayDim;
}

void MappingGenerator::GenerateStruct(const StructWrapper& Struct, std::stringstream& Data, std::stringstream& NameTable)
{
	if (!Struct.IsValid() || (Struct.IsUnrealStruct() && !IsReadableUObject(Struct.GetUnrealStruct())))
		return;

	const int32 StructNameIndex = AddNameToData(NameTable, Struct.GetRawName());
	WriteToStream(Data, StructNameIndex);

	StructWrapper Super = Struct.GetSuper();

	if (Super.IsValid())
	{
		/* Most likely adds a duplicate to the name-table. Find a better solution later! */
		const int32 SuperNameIndex = AddNameToData(NameTable, Super.GetRawName());
		WriteToStream(Data, SuperNameIndex);
	}
	else
	{
		WriteToStream(Data, static_cast<int32>(-1));
	}

	MemberManager Members = Struct.GetMembers();

	uint16 PropertyCount = 0x0;
	uint16 SerializablePropertyCount = 0x0;
	constexpr auto ExcludeEditorOnlyProps = Settings::MappingGenerator::bExcludeEditorOnlyProperties;

	for (const PropertyWrapper& Member : Members.IterateMembers())
	{
		if (!IsReadableWrapperProperty(Member))
			continue;

		if (ExcludeEditorOnlyProps && Member.HasPropertyFlags(EPropertyFlags::EditorOnly))
			continue;

		const int32 ArrayDim = Member.GetArrayDim();

		if (ArrayDim <= 0 || ArrayDim > 0xFF)
			continue;

		SerializablePropertyCount++;
		PropertyCount += ArrayDim;
	}

	/* uint16, uint16 */
	WriteToStream(Data, PropertyCount);
	WriteToStream(Data, SerializablePropertyCount);

	/* Incremented by 'Property->ArrayDim' inside 'GeneratePropertyInfo()' */
	int32 IndexIncrementedByFunction = 0x0;

	for (const PropertyWrapper& Member : Members.IterateMembers())
	{
		if (!IsReadableWrapperProperty(Member))
			continue;

		if (ExcludeEditorOnlyProps && Member.HasPropertyFlags(EPropertyFlags::EditorOnly))
			continue;

		GeneratePropertyInfo(Member, Data, NameTable, IndexIncrementedByFunction);
	}
}

void MappingGenerator::GenerateEnum(const EnumWrapper& Enum, std::stringstream& Data, std::stringstream& NameTable)
{
	if (!Enum.IsValid() || !IsReadableUObject(Enum.GetUnrealEnum()))
		return;

	const int32 EnumNameIndex = AddNameToData(NameTable, Enum.GetRawName());
	WriteToStream(Data, EnumNameIndex);

	WriteToStream(Data, static_cast<uint16>(Enum.GetNumMembers()));

	for (EnumCollisionInfo Member : Enum.GetMembers())
	{
		const int32 EnumMemberNameIdx = AddNameToData(NameTable, Member.GetUniqueName());
		WriteToStream(Data, Member.GetValue());
		WriteToStream(Data, EnumMemberNameIdx);
	}
}

std::stringstream MappingGenerator::GenerateFileData()
{
	std::stringstream NameData;
	std::stringstream StructData;
	std::stringstream EnumData;

	uint32 NumEnums = 0x0;
	uint32 NumStructsAndClasse = 0x0;

	/* Handle all Enums first */
	for (PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
	{
		if (Package.IsEmpty())
			continue;

		/* Create files and handles namespaces and includes */
		if (!Package.HasEnums())
			continue;

		for (int32 EnumIdx : Package.GetEnums())
		{
			UEEnum Enum = ObjectArray::GetByIndex<UEEnum>(EnumIdx);

			if (!IsReadableUObject(Enum))
				continue;

			GenerateEnum(EnumWrapper(Enum), EnumData, NameData);
			NumEnums++;
		}
	}
	
	/* Handle all structs and classes in one go. From the mapping-files point of view classes are the exact same as structs. */
	for (PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
	{
		if (Package.IsEmpty())
			continue;

		/* Create files and handles namespaces and includes */
		if (!Package.HasClasses() && !Package.HasStructs())
			continue;

		DependencyManager::OnVisitCallbackType GenerateStructCallback = [&](int32 Index) -> void
		{
			UEStruct Struct = ObjectArray::GetByIndex<UEStruct>(Index);

			if (!IsReadableUObject(Struct))
				return;

			GenerateStruct(StructWrapper(Struct), StructData, NameData);
			NumStructsAndClasse++;
		};

		if (Package.HasStructs())
		{
			const DependencyManager& Structs = Package.GetSortedStructs();
			Structs.VisitAllNodesWithCallback(GenerateStructCallback);
		}

		if (Package.HasClasses())
		{
			const DependencyManager& Classes = Package.GetSortedClasses();
			Classes.VisitAllNodesWithCallback(GenerateStructCallback);
		}
	}

	/* Combine all of the stringstreams into one Data block representing the entire payload of the file */
	std::stringstream ReturnBuffer;

	/* Write Name-count and names */
	WriteToStream(ReturnBuffer, static_cast<uint32>(NameCounter));
	WriteToStream(ReturnBuffer, NameData);

	if constexpr (Settings::Debug::bShouldPrintMappingDebugData)
		std::cerr << std::format("MappingGeneration: NameCounter = 0x{0:X} (Dec: {0})\n", static_cast<uint32>(NameCounter));

	/* Write Enum-count and enums */
	WriteToStream(ReturnBuffer, static_cast<uint32>(NumEnums));
	WriteToStream(ReturnBuffer, EnumData);

	if constexpr (Settings::Debug::bShouldPrintMappingDebugData)
		std::cerr << std::format("MappingGeneration: NumEnums = 0x{0:X} (Dec: {0})\n", static_cast<uint32>(NumEnums));

	/* Write Struct-count and enums */
	WriteToStream(ReturnBuffer, static_cast<uint32>(NumStructsAndClasse));
	WriteToStream(ReturnBuffer, StructData);

	if constexpr (Settings::Debug::bShouldPrintMappingDebugData)
		std::cerr << std::format("MappingGeneration: NumStructsAndClasse = 0x{0:X} (Dec: {0})\n\n", static_cast<uint32>(NumStructsAndClasse));

	return ReturnBuffer;
}


void MappingGenerator::GenerateFileHeader(StreamType& InUsmap, const std::stringstream& Data)
{
	/* Write 2bytes unsigned */
	WriteToStream(InUsmap, UsmapFileMagic);

	/* Version: ExplicitEnumValues, adds support for enums with explicit values to fix mismatches */
	WriteToStream(InUsmap, EUsmapVersion::ExplicitEnumValues);

	/* We're on 'ExplicitEnumValues' version, we need to write 'bool' (aka int32) bHasVersioning. (NoVersioning = false) -> no [int32 UE4Version, int32 UE5Version] and no [uint32 NetCL] */
	WriteToStream(InUsmap, static_cast<int32>(false));

	const uint32 UncompressedSize = static_cast<uint32>(Data.str().length());

	constexpr auto CompressionMethod = Settings::MappingGenerator::CompressionMethod;

	/* Write 'CompressionMethod' to the compression byte */
	WriteToStream(InUsmap, static_cast<uint8>(CompressionMethod));

	size_t CompressedSize = UncompressedSize;
	void* CompressedBuffer = nullptr;

	switch (CompressionMethod)
	{
	case EUsmapCompressionMethod::ZStandard:
		CompressedSize = ZSTD_compressBound(UncompressedSize);
		CompressedBuffer = malloc(CompressedSize);
		CompressedSize = ZSTD_compress(CompressedBuffer, CompressedSize, Data.str().data(), UncompressedSize, ZSTD_maxCLevel());
		break;
	default:
		CompressedBuffer = malloc(CompressedSize);
		memcpy(CompressedBuffer, Data.str().data(), CompressedSize);
		break;
	}

	if constexpr (Settings::Debug::bShouldPrintMappingDebugData)
	{
		std::cerr << std::format("MappingGeneration: CompressedSize = 0x{0:X} (Dec: {0})\n", CompressedSize);
		std::cerr << std::format("MappingGeneration: DecompressedSize = 0x{0:X} (Dec: {0})\n\n", UncompressedSize);
	}

	/* Write compressed size */
	WriteToStream(InUsmap, static_cast<uint32>(CompressedSize));

	/* Write uncompressed size */
	WriteToStream(InUsmap, UncompressedSize);

	/* Header is done, now write the payload to the file */
	InUsmap.write(static_cast<const char*>(CompressedBuffer), static_cast<uint32>(CompressedSize));

	free(CompressedBuffer);
}

void MappingGenerator::Generate()
{
	NameCounter = 0x0;

	std::string MappingsFileName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName + ".usmap");

	FileNameHelper::MakeValidFileName(MappingsFileName);

	/* Open the stream as binary data, else ofstream will add \r after numbers that can be interpreted as \n. */
	const fs::path MappingsFilePath = MainFolder / MappingsFileName;
	fs::path TempMappingsFilePath = MappingsFilePath;
	TempMappingsFilePath += ".tmp";

	std::ofstream UsmapFile(TempMappingsFilePath, std::ios::binary);

	if (!UsmapFile)
	{
		std::cerr << std::format("MappingGeneration: failed to open '{}' for writing.\n", TempMappingsFilePath.string());
		return;
	}

	/* Generate the payload of the file, containing all of the names, enums and structs. */
	std::stringstream FileData = GenerateFileData();

	/* Generate the header, and write both header and payload into the file. */
	GenerateFileHeader(UsmapFile, FileData);

	UsmapFile.close();

	if (!UsmapFile.good())
	{
		std::cerr << std::format("MappingGeneration: failed while writing '{}'.\n", TempMappingsFilePath.string());
		return;
	}

	std::error_code FileError;
	fs::rename(TempMappingsFilePath, MappingsFilePath, FileError);

	if (FileError)
	{
		FileError.clear();
		fs::remove(MappingsFilePath, FileError);
		FileError.clear();
		fs::rename(TempMappingsFilePath, MappingsFilePath, FileError);
	}

	if (FileError)
		std::cerr << std::format("MappingGeneration: failed to publish '{}': {}\n", MappingsFilePath.string(), FileError.message());
}
