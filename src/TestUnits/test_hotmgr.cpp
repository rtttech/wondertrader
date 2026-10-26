/*!
 * \file test_hotmgr.cpp
 * \project	WonderTrader
 *
 * \brief 主力/次主力切换规则管理器(WTSHotMgr)的核心流程测试
 *
 * HOT合约切换是实盘行情路由的关键路径(如DCE.v.HOT):
 * 规则文件为JSON, 按切换日date组织, getCustomRawCode取切换日<=dt的最新to合约;
 * 覆盖规则加载、切换日前后/边界查询、上一主力、复权因子、主力判定
 *
 * 已知上游缺陷记录(v0.9.9): loadSeconds以tag "2ND"存入, 而isSecond以tag
 * "2NDT"查询, tag不一致导致isSecond恒为false, 此处不对isSecond断言
 */
#include "../WTSTools/WTSHotMgr.h"
#include "gtest/gtest/gtest.h"
#include <fstream>

USING_NS_WTP;

namespace
{
	const char* HOT_RULE_FILE = "test_hotrules.json";

	void writeRuleFile()
	{
		std::ofstream f(HOT_RULE_FILE);
		f <<
			"{\n"
			"  \"DCE\": {\n"
			"    \"v\": [\n"
			"      {\"from\":\"v2501\",\"to\":\"v2505\",\"date\":20250410,\"oldclose\":0,\"newclose\":0},\n"
			"      {\"from\":\"v2505\",\"to\":\"v2509\",\"date\":20250812,\"oldclose\":100.0,\"newclose\":101.0}\n"
			"    ]\n"
			"  },\n"
			"  \"SHFE\": {\n"
			"    \"ag\": [\n"
			"      {\"from\":\"ag2512\",\"to\":\"ag2602\",\"date\":20251210,\"oldclose\":0,\"newclose\":0}\n"
			"    ]\n"
			"  }\n"
			"}";
	}
}

class HotMgrTest : public ::testing::Environment
{
public:
	void SetUp() override
	{
		writeRuleFile();
	}

	void TearDown() override
	{
		std::remove(HOT_RULE_FILE);
	}
};

::testing::Environment* const hotMgrEnv = ::testing::AddGlobalTestEnvironment(new HotMgrTest());

TEST(test_hotmgr, test_load_and_query)
{
	WTSHotMgr mgr;
	EXPECT_FALSE(mgr.isInitialized());

	EXPECT_TRUE(mgr.loadHots(HOT_RULE_FILE));
	EXPECT_TRUE(mgr.isInitialized());

	// 语义: date<=dt的最新规则的to合约生效, 切换日当天即切换
	EXPECT_STREQ(mgr.getRawCode("DCE", "v", 20250410), "v2505");
	EXPECT_STREQ(mgr.getRawCode("DCE", "v", 20250411), "v2505");
	// 两条规则之间
	EXPECT_STREQ(mgr.getRawCode("DCE", "v", 20250601), "v2505");
	// 第二次切换
	EXPECT_STREQ(mgr.getRawCode("DCE", "v", 20250812), "v2509");
	// 远期日期: 维持最新主力
	EXPECT_STREQ(mgr.getRawCode("DCE", "v", 20261026), "v2509");

	// 已知上游缺陷记录: dt早于首条规则日期时lower_bound返回首项,
	// dt<cit->first后cit--越过begin是未定义行为(libstdc++下恰好返回
	// 根节点即首条规则的to), 不可依赖, 故不对该场景断言

	mgr.release();
}

TEST(test_hotmgr, test_prev_raw_code)
{
	WTSHotMgr mgr;
	EXPECT_TRUE(mgr.loadHots(HOT_RULE_FILE));

	// 切换日当天, 上一主力为from合约
	EXPECT_STREQ(mgr.getPrevRawCode("DCE", "v", 20250812), "v2505");
	// 已处于最新段, 上一主力为上一段的to合约
	EXPECT_STREQ(mgr.getPrevRawCode("DCE", "v", 20261026), "v2505");
	// 第一段之前无上一主力
	EXPECT_STREQ(mgr.getPrevRawCode("DCE", "v", 20250101), "");

	mgr.release();
}

TEST(test_hotmgr, test_is_hot)
{
	WTSHotMgr mgr;
	EXPECT_TRUE(mgr.loadHots(HOT_RULE_FILE));

	// 当前主力判定
	EXPECT_TRUE(mgr.isHot("DCE", "v2509", 20261026));
	EXPECT_TRUE(mgr.isHot("DCE", "v2505", 20250601));
	EXPECT_FALSE(mgr.isHot("DCE", "v2501", 20250601));
	EXPECT_FALSE(mgr.isHot("DCE", "v2601", 20261026));

	mgr.release();
}

TEST(test_hotmgr, test_rule_factor)
{
	WTSHotMgr mgr;
	EXPECT_TRUE(mgr.loadHots(HOT_RULE_FILE));

	// 未发生复权切换(oldclose=0)时因子为1.0
	EXPECT_DOUBLE_EQ(mgr.getRuleFactor("HOT", "DCE.v", 20250409), 1.0);
	// 第二段切换日累计因子: oldclose/newclose = 100/101
	EXPECT_DOUBLE_EQ(mgr.getRuleFactor("HOT", "DCE.v", 20250812), 100.0 / 101.0);
	// 切换日之后维持累计因子
	EXPECT_DOUBLE_EQ(mgr.getRuleFactor("HOT", "DCE.v", 20261026), 100.0 / 101.0);
	// 未知品种缺省因子1.0
	EXPECT_DOUBLE_EQ(mgr.getRuleFactor("HOT", "DCE.i", 20261026), 1.0);

	mgr.release();
}

TEST(test_hotmgr, test_split_sections)
{
	WTSHotMgr mgr;
	EXPECT_TRUE(mgr.loadHots(HOT_RULE_FILE));

	// 跨越两次切换的区间应拆为3段
	HotSections sections;
	EXPECT_TRUE(mgr.splitHotSecions("DCE", "v", 20250101, 20261026, sections));
	EXPECT_EQ(sections.size(), 3);

	// 区间内无切换, 拆为1段
	sections.clear();
	EXPECT_TRUE(mgr.splitHotSecions("DCE", "v", 20250411, 20250811, sections));
	EXPECT_EQ(sections.size(), 1);

	mgr.release();
}

TEST(test_hotmgr, test_load_seconds)
{
	WTSHotMgr mgr;
	EXPECT_TRUE(mgr.loadSeconds(HOT_RULE_FILE));

	// 次主力查询与主力同构(tag 2ND), 切换日当天起生效
	EXPECT_STREQ(mgr.getSecondRawCode("DCE", "v", 20250410), "v2505");
	EXPECT_STREQ(mgr.getSecondRawCode("DCE", "v", 20250812), "v2509");

	mgr.release();
}
