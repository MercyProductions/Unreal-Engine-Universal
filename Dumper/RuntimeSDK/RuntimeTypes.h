#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class RuntimeEngineGeneration : uint8_t
{
	Unknown = 0,
	UnrealEngine1,
	UnrealEngine2,
	UnrealEngine3,
	UnrealEngine4,
	UnrealEngine5,
	UnrealEngine6
};

struct RuntimePropertyInfo
{
	std::string ownerName;
	std::string propertyName;
	std::string fullName;
	std::string typeName;
	std::string category;

	int32_t offset = -1;
	int32_t size = 0;
	int32_t arrayDim = 1;

	bool isBitfield = false;
	int32_t bitIndex = -1;
	int32_t bitMask = 0;

	uint64_t propertyFlags = 0;
	uintptr_t address = 0;
};

struct RuntimeFunctionInfo
{
	std::string ownerName;
	std::string functionName;
	std::string fullName;

	uintptr_t address = 0;
	uintptr_t execOffset = 0;
	uint32_t functionFlags = 0;

	std::vector<RuntimePropertyInfo> params;
	RuntimePropertyInfo returnValue;
	bool hasReturnValue = false;
};

struct RuntimeStructInfo
{
	std::string name;
	std::string fullName;
	std::string superName;

	uintptr_t address = 0;

	bool isClass = false;
	bool isStruct = false;
	bool isFunction = false;
	bool isInterface = false;

	int32_t size = 0;
	int32_t alignment = 0;

	std::vector<RuntimePropertyInfo> properties;
	std::vector<RuntimeFunctionInfo> functions;
};

struct RuntimeEnumInfo
{
	std::string name;
	std::string fullName;
	std::string underlyingType;

	uintptr_t address = 0;

	std::vector<std::pair<std::string, int64_t>> values;
};

struct RuntimeGlobalOffsets
{
	uintptr_t imageBase = 0;

	RuntimeEngineGeneration engineGeneration = RuntimeEngineGeneration::Unknown;
	std::string engineGenerationName;

	bool usesFProperty = false;
	bool usesFField = false;
	bool legacyRuntime = false;

	int32_t gObjects = -1;
	int32_t gNames = -1;
	int32_t gWorld = -1;

	int32_t processEvent = -1;
	int32_t processEventIndex = -1;

	int32_t appendString = -1;
	int32_t toString = -1;

	int32_t uLevelActors = -1;
};
