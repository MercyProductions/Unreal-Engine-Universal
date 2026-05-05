#pragma once

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "RuntimeSDK/RuntimeTypes.h"

class RuntimeDatabase
{
public:
	void Clear();

	void AddStruct(RuntimeStructInfo info);
	void AddEnum(RuntimeEnumInfo info);
	void AddFunction(RuntimeFunctionInfo info);
	void AddProperty(RuntimePropertyInfo info);

	const RuntimeStructInfo* FindStruct(const std::string& name) const;
	const RuntimeEnumInfo* FindEnum(const std::string& name) const;
	const RuntimeFunctionInfo* FindFunction(const std::string& fullName) const;
	const RuntimePropertyInfo* FindProperty(const std::string& fullName) const;

	int32_t GetOffset(const std::string& fullName) const;
	int32_t GetOffsetAny(std::initializer_list<std::string> names) const;

	bool HasStruct(const std::string& name) const;
	bool HasProperty(const std::string& fullName) const;

	RuntimeGlobalOffsets& Globals();
	const RuntimeGlobalOffsets& Globals() const;

private:
	bool InsertStructKey(const std::string& key, const RuntimeStructInfo& info);
	bool InsertEnumKey(const std::string& key, const RuntimeEnumInfo& info);
	bool InsertFunctionKey(const std::string& key, const RuntimeFunctionInfo& info);
	bool InsertPropertyKey(const std::string& key, const RuntimePropertyInfo& info);

	RuntimeGlobalOffsets globals;

	std::unordered_map<std::string, RuntimeStructInfo> structsByName;
	std::unordered_map<std::string, RuntimeEnumInfo> enumsByName;
	std::unordered_map<std::string, RuntimeFunctionInfo> functionsByFullName;
	std::unordered_map<std::string, RuntimePropertyInfo> propertiesByFullName;
};
