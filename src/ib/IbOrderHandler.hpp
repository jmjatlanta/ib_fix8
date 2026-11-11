#pragma once
// Executes FIX clients' orders through TWS and reports their progress back as FIX messages.
#include "GatewayConnector.hpp"
#include "IbTranslation.hpp"
#include "OrderRequestHandler.hpp"
#include "IBConnector.hpp"
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace ib_fix::ib {

/***
 * The bridge between the FIX acceptor and TWS.
 *
 * Requests from FIX clients become TWS placeOrder/cancelOrder calls; TWS callbacks become
 * ExecutionReports and OrderCancelRejects for the client that owns the order:
 *  - new order: ExecutionReport PendingNew when sent to TWS, New when TWS reports it
 *    PreSubmitted/Submitted, Rejected if TWS reports an error first
 *  - fills (from execDetails, de-duplicated by ExecID): PartiallyFilled / Filled
 *  - cancel: Canceled when TWS reports Cancelled, OrderCancelReject on a TWS error
 *  - cancel/replace: the order is modified in place (same TWS order id); Replaced when TWS
 *    echoes the new terms in openOrder, OrderCancelReject on a TWS error
 * One cancel or replace may be outstanding per order. Reports for a client that is not logged
 * on are queued (in memory) and delivered when it logs on again.
 *
 * Thread safety: FIX callbacks arrive on the sessions' reader threads, TWS callbacks on the
 * TWS reader thread; all state is guarded by one mutex.
 */
class IbOrderHandler : public OrderRequestHandler,
		public ib_helper::OrderHandler,
		public ib_helper::ExecutionHandler,
		public OrderErrorListener
{
	public:
	/// registers itself for the connector's order and execution callbacks
	explicit IbOrderHandler(ib_helper::IBConnector& connector);
	~IbOrderHandler() override;
	IbOrderHandler(const IbOrderHandler&) = delete;
	IbOrderHandler& operator=(const IbOrderHandler&) = delete;

	/// where reports are sent; until set, they are queued
	void setResponder(OrderResponder* responder);

	/// reports waiting for a client to log on
	size_t undeliveredCount(const std::string& clientCompId) const;

	// OrderRequestHandler (from the FIX acceptor)
	void onNewOrder(const std::string& clientCompId, const NewOrderRequest& request) override;
	void onCancel(const std::string& clientCompId, const CancelRequest& request) override;
	void onReplace(const std::string& clientCompId, const ReplaceRequest& request) override;
	void onLogon(const std::string& clientCompId) override;

	// ib_helper::OrderHandler (from TWS)
	void OnOpenOrder(int orderId, const Contract& contract, const ::Order& order, const OrderState& orderState) override;
	void OnOrderStatus(int orderId, const std::string& status, Decimal filled, Decimal remaining,
			double avgFillPrice, int permId, int parentId, double lastFillPrice, int clientId,
			const std::string& whyHeld, double mktCapPrice) override;
	void OnOpenOrderEnd() override {}
	void OnOrderBound(long orderId, int apiClientId, int apiOrderId) override {}

	// ib_helper::ExecutionHandler (from TWS)
	void OnExecDetails(int reqId, const Contract& contract, const Execution& execution) override;

	// OrderErrorListener (from TWS, via GatewayConnector)
	void onOrderError(int orderId, int errorCode, const std::string& message) override;

	private:
	struct PendingChange
	{
		bool isReplace = false;
		std::string clOrdId;
		ReplaceRequest replace;   // the new terms, when isReplace
	};

	struct OrderRecord
	{
		std::string clientCompId;
		int ibOrderId = 0;
		NewOrderRequest terms;    // current terms; terms.clOrdId is the current ClOrdID
		char ordStatus = fix_value::ExecPendingNew;
		bool acknowledged = false;
		double cumQty = 0.0;
		double avgPx = 0.0;
		std::optional<PendingChange> pending;
		std::set<std::string> execIds;
		bool isTerminal() const;
	};

	using Outbound = std::variant<ExecutionReport, OrderCancelReject>;

	// all of these expect mutex_ to be held
	OrderRecord* find(const std::string& clientCompId, const std::string& clOrdId);
	bool clOrdIdInUse(const std::string& clientCompId, const std::string& clOrdId) const;
	ExecutionReport report(const OrderRecord& order, char execType);
	std::string nextExecId();
	void reject(const std::string& clientCompId, const NewOrderRequest& request, int reason, const std::string& text);
	void cancelReject(const std::string& clientCompId, const OrderRecord* order, const std::string& clOrdId,
			const std::string& origClOrdId, bool toReplace, int reason, const std::string& text);
	void deliver(const std::string& clientCompId, Outbound msg);
	void flush(const std::string& clientCompId);

	ib_helper::IBConnector& connector_;
	mutable std::mutex mutex_;
	OrderResponder* responder_ = nullptr;
	std::unordered_map<int, OrderRecord> orders_;                  // by TWS order id
	std::map<std::pair<std::string, std::string>, int> clOrdIds_;  // (client, ClOrdID) -> TWS order id
	std::unordered_map<std::string, std::deque<Outbound>> undelivered_;
	const std::string execIdPrefix_;
	uint64_t execIdCounter_ = 0;
};

} // namespace ib_fix::ib
