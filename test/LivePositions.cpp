#include "LivePositions.hpp"
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <mutex>

namespace test_util {

namespace {

std::mutex mutex;
std::unique_ptr<TwsPositions> shared;

class LivePositionsEnvironment : public ::testing::Environment
{
	public:
	void TearDown() override
	{
		std::scoped_lock lock(mutex);
		shared.reset();
	}
};

const auto* environment = ::testing::AddGlobalTestEnvironment(new LivePositionsEnvironment);

} // namespace

TwsPositions* livePositions(const ib_fix::ib::TwsSettings& settings, std::string& error)
{
	std::scoped_lock lock(mutex);
	if (!shared)
	{
		try
		{
			shared = std::make_unique<TwsPositions>(settings.host, settings.port, settings.clientId + 9);
		}
		catch (const std::exception& e)
		{
			error = e.what();
			return nullptr;
		}
		const std::string symbol = settings.testValues.count("test.symbol") ? settings.testValues.at("test.symbol") : "AAPL";
		if (auto positions = shared->stockPositions(symbol))
			std::cout << "Positions in " << symbol << " before the tests: " << describe(*positions) << std::endl;
	}
	return shared.get();
}

} // namespace test_util
