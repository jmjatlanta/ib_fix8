#include "AcceptorSession.hpp"
#include "Acceptor.hpp"
#include <chrono>
#include <thread>

namespace ib_fix::fix42 {

namespace F42 = FIX8::FIX42;

namespace {
// BusinessRejectReason (380)
constexpr int BusinessRejectOther = 0;
constexpr int BusinessRejectUnsupportedMessageType = 3;
// how long logoutAndStop waits for the client's Logout reply
constexpr auto LogoutReplyTimeout = std::chrono::seconds(2);
} // namespace

bool AcceptorRouter::operator()(const F42::NewOrderSingle* msg) const
{
	session_.onNewOrder(*msg);
	return true;
}

bool AcceptorRouter::operator()(const F42::OrderCancelRequest* msg) const
{
	session_.onCancel(*msg);
	return true;
}

bool AcceptorRouter::operator()(const F42::OrderCancelReplaceRequest* msg) const
{
	session_.onReplace(*msg);
	return true;
}

AcceptorSession::AcceptorSession(const FIX8::F8MetaCntx& ctx, const FIX8::sender_comp_id& sci,
		FIX8::Persister* persist, FIX8::Logger* logger, FIX8::Logger* plogger)
		: FIX8::Session(ctx, sci, persist, logger, plogger), router_(*this)
{
}

bool AcceptorSession::authenticate(FIX8::SessionID& id, const FIX8::Message*)
{
	// for an acceptor the SessionID's target is the client
	const std::string compId = id.get_targetCompID()();
	if (acceptor_ == nullptr || compId.empty())
		return false;
	if (!acceptor_->registerSession(compId, this))
	{
		log("Rejecting logon from " + compId + ": already logged on in another session", FIX8::Logger::Warn);
		return false;
	}
	clientCompId_ = compId;
	registered_ = true;
	return true;
}

void AcceptorSession::state_change(const FIX8::States::SessionStates before, const FIX8::States::SessionStates after)
{
	namespace States = FIX8::States;
	if (after == States::st_continuous && !loggedOn_.exchange(true))
	{
		acceptor_->handler().onLogon(clientCompId_);
		return;
	}
	if (after == States::st_logoff_sent || after == States::st_logoff_received
			|| after == States::st_session_terminated)
		markLoggedOff();
}

void AcceptorSession::markLoggedOff()
{
	if (loggedOn_.exchange(false))
		acceptor_->handler().onLogout(clientCompId_);
	if (registered_.exchange(false))
		acceptor_->unregisterSession(clientCompId_, this);
}

bool AcceptorSession::handle_application(const unsigned seqnum, const FIX8::Message*& msg)
{
	if (enforce(seqnum, msg))
		return false;
	currentSeqNum_ = seqnum;
	if (!msg->process(router_))
		businessReject(*msg, BusinessRejectUnsupportedMessageType,
				"Unsupported message type " + msg->get_msgtype());
	return true;
}

bool AcceptorSession::handle_logout(const unsigned seqnum, const FIX8::Message* msg)
{
	const bool result = FIX8::Session::handle_logout(seqnum, msg);
	// FIX: answer a client-initiated Logout, then the client disconnects. FIX8 does neither, and the
	// session cannot be stopped from its own reader thread, so the Acceptor stops it.
	if (!logoutSent_.exchange(true))
		send(generate_logout());
	markLoggedOff();
	peerLoggedOut_ = true;
	return result;
}

void AcceptorSession::logoutAndStop(const std::string& text)
{
	if (loggedOn_ && !is_shutdown() && !logoutSent_.exchange(true))
	{
		send(generate_logout(text.c_str()));
		const auto deadline = std::chrono::steady_clock::now() + LogoutReplyTimeout;
		while (!peerLoggedOut_ && !is_shutdown() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	markLoggedOff();
	stop();
}

void AcceptorSession::businessReject(const FIX8::Message& msg, int reason, const std::string& text)
{
	if (FIX8::Message* reject = generate_business_reject(currentSeqNum_, &msg, reason, text.c_str()))
		send(reject);
}

void AcceptorSession::onNewOrder(const F42::NewOrderSingle& msg)
{
	const NewOrderRequest request = decode(msg);
	if (auto problem = validate(request))
	{
		send(encode(makeRejection(request, acceptor_->nextExecId(), fix_value::OrdRejBrokerOption, *problem)));
		return;
	}
	try
	{
		acceptor_->handler().onNewOrder(clientCompId_, request);
	}
	catch (const std::exception& e)
	{
		log(std::string("onNewOrder threw: ") + e.what(), FIX8::Logger::Error);
		send(encode(makeRejection(request, acceptor_->nextExecId(), fix_value::OrdRejBrokerOption,
				std::string("Order not accepted: ") + e.what())));
	}
}

void AcceptorSession::onCancel(const F42::OrderCancelRequest& msg)
{
	try
	{
		acceptor_->handler().onCancel(clientCompId_, decode(msg));
	}
	catch (const std::exception& e)
	{
		// the order's status is unknown here, so an OrderCancelReject cannot be filled in correctly
		log(std::string("onCancel threw: ") + e.what(), FIX8::Logger::Error);
		businessReject(msg, BusinessRejectOther, std::string("Cancel not processed: ") + e.what());
	}
}

void AcceptorSession::onReplace(const F42::OrderCancelReplaceRequest& msg)
{
	try
	{
		acceptor_->handler().onReplace(clientCompId_, decode(msg));
	}
	catch (const std::exception& e)
	{
		log(std::string("onReplace threw: ") + e.what(), FIX8::Logger::Error);
		businessReject(msg, BusinessRejectOther, std::string("Replace not processed: ") + e.what());
	}
}

} // namespace ib_fix::fix42
