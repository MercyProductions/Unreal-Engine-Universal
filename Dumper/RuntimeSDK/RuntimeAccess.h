#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include "RuntimeSDK/RuntimeSDK.h"

namespace RuntimeAccess
{
	template<typename T>
	T ReadValue(uintptr_t base, int32_t offset, T fallback = {})
	{
		if (!base || offset < 0)
			return fallback;

		return *reinterpret_cast<T*>(base + offset);
	}

	template<typename T>
	T* ReadPtr(uintptr_t base, int32_t offset)
	{
		if (!base || offset < 0)
			return nullptr;

		return *reinterpret_cast<T**>(base + offset);
	}

	int32_t Offset(const std::string& name);
	int32_t OffsetAny(std::initializer_list<std::string> names);
}
