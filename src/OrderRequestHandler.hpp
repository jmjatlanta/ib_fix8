#pragma once
// The boundary between the FIX acceptor and whatever executes orders (Interactive Brokers).
#include "OrderMessages.hpp"
#include <string>

namespace ib_fix {

/// Sends order responses to a connected FIX client.
/// Clients are identified by their SenderCompID (our TargetCompID). Safe to call from any thread.
class OrderResponder
{
	public:
	virtual ~OrderResponder() = default;
	/// @return false if the client is not logged on (the message is not sent)
	virtual bool sendExecutionReport(const std::string& clientCompId, const ExecutionReport& report) = 0;
	/// @return false if the client is not logged on (the message is not sent)
	virtual bool sendCancelReject(const std::string& clientCompId, const OrderCancelReject& reject) = 0;
};

/// Receives order requests from FIX clients. Called on the FIX session's reader thread, so
/// implementations should not block for long. If a call throws, the client gets a rejection
/// (ExecutionReport for a new order, BusinessMessageReject for a cancel or replace).
class OrderRequestHandler
{
	public:
	virtual ~OrderRequestHandler() = default;
	/// the order has already passed fix42::validate(); invalid orders are rejected before this
	virtual void onNewOrder(const std::string& clientCompId, const NewOrderRequest& request) = 0;
	virtual void onCancel(const std::string& clientCompId, const CancelRequest& request) = 0;
	/// not pre-validated: only the handler knows the original order's state, which an
	/// OrderCancelReject must report (fix42::validate() can be used on the replacement order)
	virtual void onReplace(const std::string& clientCompId, const ReplaceRequest& request) = 0;
	/// The client has logged on
	virtual void onLogon(const std::string& clientCompId) {}
	/// The client has logged out or disconnected
	virtual void onLogout(const std::string& clientCompId) {}
};

} // namespace ib_fix
