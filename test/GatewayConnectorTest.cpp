// GatewayConnector's handling of TWS connection events, played through ib_helper's callbacks.
// None of these need TWS.
#include "ib/GatewayConnector.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using ib_fix::ib::GatewayConnector;
using Status = ib_helper::IBConnector::ConnectionStatus;

class TestConnector : public GatewayConnector
{
	public:
	TestConnector() : GatewayConnector() {} // does not connect
	using GatewayConnector::error;
	using GatewayConnector::nextValidId;
	using GatewayConnector::connectionClosed;
	void SetStatus(Status in) { currentConnectionStatus = in; }
	/// TWS accepts the connection: PARTIALLY_CONNECTED, then nextValidId
	void Accept()
	{
		SetStatus(Status::PARTIALLY_CONNECTED);
		nextValidId(1);
	}
	void TwsError(int code, const std::string& message) { error(-1, time(nullptr), code, message, ""); }
	/// where the supervisor's reconnects go: a local port nothing listens on
	void SetAddress(const std::string& host, int port) { hostname = host; this->port = port; }
};

class Log
{
	public:
	std::function<void(const std::string&)> sink()
	{
		return [this](const std::string& line) { std::scoped_lock lock(m); lines.push_back(line); };
	}
	bool contains(const std::string& text)
	{
		std::scoped_lock lock(m);
		for (const auto& l : lines)
			if (l.find(text) != std::string::npos)
				return true;
		return false;
	}
	std::mutex m;
	std::vector<std::string> lines;
};

bool waitFor(std::function<bool()> pred, std::chrono::milliseconds timeout)
{
	const auto end = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < end)
	{
		if (pred())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return pred();
}

class Listener : public ib_fix::ib::TwsEventListener
{
	public:
	void onTwsConnected() override { ++connected; }
	void onTwsDisconnected() override { ++disconnected; }
	std::atomic<int> connected = 0;
	std::atomic<int> disconnected = 0;
};

class GatewayConnectorTest : public ::testing::Test
{
	protected:
	GatewayConnectorTest() { connector.setListener(&listener); }
	~GatewayConnectorTest() override { connector.setListener(nullptr); }
	Listener listener;
	TestConnector connector;
};

} // namespace

TEST_F(GatewayConnectorTest, AvailableOnceTwsAcceptsTheConnection)
{
	EXPECT_FALSE(connector.isAvailable());
	connector.Accept();
	EXPECT_TRUE(connector.isAvailable());
	EXPECT_EQ(listener.connected, 1);
}

TEST_F(GatewayConnectorTest, UnavailableWhileTwsIsCutOffFromIb)
{
	connector.Accept();
	connector.TwsError(1100, "Connectivity between IB and Trader Workstation has been lost.");
	EXPECT_FALSE(connector.isAvailable());
	EXPECT_EQ(listener.disconnected, 0); // the connection to TWS is still up

	// restored: available again, and the listener catches up on what happened meanwhile
	connector.TwsError(1102, "Connectivity between IB and Trader Workstation has been restored - data maintained.");
	EXPECT_TRUE(connector.isAvailable());
	EXPECT_EQ(listener.connected, 2);

	connector.TwsError(1100, "Connectivity between IB and Trader Workstation has been lost.");
	connector.TwsError(1101, "Connectivity between IB and Trader Workstation has been restored - data lost.");
	EXPECT_TRUE(connector.isAvailable());
	EXPECT_EQ(listener.connected, 3);
}

TEST_F(GatewayConnectorTest, RestoredBeforeTheConnectionIsAcceptedDoesNotNotify)
{
	// 1102 can arrive while the API handshake is still in progress
	connector.SetStatus(Status::PARTIALLY_CONNECTED);
	connector.TwsError(1102, "Connectivity between IB and Trader Workstation has been restored - data maintained.");
	EXPECT_EQ(listener.connected, 0);
	EXPECT_FALSE(connector.isAvailable());
}

TEST_F(GatewayConnectorTest, SocketFailureWhileConnected)
{
	for (int code : { 502, 509 })
	{
		TestConnector conn;
		Listener l;
		conn.setListener(&l);
		conn.Accept();
		ASSERT_TRUE(conn.isAvailable());
		conn.TwsError(code, "Exception caught while reading socket - Connection reset by peer");
		// ib_helper treats it as a closed connection; nothing is disconnected on this (TWS's) thread
		EXPECT_FALSE(conn.isAvailable()) << code;
		EXPECT_EQ(l.disconnected, 1) << code;
		EXPECT_EQ(conn.GetConnectionStatus(), Status::SHUTDOWN) << code;
		conn.setListener(nullptr);
	}
}

TEST_F(GatewayConnectorTest, SupervisorReconnectsAfterSocketFailure)
{
	Log log;
	connector.setLog(log.sink());
	connector.SetAddress("127.0.0.1", 1);
	connector.Accept();
	connector.enableReconnect(std::chrono::seconds(1), std::chrono::seconds(30));
	connector.TwsError(509, "Exception caught while reading socket - Connection reset by peer");
	EXPECT_TRUE(waitFor([&] { return log.contains("Reconnecting to TWS"); }, std::chrono::seconds(3)));
	connector.stopReconnect();
	connector.setLog(nullptr);
}

TEST_F(GatewayConnectorTest, SupervisorDoesNotWaitOutTheHandshakeAfterSocketFailure)
{
	// a 502/509 during the handshake: retry now rather than after the 30s handshake timeout
	Log log;
	connector.setLog(log.sink());
	connector.SetAddress("127.0.0.1", 1);
	connector.SetStatus(Status::PARTIALLY_CONNECTED);
	connector.enableReconnect(std::chrono::seconds(1), std::chrono::seconds(30));
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_FALSE(log.contains("Reconnecting to TWS")); // still waiting for the handshake
	connector.TwsError(502, "Couldn't connect to TWS.");
	EXPECT_TRUE(waitFor([&] { return log.contains("Reconnecting to TWS"); }, std::chrono::seconds(3)));
	connector.stopReconnect();
	connector.setLog(nullptr);
}
