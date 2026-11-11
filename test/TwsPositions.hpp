#pragma once
// Reads positions from TWS, for the live tests' "no position left behind" check.
// Uses its own API connection, so it does not depend on the gateway components under test.
#include "ib/GatewayConnector.hpp"
#include "AccountHandler.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace test_util {

/// account -> position in one symbol (accounts without a position in it are absent)
using Positions = std::map<std::string, double>;

std::string describe(const Positions& positions);

class TwsPositions : public ib_helper::AccountHandler
{
	public:
	/// connects to TWS; @throws std::runtime_error if it cannot within timeout
	TwsPositions(const std::string& host, int port, int clientId,
			std::chrono::seconds timeout = std::chrono::seconds(10));
	~TwsPositions();

	/// positions in a stock, in every account TWS reports; nothing if TWS does not answer in time
	std::optional<Positions> stockPositions(const std::string& symbol,
			std::chrono::seconds timeout = std::chrono::seconds(10));

	// ib_helper::AccountHandler
	void OnPosition(const std::string& account, const Contract& contract, Decimal pos, double averageCost) override;
	void OnPositionEnd() override;
	void OnAccountValueUpdate(const std::string&, const std::string&, const std::string&, const std::string&) override {}
	void OnPortfolioUpdate(const Contract&, Decimal, double, double, double, double, double, const std::string&) override {}
	void OnUpdateAccountTime(const std::string&) override {}
	void OnAccountDownloadEnd(const std::string&) override {}
	void OnCurrentTime(long) override {}
	void OnError(int, int, const std::string&, const std::string&) override {}

	private:
	struct Entry
	{
		std::string account;
		std::string symbol;
		std::string secType;
		double position;
	};

	std::unique_ptr<ib_fix::ib::GatewayConnector> connector_;
	std::mutex mutex_;
	std::condition_variable done_;
	std::map<std::pair<std::string, long>, Entry> received_; // (account, conId)
	bool complete_ = false;
};

/***
 * Remembers a stock's positions at the start of a test and checks, at the end, that they are back
 * to the same (waiting a while: TWS reports position changes a little after fills).
 */
class PositionCheck
{
	public:
	using Reader = std::function<std::optional<Positions>()>;

	/// @return an error, or empty if the starting positions were read
	std::string begin(TwsPositions& positions, const std::string& symbol);
	/// as above, reading positions with read (for tests of the check itself)
	std::string begin(Reader read, const std::string& symbol);
	bool started() const { return start_.has_value(); }
	/// @return an error, or empty if the positions are as they were at begin()
	std::string end(std::chrono::seconds settle = std::chrono::seconds(15),
			std::chrono::milliseconds pollInterval = std::chrono::seconds(1));

	private:
	Reader read_;
	std::string symbol_;
	std::optional<Positions> start_;
};

} // namespace test_util
