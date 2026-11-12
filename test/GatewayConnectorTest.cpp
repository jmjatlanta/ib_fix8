// GatewayConnector's handling of TWS connection events, played through ib_helper's callbacks.
// None of these need TWS.
#include "ib/GatewayConnector.hpp"
#include <gtest/gtest.h>
#include <atomic>

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
};

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
