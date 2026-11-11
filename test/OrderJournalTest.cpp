#include "ib/OrderJournal.hpp"
#include <gtest/gtest.h>
#include <fstream>

using namespace ib_fix;
using namespace ib_fix::ib;
namespace fv = ib_fix::fix_value;

namespace {

OrderRecord sampleRecord()
{
	OrderRecord r;
	r.clientCompId = "STRAT 1=%,x";            // characters the format must escape
	r.ibOrderId = 1234;
	r.terms.clOrdId = "ord,1 =%";
	r.terms.account = "DU111";
	r.terms.symbol = "AAPL";
	r.terms.securityType = "CS";
	r.terms.currency = "USD";
	r.terms.exchange = "SMART";
	r.terms.side = fv::SideSell;
	r.terms.orderQty = 300;
	r.terms.ordType = fv::OrdTypeStopLimit;
	r.terms.price = 189.123456789;
	r.terms.stopPx = 190.5;
	r.terms.timeInForce = fv::TifGoodTillCancel;
	r.terms.text = "line one\nline two";
	r.ordStatus = fv::OrdStatusPartiallyFilled;
	r.acknowledged = true;
	r.cumQty = 100;
	r.avgPx = 189.0 / 3.0;
	r.execIds = { "0000e0d5.6af41d6f.01.01", "exec,2" };
	r.clOrdIds = { "ord,1 =%", "cxl-1" };
	r.updated = 1794409340;
	PendingChange p;
	p.isReplace = true;
	p.clOrdId = "rep-1";
	p.replace.clOrdId = "rep-1";
	p.replace.origClOrdId = "ord,1 =%";
	p.replace.orderQty = 400;
	p.replace.ordType = fv::OrdTypeLimit;
	p.replace.price = 188.0;
	p.replace.side = fv::SideSell;
	p.replace.symbol = "AAPL";
	r.pending = p;
	return r;
}

std::filesystem::path journalPath(const std::string& name)
{
	const auto dir = std::filesystem::path(TEST_OUTPUT_DIR) / "order_journal";
	std::filesystem::create_directories(dir);
	const auto path = dir / name;
	std::filesystem::remove(path);
	return path;
}

std::time_t secondsAgo(int seconds)
{
	return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - std::chrono::seconds(seconds));
}

} // namespace

TEST(OrderRecord, RoundTripsThroughAJournalLine)
{
	const OrderRecord r = sampleRecord();
	const std::string line = toJournalLine(r);
	EXPECT_EQ(line.find('\n'), std::string::npos) << line;

	const auto back = fromJournalLine(line);
	ASSERT_TRUE(back.has_value()) << line;
	EXPECT_EQ(back->clientCompId, r.clientCompId);
	EXPECT_EQ(back->ibOrderId, r.ibOrderId);
	EXPECT_EQ(back->terms.clOrdId, r.terms.clOrdId);
	EXPECT_EQ(back->terms.account, r.terms.account);
	EXPECT_EQ(back->terms.side, r.terms.side);
	EXPECT_EQ(back->terms.orderQty, r.terms.orderQty);
	EXPECT_EQ(back->terms.ordType, r.terms.ordType);
	EXPECT_EQ(back->terms.price, r.terms.price) << "doubles must round-trip exactly";
	EXPECT_EQ(back->terms.stopPx, r.terms.stopPx);
	EXPECT_EQ(back->terms.timeInForce, r.terms.timeInForce);
	EXPECT_EQ(back->terms.text, r.terms.text);
	EXPECT_EQ(back->ordStatus, r.ordStatus);
	EXPECT_EQ(back->acknowledged, r.acknowledged);
	EXPECT_EQ(back->cumQty, r.cumQty);
	EXPECT_EQ(back->avgPx, r.avgPx);
	EXPECT_EQ(back->execIds, r.execIds);
	EXPECT_EQ(back->clOrdIds, r.clOrdIds);
	EXPECT_EQ(back->updated, r.updated);
	ASSERT_TRUE(back->pending.has_value());
	EXPECT_TRUE(back->pending->isReplace);
	EXPECT_EQ(back->pending->clOrdId, "rep-1");
	EXPECT_EQ(back->pending->replace.origClOrdId, "ord,1 =%");
	EXPECT_EQ(back->pending->replace.orderQty, 400.0);
	EXPECT_EQ(back->pending->replace.price, 188.0);
}

TEST(OrderRecord, RoundTripsWithoutOptionalParts)
{
	OrderRecord r;
	r.clientCompId = "S";
	r.ibOrderId = 7;
	r.terms.clOrdId = "o";
	r.terms.ordType = fv::OrdTypeMarket;
	const auto back = fromJournalLine(toJournalLine(r));
	ASSERT_TRUE(back.has_value());
	EXPECT_FALSE(back->terms.price.has_value());
	EXPECT_FALSE(back->terms.stopPx.has_value());
	EXPECT_FALSE(back->pending.has_value());
	EXPECT_TRUE(back->execIds.empty());
	EXPECT_TRUE(back->clOrdIds.empty());
}

TEST(OrderRecord, RejectsMalformedLines)
{
	EXPECT_FALSE(fromJournalLine("").has_value());
	EXPECT_FALSE(fromJournalLine("hello world").has_value());
	std::string line = toJournalLine(sampleRecord());
	EXPECT_FALSE(fromJournalLine(line.substr(0, line.size() / 2)).has_value()) << "truncated (e.g. by a crash)";
	EXPECT_FALSE(fromJournalLine("v=2" + line.substr(3)).has_value()) << "unknown version";
}

TEST(OrderJournal, LoadKeepsTheLatestStateOfEachOrder)
{
	const auto path = journalPath("latest.journal");
	{
		OrderJournal journal(path);
		OrderRecord r = sampleRecord();
		r.updated = secondsAgo(10);
		r.cumQty = 100;
		journal.append(r);
		r.cumQty = 200;
		journal.append(r);
		OrderRecord other = r;
		other.ibOrderId = 99;
		journal.append(other);
	}
	OrderJournal journal(path);
	const auto contents = journal.load();
	ASSERT_EQ(contents.orders.size(), 2u);
	EXPECT_EQ(contents.orders[0].ibOrderId, 99);
	EXPECT_EQ(contents.orders[1].ibOrderId, 1234);
	EXPECT_EQ(contents.orders[1].cumQty, 200.0);
	EXPECT_EQ(contents.unreadableLines, 0u);
}

TEST(OrderJournal, DropsOrdersFinishedBeforeTodayButKeepsWorkingOnes)
{
	const auto path = journalPath("days.journal");
	{
		OrderJournal journal(path);
		OrderRecord oldDone = sampleRecord();
		oldDone.ibOrderId = 1;
		oldDone.ordStatus = fv::OrdStatusFilled;
		oldDone.updated = secondsAgo(3 * 24 * 3600);
		journal.append(oldDone);
		OrderRecord oldWorking = sampleRecord();
		oldWorking.ibOrderId = 2;
		oldWorking.ordStatus = fv::ExecNew;     // e.g. a GTC order
		oldWorking.updated = secondsAgo(3 * 24 * 3600);
		journal.append(oldWorking);
		OrderRecord todayDone = sampleRecord();
		todayDone.ibOrderId = 3;
		todayDone.ordStatus = fv::ExecCanceled;
		todayDone.updated = secondsAgo(1);       // kept: its ClOrdIDs still count as used
		journal.append(todayDone);
	}
	OrderJournal journal(path);
	const auto contents = journal.load();
	ASSERT_EQ(contents.orders.size(), 2u);
	EXPECT_EQ(contents.orders[0].ibOrderId, 2);
	EXPECT_EQ(contents.orders[1].ibOrderId, 3);
}

TEST(OrderJournal, LoadCompactsTheFileAndSkipsDamagedLines)
{
	const auto path = journalPath("compact.journal");
	{
		OrderJournal journal(path);
		OrderRecord r = sampleRecord();
		r.updated = secondsAgo(1);
		for (int i = 0; i < 5; ++i)
			journal.append(r);
	}
	{
		std::ofstream damaged(path, std::ios::app);
		damaged << "v=1 id=12 client=S status=0 trunc"; // a line cut short by a crash
	}
	{
		OrderJournal journal(path);
		const auto contents = journal.load();
		EXPECT_EQ(contents.orders.size(), 1u);
		EXPECT_EQ(contents.unreadableLines, 1u);
		// appending still works after the rewrite
		OrderRecord next = sampleRecord();
		next.ibOrderId = 5;
		next.updated = secondsAgo(1);
		journal.append(next);
	}
	std::ifstream in(path);
	std::string line;
	size_t lines = 0;
	while (std::getline(in, line))
		++lines;
	EXPECT_EQ(lines, 2u) << "five copies compacted to one, plus the new order";
}
