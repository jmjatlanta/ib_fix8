#include "TwsPositions.hpp"
#include "Decimal.h"
#include <sstream>
#include <stdexcept>
#include <thread>

namespace test_util {

std::string describe(const Positions& positions)
{
	if (positions.empty())
		return "flat";
	std::ostringstream out;
	for (const auto& [account, position] : positions)
		out << (out.tellp() > 0 ? ", " : "") << account << " " << position;
	return out.str();
}

TwsPositions::TwsPositions(const std::string& host, int port, int clientId, std::chrono::seconds timeout)
		: connector_(std::make_unique<ib_fix::ib::GatewayConnector>(host, port, clientId))
{
	connector_->AddAccountHandler(this);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!connector_->IsConnected())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("cannot connect to TWS at " + host + ":" + std::to_string(port)
				+ " to read positions");
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

TwsPositions::~TwsPositions()
{
	connector_->RemoveAccountHandler(this);
	connector_->disconnect();
}

std::optional<Positions> TwsPositions::stockPositions(const std::string& symbol, std::chrono::seconds timeout)
{
	std::unique_lock lock(mutex_);
	received_.clear();
	complete_ = false;
	lock.unlock();
	connector_->RequestPositions(); // TWS sends every position, then positionEnd
	lock.lock();
	if (!done_.wait_for(lock, timeout, [this]() { return complete_; }))
		return std::nullopt;
	Positions result;
	for (const auto& [key, entry] : received_)
		if (entry.symbol == symbol && entry.secType == "STK" && entry.position != 0.0)
			result[entry.account] += entry.position;
	return result;
}

void TwsPositions::OnPosition(const std::string& account, const Contract& contract, Decimal pos, double)
{
	std::scoped_lock lock(mutex_);
	received_[{ account, contract.conId }] = { account, contract.symbol, contract.secType,
		DecimalFunctions::decimalToDouble(pos) };
}

void TwsPositions::OnPositionEnd()
{
	{
		std::scoped_lock lock(mutex_);
		complete_ = true;
	}
	done_.notify_all();
}

std::string PositionCheck::begin(TwsPositions& positions, const std::string& symbol)
{
	return begin([&positions, symbol]() { return positions.stockPositions(symbol); }, symbol);
}

std::string PositionCheck::begin(Reader read, const std::string& symbol)
{
	read_ = std::move(read);
	symbol_ = symbol;
	start_ = read_();
	if (!start_)
		return "could not read the " + symbol + " positions from TWS before the test";
	return {};
}

std::string PositionCheck::end(std::chrono::seconds settle, std::chrono::milliseconds pollInterval)
{
	if (!start_ || !read_)
		return {};
	std::optional<Positions> now;
	const auto deadline = std::chrono::steady_clock::now() + settle;
	while (true)
	{
		now = read_();
		if (now && *now == *start_)
			return {};
		if (std::chrono::steady_clock::now() >= deadline)
			break;
		std::this_thread::sleep_for(pollInterval);
	}
	if (!now)
		return "could not read the " + symbol_ + " positions from TWS after the test";
	return symbol_ + " position changed during the test: before " + describe(*start_) + ", after " + describe(*now)
		+ ". Close the difference by hand.";
}

} // namespace test_util
