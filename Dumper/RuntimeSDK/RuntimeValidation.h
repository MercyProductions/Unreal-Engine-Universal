#pragma once

#include "RuntimeSDK/RuntimeDatabase.h"

class RuntimeValidation
{
public:
	static bool Validate(const RuntimeDatabase& db);
};
