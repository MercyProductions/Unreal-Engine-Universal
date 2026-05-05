#include "RuntimeSDK/RuntimeDatabase.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <unordered_set>

namespace
{
	bool IsClassPrefix(char value)
	{
		return value == 'A' || value == 'U' || value == 'F';
	}

	std::string TrimTypePrefix(std::string value)
	{
		constexpr const char* prefixes[] = { "class ", "struct ", "enum " };

		bool changed = true;
		while (changed)
		{
			changed = false;
			for (const char* prefix : prefixes)
			{
				const size_t length = std::strlen(prefix);
				if (value.size() > length && value.rfind(prefix, 0) == 0)
				{
					value.erase(0, length);
					changed = true;
				}
			}
		}

		return value;
	}

	std::string StripFullNameObjectType(const std::string& fullName)
	{
		const size_t space = fullName.find(' ');
		if (space == std::string::npos || space + 1 >= fullName.size())
			return fullName;

		return fullName.substr(space + 1);
	}

	std::string LastPathToken(const std::string& value)
	{
		const size_t dot = value.find_last_of('.');
		if (dot != std::string::npos && dot + 1 < value.size())
			return value.substr(dot + 1);

		const size_t scope = value.find_last_of(':');
		if (scope != std::string::npos && scope + 1 < value.size())
			return value.substr(scope + 1);

		return value;
	}

	void AddUnique(std::vector<std::string>& values, const std::string& value)
	{
		if (value.empty() || std::find(values.begin(), values.end(), value) != values.end())
			return;

		values.push_back(value);
	}

	std::string StripLeadingClassPrefix(const std::string& value)
	{
		if (value.rfind("AI", 0) == 0 || value.rfind("UInt", 0) == 0)
			return value;

		if (value.size() > 1 && IsClassPrefix(value[0]) && std::isupper(static_cast<unsigned char>(value[1])))
			return value.substr(1);

		return value;
	}

	std::vector<std::string> BuildNameAliases(const std::string& name, const std::string& fullName = {})
	{
		std::vector<std::string> aliases;

		const std::string cleanName = TrimTypePrefix(name);
		AddUnique(aliases, cleanName);
		AddUnique(aliases, StripLeadingClassPrefix(cleanName));

		if (!fullName.empty())
		{
			const std::string pathName = StripFullNameObjectType(fullName);
			AddUnique(aliases, pathName);
			AddUnique(aliases, LastPathToken(pathName));
			AddUnique(aliases, StripLeadingClassPrefix(LastPathToken(pathName)));

			const size_t dot = pathName.find_last_of('.');
			if (dot != std::string::npos)
			{
				const std::string packageName = pathName.substr(0, dot);
				const std::string leaf = pathName.substr(dot + 1);
				AddUnique(aliases, packageName + "." + leaf);
				AddUnique(aliases, packageName + "." + StripLeadingClassPrefix(leaf));
			}
		}

		return aliases;
	}

	bool SplitScopedMember(const std::string& value, std::string& ownerName, std::string& memberName)
	{
		const size_t scope = value.rfind("::");
		if (scope == std::string::npos || scope == 0 || scope + 2 >= value.size())
			return false;

		ownerName = value.substr(0, scope);
		memberName = value.substr(scope + 2);
		return true;
	}

	std::vector<std::string> BuildMemberAliases(const std::string& ownerName, const std::string& ownerFullName, const std::string& memberName)
	{
		std::vector<std::string> aliases;
		for (const std::string& ownerAlias : BuildNameAliases(ownerName, ownerFullName))
			AddUnique(aliases, ownerAlias + "::" + memberName);

		return aliases;
	}

	void LogDuplicate(const char* kind, const std::string& key, const std::string& existing, const std::string& incoming)
	{
		std::cerr << "[RuntimeSDK] Duplicate " << kind << " key ignored: " << key
			<< " existing=" << existing << " incoming=" << incoming << "\n";
	}
}

void RuntimeDatabase::Clear()
{
	globals = {};
	structsByName.clear();
	enumsByName.clear();
	functionsByFullName.clear();
	propertiesByFullName.clear();
}

void RuntimeDatabase::AddStruct(RuntimeStructInfo info)
{
	for (const RuntimePropertyInfo& property : info.properties)
	{
		AddProperty(property);
		for (const std::string& key : BuildMemberAliases(info.name, info.fullName, property.propertyName))
			InsertPropertyKey(key, property);
	}

	for (const RuntimeFunctionInfo& function : info.functions)
	{
		AddFunction(function);
		for (const std::string& key : BuildMemberAliases(info.name, info.fullName, function.functionName))
			InsertFunctionKey(key, function);
	}

	for (const std::string& key : BuildNameAliases(info.name, info.fullName))
		InsertStructKey(key, info);
}

void RuntimeDatabase::AddEnum(RuntimeEnumInfo info)
{
	for (const std::string& key : BuildNameAliases(info.name, info.fullName))
		InsertEnumKey(key, info);
}

void RuntimeDatabase::AddFunction(RuntimeFunctionInfo info)
{
	InsertFunctionKey(info.fullName, info);
	for (const std::string& key : BuildMemberAliases(info.ownerName, {}, info.functionName))
		InsertFunctionKey(key, info);
}

void RuntimeDatabase::AddProperty(RuntimePropertyInfo info)
{
	InsertPropertyKey(info.fullName, info);
	for (const std::string& key : BuildMemberAliases(info.ownerName, {}, info.propertyName))
		InsertPropertyKey(key, info);
}

const RuntimeStructInfo* RuntimeDatabase::FindStruct(const std::string& name) const
{
	const auto it = structsByName.find(name);
	return it != structsByName.end() ? &it->second : nullptr;
}

const RuntimeEnumInfo* RuntimeDatabase::FindEnum(const std::string& name) const
{
	const auto it = enumsByName.find(name);
	return it != enumsByName.end() ? &it->second : nullptr;
}

const RuntimeFunctionInfo* RuntimeDatabase::FindFunction(const std::string& fullName) const
{
	const auto it = functionsByFullName.find(fullName);
	if (it != functionsByFullName.end())
		return &it->second;

	std::string ownerName;
	std::string functionName;
	if (!SplitScopedMember(fullName, ownerName, functionName))
		return nullptr;

	std::unordered_set<std::string> visited;
	for (const RuntimeStructInfo* owner = FindStruct(ownerName); owner != nullptr && !owner->superName.empty();)
	{
		if (!visited.insert(owner->name).second)
			break;

		owner = FindStruct(owner->superName);
		if (!owner)
			break;

		const auto inherited = functionsByFullName.find(owner->name + "::" + functionName);
		if (inherited != functionsByFullName.end())
			return &inherited->second;
	}

	return nullptr;
}

const RuntimePropertyInfo* RuntimeDatabase::FindProperty(const std::string& fullName) const
{
	const auto it = propertiesByFullName.find(fullName);
	if (it != propertiesByFullName.end())
		return &it->second;

	std::string ownerName;
	std::string propertyName;
	if (!SplitScopedMember(fullName, ownerName, propertyName))
		return nullptr;

	std::unordered_set<std::string> visited;
	for (const RuntimeStructInfo* owner = FindStruct(ownerName); owner != nullptr && !owner->superName.empty();)
	{
		if (!visited.insert(owner->name).second)
			break;

		owner = FindStruct(owner->superName);
		if (!owner)
			break;

		const auto inherited = propertiesByFullName.find(owner->name + "::" + propertyName);
		if (inherited != propertiesByFullName.end())
			return &inherited->second;
	}

	return nullptr;
}

int32_t RuntimeDatabase::GetOffset(const std::string& fullName) const
{
	if (fullName == "ULevel::Actors" || fullName == "Level::Actors" || fullName == "Engine.Level::Actors")
		return globals.uLevelActors;

	const RuntimePropertyInfo* property = FindProperty(fullName);
	return property ? property->offset : -1;
}

int32_t RuntimeDatabase::GetOffsetAny(std::initializer_list<std::string> names) const
{
	for (const std::string& name : names)
	{
		const int32_t offset = GetOffset(name);
		if (offset >= 0)
			return offset;
	}

	return -1;
}

bool RuntimeDatabase::HasStruct(const std::string& name) const
{
	return FindStruct(name) != nullptr;
}

bool RuntimeDatabase::HasProperty(const std::string& fullName) const
{
	return FindProperty(fullName) != nullptr;
}

RuntimeGlobalOffsets& RuntimeDatabase::Globals()
{
	return globals;
}

const RuntimeGlobalOffsets& RuntimeDatabase::Globals() const
{
	return globals;
}

bool RuntimeDatabase::InsertStructKey(const std::string& key, const RuntimeStructInfo& info)
{
	auto [it, inserted] = structsByName.emplace(key, info);
	if (!inserted && it->second.address != info.address)
	{
		LogDuplicate("struct", key, it->second.fullName, info.fullName);
		return false;
	}

	return inserted;
}

bool RuntimeDatabase::InsertEnumKey(const std::string& key, const RuntimeEnumInfo& info)
{
	auto [it, inserted] = enumsByName.emplace(key, info);
	if (!inserted && it->second.address != info.address)
	{
		LogDuplicate("enum", key, it->second.fullName, info.fullName);
		return false;
	}

	return inserted;
}

bool RuntimeDatabase::InsertFunctionKey(const std::string& key, const RuntimeFunctionInfo& info)
{
	auto [it, inserted] = functionsByFullName.emplace(key, info);
	if (!inserted && it->second.address != info.address)
	{
		LogDuplicate("function", key, it->second.fullName, info.fullName);
		return false;
	}

	return inserted;
}

bool RuntimeDatabase::InsertPropertyKey(const std::string& key, const RuntimePropertyInfo& info)
{
	auto [it, inserted] = propertiesByFullName.emplace(key, info);
	if (!inserted && (it->second.address != info.address || it->second.offset != info.offset))
	{
		LogDuplicate("property", key, it->second.fullName, info.fullName);
		return false;
	}

	return inserted;
}
