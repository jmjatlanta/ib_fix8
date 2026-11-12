#pragma once
// Executes FIX clients' orders through TWS and reports their progress back as FIX messages.
#include "GatewayConnector.hpp"
#include "IbTranslation.hpp"
#include "OrderJournal.hpp"
#include "OrderRecord.hpp"
#include "OrderRequestHandler.hpp"
#include "IBConnector.hpp"
#include <cstdint>
#include <deque>
#include <functional>
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
 * While TWS is not connected, new orders, cancels and replaces are rejected. Whenever the
 * connection is (re)established the handler catches up: TWS's executions for the day are
 * requested (fills missed while disconnected are reported; repeats are ignored) and so are this
 * client's open orders. Afterwards, an order that is neither filled nor open in TWS is reported
 * Canceled, and a cancel or replace still pending on an open order is rejected as unconfirmed.
 *
 * With an OrderJournal, each order's state is saved as the client is told about it (when a
 * report is handed to the FIX session, not when it is queued), so after a restart recover()
 * restores the orders and the same catch-up brings them up to date: fills already reported are
 * not reported again, and what happened meanwhile is reported (at least once: a report sent just
 * before a crash, but not yet saved, is sent again with the same ExecID).
 *
 * Thread safety: FIX callbacks arrive on the sessions' reader threads, TWS callbacks on the
 * TWS reader thread; all state is guarded by one mutex.
 */
class IbOrderHandler : public OrderRequestHandler,
		public ib_helper::OrderHandler,
		public ib_helper::ExecutionHandler,
		public TwsEventListener
{
	public:
	/// registers itself for the connector's order and execution callbacks
	explicit IbOrderHandler(GatewayConnector& connector);
	~IbOrderHandler() override;
	IbOrderHandler(const IbOrderHandler&) = delete;
	IbOrderHandler& operator=(const IbOrderHandler&) = delete;

	/// where reports are sent; until set, they are queued
	void setResponder(OrderResponder* responder);
	/// account for orders that do not specify one (Account, tag 1); empty: let TWS decide.
	/// Logins with several accounts (e.g. an advisor's) reject orders without an account.
	void setDefaultAccount(const std::string& account);

	/// reports waiting for a client to log on
	size_t undeliveredCount(const std::string& clientCompId) const;
	/// where order state is saved; nullptr: not saved
	void setJournal(OrderJournal* journal);
	/// receives warnings (e.g. orders in TWS that are not in the journal)
	void setLog(std::function<void(const std::string&)> log);
	/***
	 * Take back orders saved by an earlier run (OrderJournal::load). Call before the first catch-up
	 * (onTwsConnected), which then brings them up to date with TWS.
	 * @return the number of orders still working
	 */
	size_t recover(const std::vector<OrderRecord>& orders);

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
	void OnOpenOrderEnd() override;
	void OnOrderBound(long orderId, int apiClientId, int apiOrderId) override {}
	/// a TWS error, warning or confirmation for an order. Info and warnings (e.g. 202, 399, 2109)
	/// are ignored; the order's status updates report what happened.
	void OnOrderError(int orderId, const ib_helper::IbError& err) override;

	// ib_helper::ExecutionHandler (from TWS)
	void OnExecDetails(int reqId, const Contract& contract, const Execution& execution) override;
	void OnExecDetailsEnd(int reqId) override;

	// TwsEventListener (from TWS, via GatewayConnector)
	void onTwsConnected() override;
	void onTwsDisconnected() override;
	/// catching up with TWS after a (re)connect
	bool isResyncing() const;

	private:
	using Outbound = std::variant<ExecutionReport, OrderCancelReject>;
	struct Queued
	{
		Outbound msg;
		std::optional<OrderRecord> snapshot; // the order's state once the client has this message
	};

	// all of these expect mutex_ to be held
	OrderRecord* find(const std::string& clientCompId, const std::string& clOrdId);
	bool clOrdIdInUse(const std::string& clientCompId, const std::string& clOrdId) const;
	ExecutionReport report(const OrderRecord& order, char execType);
	std::string nextExecId();
	void reject(const std::string& clientCompId, const NewOrderRequest& request, int reason, const std::string& text);
	void cancelReject(const std::string& clientCompId, const OrderRecord* order, const std::string& clOrdId,
			const std::string& origClOrdId, bool toReplace, int reason, const std::string& text);
	void deliver(const std::string& clientCompId, Outbound msg, const OrderRecord* order = nullptr);
	void addClOrdId(OrderRecord& order, const std::string& clOrdId);
	void save(const OrderRecord& order);
	void flush(const std::string& clientCompId);
	void finishResyncIfDone();
	void reconcile();

	GatewayConnector& connector_;
	mutable std::mutex mutex_;
	OrderResponder* responder_ = nullptr;
	std::string defaultAccount_;
	std::unordered_map<int, OrderRecord> orders_;                  // by TWS order id
	std::map<std::pair<std::string, std::string>, int> clOrdIds_;  // (client, ClOrdID) -> TWS order id
	std::unordered_map<std::string, std::deque<Queued>> undelivered_;
	OrderJournal* journal_ = nullptr;
	std::function<void(const std::string&)> log_;
	// catching up after a (re)connect
	bool resyncing_ = false;
	bool resyncExecutionsDone_ = false;
	bool resyncOpenOrdersDone_ = false;
	std::set<int> openInTws_;
	const std::string execIdPrefix_;
	uint64_t execIdCounter_ = 0;
};

} // namespace ib_fix::ib
