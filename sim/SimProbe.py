# -*- coding: utf-8 -*-
"""
SimProbe —— 模拟测试探针策略（sim/run_sim.sh 自动化用）

职责
----
1. 订阅指定合约 tick + 主 K 线（m1），验证 ParserUDP→引擎→策略链路；
2. 每 1000 笔 tick / 每根 bar 输出心跳日志（供 run_sim.sh 判 PASS）；
3. 第 2 根 bar 买入开 1 手、第 5 根 bar 平仓——验证下单→TraderAdapter→
   TraderMocker 撮合→on_order/on_trade 回调全链路；
4. on_session_begin / on_order / on_trade 全部落日志，链路各环节可观测。
"""
from wtpy import BaseCtaStrategy
from wtpy import CtaContext


class StraSimProbe(BaseCtaStrategy):

    def __init__(self, name: str, code: str = "SHFE.ag.HOT", lots: int = 1):
        BaseCtaStrategy.__init__(self, name)
        self.__code__ = code
        self.__lots__ = lots
        self.__tick_cnt__ = 0
        self.__bar_cnt__ = 0
        self.__traded__ = False
        self.__closed__ = False

    def on_init(self, context: CtaContext):
        context.stra_log_text("[SimProbe] init, code=%s" % self.__code__)
        context.stra_get_bars(self.__code__, "m1", 60, isMain=True)
        context.stra_sub_ticks(self.__code__)

    def on_session_begin(self, context: CtaContext, curTDate: int):
        context.stra_log_text("[SimProbe] on_session_begin %d" % curTDate)

    def on_tick(self, context: CtaContext, stdCode: str, newTick: dict):
        self.__tick_cnt__ += 1
        if self.__tick_cnt__ <= 3 or self.__tick_cnt__ % 1000 == 0:
            context.stra_log_text(
                "[SimProbe] on_tick #%d %s px=%s t=%s" %
                (self.__tick_cnt__, stdCode,
                 newTick.get("price"), newTick.get("action_time")))
        # 用 tick 计数触发交易（standalone runner 无 datakit writer，on_bar 不触发）
        # feeder 0.5s/tick：#60≈30s 开多，#240≈120s 平仓
        if self.__tick_cnt__ == 60 and not self.__traded__:
            context.stra_log_text("[SimProbe] enter long %d lot @tick#60" % self.__lots__)
            context.stra_enter_long(self.__code__, self.__lots__, "probe")
            self.__traded__ = True
        elif self.__tick_cnt__ == 240 and self.__traded__ and not self.__closed__:
            context.stra_log_text("[SimProbe] exit long @tick#240")
            context.stra_exit_long(self.__code__, self.__lots__, "probe")
            self.__closed__ = True

    def on_bar(self, context: CtaContext, stdCode: str, period: str, newBar: dict):
        self.__bar_cnt__ += 1
        context.stra_log_text(
            "[SimProbe] on_bar #%d %s %s close=%.1f time=%s" %
            (self.__bar_cnt__, stdCode, period,
             float(newBar.get("close", 0.0) or 0.0), newBar.get("time")))

    def on_order(self, context: CtaContext, stdCode: str, bBuy: bool, totalQty: int,
                 leftQty: int, price: float, bCanceled: bool):
        context.stra_log_text(
            "[SimProbe] on_order %s %s total=%d left=%d px=%.1f canceled=%s" %
            (stdCode, "buy" if bBuy else "sell", totalQty, leftQty, price, bCanceled))

    def on_trade(self, context: CtaContext, stdCode: str, bBuy: bool, qty: int,
                 price: float):
        context.stra_log_text(
            "[SimProbe] on_trade FILLED %s %s qty=%d px=%.1f" %
            (stdCode, "buy" if bBuy else "sell", qty, price))
