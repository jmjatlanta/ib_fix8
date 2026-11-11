#pragma once
// Order state saved on disk so the gateway can pick up its orders again after a restart.
// An append-only text file: each line is the latest state of one order (see OrderRecord.hpp).
#include "OrderRecord.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ib_fix::ib {

class OrderJournal
{
	public:
	struct Contents
	{
		std::vector<OrderRecord> orders;  ///< latest state of each order
		size_t unreadableLines = 0;       ///< lines that could not be parsed (skipped)
	};

	/// creates the file (and its directory) if needed
	explicit OrderJournal(std::filesystem::path file);
	OrderJournal(const OrderJournal&) = delete;
	OrderJournal& operator=(const OrderJournal&) = delete;

	/***
	 * Read the journal: the latest state of each order. Orders that finished before the start of
	 * today (local time) are dropped; working orders are kept however old. The file is then
	 * rewritten with just what was kept.
	 * @throws std::runtime_error if the file cannot be read or rewritten
	 */
	Contents load(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

	/// record an order's state; flushed to the operating system before returning
	void append(const OrderRecord& record);

	const std::filesystem::path& file() const { return file_; }

	private:
	void openForAppend();

	std::filesystem::path file_;
	std::ofstream out_;
};

} // namespace ib_fix::ib
