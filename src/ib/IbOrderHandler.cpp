#include "IbOrderHandler.hpp"
#include "fix/Fix42Codec.hpp"
#include "Decimal.h"
#include "Execution.h"
#include "OrderCancel.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace ib_fix::ib {

namespace {

namespace fv = fix_value;

std::string makeExecIdPrefix()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return "IBG" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

double toDouble(Decimal d)
{
	return DecimalFunctions::decimalToDouble(d);
}

bool samePrice(double a, double b)
{
	return std::abs(a - b) < 1e-9;
}

/// TWS error codes that do not mean a request failed
bool isInformational(int errorCode)
{
	return errorCode == 202                              // "Order Canceled" - the status update reports it
		|| errorCode == 399                              // order message/warning, e.g. queued until open
		|| errorCode == 10167 || errorCode == 10168      // delayed / no market data
		|| (errorCode >= 2100 && errorCode <= 2199);     // warnings
}

int cancelRejectReason(int errorCode)
{
	switch (errorCode)
	{
		case 10147:        // OrderId to cancel not found
		case 135:          // can't find order with id
			return fv::CxlRejUnknownOrder;
		case 161:          // cancel attempted when order is not in a cancellable state
			return fv::CxlRejTooLate;
		default:
			return fv::CxlRejBrokerOption;
	}
}

std::string twsErrorText(int errorCode, const std::string& message)
{
	return "TWS error " + std::to_string(errorCode) + ": " + message;
}

} // namespace

bool IbOrderHandler::OrderRecord::isTerminal() const
{
	return ordStatus == fv::OrdStatusFilled || ordStatus == fv::ExecCanceled || ordStatus == fv::ExecRejected;
}

IbOrderHandler::IbOrderHandler(GatewayConnector& connector)
		: connector_(connector), execIdPrefix_(makeExecIdPrefix())
{
	connector_.AddOrderHandler(this);
	connector_.AddExecutionHandler(this);
}

IbOrderHandler::~IbOrderHandler()
{
	connector_.RemoveExecutionHandler(this);
	connector_.RemoveOrderHandler(this);
}

void IbOrderHandler::setResponder(OrderResponder* responder)
{
	std::scoped_lock lock(mutex_);
	responder_ = responder;
}

void IbOrderHandler::setDefaultAccount(const std::string& account)
{
	std::scoped_lock lock(mutex_);
	defaultAccount_ = account;
}

size_t IbOrderHandler::undeliveredCount(const std::string& clientCompId) const
{
	std::scoped_lock lock(mutex_);
	auto it = undelivered_.find(clientCompId);
	return it == undelivered_.end() ? 0 : it->second.size();
}

// ---------------------------------------------------------------------------------------------
// requests from FIX clients
// ---------------------------------------------------------------------------------------------

void IbOrderHandler::onNewOrder(const std::string& clientCompId, const NewOrderRequest& original)
{
	std::scoped_lock lock(mutex_);
	NewOrderRequest request = original;
	if (request.account.empty())
		request.account = defaultAccount_;
	if (clOrdIdInUse(clientCompId, request.clOrdId))
		return reject(clientCompId, request, fv::OrdRejDuplicateOrder, "Duplicate ClOrdID " + request.clOrdId);
	Translation ib = toIb(request);
	if (!ib.result)
		return reject(clientCompId, request, fv::OrdRejBrokerOption, ib.error);
	if (!connector_.isAvailable())
		return reject(clientCompId, request, fv::OrdRejBrokerOption, "Not connected to TWS");

	const int ibOrderId = static_cast<int>(connector_.GetNextOrderId());
	// TWS callbacks for this order wait on mutex_, so they find the record complete
	if (!connector_.placeOrder(ibOrderId, ib.result->contract, ib.result->order))
		return reject(clientCompId, request, fv::OrdRejBrokerOption, "Not connected to TWS");
	OrderRecord& order = orders_[ibOrderId];
	order.clientCompId = clientCompId;
	order.ibOrderId = ibOrderId;
	order.terms = request;
	clOrdIds_[{ clientCompId, request.clOrdId }] = ibOrderId;
	deliver(clientCompId, report(order, fv::ExecPendingNew));
}

void IbOrderHandler::onCancel(const std::string& clientCompId, const CancelRequest& request)
{
	std::scoped_lock lock(mutex_);
	OrderRecord* order = find(clientCompId, request.origClOrdId);
	if (order == nullptr)
		return cancelReject(clientCompId, nullptr, request.clOrdId, request.origClOrdId, false,
				fv::CxlRejUnknownOrder, "Unknown order " + request.origClOrdId);
	if (order->isTerminal())
		return cancelReject(clientCompId, order, request.clOrdId, request.origClOrdId, false,
				fv::CxlRejTooLate, "Order is already done");
	if (order->pending)
		return cancelReject(clientCompId, order, request.clOrdId, request.origClOrdId, false,
				fv::CxlRejAlreadyPending, "A cancel or replace is already pending");
	if (clOrdIdInUse(clientCompId, request.clOrdId))
		return cancelReject(clientCompId, order, request.clOrdId, request.origClOrdId, false,
				fv::CxlRejBrokerOption, "Duplicate ClOrdID " + request.clOrdId);

	if (!connector_.cancelOrder(order->ibOrderId))
		return cancelReject(clientCompId, order, request.clOrdId, request.origClOrdId, false,
				fv::CxlRejBrokerOption, "Not connected to TWS");
	order->pending = PendingChange{ false, request.clOrdId, {} };
	clOrdIds_[{ clientCompId, request.clOrdId }] = order->ibOrderId;
}

void IbOrderHandler::onReplace(const std::string& clientCompId, const ReplaceRequest& original)
{
	std::scoped_lock lock(mutex_);
	ReplaceRequest request = original;
	OrderRecord* order = find(clientCompId, request.origClOrdId);
	if (order != nullptr && request.account.empty())
		request.account = order->terms.account; // a replace keeps the order's account
	auto rejectReplace = [&](int reason, const std::string& text) {
		cancelReject(clientCompId, order, request.clOrdId, request.origClOrdId, true, reason, text);
	};
	if (order == nullptr)
		return rejectReplace(fv::CxlRejUnknownOrder, "Unknown order " + request.origClOrdId);
	if (order->isTerminal())
		return rejectReplace(fv::CxlRejTooLate, "Order is already done");
	if (order->pending)
		return rejectReplace(fv::CxlRejAlreadyPending, "A cancel or replace is already pending");
	if (clOrdIdInUse(clientCompId, request.clOrdId))
		return rejectReplace(fv::CxlRejBrokerOption, "Duplicate ClOrdID " + request.clOrdId);
	if (request.symbol != order->terms.symbol || request.side != order->terms.side)
		return rejectReplace(fv::CxlRejBrokerOption, "Symbol and Side cannot be changed");
	if (auto problem = fix42::validate(request))
		return rejectReplace(fv::CxlRejBrokerOption, *problem);
	if (!(request.orderQty > order->cumQty))
		return rejectReplace(fv::CxlRejBrokerOption, "OrderQty must be more than the quantity already filled");
	Translation ib = toIb(request);
	if (!ib.result)
		return rejectReplace(fv::CxlRejBrokerOption, ib.error);

	// placing an order with an existing id modifies it
	if (!connector_.placeOrder(order->ibOrderId, ib.result->contract, ib.result->order))
		return rejectReplace(fv::CxlRejBrokerOption, "Not connected to TWS");
	order->pending = PendingChange{ true, request.clOrdId, request };
	clOrdIds_[{ clientCompId, request.clOrdId }] = order->ibOrderId;
}

void IbOrderHandler::onLogon(const std::string& clientCompId)
{
	std::scoped_lock lock(mutex_);
	flush(clientCompId);
}

// ---------------------------------------------------------------------------------------------
// callbacks from TWS
// ---------------------------------------------------------------------------------------------

void IbOrderHandler::OnOrderStatus(int orderId, const std::string& status, Decimal, Decimal, double, int, int,
		double, int, const std::string& whyHeld, double)
{
	std::scoped_lock lock(mutex_);
	auto it = orders_.find(orderId);
	if (it == orders_.end() || it->second.isTerminal())
		return;
	OrderRecord& order = it->second;

	if (status == "PreSubmitted" || status == "Submitted")
	{
		if (!order.acknowledged)
		{
			order.acknowledged = true;
			if (order.ordStatus == fv::ExecPendingNew)
			{
				order.ordStatus = fv::ExecNew;
				deliver(order.clientCompId, report(order, fv::ExecNew));
			}
		}
	}
	else if (status == "Cancelled" || status == "ApiCancelled")
	{
		order.ordStatus = fv::ExecCanceled;
		ExecutionReport canceled = report(order, fv::ExecCanceled);
		std::optional<PendingChange> pending = std::move(order.pending);
		order.pending.reset();
		if (pending && !pending->isReplace)
		{
			// the client's cancel: report it against the cancel request's ClOrdID
			canceled.origClOrdId = canceled.clOrdId;
			canceled.clOrdId = pending->clOrdId;
		}
		deliver(order.clientCompId, canceled);
		if (pending && pending->isReplace)
			cancelReject(order.clientCompId, &order, pending->clOrdId, order.terms.clOrdId, true, fv::CxlRejTooLate,
					"Order was canceled");
	}
	else if (status == "Inactive" && !order.acknowledged)
	{
		// usually accompanied by an error (which may already have rejected the order)
		order.ordStatus = fv::ExecRejected;
		ExecutionReport rejected = report(order, fv::ExecRejected);
		rejected.ordRejReason = fv::OrdRejBrokerOption;
		rejected.text = whyHeld.empty() ? "Order inactive in TWS" : "Order inactive in TWS: " + whyHeld;
		deliver(order.clientCompId, rejected);
	}
	// PendingSubmit, PendingCancel, ApiPending: nothing to report. Filled: reported from execDetails.
}

void IbOrderHandler::OnOpenOrder(int orderId, const Contract&, const ::Order& ibOrder, const OrderState&)
{
	std::scoped_lock lock(mutex_);
	if (resyncing_)
		openInTws_.insert(orderId);
	auto it = orders_.find(orderId);
	if (it == orders_.end())
		return;
	OrderRecord& order = it->second;
	if (!order.pending || !order.pending->isReplace || order.isTerminal())
		return;

	// TWS echoes the order after a modification; report Replaced once it shows the new terms
	const ReplaceRequest& replace = order.pending->replace;
	const bool qtyMatches = samePrice(toDouble(ibOrder.totalQuantity), replace.orderQty);
	const bool priceMatches = !replace.price || samePrice(ibOrder.lmtPrice, *replace.price);
	const bool stopMatches = !replace.stopPx || samePrice(ibOrder.auxPrice, *replace.stopPx);
	if (!qtyMatches || !priceMatches || !stopMatches)
		return;

	const std::string origClOrdId = order.terms.clOrdId;
	order.terms = replace; // the replacement terms, including the new ClOrdID
	order.pending.reset();
	order.acknowledged = true;
	order.ordStatus = order.cumQty > 0.0 ? fv::OrdStatusPartiallyFilled : fv::ExecNew;
	ExecutionReport replaced = report(order, fv::ExecReplaced);
	replaced.origClOrdId = origClOrdId;
	deliver(order.clientCompId, replaced);
}

void IbOrderHandler::OnExecDetails(int, const Contract&, const Execution& execution)
{
	std::scoped_lock lock(mutex_);
	auto it = orders_.find(static_cast<int>(execution.orderId));
	if (it == orders_.end())
		return;
	OrderRecord& order = it->second;
	if (!order.execIds.insert(execution.execId).second)
		return; // TWS repeats executions, e.g. after reconnecting

	order.acknowledged = true;
	order.cumQty = toDouble(execution.cumQty);
	order.avgPx = execution.avgPrice;
	const bool filled = order.cumQty >= order.terms.orderQty - 1e-9;
	order.ordStatus = filled ? fv::OrdStatusFilled : fv::OrdStatusPartiallyFilled;

	ExecutionReport fill = report(order, filled ? fv::ExecFill : fv::ExecPartialFill);
	fill.execId = execution.execId;
	fill.lastShares = toDouble(execution.shares);
	fill.lastPx = execution.price;
	deliver(order.clientCompId, fill);

	if (filled && order.pending)
	{
		const PendingChange pending = *order.pending;
		order.pending.reset();
		cancelReject(order.clientCompId, &order, pending.clOrdId, order.terms.clOrdId, pending.isReplace,
				fv::CxlRejTooLate, "Order is filled");
	}
}

void IbOrderHandler::onOrderError(int orderId, int errorCode, const std::string& message)
{
	if (isInformational(errorCode))
		return;
	std::scoped_lock lock(mutex_);
	auto it = orders_.find(orderId);
	if (it == orders_.end())
		return;
	OrderRecord& order = it->second;
	const std::string text = twsErrorText(errorCode, message);

	if (order.pending)
	{
		// the cancel or modification failed; the order itself is unchanged
		const PendingChange pending = *order.pending;
		order.pending.reset();
		cancelReject(order.clientCompId, &order, pending.clOrdId, order.terms.clOrdId, pending.isReplace,
				cancelRejectReason(errorCode), text);
		return;
	}
	if (!order.acknowledged && !order.isTerminal())
	{
		order.ordStatus = fv::ExecRejected;
		ExecutionReport rejected = report(order, fv::ExecRejected);
		rejected.ordRejReason = fv::OrdRejBrokerOption;
		rejected.text = text;
		deliver(order.clientCompId, rejected);
	}
	// errors on a working order with nothing pending (e.g. a TWS warning we do not know) are not
	// reported; the order's status updates still are
}

// ---------------------------------------------------------------------------------------------
// catching up with TWS after a (re)connect
// ---------------------------------------------------------------------------------------------

void IbOrderHandler::onTwsConnected()
{
	std::scoped_lock lock(mutex_);
	resyncing_ = true;
	resyncExecutionsDone_ = false;
	resyncOpenOrdersDone_ = false;
	openInTws_.clear();
	if (!connector_.requestOrderRefresh())
		resyncing_ = false;
}

void IbOrderHandler::onTwsDisconnected()
{
	std::scoped_lock lock(mutex_);
	resyncing_ = false; // starts again on the next connect
}

bool IbOrderHandler::isResyncing() const
{
	std::scoped_lock lock(mutex_);
	return resyncing_;
}

void IbOrderHandler::OnExecDetailsEnd(int)
{
	std::scoped_lock lock(mutex_);
	if (!resyncing_)
		return;
	resyncExecutionsDone_ = true;
	finishResyncIfDone();
}

void IbOrderHandler::OnOpenOrderEnd()
{
	std::scoped_lock lock(mutex_);
	if (!resyncing_)
		return;
	resyncOpenOrdersDone_ = true;
	finishResyncIfDone();
}

void IbOrderHandler::finishResyncIfDone()
{
	if (!resyncExecutionsDone_ || !resyncOpenOrdersDone_)
		return;
	reconcile();
	resyncing_ = false;
}

void IbOrderHandler::reconcile()
{
	for (auto& [ibOrderId, order] : orders_)
	{
		if (order.isTerminal())
			continue;
		if (openInTws_.count(ibOrderId) == 0)
		{
			// not filled (executions were replayed first) and no longer working in TWS
			std::optional<PendingChange> pending = std::move(order.pending);
			order.pending.reset();
			order.ordStatus = fv::ExecCanceled;
			ExecutionReport canceled = report(order, fv::ExecCanceled);
			canceled.text = "Order is no longer open in TWS (checked after connecting to TWS)";
			if (pending && !pending->isReplace)
			{
				canceled.origClOrdId = canceled.clOrdId;
				canceled.clOrdId = pending->clOrdId;
			}
			deliver(order.clientCompId, canceled);
			if (pending && pending->isReplace)
				cancelReject(order.clientCompId, &order, pending->clOrdId, order.terms.clOrdId, true,
						fv::CxlRejTooLate, "Order is no longer open in TWS");
		}
		else if (order.pending)
		{
			// TWS may never have received the request; the client can send it again
			const PendingChange pending = *order.pending;
			order.pending.reset();
			cancelReject(order.clientCompId, &order, pending.clOrdId, order.terms.clOrdId, pending.isReplace,
					fv::CxlRejBrokerOption, "Not confirmed by TWS before the connection was lost; the order is still open");
		}
	}
}

// ---------------------------------------------------------------------------------------------
// helpers (mutex_ held)
// ---------------------------------------------------------------------------------------------

IbOrderHandler::OrderRecord* IbOrderHandler::find(const std::string& clientCompId, const std::string& clOrdId)
{
	auto it = clOrdIds_.find({ clientCompId, clOrdId });
	if (it == clOrdIds_.end())
		return nullptr;
	auto order = orders_.find(it->second);
	if (order == orders_.end())
		return nullptr;
	// only the order's current ClOrdID identifies it (not an earlier one, or a cancel request's)
	return order->second.terms.clOrdId == clOrdId ? &order->second : nullptr;
}

bool IbOrderHandler::clOrdIdInUse(const std::string& clientCompId, const std::string& clOrdId) const
{
	return clOrdIds_.count({ clientCompId, clOrdId }) > 0;
}

std::string IbOrderHandler::nextExecId()
{
	return execIdPrefix_ + "-" + std::to_string(++execIdCounter_);
}

ExecutionReport IbOrderHandler::report(const OrderRecord& order, char execType)
{
	ExecutionReport r;
	r.orderId = std::to_string(order.ibOrderId);
	r.execId = nextExecId();
	r.clOrdId = order.terms.clOrdId;
	r.execType = execType;
	r.ordStatus = order.ordStatus;
	r.account = order.terms.account;
	r.symbol = order.terms.symbol;
	r.side = order.terms.side;
	r.orderQty = order.terms.orderQty;
	r.ordType = order.terms.ordType;
	r.price = order.terms.price;
	r.stopPx = order.terms.stopPx;
	r.timeInForce = order.terms.timeInForce;
	r.cumQty = order.cumQty;
	r.avgPx = order.avgPx;
	const bool done = order.ordStatus == fv::ExecCanceled || order.ordStatus == fv::ExecRejected;
	r.leavesQty = done ? 0.0 : std::max(0.0, order.terms.orderQty - order.cumQty);
	return r;
}

void IbOrderHandler::reject(const std::string& clientCompId, const NewOrderRequest& request, int reason,
		const std::string& text)
{
	deliver(clientCompId, fix42::makeRejection(request, nextExecId(), reason, text));
}

void IbOrderHandler::cancelReject(const std::string& clientCompId, const OrderRecord* order,
		const std::string& clOrdId, const std::string& origClOrdId, bool toReplace, int reason, const std::string& text)
{
	OrderCancelReject r;
	r.orderId = order ? std::to_string(order->ibOrderId) : "NONE";
	r.clOrdId = clOrdId;
	r.origClOrdId = origClOrdId;
	r.ordStatus = order ? order->ordStatus : fv::ExecRejected;
	r.cxlRejResponseTo = toReplace ? fv::CxlRejToReplace : fv::CxlRejToCancel;
	r.cxlRejReason = reason;
	r.text = text;
	deliver(clientCompId, r);
}

void IbOrderHandler::deliver(const std::string& clientCompId, Outbound msg)
{
	// queue behind anything already waiting, so the client sees reports in order
	undelivered_[clientCompId].push_back(std::move(msg));
	flush(clientCompId);
}

void IbOrderHandler::flush(const std::string& clientCompId)
{
	auto it = undelivered_.find(clientCompId);
	if (it == undelivered_.end() || responder_ == nullptr)
		return;
	auto& queue = it->second;
	while (!queue.empty())
	{
		const bool sent = std::visit([&](const auto& m) {
			using T = std::decay_t<decltype(m)>;
			if constexpr (std::is_same_v<T, ExecutionReport>)
				return responder_->sendExecutionReport(clientCompId, m);
			else
				return responder_->sendCancelReject(clientCompId, m);
		}, queue.front());
		if (!sent)
			return; // client not logged on: keep the rest for onLogon
		queue.pop_front();
	}
	undelivered_.erase(it);
}

} // namespace ib_fix::ib
