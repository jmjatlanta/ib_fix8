// The live tests' "no position left behind" check, with positions supplied by the test
#include "TwsPositions.hpp"
#include <gtest/gtest.h>
#include <deque>

using test_util::PositionCheck;
using test_util::Positions;
using namespace std::chrono_literals;

namespace {

/// returns the given readings in turn, repeating the last one
PositionCheck::Reader readings(std::deque<std::optional<Positions>> values)
{
	auto queue = std::make_shared<std::deque<std::optional<Positions>>>(std::move(values));
	return [queue]() {
		auto value = queue->front();
		if (queue->size() > 1)
			queue->pop_front();
		return value;
	};
}

} // namespace

TEST(PositionCheck, PassesWhenThePositionIsUnchanged)
{
	PositionCheck check;
	EXPECT_EQ(check.begin(readings({ Positions{ { "DU1", 1 } } }), "AAPL"), "");
	EXPECT_EQ(check.end(1s, 10ms), "");
}

TEST(PositionCheck, FailsWhenThePositionChangedAndStaysChanged)
{
	PositionCheck check;
	ASSERT_EQ(check.begin(readings({ Positions{}, Positions{ { "DU1", 1 } } }), "AAPL"), "");
	const std::string problem = check.end(1s, 10ms);
	EXPECT_NE(problem.find("AAPL position changed"), std::string::npos) << problem;
	EXPECT_NE(problem.find("before flat"), std::string::npos) << problem;
	EXPECT_NE(problem.find("after DU1 1"), std::string::npos) << problem;
}

TEST(PositionCheck, WaitsForTwsToCatchUp)
{
	// TWS reports the position change from the test's last fill a little late
	PositionCheck check;
	ASSERT_EQ(check.begin(readings({ Positions{}, Positions{ { "DU1", 1 } }, Positions{ { "DU1", 1 } }, Positions{} }), "AAPL"), "");
	EXPECT_EQ(check.end(1s, 10ms), "");
}

TEST(PositionCheck, FailsWhenPositionsCannotBeRead)
{
	PositionCheck check;
	EXPECT_NE(check.begin(readings({ std::nullopt }), "AAPL"), "");
	EXPECT_FALSE(check.started());

	PositionCheck later;
	ASSERT_EQ(later.begin(readings({ Positions{}, std::nullopt }), "AAPL"), "");
	EXPECT_NE(later.end(1s, 10ms).find("could not read"), std::string::npos);
}
