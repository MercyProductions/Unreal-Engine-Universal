#pragma once

#include "RuntimeSDK/RuntimeDatabase.h"

namespace RuntimeSDK
{
	bool Initialize();
	bool IsReady();
	RuntimeDatabase& GetDatabase();
	void Shutdown();
}
