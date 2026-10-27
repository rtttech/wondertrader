/*!
 * \file test_timeutils.cpp
 * \project	WonderTrader
 *
 * \brief TimeUtils 日期/时间纯函数的核心流程测试
 *
 * 覆盖交易日历运算(跨月末/跨年/闰日)、分钟回绕、时间戳构造与还原、
 * 分钟条编码(timeToMinBar, 即dsb文件的bartime编码)往返
 */
#include "../Share/TimeUtils.hpp"
#include "gtest/gtest/gtest.h"

TEST(test_timeutils, test_get_next_date)
{
	// 普通进位
	EXPECT_EQ(TimeUtils::getNextDate(20261026), 20261027);
	EXPECT_EQ(TimeUtils::getNextDate(20261031), 20261101);

	// 跨月末
	EXPECT_EQ(TimeUtils::getNextDate(20260131), 20260201);
	EXPECT_EQ(TimeUtils::getNextDate(20260430), 20260501);

	// 平年2月
	EXPECT_EQ(TimeUtils::getNextDate(20260228), 20260301);

	// 闰年2月
	EXPECT_EQ(TimeUtils::getNextDate(20240228), 20240229);
	EXPECT_EQ(TimeUtils::getNextDate(20240229), 20240301);

	// 跨年
	EXPECT_EQ(TimeUtils::getNextDate(20261231), 20270101);

	// 指定天数
	EXPECT_EQ(TimeUtils::getNextDate(20261026, 7), 20261102);
	EXPECT_EQ(TimeUtils::getNextDate(20261228, 5), 20270102);
}

TEST(test_timeutils, test_get_next_month)
{
	// 普通进位
	EXPECT_EQ(TimeUtils::getNextMonth(202610), 202611);
	EXPECT_EQ(TimeUtils::getNextMonth(202611, 2), 202701);

	// 跨年
	EXPECT_EQ(TimeUtils::getNextMonth(202612), 202701);
	EXPECT_EQ(TimeUtils::getNextMonth(202612, 13), 202801);

	// 多年偏移
	EXPECT_EQ(TimeUtils::getNextMonth(202601, 24), 202801);
}

TEST(test_timeutils, test_get_next_minute)
{
	// 同小时进位
	EXPECT_EQ(TimeUtils::getNextMinute(1038, 1), 1039);
	EXPECT_EQ(TimeUtils::getNextMinute(1038, 22), 1100);

	// 跨小时
	EXPECT_EQ(TimeUtils::getNextMinute(1059, 1), 1100);

	// 跨日回绕
	EXPECT_EQ(TimeUtils::getNextMinute(2359, 1), 0);
	EXPECT_EQ(TimeUtils::getNextMinute(2359, 2), 1);

	// 反向偏移
	EXPECT_EQ(TimeUtils::getNextMinute(100, -1), 59);
	EXPECT_EQ(TimeUtils::getNextMinute(0, -1), 2359);
	EXPECT_EQ(TimeUtils::getNextMinute(1038, -38), 1000);
}

TEST(test_timeutils, test_make_time)
{
	// 构造时间戳并用timeToString还原, 与9位HHMMSSmmm格式对齐
	int64_t ts = TimeUtils::makeTime(20261026, 103800500);
	EXPECT_EQ(TimeUtils::timeToString(ts), "20261026103800.500");

	ts = TimeUtils::makeTime(20261026, 235959999);
	EXPECT_EQ(TimeUtils::timeToString(ts), "20261026235959.999");

	// 整分无毫秒
	ts = TimeUtils::makeTime(20261026, 90000000);
	EXPECT_EQ(TimeUtils::timeToString(ts), "20261026090000");
}

TEST(test_timeutils, test_minbar_encoding)
{
	// dsb分钟条bartime编码: (date-19900000)*10000 + time
	uint64_t bar = TimeUtils::timeToMinBar(20261026, 1340);
	EXPECT_EQ(bar, 3610261340ULL);

	// 往返还原
	EXPECT_EQ(TimeUtils::minBarToDate(bar), 20261026);
	EXPECT_EQ(TimeUtils::minBarToTime(bar), 1340);

	// 跨年日期编码
	bar = TimeUtils::timeToMinBar(20270103, 900);
	EXPECT_EQ(TimeUtils::minBarToDate(bar), 20270103);
	EXPECT_EQ(TimeUtils::minBarToTime(bar), 900);
}

TEST(test_timeutils, test_weekday_anchor)
{
	// 已知日历锚点: 2000-01-01为星期六(tm_wday=6)
	EXPECT_EQ(TimeUtils::getWeekDay(20000101), 6);
	// 2020-01-01为星期三(tm_wday=3)
	EXPECT_EQ(TimeUtils::getWeekDay(20200101), 3);
}
