#pragma once
// Test helpers: a FIX 4.2 initiator (playing the strategy) and configuration files for it and the acceptor
#include "fix/Fix42Codec.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace test_util {

using namespace std::chrono_literals;

/// tag -> value for a raw FIX message (later duplicates overwrite earlier ones)
std::map<int, std::string> parseFix(const std::string& wire);

/// a TCP port on 127.0.0.1 that was free a moment ago
int freeTcpPort();

/// a fresh, empty directory under the test output directory
std::filesystem::path makeOutputDir(const std::string& name);

/// writes a FIX8 acceptor config (session name "IBFIX", SenderCompID "IBFIX"), returns its path
std::filesystem::path writeAcceptorConfig(const std::filesystem::path& dir, int port);

bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5000ms);

struct ReceivedMessage
{
	std::string msgType;
	std::string wire;
	std::map<int, std::string> fields;
	std::string field(int tag) const;
};

/// application messages received by the test client
class MessageQueue
{
	public:
	void push(ReceivedMessage msg);
	/// waits for (and removes) the first message of the given type
	std::optional<ReceivedMessage> waitFor(const std::string& msgType, std::chrono::milliseconds timeout = 5000ms);
	size_t size() const;

	private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<ReceivedMessage> messages_;
};

class TestClientSession : public FIX8::Session
{
	public:
	TestClientSession(const FIX8::F8MetaCntx& ctx, const FIX8::SessionID& sid, FIX8::Persister* persist = nullptr,
			FIX8::Logger* logger = nullptr, FIX8::Logger* plogger = nullptr)
			: FIX8::Session(ctx, sid, persist, logger, plogger) {}
	bool handle_application(const unsigned seqnum, const FIX8::Message*& msg) override;
	/// FIX8 neither answers a Logout nor changes state on one; behave like a FIX client that does
	bool handle_logout(const unsigned seqnum, const FIX8::Message* msg) override;
	MessageQueue received;
	std::atomic<bool> logoutSent = false;
	std::atomic<bool> logoutReceived = false;
};

/// a strategy connecting to the acceptor
class TestClient
{
	public:
	TestClient(const std::filesystem::path& dir, int port, const std::string& senderCompId);
	~TestClient();
	/// connect and log on; @return true once the session is established
	bool logon(std::chrono::milliseconds timeout = 5000ms);
	/// send Logout, wait for the reply, then disconnect
	void logout();
	void send(FIX8::Message* msg);
	bool isLoggedOn();
	TestClientSession& session();

	private:
	std::unique_ptr<FIX8::ClientSessionBase> client_;
	TestClientSession* session_ = nullptr;
	bool stopped_ = false;
};

} // namespace test_util
