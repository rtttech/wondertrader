# -*- coding: utf-8 -*-
"""
udp_tick_feeder —— 合成行情发生器（UDP → ParserUDP → wtpy CTA 引擎）

用途
----
在无行情前置（CTP 网段不可达）的环境里，向 ParserUDP 广播端口灌入合成 tick，
驱动 WtCtaEngine 完整跑通：ParserUDP → WtCtaRtTicker.on_tick → 分钟收线 →
策略 on_tick/on_bar → TraderAdapter 下单 → TraderMocker 撮合成交。
用于源码补丁后的自动化模拟测试（sim/run_sim.sh 编排）。

报文格式（ParserUDP.cpp，与 src/Includes/WTSStruct.h 严格一致）
--------------------------------------------------------------
    [0:4)   uint32  _type = 0x200 (UDP_MSG_PUSHTICK)
    [4:516) wtp::WTSTickStruct（UDPDataPacket 定义在 pragma pack(push,1) 区段内，
            _data 紧跟 _type 无对齐填充；结构体本身 pack(8) sizeof=512）

时间基准
--------
    默认：以当前墙钟为起点（正处期货交易时段时分钟收线与真实时间对齐）；
    墙钟不在交易时段时自动改用 14:00:00 起（isInTradingTime 引擎侧校验按
    tick 时间戳判定，非交易时段灌 14:00+ 的戳照样触发收线）。
    注意：tdparsers.yaml 里 ParserUDP 必须配 check_time: false，
    否则强制时间基准与墙钟差 >1h 会被 ParserAdapter 拒收。

用法
----
    python3 udp_tick_feeder.py --host 127.0.0.1 --bport 9001 \
        --codes SHFE.ag.HOT --seconds 120 [--start-time 14:00:00] [--interval 0.5]
"""
import argparse
import ctypes
import json
import socket
import struct
import sys
import time
from datetime import datetime, timedelta

# --------------------------------------------------------------------------- #
# wtp::WTSTickStruct  (WTSStruct.h, pragma pack(push,8), MAX_EXCHANGE_LENGTH=16,
#                      MAX_INSTRUMENT_LENGTH=32; sizeof=512 编译期探针验证)
# --------------------------------------------------------------------------- #
class WTSTickStruct(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("exchg",         ctypes.c_char * 16),
        ("code",          ctypes.c_char * 32),
        ("price",         ctypes.c_double),
        ("open",          ctypes.c_double),
        ("high",          ctypes.c_double),
        ("low",           ctypes.c_double),
        ("settle_price",  ctypes.c_double),
        ("upper_limit",   ctypes.c_double),
        ("lower_limit",   ctypes.c_double),
        ("total_volume",  ctypes.c_double),
        ("volume",        ctypes.c_double),
        ("total_turnover", ctypes.c_double),
        ("turn_over",     ctypes.c_double),
        ("open_interest", ctypes.c_double),
        ("diff_interest", ctypes.c_double),
        ("trading_date",  ctypes.c_uint32),
        ("action_date",   ctypes.c_uint32),
        ("action_time",   ctypes.c_uint32),   # HHMMSSmmm
        ("reserve_",      ctypes.c_uint32),
        ("pre_close",     ctypes.c_double),
        ("pre_settle",    ctypes.c_double),
        ("pre_interest",  ctypes.c_double),
        ("bid_prices",    ctypes.c_double * 10),
        ("ask_prices",    ctypes.c_double * 10),
        ("bid_qty",       ctypes.c_double * 10),
        ("ask_qty",       ctypes.c_double * 10),
    ]

assert ctypes.sizeof(WTSTickStruct) == 512, \
    "WTSTickStruct layout mismatch: %d != 512" % ctypes.sizeof(WTSTickStruct)

UDP_MSG_PUSHTICK = 0x200
PRICETICK = 1.0          # SHFE ag 每跳 1 元
SESSIONS = [(9 * 3600, 11 * 3600 + 30 * 60), (13 * 3600, 15 * 3600),
            (21 * 3600, 23 * 3600 + 59 * 60)]


def in_trading_time(secs_of_day: int) -> bool:
    for a, b in SESSIONS:
        if a <= secs_of_day < b:
            return True
    return False


def hhmmss_to_secs(s: str) -> int:
    hh, mm, ss = (int(x) for x in s.split(":"))
    return hh * 3600 + mm * 60 + ss


def secs_to_hhmmssmmm(secs: int, ms: int = 0) -> int:
    hh, rem = divmod(secs, 3600)
    mm, ss = divmod(rem, 60)
    return ((hh * 10000 + mm * 100 + ss) * 1000 + ms)


def resolve_hot(hots_path: str, exchg: str, prod: str, date: int) -> str:
    """按 hots.json 切换表解析主力合约：取 date 之前最后一次切换的 to"""
    with open(hots_path, encoding="utf-8") as f:
        table = json.load(f)
    switches = table.get(exchg, {}).get(prod, [])
    real = None
    for sw in switches:
        if int(sw["date"]) <= date and sw.get("to"):
            real = sw["to"]
    if real is None:
        raise SystemExit("[feeder] 无法解析主力: %s.%s @%d" % (exchg, prod, date))
    return real


def main():
    ap = argparse.ArgumentParser(description="synthetic tick feeder for ParserUDP")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--bport", type=int, default=9001,
                    help="ParserUDP 行情端口")
    ap.add_argument("--mport", type=int, default=0,
                    help="TraderMocker 撮合行情端口（0=不发送；与 --bport 同报文）")
    ap.add_argument("--codes", default="SHFE.ag.HOT", help="逗号分隔, 如 SHFE.ag.HOT,DCE.v.HOT")
    ap.add_argument("--seconds", type=int, default=120, help="发送时长（秒）")
    ap.add_argument("--interval", type=float, default=0.5, help="每轮发送间隔（秒）")
    ap.add_argument("--start-price", type=float, default=12000.0)
    ap.add_argument("--start-time", default="", help="强制时间基准 HH:MM:SS（缺省自动判定）")
    ap.add_argument("--date", default="", help="tick 日期 YYYYMMDD（缺省今天；盘外模拟传下一交易日以通过 ticker 墙钟基线守卫）")
    ap.add_argument("--hots", default="common/hots.json", help="hots.json 路径（解析 .HOT 用）")
    args = ap.parse_args()

    # 时间基准：交易时段内用当前墙钟，否则用 14:00:00
    now = datetime.now()
    if args.start_time:
        base_secs = hhmmss_to_secs(args.start_time)
    elif in_trading_time(now.hour * 3600 + now.minute * 60 + now.second):
        base_secs = now.hour * 3600 + now.minute * 60 + now.second
    else:
        base_secs = 14 * 3600
        print("[feeder] wall clock out of session, force time base 14:00:00")

    today = int(args.date) if args.date else int(now.strftime("%Y%m%d"))
    t0 = time.time()
    # .HOT 解析为真实主力合约（ParserAdapter 订阅的是解析后的合约，非 .HOT 代码）
    codes = []
    for c in args.codes.split(","):
        parts = c.split(".")
        exchg, code = parts[0], ".".join(parts[1:])
        if code.endswith(".HOT"):
            real = resolve_hot(args.hots, exchg, code[:-4], today)
            print("[feeder] HOT resolve: %s.%s -> %s" % (exchg, code, real))
            code = real
        codes.append((exchg, code))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 目标端口集合：ParserUDP(bport) + TraderMocker(mport)，同一报文双发
    ports = [p for p in (args.bport, args.mport) if p > 0]
    n_sent = 0
    prices = {c[1]: args.start_price for c in codes}
    totals = {c[1]: 10000.0 for c in codes}

    print("[feeder] sending to %s ports=%s codes=%s for %ds" %
          (args.host, ports, args.codes, args.seconds))
    while True:
        elapsed = time.time() - t0
        if elapsed >= args.seconds:
            break
        stamp_secs = int(base_secs + elapsed)
        stamp_ms = int((elapsed % 1) * 1000) if (args.interval < 1) else 0
        for exchg, code in codes:
            px = prices[code] + (1.0 if int(elapsed * 2) % 3 == 0 else -1.0)
            prices[code] = px
            t = WTSTickStruct()
            t.exchg = exchg.encode()
            t.code = code.encode()
            t.price = px
            t.open = t.high = t.low = px
            t.upper_limit = px * 1.1
            t.lower_limit = px * 0.9
            totals[code] += 2.0
            t.total_volume = totals[code]
            t.volume = 2.0
            t.total_turnover = t.turn_over = totals[code] * px
            t.open_interest = 50000.0
            t.diff_interest = 1.0
            t.trading_date = today
            t.action_date = today
            t.action_time = secs_to_hhmmssmmm(stamp_secs, stamp_ms)
            t.pre_close = t.pre_settle = args.start_price
            t.pre_interest = 50000.0
            t.bid_prices[0] = px - PRICETICK
            t.ask_prices[0] = px + PRICETICK
            t.bid_qty[0] = 10.0
            t.ask_qty[0] = 10.0
            pkt = struct.pack("<I", UDP_MSG_PUSHTICK) + bytes(t)
            assert len(pkt) == 516, "packet size %d != 516" % len(pkt)
            for port in ports:
                sock.sendto(pkt, (args.host, port))
            n_sent += 1
        time.sleep(min(args.interval, max(0.0, args.seconds - (time.time() - t0))))

    print("[feeder] done: %d packets sent" % n_sent)


if __name__ == "__main__":
    main()
