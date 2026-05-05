#include "RuntimeSDK/RuntimeAccess.h"

namespace RuntimeAccess
{
	int32_t Offset(const std::string& name)
	{
		return RuntimeSDK::GetDatabase().GetOffset(name);
	}

	int32_t OffsetAny(std::initializer_list<std::string> names)
	{
		return RuntimeSDK::GetDatabase().GetOffsetAny(names);
	}
}
