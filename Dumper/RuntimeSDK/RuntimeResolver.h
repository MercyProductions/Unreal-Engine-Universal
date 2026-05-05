#pragma once

#include "RuntimeSDK/RuntimeDatabase.h"

class RuntimeResolver
{
public:
	static bool BuildFromReflection(RuntimeDatabase& db);
};
