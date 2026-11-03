/*!
 * \file test_nightstale.cpp
 * \project	WonderTrader
 *
 * \brief 夜盘残留快照修复(hotfix/night-tick-stale)的单元测试
 *
 * 覆盖两个修复点:
 * 1. TimeUtils::chronoKeyOf / TimeUtils::isTickStale
 *    4个实时ticker(WtCta/WtHft/WtSel/WtUft)的行情陈旧过滤统一改用的交易日时序比较
 *    缺陷背景: 原实现直接比较actiontime(HHMMSSmmm)整数, 同一交易日内夜盘时段(>=21:00)
 *    整数上大于日盘时段, CTP登录快照携带的上一夜盘残留tick(如23:01)在日盘09:00后收到时
 *    被误判为最新行情, 导致分钟游标钉死夜盘位、引擎时钟冻结在23:01
 * 2. 信号门 isInTradingTime(tick->actiontime()/100000) 口径
 *    引擎时钟被钉死后, 原实现按引擎全局时钟判交易时间, 夜盘23:00收盘的品种(如DCE.v)
 *    其信号将永远无法交付执行器; 改用tick自身交易时间后与全局时钟解耦
 */
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "gtest/gtest/gtest.h"

USING_NS_WTP;

/*
 * 时序键映射规则: 2000->0, 2100->100, 2300->300, 0000->400, 0900->1300, 1500->1900
 */
TEST(test_nightstale, test_chronokey_mapping)
{
	// 夜盘时段(>=20:00)映射到交易日最前段
	EXPECT_EQ(TimeUtils::chronoKeyOf(2000), 0);
	EXPECT_EQ(TimeUtils::chronoKeyOf(2059), 59);
	EXPECT_EQ(TimeUtils::chronoKeyOf(2100), 100);
	EXPECT_EQ(TimeUtils::chronoKeyOf(2300), 300);
	EXPECT_EQ(TimeUtils::chronoKeyOf(2301), 301);
	EXPECT_EQ(TimeUtils::chronoKeyOf(2359), 359);

	// 跨日的凌晨时段(00:00~02:59)紧随夜盘
	EXPECT_EQ(TimeUtils::chronoKeyOf(0), 400);
	EXPECT_EQ(TimeUtils::chronoKeyOf(100), 500);
	EXPECT_EQ(TimeUtils::chronoKeyOf(230), 630);

	// 日盘时段
	EXPECT_EQ(TimeUtils::chronoKeyOf(900), 1300);
	EXPECT_EQ(TimeUtils::chronoKeyOf(1038), 1438);
	EXPECT_EQ(TimeUtils::chronoKeyOf(1500), 1900);
	EXPECT_EQ(TimeUtils::chronoKeyOf(1515), 1915);
}

/*
 * 核心缺陷场景: 日盘09:00启动, CTP登录快照携带上一夜盘残留tick
 * 交易日20261026, 快照tick: actiondate=20261026, actiontime=2301xxx(昨日夜盘收盘后)
 * 修复前: 230100000 > 090001000 整数比较放行 -> 污染(此处断言修复后必须拒绝)
 */
TEST(test_nightstale, test_stale_night_snapshot)
{
	// 09:00启动, init时TimeUtils::getDateTime写入本地墙钟
	uint32_t lDate = 20261026;
	uint32_t lTime = 90001000;

	// 快照残留tick(actiondate=前一自然日20261025), 覆盖夜盘全时段与跨零点时段,
	// 全部必须判陈旧(投影后残留tick交易日=20261026, 不晚于本地交易日20261026)
	// 注意actiontime为9位HHMMSSmmm: 10000000即01:00:00.000, 100000000即10:00:00.000
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 210000000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 220000000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 230000000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 230100000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 235959999, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 0, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 10000000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 20000000, lDate, lTime));

	// 同日(20261026)夜盘时刻的tick是"今晚尚未发生的行情"(未来tick),
	// 交易日投影后归属20261027 > 本地20261026, 按上游未来tick语义放行,
	// 由Parser层check_time(未来时间戳过滤)负责拦截, 此处仅记录该行为契约
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 210000000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 230100000, lDate, lTime));

	// 日盘实时tick不判陈旧(10:38为本次实盘丢信号的真实信号时刻)
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 900000000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 103800000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 150000000, lDate, lTime));
}

/*
 * 防御纵深: 若污染已发生(引擎时钟被钉死在夜盘尾段23:01),
 * 日盘tick相对被污染的本地时标仍可正常放行, 引擎时钟可自愈推进
 */
TEST(test_nightstale, test_stale_recovery)
{
	// 本地时标已被快照污染
	uint32_t lDate = 20261026;
	uint32_t lTime = 230100000;

	// 更晚的夜盘时刻相对23:01是正常推进, 放行
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 230200000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 235900000, lDate, lTime));

	// 更早的夜盘残留(23:00相对23:01)拒绝
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 230000000, lDate, lTime));

	// 日盘tick放行, 时序键(900)=1300 > (2301)=301
	// 注意: 夜盘23:01之后收到的日盘tick, 日历日期已是次日(20261027),
	// 投影后交易日同为20261027; 若日历日期仍为20261026, 则是12小时前的残留, 必须判陈旧
	EXPECT_FALSE(TimeUtils::isTickStale(20261027, 900000000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261027, 103800000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 103800000, lDate, lTime));
}

/*
 * 夜盘正常交易时段与盘内跨零点(21:00启动, 23:59->00:00)
 */
TEST(test_nightstale, test_stale_night_session)
{
	// 20:55启动
	uint32_t lDate = 20261026;
	uint32_t lTime = 205500000;

	// 夜盘实时tick正常放行
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 210000000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 230000000, lDate, lTime));

	// 夜盘盘内: 23:59相对20:55是更新时刻, 放行
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 235959999, lDate, lTime));

	// 盘内跨零点: 日历日期翻转为20261027后, 00:00在23:59之后, 放行;
	// 同日历日(20261026)的00:00是24小时前的残留, 必须拒绝;
	// 23:59的迟到tick在00:00后必须拒绝
	uint32_t lTime2 = 235959999;
	EXPECT_FALSE(TimeUtils::isTickStale(20261027, 0, 20261026, lTime2));
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 0, 20261026, lTime2));
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 235900000, 20261027, lTime2));

	// 已推进到凌晨01:00后, 收到23:xx的迟到tick必须拒绝
	// 注意: 01:00时本地日历日期已是次日(20261027), 残留tick(20261026,23:00)
	// 投影后交易日同为20261027, 时序键(23:00)=300 < (01:00)=500 -> 陈旧
	uint32_t lTime3 = 10000000;
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 230000000, 20261027, lTime3));
}

/*
 * 同分钟毫秒乱序、迟到tick、跨交易日、首笔行情
 */
TEST(test_nightstale, test_stale_intraday)
{
	uint32_t lDate = 20261026;
	uint32_t lTime = 103800500;

	// 同一分钟内毫秒乱序保持上游拒绝语义
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 103800000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 103800499, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 103800500, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 103800999, lDate, lTime));

	// 早于本地时钟的迟到tick拒绝(11:02重启后收到10:38)
	uint32_t rTime = 110200000;
	EXPECT_TRUE(TimeUtils::isTickStale(20261026, 103800000, 20261026, rTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 110200000, 20261026, rTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 113000000, 20261026, rTime));

	// 跨交易日: 昨日tick一律陈旧(含昨日夜盘时段)
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 103800000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261025, 210000000, lDate, lTime));

	// 异常的未来日期tick放行(与上游行为一致)
	EXPECT_FALSE(TimeUtils::isTickStale(20261027, 103800000, lDate, lTime));

	// 首笔行情(lDate==0)一律不判陈旧
	EXPECT_FALSE(TimeUtils::isTickStale(20261026, 0, 0, 0));
	EXPECT_FALSE(TimeUtils::isTickStale(20261025, 230000000, 0, 0));
}

/*
 * 信号门口径: 引擎时钟冻结在23:01时, 原实现isInTradingTime(引擎raw_time)导致
 * 夜盘23:00收盘品种(DCE.v)全天信号被挡, 而01:00收盘品种(SHFE.ni)侥幸通过,
 * 复现实盘品种间不对称; 修复后按tick自身actiontime判断, 与全局时钟解耦
 * 注意: 会话段必须按交易时序添加(日盘段在前、夜盘段在后), 与上游templates.json一致,
 * timeToMinutes按段顺序遍历, 遇到小于段下界即返回INVALID
 */
TEST(test_nightstale, test_signal_gate_session)
{
	// DCE.v会话: 日盘三段 + 夜盘21:00-23:00
	WTSSessionInfo* vInfo = WTSSessionInfo::create("V", "DCE.v", 0);
	vInfo->addTradingSection(900, 1015);
	vInfo->addTradingSection(1030, 1130);
	vInfo->addTradingSection(1330, 1500);
	vInfo->addTradingSection(2100, 2300);

	// 引擎时钟冻结在23:01(HHMM=2301), 原口径下v全天被挡(23:01不在v会话内)
	EXPECT_FALSE(vInfo->isInTradingTime(2301));
	EXPECT_FALSE(vInfo->isInTradingTime(2301, true));

	// 修复后口径: 按tick自身时间判断, 10:38信号在门内
	EXPECT_TRUE(vInfo->isInTradingTime(1038, true));
	EXPECT_TRUE(vInfo->isInTradingTime(900, true));
	EXPECT_TRUE(vInfo->isInTradingTime(1330, true));

	// 夜盘时段内的tick在门内(夜盘交易中)
	EXPECT_TRUE(vInfo->isInTradingTime(2200, true));

	// 会话外
	EXPECT_FALSE(vInfo->isInTradingTime(1600));
	EXPECT_FALSE(vInfo->isInTradingTime(1959));
	vInfo->release();

	// SHFE.ni会话: 日盘三段 + 夜盘21:00-01:00(跨零点)
	WTSSessionInfo* niInfo = WTSSessionInfo::create("NI", "SHFE.ni", 0);
	niInfo->addTradingSection(900, 1015);
	niInfo->addTradingSection(1030, 1130);
	niInfo->addTradingSection(1330, 1500);
	niInfo->addTradingSection(2100, 2400);
	niInfo->addTradingSection(0, 100);

	// 同样冻结在23:01, ni因夜盘时段覆盖23:01而侥幸通过(实盘不对称的机制)
	EXPECT_TRUE(niInfo->isInTradingTime(2301));
	EXPECT_TRUE(niInfo->isInTradingTime(1038, true));
	niInfo->release();

	/*
	 * 已知上游缺陷记录(v0.9.9, 非本次补丁引入):
	 * WTSSessionInfo.hpp中TradingSection构造函数 second_raw(second_raw) 自赋值,
	 * second_raw成员未被赋值(未定义行为), 导致isLastOfSection恒为false,
	 * bStrict的段尾最后一分钟排除语义失效。对信号门影响极小(仅多放行段尾分钟),
	 * 故此处不对段尾分钟(如1500的bStrict行为)做断言
	 */
}

/*
 * 2026.11.03 夜盘事故回归: 日盘收盘~夜盘开盘间隙时刻(15:01~19:59)毒化基线
 * 场景: 夜盘22:14运行中重启, init基线=墙钟(20261103, 2214),
 * CTP登录重放携带间隙时刻残留快照(19:03)
 * 修复前: chronoKey(1903)=2303 > chronoKey(2214)=214, 同日比较放行 -> 基线钉死19:03,
 *         此后全部夜盘实时tick(key=215左右)被判陈旧, 引擎时钟冻结(inTrading=0)
 * 修复后: isTickStale先做交易日投影(夜盘>=20:00归属下一交易日),
 *         毒tick tDay=20261103 < 本地tDay=20261104, 日期域先分 -> 拒绝
 */
TEST(test_nightstale, test_gap_time_poison)
{
	// 夜盘22:14重启, init基线为墙钟
	uint32_t lDate = 20261103;
	uint32_t lTime = 221403000;

	// 间隙时刻残留(15:01~19:59)全部必须拒绝 —— 本次事故的毒tick形态
	EXPECT_TRUE(TimeUtils::isTickStale(20261103, 190321000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261103, 150100000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261103, 153000000, lDate, lTime));
	EXPECT_TRUE(TimeUtils::isTickStale(20261103, 195959999, lDate, lTime));

	// 夜盘实时tick必须放行(与基线同时刻或更新)
	EXPECT_FALSE(TimeUtils::isTickStale(20261103, 221403000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261103, 221500000, lDate, lTime));
	EXPECT_FALSE(TimeUtils::isTickStale(20261103, 225959999, lDate, lTime));

	// 基线已推进到22:30后, 间隙残留依旧拒绝
	uint32_t lTime2 = 223000000;
	EXPECT_TRUE(TimeUtils::isTickStale(20261103, 190321000, lDate, lTime2));

	// 跨零点投影一致性: 22:50基线(投影到20261104)之后, 次日00:30(同投影20261104)放行
	EXPECT_FALSE(TimeUtils::isTickStale(20261104, 30000000, 20261103, 225000000));

	// 对照: 日盘15:30运行中收到19:03(时钟上晚于本地的间隙时刻), 按上游未来tick语义放行,
	// 由Parser层check_time(未来时间戳过滤)负责拦截
	EXPECT_FALSE(TimeUtils::isTickStale(20261103, 190321000, 20261103, 153000000));
}
