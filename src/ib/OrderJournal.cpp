#include "OrderJournal.hpp"
#include <map>
#include <stdexcept>

namespace ib_fix::ib {

namespace {

std::time_t startOfLocalDay(std::chrono::system_clock::time_point now)
{
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	std::tm local{};
	localtime_r(&t, &local);
	local.tm_hour = 0;
	local.tm_min = 0;
	local.tm_sec = 0;
	return std::mktime(&local);
}

} // namespace

OrderJournal::OrderJournal(std::filesystem::path file) : file_(std::move(file))
{
	if (file_.has_parent_path())
		std::filesystem::create_directories(file_.parent_path());
	openForAppend();
}

void OrderJournal::openForAppend()
{
	out_.close();
	out_.clear();
	out_.open(file_, std::ios::app);
	if (!out_)
		throw std::runtime_error("Cannot open order journal " + file_.string());
}

OrderJournal::Contents OrderJournal::load(std::chrono::system_clock::time_point now)
{
	Contents contents;
	std::map<int, OrderRecord> latest;
	{
		std::ifstream in(file_);
		if (!in)
			throw std::runtime_error("Cannot read order journal " + file_.string());
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty())
				continue;
			if (auto record = fromJournalLine(line))
				latest[record->ibOrderId] = std::move(*record);
			else
				++contents.unreadableLines;
		}
	}

	const std::time_t today = startOfLocalDay(now);
	for (auto& [id, record] : latest)
		if (!record.isTerminal() || record.updated >= today)
			contents.orders.push_back(std::move(record));

	// compact: rewrite with only what was kept, then switch atomically
	const std::filesystem::path temp = file_.string() + ".tmp";
	{
		std::ofstream out(temp, std::ios::trunc);
		for (const auto& record : contents.orders)
			out << toJournalLine(record) << '\n';
		out.flush();
		if (!out)
			throw std::runtime_error("Cannot write order journal " + temp.string());
	}
	out_.close();
	std::filesystem::rename(temp, file_);
	openForAppend();
	return contents;
}

void OrderJournal::append(const OrderRecord& record)
{
	out_ << toJournalLine(record) << '\n';
	out_.flush();
}

} // namespace ib_fix::ib
