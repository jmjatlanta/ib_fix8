#pragma once
// One FIX 4.2 connection from a client (strategy). Created by FIX8 for each accepted connection.
#include "Fix42Codec.hpp"
#include <atomic>
#include <string>

namespace ib_fix::fix42 {

class Acceptor;
class AcceptorSession;

/// Dispatches decoded FIX 4.2 application messages to the session
class AcceptorRouter : public FIX8::FIX42::FIX42_Router
{
	public:
	explicit AcceptorRouter(AcceptorSession& session) : session_(session) {}
	using FIX8::FIX42::FIX42_Router::operator();
	bool operator()(const FIX8::FIX42::NewOrderSingle* msg) const override;
	bool operator()(const FIX8::FIX42::OrderCancelRequest* msg) const override;
	bool operator()(const FIX8::FIX42::OrderCancelReplaceRequest* msg) const override;

	private:
	AcceptorSession& session_;
};

class AcceptorSession : public FIX8::Session
{
	public:
	/// signature required by FIX8::ServerSession
	AcceptorSession(const FIX8::F8MetaCntx& ctx, const FIX8::sender_comp_id& sci, FIX8::Persister* persist = nullptr,
			FIX8::Logger* logger = nullptr, FIX8::Logger* plogger = nullptr);

	/// must be called before the session is started
	void attach(Acceptor& acceptor) { acceptor_ = &acceptor; }

	/// the client's SenderCompID; empty until the client's Logon has been accepted
	const std::string& clientCompId() const { return clientCompId_; }
	bool isLoggedOn() const { return loggedOn_; }
	/// the client sent Logout and we replied; the owner should stop the session
	bool peerLoggedOut() const { return peerLoggedOut_; }

	/// Send Logout, give the client a moment to reply, then stop the session.
	/// Must not be called from the session's own reader thread.
	void logoutAndStop(const std::string& text);

	// FIX8::Session overrides
	bool handle_application(const unsigned seqnum, const FIX8::Message*& msg) override;
	bool handle_logout(const unsigned seqnum, const FIX8::Message* msg) override;
	bool authenticate(FIX8::SessionID& id, const FIX8::Message* msg) override;
	void state_change(const FIX8::States::SessionStates before, const FIX8::States::SessionStates after) override;

	private:
	friend class AcceptorRouter;
	friend class Acceptor;
	void onNewOrder(const FIX8::FIX42::NewOrderSingle& msg);
	void onCancel(const FIX8::FIX42::OrderCancelRequest& msg);
	void onReplace(const FIX8::FIX42::OrderCancelReplaceRequest& msg);
	/// BusinessMessageReject for the message currently being processed
	void businessReject(const FIX8::Message& msg, int reason, const std::string& text);
	/// tell the handler the client is gone (once) and release its CompID
	void markLoggedOff();

	AcceptorRouter router_;
	Acceptor* acceptor_ = nullptr;
	std::string clientCompId_;
	unsigned currentSeqNum_ = 0;  // inbound MsgSeqNum being processed (reader thread only)
	std::atomic<bool> loggedOn_ = false;
	std::atomic<bool> logoutSent_ = false;
	std::atomic<bool> peerLoggedOut_ = false;
	std::atomic<bool> registered_ = false;
};

} // namespace ib_fix::fix42
