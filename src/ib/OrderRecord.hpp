#pragma once
// The gateway's record of one order: who owns it, what the client asked for, and what the client
// has been told about it. Kept in memory by IbOrderHandler and saved in the OrderJournal.
#include "OrderMessages.hpp"
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ib_fix::ib {

/// a cancel or cancel/replace sent to TWS and not answered yet
struct PendingChange
{
	bool isReplace = false;
	std::string clOrdId;
	ReplaceRequest replace;   ///< the new terms, when isReplace
};

struct OrderRecord
{
	std::string clientCompId;
	int ibOrderId = 0;
	NewOrderRequest terms;    ///< current terms; terms.clOrdId is the current ClOrdID
	char ordStatus = fix_value::ExecPendingNew;
	bool acknowledged = false;
	double cumQty = 0.0;
	double avgPx = 0.0;
	std::optional<PendingChange> pending;
	std::set<std::string> execIds;         ///< TWS executions reported for this order
	std::vector<std::string> clOrdIds;     ///< every ClOrdID that has referred to it (order, cancels, replaces)
	std::time_t updated = 0;               ///< when this state was recorded

	bool isTerminal() const
	{
		return ordStatus == fix_value::OrdStatusFilled || ordStatus == fix_value::ExecCanceled
			|| ordStatus == fix_value::ExecRejected;
	}
};

/// one line of text (no newline) holding the whole record
std::string toJournalLine(const OrderRecord& record);
/// @return nothing if the line is not a valid record
std::optional<OrderRecord> fromJournalLine(const std::string& line);

} // namespace ib_fix::ib
