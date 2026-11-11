#pragma once
// ib_helper::IBConnector only passes connection errors (502, 509, 1100, 1102) to its monitors;
// order errors such as 201 (order rejected) are only logged. This subclass also passes every
// error that refers to a request id to an OrderErrorListener.
#include "IBConnector.hpp"
#include <atomic>
#include <string>

namespace ib_fix::ib {

class OrderErrorListener
{
	public:
	virtual ~OrderErrorListener() = default;
	/// @param id the order (or request) id the error refers to; always > 0
	virtual void onOrderError(int id, int errorCode, const std::string& message) = 0;
};

class GatewayConnector : public ib_helper::IBConnector
{
	public:
	/// connects to TWS / IB Gateway (see ib_helper::IBConnector)
	using ib_helper::IBConnector::IBConnector;
	void setOrderErrorListener(OrderErrorListener* listener) { listener_ = listener; }

	protected:
	void error(int id, time_t errorTime, int errorCode, const std::string& errorString,
			const std::string& advancedOrderRejectJson) override;

	private:
	std::atomic<OrderErrorListener*> listener_ = nullptr;
};

} // namespace ib_fix::ib
