// Prints the positions in a stock across all accounts of a TWS session (read-only).
// Usage: tws_positions [symbol]    (TWS connection from the live tests' settings file)
#include "TwsPositions.hpp"
#include "ib/TwsSettings.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[])
{
	try
	{
		const char* path = std::getenv("IB_FIX_TWS_CONFIG");
		const auto settings = ib_fix::ib::loadTwsSettings(path ? std::filesystem::path(path)
			: std::filesystem::path(TEST_RESOURCES_DIR) / "tws_paper.cfg");
		const std::string symbol = argc > 1 ? argv[1]
			: (settings.testValues.count("test.symbol") ? settings.testValues.at("test.symbol") : "AAPL");
		test_util::TwsPositions positions(settings.host, settings.port, settings.clientId + 9);
		const auto result = positions.stockPositions(symbol);
		if (!result)
		{
			std::cerr << "TWS did not report positions\n";
			return 1;
		}
		std::cout << symbol << ": " << test_util::describe(*result) << std::endl;
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}
}
