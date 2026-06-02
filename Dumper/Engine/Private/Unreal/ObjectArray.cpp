
#include <iostream>
#include <fstream>
#include <format>
#include <filesystem>
#include <algorithm>
#include <cstdint>

#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Utils.h"

#include "Platform.h"


namespace fs = std::filesystem;

namespace
{
	template<typename T>
	bool TryReadObjectArrayValue(const void* Address, T& OutValue)
	{
		if (!Address || Platform::IsBadReadPtr(Address))
			return false;

		__try
		{
			OutValue = *reinterpret_cast<const T*>(Address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

constexpr inline std::array FFixedUObjectArrayLayouts =
{
	FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
	{
		.ObjectsOffset = 0x0,								// 0x00
		.MaxObjectsOffset = sizeof(void*),					// 0x08 (64bit) OR 0x04 (32bit)
		.NumObjectsOffset = sizeof(void*) + sizeof(int)		// 0x0C (64bit) OR 0x08 (32bit)
	}
};

constexpr inline std::array FLegacyTArrayUObjectLayouts =
{
	FLegacyTArrayUObjectLayout // UE1/UE2/UE3 style TArray<UObject*>
	{
		.ObjectsOffset = 0x0,
		.NumObjectsOffset = sizeof(void*),
		.MaxObjectsOffset = sizeof(void*) + sizeof(int32)
	}
};

constexpr inline std::array FChunkedFixedUObjectArrayLayouts =
{
	FChunkedFixedUObjectArrayLayout // Default UE4.21 and above
	{
		.ObjectsOffset = 0x00,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x18,
		.NumChunksOffset = 0x1C,
	},
	FChunkedFixedUObjectArrayLayout // Back4Blood
	{
		.ObjectsOffset = 0x10, // last
		.MaxElementsOffset = 0x00,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x08,
		.NumChunksOffset = 0x0C,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	},
	FChunkedFixedUObjectArrayLayout // MindsEye
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x00, // first
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x10,
		.NumChunksOffset = 0x04,
	}
};

bool IsAddressValidGObjects(const uintptr_t Address, const FFixedUObjectArrayLayout& Layout)
{
	/* It is assumed that the FUObjectItem layout is constant amongst all games using FFixedUObjectArray for ObjObjects. */
	struct FUObjectItem
	{
		void* Object;
		uint8_t Pad[sizeof(void*) * 2];
	};

	void* Objects = nullptr;
	int32 MaxElements = 0;
	int32 NumElements = 0;
	if (!TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.ObjectsOffset), Objects)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.MaxObjectsOffset), MaxElements)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.NumObjectsOffset), NumElements))
	{
		return false;
	}

	FUObjectItem* ObjectsButDecrypted = reinterpret_cast<FUObjectItem*>(ObjectArray::DecryptPtr(Objects));

	if (NumElements > MaxElements)
		return false;

	if (MaxElements > 0x400000)
		return false;

	if (NumElements < 0x1000)
		return false;

	if (Platform::IsBadReadPtr(ObjectsButDecrypted))
		return false;

	FUObjectItem FifthItem{};
	if (!TryReadObjectArrayValue(&ObjectsButDecrypted[0x5], FifthItem)
		|| Platform::IsBadReadPtr(FifthItem.Object))
	{
		return false;
	}

	const uintptr_t FifthObject = reinterpret_cast<uintptr_t>(FifthItem.Object);
	int32 IndexOfFithobject = -1;
	if (!TryReadObjectArrayValue(reinterpret_cast<const void*>(FifthObject + sizeof(void*) + sizeof(int32)), IndexOfFithobject)) // FifthObject -> InternalIndex
		return false;

	if (IndexOfFithobject != 0x5)
		return false;

	return true;
}

bool IsAddressValidGObjects(const uintptr_t Address, const FLegacyTArrayUObjectLayout& Layout)
{
	void* Objects = nullptr;
	int32 NumElements = 0;
	int32 MaxElements = 0;
	if (!TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.ObjectsOffset), Objects)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.NumObjectsOffset), NumElements)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.MaxObjectsOffset), MaxElements))
	{
		return false;
	}

	uint8_t* ObjectsButDecrypted = ObjectArray::DecryptPtr(Objects);

	if (!ObjectsButDecrypted || Platform::IsBadReadPtr(ObjectsButDecrypted))
		return false;

	if (NumElements <= 0x100 || NumElements > MaxElements)
		return false;

	if (MaxElements > 0x400000)
		return false;

	int32 ValidObjects = 0;
	int32 MatchedIndexOffsets[0x81 / sizeof(int32)]{};
	const int32 SampleSpan = NumElements < 0x400 ? NumElements : 0x400;
	const int32 SampleStep = SampleSpan > 0x80 ? 0x8 : 0x1;

	for (int32 Index = 0; Index < SampleSpan && ValidObjects < 32; Index += SampleStep)
	{
		if (Index < 0 || Index >= NumElements)
			continue;

		void* Object = nullptr;
		if (!TryReadObjectArrayValue(reinterpret_cast<void**>(ObjectsButDecrypted) + Index, Object)
			|| !Object
			|| Platform::IsBadReadPtr(Object))
		{
			continue;
		}

		void* Vft = nullptr;
		if (!TryReadObjectArrayValue(Object, Vft) || !Vft || Platform::IsBadReadPtr(Vft))
			continue;

		if (!Platform::IsAddressInProcessRange(Vft))
			continue;

		ValidObjects++;

		for (int32 Offset = sizeof(int32); Offset <= 0x80; Offset += sizeof(int32))
		{
			int32 PossibleIndex = -1;
			if (!TryReadObjectArrayValue(reinterpret_cast<const uint8_t*>(Object) + Offset, PossibleIndex))
				continue;

			if (PossibleIndex == Index)
			{
				MatchedIndexOffsets[Offset / sizeof(int32)]++;
				break;
			}
		}
	}

	if (ValidObjects < 3)
		return false;

	for (const int32 MatchCount : MatchedIndexOffsets)
	{
		if (MatchCount >= 2)
			return true;
	}

	return ValidObjects >= 8;
}

bool IsAddressValidGObjects(const uintptr_t Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	void* Objects = nullptr;
	int32 MaxElements = 0;
	int32 NumElements = 0;
	int32 MaxChunks = 0;
	int32 NumChunks = 0;
	if (!TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.ObjectsOffset), Objects)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.MaxElementsOffset), MaxElements)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.NumElementsOffset), NumElements)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.MaxChunksOffset), MaxChunks)
		|| !TryReadObjectArrayValue(reinterpret_cast<const void*>(Address + Layout.NumChunksOffset), NumChunks))
	{
		return false;
	}

	void** ObjectsPtrButDecrypted = reinterpret_cast<void**>(ObjectArray::DecryptPtr(Objects));

	if (NumChunks > 0x14 || NumChunks < 0x1)
		return false;

	if (MaxChunks > 0x5FF || MaxChunks < 0x6)
		return false;

	if (NumElements <= 0x800 || MaxElements <= 0x10000)
		return false;

	if (NumElements > MaxElements || NumChunks > MaxChunks)
		return false;

	if ((MaxElements % 0x10) != 0)
		return false;

	const int32_t ElementsPerChunk = MaxElements / MaxChunks;

	if ((ElementsPerChunk % 0x10) != 0)
		return false;

	if (ElementsPerChunk < 0x8000 || ElementsPerChunk > 0x80000)
		return false;

	const bool bNumChunksFitsNumElements = ((NumElements / ElementsPerChunk) + 1) == NumChunks;

	if (!bNumChunksFitsNumElements)
		return false;

	const bool bMaxChunksFitsMaxElements = (MaxElements / ElementsPerChunk) == MaxChunks;

	if (!bMaxChunksFitsMaxElements)
		return false;

	if (!ObjectsPtrButDecrypted || Platform::IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;

	for (int i = 0; i < NumChunks; i++)
	{
		void* Chunk = nullptr;
		if (!TryReadObjectArrayValue(&ObjectsPtrButDecrypted[i], Chunk)
			|| !Chunk
			|| Platform::IsBadReadPtr(Chunk))
		{
			return false;
		}
	}

	return true;
}


void ObjectArray::InitializeFUObjectItem(uint8_t* FirstItemPtr)
{
	if (!FirstItemPtr || Platform::IsBadReadPtr(FirstItemPtr))
		return;

	for (int i = 0x0; i < 0x20; i += 4)
	{
		uint8_t* Candidate = nullptr;
		if (TryReadObjectArrayValue(FirstItemPtr + i, Candidate) && !Platform::IsBadReadPtr(Candidate))
		{
			FUObjectItemInitialOffset = i;
			break;
		}
	}

	for (int i = FUObjectItemInitialOffset + sizeof(void*); i <= 0x38; i += 4)
	{
		void* SecondObject = nullptr;
		void* ThirdObject = nullptr;
		if (!TryReadObjectArrayValue(FirstItemPtr + i, SecondObject)
			|| !TryReadObjectArrayValue(FirstItemPtr + (i * 2) - FUObjectItemInitialOffset, ThirdObject))
		{
			continue;
		}

		void* SecondVft = nullptr;
		void* ThirdVft = nullptr;
		if (!Platform::IsBadReadPtr(SecondObject)
			&& TryReadObjectArrayValue(SecondObject, SecondVft)
			&& !Platform::IsBadReadPtr(SecondVft)
			&& !Platform::IsBadReadPtr(ThirdObject)
			&& TryReadObjectArrayValue(ThirdObject, ThirdVft)
			&& !Platform::IsBadReadPtr(ThirdVft))
		{
			SizeOfFUObjectItem = i - FUObjectItemInitialOffset;
			break;
		}
	}

	Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
	Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;

	std::cerr << "Off::InSDK::ObjArray::FUObjectItemSize: " << Off::InSDK::ObjArray::FUObjectItemSize << "\n" << std::endl;
}

void ObjectArray::InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr)
{
	DecryptPtr = DecryptionFunction;
	DecryptionLambdaStr = DecryptionLambdaAsStr;
}


/* We don't speak about this function... */
void ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
	if (!bScanAllMemory)
	{
		std::cerr << "\nDumper-7 by me, you & him\n\n\n";
		std::cerr << "Searching for GObjects...\n\n";
	}

	auto MatchesAnyLayout = []<typename ArrayLayoutType, size_t Size>(const std::array<ArrayLayoutType, Size>& ObjectArrayLayouts, uintptr_t Address)
	{
		for (const ArrayLayoutType& Layout : ObjectArrayLayouts)
		{
			if (!IsAddressValidGObjects(Address, Layout))
				continue;

			if constexpr (std::is_same_v<ArrayLayoutType, FFixedUObjectArrayLayout>)
			{
				Off::FUObjectArray::bIsChunked = false;
				Off::FUObjectArray::bIsLegacyTArray = false;
				Off::FUObjectArray::FixedLayout = Layout;
			}
			else if constexpr (std::is_same_v<ArrayLayoutType, FChunkedFixedUObjectArrayLayout>)
			{
				Off::FUObjectArray::bIsChunked = true;
				Off::FUObjectArray::bIsLegacyTArray = false;
				Off::FUObjectArray::ChunkedFixedLayout = Layout;
			}
			else if constexpr (std::is_same_v<ArrayLayoutType, FLegacyTArrayUObjectLayout>)
			{
				Off::FUObjectArray::bIsChunked = false;
				Off::FUObjectArray::bIsLegacyTArray = true;
				Off::FUObjectArray::LegacyTArrayLayout = Layout;
			}

			return true;
		}
		
		return false;
	};

	enum class EObjectArrayKind
	{
		None,
		Fixed,
		Chunked,
		LegacyTArray
	};

	EObjectArrayKind ObjectArrayKind = EObjectArrayKind::None;
	auto IsAddressValidModernGObjects = [MatchesAnyLayout, &ObjectArrayKind](const void* CurrentAddress) -> bool
	{
		//std::cerr << "checking addr: " << CurrentAddress << "\n";
		if (MatchesAnyLayout(FFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			ObjectArrayKind = EObjectArrayKind::Fixed;
			return true;
		}
		else if (MatchesAnyLayout(FChunkedFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			ObjectArrayKind = EObjectArrayKind::Chunked;
			return true;
		}

		return false;
	};

	auto IsAddressValidLegacyGObjects = [MatchesAnyLayout, &ObjectArrayKind](const void* CurrentAddress) -> bool
	{
		if (MatchesAnyLayout(FLegacyTArrayUObjectLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			ObjectArrayKind = EObjectArrayKind::LegacyTArray;
			return true;
		}

		return false;
	};

	void* GObjectsAddress = nullptr;
	constexpr uint64_t FastSectionScanLimit = 0x800000; // 32 MiB at 4-byte granularity
	constexpr uint64_t SecondarySectionScanLimit = 0x400000; // 16 MiB at 4-byte granularity
	constexpr uint64_t BoundedAllSectionScanLimit = 0x1800000; // 96 MiB at 4-byte granularity

	auto ScanNamedSection = [&](const char* SectionName, uint64_t MaxIterations, const std::function<bool(void* Address)>& Callback, const char* PassName) -> void*
	{
		uint64_t Iterations = 0;
		std::cerr << "[ObjectArray] Scanning " << SectionName << " for " << PassName << " GObjects";
		if (MaxIterations != 0)
			std::cerr << " (bounded)";
		std::cerr << "...\n";

		void* Result = Platform::IterateSectionWithCallbackLimited(Platform::GetSectionInfo(SectionName, ModuleName), Callback, 0x4, 0x50, MaxIterations, &Iterations);
		if (!Result)
			std::cerr << "[ObjectArray] " << SectionName << " scan checked 0x" << std::hex << Iterations << std::dec << " candidates\n";

		return Result;
	};

	if (bScanAllMemory)
	{
		uint64_t Iterations = 0;
		std::cerr << "[ObjectArray] Running bounded all-section modern GObjects scan...\n";
		GObjectsAddress = Platform::IterateAllSectionsWithCallbackLimited(IsAddressValidModernGObjects, 0x4, 0x50, BoundedAllSectionScanLimit, &Iterations, ModuleName);
		if (!GObjectsAddress)
		{
			std::cerr << "[ObjectArray] Modern all-section scan checked 0x" << std::hex << Iterations << std::dec << " candidates\n";
			Iterations = 0;
			std::cerr << "[ObjectArray] Running bounded all-section legacy GObjects scan...\n";
			GObjectsAddress = Platform::IterateAllSectionsWithCallbackLimited(IsAddressValidLegacyGObjects, 0x4, 0x50, BoundedAllSectionScanLimit, &Iterations, ModuleName);
		}
		if (!GObjectsAddress)
			std::cerr << "[ObjectArray] Bounded all-section scan checked 0x" << std::hex << Iterations << std::dec << " candidates\n";
	}
	else
	{
		GObjectsAddress = ScanNamedSection(".data", FastSectionScanLimit, IsAddressValidModernGObjects, "modern");
		if (!GObjectsAddress)
			GObjectsAddress = ScanNamedSection(".rdata", SecondarySectionScanLimit, IsAddressValidModernGObjects, "modern");
		if (!GObjectsAddress)
			GObjectsAddress = ScanNamedSection(".bss", SecondarySectionScanLimit, IsAddressValidModernGObjects, "modern");

		if (!GObjectsAddress)
			GObjectsAddress = ScanNamedSection(".data", FastSectionScanLimit, IsAddressValidLegacyGObjects, "legacy");
		if (!GObjectsAddress)
			GObjectsAddress = ScanNamedSection(".rdata", SecondarySectionScanLimit, IsAddressValidLegacyGObjects, "legacy");
		if (!GObjectsAddress)
			GObjectsAddress = ScanNamedSection(".bss", SecondarySectionScanLimit, IsAddressValidLegacyGObjects, "legacy");
	}


	if (GObjectsAddress)
	{
		if (ObjectArrayKind == EObjectArrayKind::Fixed)
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			NumElementsPerChunk = -1;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);

			std::cerr << "Found FFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= Num())
					return nullptr;

				uint8_t* Objects = nullptr;
				if (!TryReadObjectArrayValue(ObjectsArray, Objects))
					return nullptr;

				uint8_t* ChunkPtr = DecryptPtr(Objects);
				if (!ChunkPtr || Platform::IsBadReadPtr(ChunkPtr))
					return nullptr;

				void* Result = nullptr;
				if (!TryReadObjectArrayValue(ChunkPtr + FUObjectItemOffset + (Index * FUObjectItemSize), Result))
					return nullptr;

				return Result;
			};

			uint8_t* Objects = nullptr;
			TryReadObjectArrayValue(GObjects + Off::FUObjectArray::GetObjectsOffset(), Objects);
			uint8_t* FirstItem = DecryptPtr(Objects);

			ObjectArray::InitializeFUObjectItem(FirstItem);
		}
		else if (ObjectArrayKind == EObjectArrayKind::LegacyTArray)
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			NumElementsPerChunk = 0;
			SizeOfFUObjectItem = sizeof(void*);
			FUObjectItemInitialOffset = 0x0;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);
			Off::InSDK::ObjArray::ChunkSize = 0;
			Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;
			Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;

			std::cerr << "Found legacy TArray<UObject*> GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";
			std::cerr << "Off::InSDK::ObjArray::FUObjectItemSize: " << std::dec << Off::InSDK::ObjArray::FUObjectItemSize << "\n" << std::endl;

			ByIndex = [](void* ObjectsArray, int32 Index, uint32, uint32, uint32) -> void*
			{
				if (Index < 0 || Index >= Num())
					return nullptr;

				uint8_t* Objects = nullptr;
				if (!TryReadObjectArrayValue(ObjectsArray, Objects) || !Objects)
					return nullptr;

				uint8_t* ObjectPtrArray = DecryptPtr(Objects);
				if (!ObjectPtrArray || Platform::IsBadReadPtr(ObjectPtrArray))
					return nullptr;

				void* Result = nullptr;
				if (!TryReadObjectArrayValue(reinterpret_cast<void**>(ObjectPtrArray) + Index, Result))
					return nullptr;

				return Result;
			};
		}
		else
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			
			const int32 MaxChunksValue = MaxChunks();
			NumElementsPerChunk = MaxChunksValue > 0 ? (Max() / MaxChunksValue) : 0;
			if (NumElementsPerChunk == 0)
			{
				std::cerr << "Found chunked GObjects, but chunk size could not be resolved safely\n";
				GObjects = nullptr;
				return;
			}
			Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;

			SizeOfFUObjectItem = sizeof(void*) + sizeof(int32) + sizeof(int32);
			FUObjectItemInitialOffset = 0x0;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);

			std::cerr << "Found FChunkedFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= Num() || PerChunk == 0)
					return nullptr;

				const int32 ChunkIndex = Index / PerChunk;
				const int32 InChunkIdx = Index % PerChunk;

				uint8_t* Objects = nullptr;
				if (!TryReadObjectArrayValue(ObjectsArray, Objects))
					return nullptr;

				uint8_t* ChunkPtr = DecryptPtr(Objects);
				if (!ChunkPtr || Platform::IsBadReadPtr(ChunkPtr))
					return nullptr;

				uint8_t* Chunk = nullptr;
				if (!TryReadObjectArrayValue(reinterpret_cast<uint8_t**>(ChunkPtr) + ChunkIndex, Chunk)
					|| !Chunk
					|| Platform::IsBadReadPtr(Chunk))
				{
					return nullptr;
				}
				uint8_t* ItemPtr = Chunk + (InChunkIdx * FUObjectItemSize);

				void* Result = nullptr;
				if (!TryReadObjectArrayValue(ItemPtr + FUObjectItemOffset, Result))
					return nullptr;

				return Result;
			};
			
			uint8_t* Objects = nullptr;
			TryReadObjectArrayValue(GObjects + Off::FUObjectArray::GetObjectsOffset(), Objects);
			uint8_t* ChunksPtr = DecryptPtr(Objects);

			uint8_t* FirstChunk = nullptr;
			TryReadObjectArrayValue(ChunksPtr, FirstChunk);
			ObjectArray::InitializeFUObjectItem(FirstChunk);
		}

		return;
	}

	if (!bScanAllMemory)
	{
		ObjectArray::Init(true);
		return;
	}

	if (GObjects == nullptr)
	{
		std::cerr << "\nGObjects couldn't be found, please overwrite the offset in Generator.cpp.\n\n\n";
		Sleep(10000);
		exit(1);
	}
}

void ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	std::cerr << "GObjects: 0x" << (void*)GObjects << "\n" << std::endl;

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::bIsLegacyTArray = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		uint8_t* Objects = nullptr;
		if (!TryReadObjectArrayValue(ObjectsArray, Objects) || !Objects)
			return nullptr;

		uint8_t* ItemPtr = Objects + (Index * FUObjectItemSize);

		void* Result = nullptr;
		if (!TryReadObjectArrayValue(ItemPtr + FUObjectItemOffset, Result))
			return nullptr;

		return Result;
	};

	uint8_t* Objects = nullptr;
	TryReadObjectArrayValue(GObjects + Off::FUObjectArray::GetObjectsOffset(), Objects);
	uint8_t* ChunksPtr = DecryptPtr(Objects);

	std::cerr << "Overwrote FFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	uint8_t* FirstItem = nullptr;
	TryReadObjectArrayValue(ChunksPtr, FirstItem);
	ObjectArray::InitializeFUObjectItem(FirstItem);
}

void ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::bIsLegacyTArray = false;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	NumElementsPerChunk = ElementsPerChunk;
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num() || PerChunk == 0)
			return nullptr;

		const int32 ChunkIndex = Index / PerChunk;
		const int32 InChunkIdx = Index % PerChunk;

		uint8_t** Chunks = nullptr;
		if (!TryReadObjectArrayValue(ObjectsArray, Chunks) || !Chunks)
			return nullptr;

		uint8_t* Chunk = nullptr;
		if (!TryReadObjectArrayValue(Chunks + ChunkIndex, Chunk)
			|| !Chunk
			|| Platform::IsBadReadPtr(Chunk))
		{
			return nullptr;
		}
		uint8_t* ItemPtr = reinterpret_cast<uint8_t*>(Chunk) + (InChunkIdx * FUObjectItemSize);

		void* Result = nullptr;
		if (!TryReadObjectArrayValue(ItemPtr + FUObjectItemOffset, Result))
			return nullptr;

		return Result;
	};

	uint8_t* Objects = nullptr;
	TryReadObjectArrayValue(GObjects + Off::FUObjectArray::GetObjectsOffset(), Objects);
	uint8_t* ChunksPtr = DecryptPtr(Objects);

	std::cerr << "Overwrote FChunkedFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	uint8_t* FirstChunk = nullptr;
	TryReadObjectArrayValue(ChunksPtr, FirstChunk);
	ObjectArray::InitializeFUObjectItem(FirstChunk);
}

void ObjectArray::Init(int32 GObjectsOffset, const FLegacyTArrayUObjectLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::bIsLegacyTArray = true;
	Off::FUObjectArray::LegacyTArrayLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FLegacyTArrayUObjectLayouts[0];

	NumElementsPerChunk = 0;
	SizeOfFUObjectItem = sizeof(void*);
	FUObjectItemInitialOffset = 0x0;

	Off::InSDK::ObjArray::ChunkSize = 0;
	Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;
	Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32, uint32, uint32) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		uint8_t* Objects = nullptr;
		if (!TryReadObjectArrayValue(ObjectsArray, Objects) || !Objects)
			return nullptr;

		uint8_t* ObjectPtrArray = DecryptPtr(Objects);
		if (!ObjectPtrArray || Platform::IsBadReadPtr(ObjectPtrArray))
			return nullptr;

		void* Result = nullptr;
		if (!TryReadObjectArrayValue(reinterpret_cast<void**>(ObjectPtrArray) + Index, Result))
			return nullptr;

		return Result;
	};

	std::cerr << "Overwrote legacy TArray<UObject*> GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;
	std::cerr << "Off::InSDK::ObjArray::FUObjectItemSize: " << std::dec << Off::InSDK::ObjArray::FUObjectItemSize << "\n" << std::endl;
}

void ObjectArray::DumpObjects(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}
	}

	DumpStream.close();
}

void ObjectArray::DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump-WithProperties.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}

		if (Object.IsA(EClassCastFlags::Struct))
		{
			for (UEProperty Prop : Object.Cast<UEStruct>().GetProperties())
			{
				DumpStream << std::format("[{:08X}] {{{}}}     {} {}\n", Prop.GetOffset(), Prop.GetAddress(), Prop.GetPropClassName(), Prop.GetName());
			}
		}
	}

	DumpStream.close();
}


int32 ObjectArray::Num()
{
	int32 Value = 0;
	const int32 Offset = Off::FUObjectArray::GetNumElementsOffset();
	if (!GObjects || Offset < 0 || !TryReadObjectArrayValue(GObjects + Offset, Value))
		return 0;

	return std::clamp(Value, 0, 0x800000);
}

int32 ObjectArray::Max()
{
	int32 Value = 0;
	const int32 Offset = Off::FUObjectArray::GetMaxElementsOffset();
	if (!GObjects || Offset < 0 || !TryReadObjectArrayValue(GObjects + Offset, Value))
		return 0;

	return std::clamp(Value, 0, 0x800000);
}

int32 ObjectArray::NumChunks()
{
	int32 Value = 0;
	const int32 Offset = Off::FUObjectArray::GetNumChunksOffset();
	if (!GObjects || Offset < 0 || !TryReadObjectArrayValue(GObjects + Offset, Value))
		return 0;

	return std::clamp(Value, 0, 0x10000);
}

int32 ObjectArray::MaxChunks()
{
	int32 Value = 0;
	const int32 Offset = Off::FUObjectArray::GetMaxChunksOffset();
	if (!GObjects || Offset < 0 || !TryReadObjectArrayValue(GObjects + Offset, Value))
		return 0;

	return std::clamp(Value, 0, 0x10000);
}

template<typename UEType>
static UEType ObjectArray::GetByIndex(int32 Index)
{
	if (!GObjects || !ByIndex || Index < 0 || Index >= Num())
		return UEType();

	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
		if (Object.IsA(RequiredType) && Object.GetFullName() == FullName)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFast(const std::string& Name, EClassCastFlags RequiredType)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.IsA(RequiredType) && Object.GetName() == Name)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
static UEType ObjectArray::FindObjectFastInOuter(const std::string& Name, std::string Outer)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.GetName() == Name && Object.GetOuter().GetName() == Outer)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

UEStruct ObjectArray::FindStruct(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEStruct ObjectArray::FindStructFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEClass ObjectArray::FindClass(const std::string& FullName)
{
	return FindObject<UEClass>(FullName, EClassCastFlags::Class);
}

UEClass ObjectArray::FindClassFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Class);
}

ObjectArray::ObjectsIterator ObjectArray::begin()
{
	return ObjectsIterator();
}
ObjectArray::ObjectsIterator ObjectArray::end()
{
	return ObjectsIterator(Num());
}


ObjectArray::ObjectsIterator::ObjectsIterator(int32 StartIndex)
	: CurrentIndex(StartIndex), CurrentObject(ObjectArray::GetByIndex(StartIndex))
{
}

UEObject ObjectArray::ObjectsIterator::operator*() const
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);

	while (!CurrentObject && CurrentIndex < (ObjectArray::Num() - 1))
	{
		CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);
	}

	if (!CurrentObject && CurrentIndex == (ObjectArray::Num() - 1)) [[unlikely]]
		CurrentIndex++;

	return *this;
}

bool ObjectArray::ObjectsIterator::operator==(const ObjectsIterator& Other) const
{
	return CurrentIndex == Other.CurrentIndex;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	return CurrentIndex != Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

bool AllFieldIterator::operator!=(const AllFieldIterator& Other) const
{
	return CurrentObject != Other.CurrentObject || PropertyIndex != Other.PropertyIndex;
}

AllFieldIterator& AllFieldIterator::operator++()
{
	if (CurrenStructHasMoreMembers())
	{
		PropertyIndex++;

		return *this;
	}

	IterateToNextStructWithMembers();

	return *this;
}

UEProperty AllFieldIterator::operator*() const
{
	return Fields[PropertyIndex];
}


void AllFieldIterator::IterateToNextStruct()
{
	if (IsEndIterator())
		return;

	++CurrentObject;

	while (CurrentObject != ObjectEndIterator && !IsCurrentObjectStruct())
		++CurrentObject;
}
void AllFieldIterator::IterateToNextStructWithMembers()
{
	// Loop, in case we meet a struct wihtout any properties
	while (!CurrenStructHasMoreMembers())
	{
		IterateToNextStruct();
		PropertyIndex = 0;

		if (IsEndIterator())
			return;

		Fields = GetCurrentStruct().GetProperties();
	}
}


/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
template UEObject ObjectArray::FindObject<UEObject>(const std::string& FullName, EClassCastFlags RequiredType);
template UEField ObjectArray::FindObject<UEField>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnum ObjectArray::FindObject<UEEnum>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStruct ObjectArray::FindObject<UEStruct>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClass ObjectArray::FindObject<UEClass>(const std::string& FullName, EClassCastFlags RequiredType);
template UEFunction ObjectArray::FindObject<UEFunction>(const std::string& FullName, EClassCastFlags RequiredType);
template UEProperty ObjectArray::FindObject<UEProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEByteProperty ObjectArray::FindObject<UEByteProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEBoolProperty ObjectArray::FindObject<UEBoolProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEObjectProperty ObjectArray::FindObject<UEObjectProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClassProperty ObjectArray::FindObject<UEClassProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStructProperty ObjectArray::FindObject<UEStructProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEArrayProperty ObjectArray::FindObject<UEArrayProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEMapProperty ObjectArray::FindObject<UEMapProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UESetProperty ObjectArray::FindObject<UESetProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnumProperty ObjectArray::FindObject<UEEnumProperty>(const std::string& FullName, EClassCastFlags RequiredType);

template UEObject ObjectArray::FindObjectFast<UEObject>(const std::string& FullName, EClassCastFlags RequiredType);
template UEField ObjectArray::FindObjectFast<UEField>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnum ObjectArray::FindObjectFast<UEEnum>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStruct ObjectArray::FindObjectFast<UEStruct>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClass ObjectArray::FindObjectFast<UEClass>(const std::string& FullName, EClassCastFlags RequiredType);
template UEFunction ObjectArray::FindObjectFast<UEFunction>(const std::string& FullName, EClassCastFlags RequiredType);
template UEProperty ObjectArray::FindObjectFast<UEProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEByteProperty ObjectArray::FindObjectFast<UEByteProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEBoolProperty ObjectArray::FindObjectFast<UEBoolProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEObjectProperty ObjectArray::FindObjectFast<UEObjectProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClassProperty ObjectArray::FindObjectFast<UEClassProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStructProperty ObjectArray::FindObjectFast<UEStructProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEArrayProperty ObjectArray::FindObjectFast<UEArrayProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEMapProperty ObjectArray::FindObjectFast<UEMapProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UESetProperty ObjectArray::FindObjectFast<UESetProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnumProperty ObjectArray::FindObjectFast<UEEnumProperty>(const std::string& FullName, EClassCastFlags RequiredType);

template UEObject ObjectArray::FindObjectFastInOuter<UEObject>(const std::string& FullName, std::string Outer);
template UEField ObjectArray::FindObjectFastInOuter<UEField>(const std::string& FullName, std::string Outer);
template UEEnum ObjectArray::FindObjectFastInOuter<UEEnum>(const std::string& FullName, std::string Outer);
template UEStruct ObjectArray::FindObjectFastInOuter<UEStruct>(const std::string& FullName, std::string Outer);
template UEClass ObjectArray::FindObjectFastInOuter<UEClass>(const std::string& FullName, std::string Outer);
template UEFunction ObjectArray::FindObjectFastInOuter<UEFunction>(const std::string& FullName, std::string Outer);
template UEProperty ObjectArray::FindObjectFastInOuter<UEProperty>(const std::string& FullName, std::string Outer);
template UEByteProperty ObjectArray::FindObjectFastInOuter<UEByteProperty>(const std::string& FullName, std::string Outer);
template UEBoolProperty ObjectArray::FindObjectFastInOuter<UEBoolProperty>(const std::string& FullName, std::string Outer);
template UEObjectProperty ObjectArray::FindObjectFastInOuter<UEObjectProperty>(const std::string& FullName, std::string Outer);
template UEClassProperty ObjectArray::FindObjectFastInOuter<UEClassProperty>(const std::string& FullName, std::string Outer);
template UEStructProperty ObjectArray::FindObjectFastInOuter<UEStructProperty>(const std::string& FullName, std::string Outer);
template UEArrayProperty ObjectArray::FindObjectFastInOuter<UEArrayProperty>(const std::string& FullName, std::string Outer);
template UEMapProperty ObjectArray::FindObjectFastInOuter<UEMapProperty>(const std::string& FullName, std::string Outer);
template UESetProperty ObjectArray::FindObjectFastInOuter<UESetProperty>(const std::string& FullName, std::string Outer);
template UEEnumProperty ObjectArray::FindObjectFastInOuter<UEEnumProperty>(const std::string& FullName, std::string Outer);

template UEObject ObjectArray::GetByIndex<UEObject>(int32 Index);
template UEField ObjectArray::GetByIndex<UEField>(int32 Index);
template UEEnum ObjectArray::GetByIndex<UEEnum>(int32 Index);
template UEStruct ObjectArray::GetByIndex<UEStruct>(int32 Index);
template UEClass ObjectArray::GetByIndex<UEClass>(int32 Index);
template UEFunction ObjectArray::GetByIndex<UEFunction>(int32 Index);
template UEProperty ObjectArray::GetByIndex<UEProperty>(int32 Index);
template UEByteProperty ObjectArray::GetByIndex<UEByteProperty>(int32 Index);
template UEBoolProperty ObjectArray::GetByIndex<UEBoolProperty>(int32 Index);
template UEObjectProperty ObjectArray::GetByIndex<UEObjectProperty>(int32 Index);
template UEClassProperty ObjectArray::GetByIndex<UEClassProperty>(int32 Index);
template UEStructProperty ObjectArray::GetByIndex<UEStructProperty>(int32 Index);
template UEArrayProperty ObjectArray::GetByIndex<UEArrayProperty>(int32 Index);
template UEMapProperty ObjectArray::GetByIndex<UEMapProperty>(int32 Index);
template UESetProperty ObjectArray::GetByIndex<UESetProperty>(int32 Index);
template UEEnumProperty ObjectArray::GetByIndex<UEEnumProperty>(int32 Index);
