// ib_fix gateway: FIX 4.2 in front of Interactive Brokers TWS.
// Usage: ib_fix_gateway <config file>    (Ctrl-C or SIGTERM to stop)
#include "engine/Engine.hpp"
#include <csignal>
#include <iostream>

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		std::cerr << "Usage: " << argv[0] << " <config file>\n";
		return 2;
	}

	// Block SIGINT/SIGTERM before the engine starts its threads (they inherit the mask), then
	// wait for one here instead of handling it asynchronously. A shell starts background jobs
	// with SIGINT ignored, and an ignored signal is discarded rather than waited for, so restore
	// the default disposition first.
	std::signal(SIGINT, SIG_DFL);
	std::signal(SIGTERM, SIG_DFL);
	sigset_t stopSignals;
	sigemptyset(&stopSignals);
	sigaddset(&stopSignals, SIGINT);
	sigaddset(&stopSignals, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);

	try
	{
		ib_fix::EngineConfig config = ib_fix::loadEngineConfig(argv[1]);
		config.log = [](const std::string& message) { std::cout << "ib_fix: " << message << std::endl; };

		ib_fix::Engine engine(config);
		std::cout << "ib_fix: Running. Press Ctrl-C to stop." << std::endl;
		int signal = 0;
		sigwait(&stopSignals, &signal);
		// engine's destructor does the clean shutdown
	}
	catch (const std::exception& e)
	{
		std::cerr << "ib_fix: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
