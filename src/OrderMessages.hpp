#pragma once
// FIX 4.2 order-entry messages as plain structs, independent of the FIX engine.
// Character fields hold the FIX 4.2 wire values (e.g. Side '1' = Buy, OrdType '2' = Limit).
#include <optional>
#include <string>

namespace ib_fix {

namespace fix_value {
// Side (54)
inline constexpr char SideBuy = '1';
inline constexpr char SideSell = '2';
inline constexpr char SideSellShort = '5';
// OrdType (40)
inline constexpr char OrdTypeMarket = '1';
inline constexpr char OrdTypeLimit = '2';
inline constexpr char OrdTypeStop = '3';
inline constexpr char OrdTypeStopLimit = '4';
// TimeInForce (59)
inline constexpr char TifDay = '0';
inline constexpr char TifGoodTillCancel = '1';
inline constexpr char TifImmediateOrCancel = '3';
inline constexpr char TifFillOrKill = '4';
// ExecTransType (20)
inline constexpr char ExecTransNew = '0';
// ExecType (150) / OrdStatus (39)
inline constexpr char ExecNew = '0';
inline constexpr char ExecPartialFill = '1';
inline constexpr char ExecFill = '2';
inline constexpr char ExecCanceled = '4';
inline constexpr char ExecReplaced = '5';
inline constexpr char ExecPendingCancel = '6';
inline constexpr char ExecRejected = '8';
inline constexpr char ExecPendingNew = 'A';
inline constexpr char ExecPendingReplace = 'E';
// OrdRejReason (103)
inline constexpr int OrdRejBrokerOption = 0;
inline constexpr int OrdRejUnknownSymbol = 1;
inline constexpr int OrdRejUnknownOrder = 5;
inline constexpr int OrdRejDuplicateOrder = 6;
// CxlRejResponseTo (434)
inline constexpr char CxlRejToCancel = '1';
inline constexpr char CxlRejToReplace = '2';
// CxlRejReason (102)
inline constexpr int CxlRejTooLate = 0;
inline constexpr int CxlRejUnknownOrder = 1;
inline constexpr int CxlRejBrokerOption = 2;
inline constexpr int CxlRejAlreadyPending = 3;
// TimeInForce (59), continued
inline constexpr char TifAtTheOpening = '2';
// OrdStatus (39) values that are not also ExecType values in FIX 4.2
inline constexpr char OrdStatusPartiallyFilled = '1';
inline constexpr char OrdStatusFilled = '2';
} // namespace fix_value

/// NewOrderSingle (D)
struct NewOrderRequest
{
	std::string clOrdId;
	std::string account;
	std::string symbol;
	std::string securityType;     // SecurityType (167), e.g. "CS", "FUT", "OPT"; empty if not sent
	std::string currency;         // Currency (15)
	std::string exchange;         // SecurityExchange (207)
	char side = 0;
	double orderQty = 0.0;
	char ordType = 0;
	std::optional<double> price;  // Price (44), limit orders
	std::optional<double> stopPx; // StopPx (99), stop orders
	char timeInForce = fix_value::TifDay; // FIX default when TimeInForce is absent
	std::string text;
};

/// OrderCancelReplaceRequest (G): the full replacement order plus the order it replaces
struct ReplaceRequest : NewOrderRequest
{
	std::string origClOrdId;
	std::string orderId;          // OrderID (37), if the client sent it
};

/// OrderCancelRequest (F)
struct CancelRequest
{
	std::string clOrdId;
	std::string origClOrdId;
	std::string orderId;
	std::string symbol;
	char side = 0;
	std::optional<double> orderQty;
	std::string text;
};

/// ExecutionReport (8) sent to the client
struct ExecutionReport
{
	std::string orderId;
	std::string execId;
	std::string clOrdId;
	std::string origClOrdId;      // set for cancel/replace acknowledgements
	char execTransType = fix_value::ExecTransNew;
	char execType = 0;
	char ordStatus = 0;
	std::string account;
	std::string symbol;
	char side = 0;
	std::optional<double> orderQty;
	char ordType = 0;             // 0 = omit
	std::optional<double> price;
	std::optional<double> stopPx;
	char timeInForce = 0;         // 0 = omit
	std::optional<double> lastShares;
	std::optional<double> lastPx;
	double leavesQty = 0.0;
	double cumQty = 0.0;
	double avgPx = 0.0;
	std::optional<int> ordRejReason;
	std::string text;
};

/// OrderCancelReject (9) sent to the client
struct OrderCancelReject
{
	std::string orderId;          // "NONE" when the order is unknown
	std::string clOrdId;
	std::string origClOrdId;
	char ordStatus = 0;
	char cxlRejResponseTo = fix_value::CxlRejToCancel;
	std::optional<int> cxlRejReason;
	std::string text;
};

} // namespace ib_fix
