#include "Fix42Codec.hpp"

namespace ib_fix::fix42 {

namespace F42 = FIX8::FIX42;

namespace {

template<typename FieldT>
std::string getString(const FIX8::MessageBase& msg)
{
	FieldT field;
	return msg.get(field) ? std::string(field()) : std::string();
}

template<typename FieldT>
char getChar(const FIX8::MessageBase& msg, char defaultValue = 0)
{
	FieldT field;
	return msg.get(field) ? field() : defaultValue;
}

template<typename FieldT>
std::optional<double> getDouble(const FIX8::MessageBase& msg)
{
	FieldT field;
	if (msg.get(field))
		return static_cast<double>(field());
	return std::nullopt;
}

/// fields shared by NewOrderSingle and OrderCancelReplaceRequest
void decodeOrder(const FIX8::MessageBase& msg, NewOrderRequest& out)
{
	out.clOrdId = getString<F42::ClOrdID>(msg);
	out.account = getString<F42::Account>(msg);
	out.symbol = getString<F42::Symbol>(msg);
	out.securityType = getString<F42::SecurityType>(msg);
	out.currency = getString<F42::Currency>(msg);
	out.exchange = getString<F42::SecurityExchange>(msg);
	out.side = getChar<F42::Side>(msg);
	out.orderQty = getDouble<F42::OrderQty>(msg).value_or(0.0);
	out.ordType = getChar<F42::OrdType>(msg);
	out.price = getDouble<F42::Price>(msg);
	out.stopPx = getDouble<F42::StopPx>(msg);
	out.timeInForce = getChar<F42::TimeInForce>(msg, fix_value::TifDay);
	out.text = getString<F42::Text>(msg);
}

template<typename FieldT, typename ValueT>
void addIfSet(FIX8::MessageBase& msg, const std::optional<ValueT>& value)
{
	if (value)
		msg << new FieldT(*value);
}

void addIfSet(FIX8::MessageBase& msg, FIX8::BaseField* field, bool set)
{
	if (set)
		msg << field;
	else
		delete field;
}

} // namespace

NewOrderRequest decode(const F42::NewOrderSingle& msg)
{
	NewOrderRequest out;
	decodeOrder(msg, out);
	return out;
}

ReplaceRequest decode(const F42::OrderCancelReplaceRequest& msg)
{
	ReplaceRequest out;
	decodeOrder(msg, out);
	out.origClOrdId = getString<F42::OrigClOrdID>(msg);
	out.orderId = getString<F42::OrderID>(msg);
	return out;
}

CancelRequest decode(const F42::OrderCancelRequest& msg)
{
	CancelRequest out;
	out.clOrdId = getString<F42::ClOrdID>(msg);
	out.origClOrdId = getString<F42::OrigClOrdID>(msg);
	out.orderId = getString<F42::OrderID>(msg);
	out.symbol = getString<F42::Symbol>(msg);
	out.side = getChar<F42::Side>(msg);
	out.orderQty = getDouble<F42::OrderQty>(msg);
	out.text = getString<F42::Text>(msg);
	return out;
}

std::optional<std::string> validate(const NewOrderRequest& order)
{
	if (!(order.orderQty > 0.0))
		return "OrderQty must be greater than zero";
	const bool isLimit = order.ordType == fix_value::OrdTypeLimit || order.ordType == fix_value::OrdTypeStopLimit;
	const bool isStop = order.ordType == fix_value::OrdTypeStop || order.ordType == fix_value::OrdTypeStopLimit;
	if (isLimit && !order.price)
		return "Price is required for limit orders";
	if (isStop && !order.stopPx)
		return "StopPx is required for stop orders";
	return std::nullopt;
}

F42::ExecutionReport* encode(const ExecutionReport& report)
{
	auto* msg = new F42::ExecutionReport;
	*msg << new F42::OrderID(report.orderId)
		<< new F42::ExecID(report.execId)
		<< new F42::ExecTransType(report.execTransType)
		<< new F42::ExecType(report.execType)
		<< new F42::OrdStatus(report.ordStatus)
		<< new F42::Symbol(report.symbol)
		<< new F42::Side(report.side)
		<< new F42::LeavesQty(report.leavesQty)
		<< new F42::CumQty(report.cumQty)
		<< new F42::AvgPx(report.avgPx)
		<< new F42::TransactTime;
	addIfSet(*msg, new F42::ClOrdID(report.clOrdId), !report.clOrdId.empty());
	addIfSet(*msg, new F42::OrigClOrdID(report.origClOrdId), !report.origClOrdId.empty());
	addIfSet(*msg, new F42::Account(report.account), !report.account.empty());
	addIfSet(*msg, new F42::OrdType(report.ordType), report.ordType != 0);
	addIfSet(*msg, new F42::TimeInForce(report.timeInForce), report.timeInForce != 0);
	addIfSet(*msg, new F42::Text(report.text), !report.text.empty());
	addIfSet<F42::OrderQty>(*msg, report.orderQty);
	addIfSet<F42::Price>(*msg, report.price);
	addIfSet<F42::StopPx>(*msg, report.stopPx);
	addIfSet<F42::LastShares>(*msg, report.lastShares);
	addIfSet<F42::LastPx>(*msg, report.lastPx);
	addIfSet<F42::OrdRejReason>(*msg, report.ordRejReason);
	return msg;
}

F42::OrderCancelReject* encode(const OrderCancelReject& reject)
{
	auto* msg = new F42::OrderCancelReject;
	*msg << new F42::OrderID(reject.orderId.empty() ? std::string("NONE") : reject.orderId)
		<< new F42::ClOrdID(reject.clOrdId)
		<< new F42::OrigClOrdID(reject.origClOrdId)
		<< new F42::OrdStatus(reject.ordStatus)
		<< new F42::CxlRejResponseTo(reject.cxlRejResponseTo);
	addIfSet<F42::CxlRejReason>(*msg, reject.cxlRejReason);
	addIfSet(*msg, new F42::Text(reject.text), !reject.text.empty());
	return msg;
}

ExecutionReport makeRejection(const NewOrderRequest& order, const std::string& execId, int ordRejReason,
		const std::string& text)
{
	ExecutionReport report;
	report.orderId = "NONE";
	report.execId = execId;
	report.clOrdId = order.clOrdId;
	report.execType = fix_value::ExecRejected;
	report.ordStatus = fix_value::ExecRejected;
	report.account = order.account;
	report.symbol = order.symbol;
	report.side = order.side;
	report.orderQty = order.orderQty;
	report.ordType = order.ordType;
	report.price = order.price;
	report.stopPx = order.stopPx;
	report.ordRejReason = ordRejReason;
	report.text = text;
	return report;
}

} // namespace ib_fix::fix42
