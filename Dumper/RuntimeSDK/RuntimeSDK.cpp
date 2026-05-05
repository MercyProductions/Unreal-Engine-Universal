#include "RuntimeSDK/RuntimeSDK.h"

#include <atomic>
#include <iostream>

#include "RuntimeSDK/RuntimeResolver.h"
#include "RuntimeSDK/RuntimeValidation.h"

namespace
{
	RuntimeDatabase gDatabase;
	std::atomic_bool gReady = false;
}

namespace RuntimeSDK
{
	bool Initialize()
	{
		if (gReady.load())
			return true;

		std::cerr << "[RuntimeSDK] Initializing\n";

		if (!RuntimeResolver::BuildFromReflection(gDatabase))
		{
			std::cerr << "[RuntimeSDK] Initialization failed: reflection build did not produce runtime metadata\n";
			gReady.store(false);
			return false;
		}

		const bool valid = RuntimeValidation::Validate(gDatabase);
		gReady.store(valid);
		return valid;
	}

	bool IsReady()
	{
		return gReady.load();
	}

	RuntimeDatabase& GetDatabase()
	{
		return gDatabase;
	}

	void Shutdown()
	{
		gReady.store(false);
		gDatabase.Clear();
	}
}
