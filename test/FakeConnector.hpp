#pragma once
// Stands in for TWS: records the orders and cancels sent to it. Tests play TWS's side by calling
// the handler's callbacks (OnOrderStatus, OnExecDetails, OnOpenOrder, onOrderError, ...) directly.
#include "ib/GatewayConnector.hpp"
#include "Decimal.h"
#include "Execution.h"
#include <mutex>
#include <vector>

namespace test_util {

class FakeConnector : public ib_fix::ib::GatewayConnector
{
	public:
	struct Placed
	{
		int orderId;
		Contract contract;
		::Order order;
	};

	FakeConnector() : ib_fix::ib::GatewayConnector() {} // the protected constructor does not connect

	bool placeOrder(int orderId, const Contract& contract, const ::Order& order) override
	{
		if (!connected || failNextSend)
			return failNextSend = false;
		std::scoped_lock lock(mutex_);
		placed_.push_back({ orderId, contract, order });
		return true;
	}
	bool cancelOrder(int orderId) override
	{
		if (!connected || failNextSend)
			return failNextSend = false;
		std::scoped_lock lock(mutex_);
		canceled_.push_back(orderId);
		return true;
	}
	bool requestOrderRefresh() override
	{
		if (!connected)
			return false;
		++refreshRequests;
		return true;
	}
	int GetNextOrderId() override { return ++nextId_; }
	bool isAvailable() const override { return connected; }

	std::vector<Placed> placed() const
	{
		std::scoped_lock lock(mutex_);
		return placed_;
	}
	std::vector<int> canceled() const
	{
		std::scoped_lock lock(mutex_);
		return canceled_;
	}

	std::atomic<bool> connected = true;
	/// the next placeOrder/cancelOrder fails as if the connection had just dropped
	std::atomic<bool> failNextSend = false;
	std::atomic<int> refreshRequests = 0;

	private:
	mutable std::mutex mutex_;
	std::vector<Placed> placed_;
	std::vector<int> canceled_;
	int nextId_ = 100;
};

inline Decimal dec(double value)
{
	return DecimalFunctions::doubleToDecimal(value);
}

inline Execution makeExecution(int orderId, const std::string& execId, double shares, double price,
		double cumQty, double avgPrice)
{
	Execution e;
	e.orderId = orderId;
	e.execId = execId;
	e.shares = dec(shares);
	e.price = price;
	e.cumQty = dec(cumQty);
	e.avgPrice = avgPrice;
	return e;
}

} // namespace test_util
