#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "Platform.h"
#include "RuntimeSDK/RuntimeSDK.h"

namespace RuntimeAccess
{
	inline bool IsReadableRange(uintptr_t address, size_t size)
	{
		if (!address || size == 0)
			return false;

		const uintptr_t last = address + size - 1;
		if (last < address)
			return false;

		return !Platform::IsBadReadPtr(reinterpret_cast<const void*>(address))
			&& !Platform::IsBadReadPtr(reinterpret_cast<const void*>(last));
	}

	template<typename T>
	T ReadValue(uintptr_t base, int32_t offset, T fallback = {})
	{
		if (!base || offset < 0)
			return fallback;

		const uintptr_t address = base + static_cast<uintptr_t>(offset);
		if (address < base || !IsReadableRange(address, sizeof(T)))
			return fallback;

		__try
		{
			return *reinterpret_cast<const T*>(address);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return fallback;
		}
	}

	template<typename T>
	T* ReadPtr(uintptr_t base, int32_t offset)
	{
		if (!base || offset < 0)
			return nullptr;

		return ReadValue<T*>(base, offset, nullptr);
	}

	int32_t Offset(const std::string& name);
	int32_t OffsetAny(std::initializer_list<std::string> names);
}
