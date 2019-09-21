/*
 * report.cpp
 *
 *  Created on: 17. 5. 2019
 *      Author: ondra
 */

#include "report.h"

#include <imtjson/value.h>
#include <imtjson/object.h>
#include <imtjson/array.h>
#include <chrono>
#include <numeric>

#include "../shared/linear_map.h"
#include "../shared/logOutput.h"
#include "../shared/range.h"
#include "../shared/stdLogOutput.h"
#include "sgn.h"

using ondra_shared::logError;
using std::chrono::_V2::system_clock;

using namespace json;




void Report::genReport() {

	Object st;
	exportCharts(st.object("charts"));
	exportOrders(st.array("orders"));
	exportTitles(st.object("info"));
	exportPrices(st.object("prices"));
	exportMisc(st.object("misc"));
	st.set("interval", interval_in_ms);
	st.set("time", std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch()
				   ).count());
	st.set("log", logLines);
	while (logLines.size()>30) logLines.erase(0);
	report->store(st);
}


struct ChartData {
	Array records;
	double last_price = 0;
	double sums = 0;
	double assets = 0;
	double init_price = 0;
};

void Report::setOrders(StrViewA symb, const std::optional<IStockApi::Order> &buy,
	  	  	  	  	  	  	  	     const std::optional<IStockApi::Order> &sell) {
	const json::Value &info = infoMap[symb];
	bool inverted = info["inverted"].getBool();

	int buyid = inverted?-1:1;

	OKey buyKey {symb, buyid};
	OKey sellKey {symb, -buyid};

	if (buy.has_value()) {
		orderMap[buyKey] = {inverted?1.0/buy->price:buy->price, buy->size*buyid};
	} else{
		orderMap[buyKey] = {0, 0};
	}

	if (sell.has_value()) {
		orderMap[sellKey] = {inverted?1.0/sell->price:sell->price, sell->size*buyid};
	} else {
		orderMap[sellKey] = {0, 0};
	}


}


void Report::setTrades(StrViewA symb, StringView<IStockApi::TradeWithBalance> trades, bool margin) {

	using ondra_shared::range;

	json::Array records;

	const json::Value &info = infoMap[symb];
	bool inverted = info["inverted"].getBool();
	double pos = info["po"].getNumber();



	if (!trades.empty()) {

		const auto &last = trades[trades.length-1];
		std::size_t last_time = last.time;
		std::size_t first = last_time - interval_in_ms;


		auto tend = trades.end();
		auto iter = trades.begin();
		auto &&t = *iter;

		std::size_t invest_beg_time = t.time;
		double invst_value = t.eff_price*t.balance;

		//so the first trade doesn't change the value of portfolio
//		double init_value = init_balance*t.eff_price+init_fiat;
		//
		double init_price = t.eff_price;


		double prev_balance = t.balance-t.eff_size;
		double prev_price = init_price;
		double cur_sum = 0;
		double cur_fromPos = 0;
		double norm_sum_ass = 0;
		double norm_sum_cur = 0;
		double potentialpl = 0;
		double neutral_price = 0;


		while (iter != tend) {

			auto &&t = *iter;

			double gain = (t.eff_price - prev_price)*pos ;
			double earn = -t.eff_price * t.eff_size;
			double bal_chng = (t.balance - prev_balance) - t.eff_size;
			invst_value += bal_chng * t.eff_price;


			double calcbal = prev_balance * sqrt(prev_price/t.eff_price);
			double asschg = (prev_balance+t.eff_size) - calcbal ;
			double curchg = -(calcbal * t.eff_price -  prev_balance * prev_price - earn);
			double norm_chng = 0;

			if (iter != trades.begin() && !iter->manual_trade) {
				cur_fromPos += gain;
				cur_sum += earn;

				norm_sum_ass += asschg;
				norm_sum_cur += curchg;
				norm_chng = curchg+asschg * t.eff_price;

				pos += t.eff_size;


				double np = t.balance-pos;
				neutral_price = t.eff_price * pow2(t.balance/np);
				potentialpl = cur_fromPos + pos*(neutral_price-sqrt(t.eff_price*neutral_price));


			}
			if (iter->manual_trade) {
				invst_value += earn;
			}
			double norm;
			norm = norm_sum_cur;


			prev_balance = t.balance;
			prev_price = t.eff_price;

			double invst_time = t.time - invest_beg_time;
			double invst_n = norm/invst_time;
			if (!std::isfinite(invst_n)) invst_n = 0;


			if (t.time >= first) {
				records.push_back(Object
						("id", t.id)
						("time", t.time)
						("achg", (inverted?-1:1)*t.eff_size)
						("gain", gain)
						("norm", margin?Value():Value(norm))
						("normch", norm_chng)
						("nacum", margin?Value():Value((inverted?-1:1)*norm_sum_ass))
						("pos", (inverted?-1:1)*pos)
						("pl", cur_fromPos)
						("pln", potentialpl)
						("price", (inverted?1.0/t.price:t.price))
						("invst_v", invst_value)
						("invst_n", invst_n)
						("volume", (inverted?1:-1)*t.eff_price*t.eff_size)
						("man",t.manual_trade)
						("np",(inverted?1.0/neutral_price:neutral_price))
				);
			}


			++iter;
		}

	}
	tradeMap[symb] = records;
}


void Report::exportCharts(json::Object&& out) {

	for (auto &&rec: tradeMap) {
		out.set(rec.first, rec.second);
	}
}

bool Report::OKeyCmp::operator ()(const OKey& a, const OKey& b) const {
	int cmp = a.symb.compare(b.symb);
	if (cmp == 0) {
		return a.dir < b.dir;
	} else {
		return cmp < 0;
	}
}

void Report::setInfo(StrViewA symb, const InfoObj &infoObj) {
	infoMap[symb] = Object
			("title",infoObj.title)
			("currency", infoObj.currencySymb)
			("asset", infoObj.assetSymb)
			("price_symb", infoObj.priceSymb)
			("inverted", infoObj.inverted)
			("emulated",infoObj.emulated)
			("po", infoObj.position_offset);
}

void Report::setPrice(StrViewA symb, double price) {

	const json::Value &info = infoMap[symb];
	bool inverted = info["inverted"].getBool();

	priceMap[symb] = inverted?1.0/price:price;;
}


void Report::exportOrders(json::Array &&out) {

	for (auto &&ord : orderMap) {
		if (ord.second.size) {
			out.push_back(Object
					("symb",ord.first.symb)
					("dir",static_cast<int>(ord.first.dir))
					("size",ord.second.size)
					("price",ord.second.price)
			);
		}
	}
}

void Report::exportTitles(json::Object&& out) {
	for (auto &&rec: infoMap) {
			out.set(rec.first, rec.second);
	}
}

void Report::exportPrices(json::Object &&out) {
	for (auto &&rec: priceMap) {
			out.set(rec.first, rec.second);
	}
}

void Report::setError(StrViewA symb, const ErrorObj &errorObj) {

	const json::Value &info = infoMap[symb];
	bool inverted = info["inverted"].getBool();

	Object obj;
	if (!errorObj.genError.empty()) obj.set("gen", errorObj.genError);
	if (!errorObj.buyError.empty()) obj.set(inverted?"sell":"buy", errorObj.buyError);
	if (!errorObj.sellError.empty()) obj.set(inverted?"buy":"sell", errorObj.sellError);
	errorMap[symb] = obj;
}

void Report::exportMisc(json::Object &&out) {
	for (auto &&rec: miscMap) {
			auto erritr = errorMap.find(rec.first);
			Value err = erritr == errorMap.end()?Value():erritr->second;
			out.set(rec.first, rec.second.replace("error", err));
	}
}

void Report::addLogLine(StrViewA ln) {
	logLines.push_back(ln);
}

using namespace ondra_shared;

class CaptureLog: public ondra_shared::StdLogProviderFactory {
public:
	CaptureLog(Report &rpt, ondra_shared::PStdLogProviderFactory target):rpt(rpt),target(target) {}

	virtual void writeToLog(const StrViewA &line, const std::time_t &, LogLevel level) override;
	virtual bool isLogLevelEnabled(ondra_shared::LogLevel lev) const override;


protected:
	Report &rpt;
	ondra_shared::PStdLogProviderFactory target;
};

inline void CaptureLog::writeToLog(const StrViewA& line, const std::time_t&tm, LogLevel level) {
	if (level >= LogLevel::info) rpt.addLogLine(line);
	target->sendToLog(line, tm, level);
}

inline bool CaptureLog::isLogLevelEnabled(ondra_shared::LogLevel lev) const {
	return target->isLogLevelEnabled(lev);
}

ondra_shared::PStdLogProviderFactory Report::captureLog(ondra_shared::PStdLogProviderFactory target) {
	return new CaptureLog(*this, target);
}

void Report::setMisc(StrViewA symb, const MiscData &miscData) {

	const json::Value &info = infoMap[symb];
	bool inverted = info["inverted"].getBool();

	double spread;
	if (inverted) {
		spread = 1.0/miscData.calc_price - 1.0/(miscData.spread+miscData.calc_price) ;
	} else {
		spread = miscData.spread;
	}


	if (inverted) {

		miscMap[symb] = Object
				("t",-miscData.trade_dir)
				("a", miscData.achieve)
				("mcp", 1.0/miscData.calc_price)
				("mv", miscData.value)
				("ms", spread)
				("mdmb", miscData.dynmult_sell)
				("mdms", miscData.dynmult_buy)
				("sm", miscData.size_mult)
				("mb",miscData.boost)
				("ml",1.0/miscData.highest_price)
				("mh",1.0/miscData.lowest_price)
				("mt",miscData.total_trades)
				("tt",miscData.total_time);
	} else {
		miscMap[symb] = Object
				("t",miscData.trade_dir)
				("a", miscData.achieve)
				("mcp", miscData.calc_price)
				("mv", miscData.value)
				("ms", spread)
				("mdmb", miscData.dynmult_buy)
				("mdms", miscData.dynmult_sell)
				("sm", miscData.size_mult)
				("mb",miscData.boost)
				("ml",miscData.lowest_price)
				("mh",miscData.highest_price)
				("mt",miscData.total_trades)
				("tt",miscData.total_time);
	}
}

void Report::clear(StrViewA symb) {
	tradeMap.erase(symb);
	infoMap.erase(symb);
	priceMap.erase(symb);
	miscMap.erase(symb);
	errorMap.erase(symb);
	orderMap.clear();
}

