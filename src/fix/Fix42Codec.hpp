#pragma once
// Conversion between FIX8's generated FIX 4.2 messages and ib_fix's order structs
#include "../OrderMessages.hpp"
#include <fix8/f8includes.hpp>
#include "FIX42_types.hpp"
#include "FIX42_router.hpp"
#include "FIX42_classes.hpp"
#include <optional>
#include <string>

namespace ib_fix::fix42 {

NewOrderRequest decode(const FIX8::FIX42::NewOrderSingle& msg);
CancelRequest decode(const FIX8::FIX42::OrderCancelRequest& msg);
ReplaceRequest decode(const FIX8::FIX42::OrderCancelReplaceRequest& msg);

/// Checks the conditional requirements the FIX 4.2 dictionary cannot express: a positive OrderQty,
/// Price for limit orders and StopPx for stop orders.
/// @return a description of the problem, or nothing if the order is well formed
std::optional<std::string> validate(const NewOrderRequest& order);

/// @return a new message; the caller owns it (FIX8::Session::send takes ownership by default)
FIX8::FIX42::ExecutionReport* encode(const ExecutionReport& report);
FIX8::FIX42::OrderCancelReject* encode(const OrderCancelReject& reject);

/// ExecutionReport rejecting an order that failed validation or could not be accepted
/// @param execId must be unique for the session
ExecutionReport makeRejection(const NewOrderRequest& order, const std::string& execId, int ordRejReason,
		const std::string& text);

} // namespace ib_fix::fix42
