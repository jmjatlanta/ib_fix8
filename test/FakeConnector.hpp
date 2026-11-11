#pragma once
// Stands in for TWS: records the orders and cancels sent to it. Tests play TWS's side by calling
// the handler's callbacks (OnOrderStatus, OnExecDetails, OnOpenOrder, onOrderError) directly.
#include "IBConnector.hpp"
#include "Decimal.h"
#include "Execution.h"
#include <mutex>
#include <vector>

namespace test_util {

class FakeConnector : public ib_helper::IBConnector
{
	public:
	struct Placed
	{
		int orderId;
		Contract contract;
		::Order order;
	};

	FakeConnector() : ib_helper::IBConnector() {} // the protected constructor does not connect

	void PlaceOrder(int orderId, const Contract& contract, const ::Order& order) override
	{
		std::scoped_lock lock(mutex_);
		placed_.push_back({ orderId, contract, order });
	}
	void CancelOrder(int orderId, const OrderCancel&) override
	{
		std::scoped_lock lock(mutex_);
		canceled_.push_back(orderId);
	}
	uint32_t GetNextOrderId() override { return ++nextId_; }
	bool IsConnected() const override { return connected; }

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

	bool connected = true;

	private:
	mutable std::mutex mutex_;
	std::vector<Placed> placed_;
	std::vector<int> canceled_;
	uint32_t nextId_ = 100;
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
