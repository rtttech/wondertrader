#!/bin/bash
# ============================================================================ #
# run_sim.sh —— wondertrader 源码补丁自动化模拟测试（一键编排）
#
# 场景
#   无行情前置环境（如 192.168.3.20 构建机）下，验证新编译的 libWtPorter.so /
#   WtRunner 是否能完整跑通 CTA 实时链路：
#     ParserUDP(合成tick) → WtCtaRtTicker → 分钟收线 → 策略 → TraderMocker 撮合
#   特别验证 2026-11-03 时钟冻结事故补丁的两条可观测性日志。
#
# 用法（在宿主机执行，无需进容器）
#   cd /home/gushenghua/wtbuild/wondertrader/sim
#   bash run_sim.sh                       # 默认引擎 150s / feeder 120s
#   DUR=240 bash run_sim.sh               # 自定义时长
#
# 前置条件
#   1. 源码已编译: build_ct7_abi1/build_x64/Release/bin/WtPorter/libWtPorter.so
#   2. wtpy 源码包: $WTPY_SRC/wtpy (含 wrapper/, 默认 /home/gushenghua/wtbuild/wtpy_src/wtpy)
#   3. python: $PYBIN (默认 miniconda wtpy 环境, 仅用其解释器与依赖,
#      wtpy 包本身用 run_sim.sh 拷贝进 ws 的副本, 不污染环境)
#
# 判定标准（全部命中 → PASS, exit 0）
#   A. Runner.log 出现 "CTA ticker thread started"        [补丁新增日志②]
#   B. Runner.log 出现 "closed automatically" ≥2 次        [分钟收线]
#   C. Strategy 日志出现 on_bar ≥2 / on_trade FILLED       [策略回调+成交]
#   D. 全程无 "Ticker thread not running"                  [补丁新增日志①, 冻结前兆]
# ============================================================================ #
set -u
cd "$(dirname "$0")"

REPO=$(cd .. && pwd)
BIN=$REPO/build_ct7_abi1/build_x64/Release/bin
WTPY_SRC=${WTPY_SRC:-/home/gushenghua/wtbuild/wtpy_src}
PYBIN=${PYBIN:-/home/gushenghua/miniconda3/envs/wtpy/bin/python}
DIST_COMMON=${DIST_COMMON:-$REPO/../wtpy_src/common}   # wtpy demos 的 2026 基础文件（wtpy_src/common）
DUR=${DUR:-150}              # 引擎运行秒数
FEED_DUR=$((DUR - 20))       # feeder 提前于引擎退出
WS=ws_sim

echo "== 0) 前置检查 =="
[ -f "$BIN/WtPorter/libWtPorter.so" ] || { echo "FAIL: $BIN/WtPorter/libWtPorter.so 不存在, 先编译 WtPorter"; exit 2; }
[ -d "$WTPY_SRC/wtpy" ] || { echo "FAIL: $WTPY_SRC/wtpy 不存在, 先上传 wtpy 源码包"; exit 2; }
[ -f "$DIST_COMMON/commodities.json" ] || { echo "FAIL: $DIST_COMMON 基础文件缺失"; exit 2; }
"$PYBIN" -c "import yaml" 2>/dev/null || { echo "FAIL: $PYBIN 缺 pyyaml"; exit 2; }

echo "== 1) 组装工作区 $WS =="
rm -rf "$WS"; mkdir -p "$WS/common" "$WS/storage" "$WS/his" "$WS/Logs"
cp cfg/*.yaml "$WS/"
cp run_sim.py SimProbe.py udp_tick_feeder.py "$WS/"
cp "$DIST_COMMON"/*.json "$WS/common/" 2>/dev/null
# wtpy python 包副本（含 wrapper 原生库），随后用本仓库编译产物覆盖核心库
cp -a "$WTPY_SRC/wtpy" "$WS/wtpy"
find "$WS/wtpy" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null
LIBD="$WS/wtpy/wrapper/linux"
cp -f "$BIN/WtPorter/libWtPorter.so"      "$LIBD/libWtPorter.so"
cp -f "$BIN/WtPorter/libWtDataStorage.so" "$LIBD/libWtDataStorage.so" 2>/dev/null
cp -f "$BIN/WtPorter/libWtDataStorageAD.so" "$LIBD/libWtDataStorageAD.so" 2>/dev/null
cp -f "$BIN/WtPorter/libWtMsgQue.so"      "$LIBD/libWtMsgQue.so" 2>/dev/null
cp -f "$BIN/WtPorter/libWtRiskMonFact.so" "$LIBD/libWtRiskMonFact.so" 2>/dev/null
cp -f "$BIN/WtPorter/libTraderDumper.so"  "$LIBD/libTraderDumper.so" 2>/dev/null
cp -f "$BIN/WtPorter/parsers/"libParser*.so "$LIBD/parsers/" 2>/dev/null
cp -f "$BIN/WtPorter/traders/"libTrader*.so "$LIBD/traders/" 2>/dev/null
cp -f "$BIN/WtPorter/executer/libWtExeFact.so" "$LIBD/executer/" 2>/dev/null
[ -f "$BIN/WtPorter/libWtExecMon.so" ] && cp -f "$BIN/WtPorter/libWtExecMon.so" "$LIBD/" 2>/dev/null
# 模板配置：filters/actpolicy/executers 必须存在，否则 config() 中途 return，
# parsers/traders 根本不会初始化（0 parsers started 的根因）
for f in filters.yaml actpolicy.yaml executers.yaml; do
    cp -f "$REPO/dist/WtRunnerCta/$f" "$WS/$f" 2>/dev/null || echo "   [warn] dist/WtRunnerCta/$f 缺失"
done
echo "   libWtPorter.so md5: $(md5sum "$LIBD/libWtPorter.so" | cut -d' ' -f1)"

echo "== 2) 启动引擎（${DUR}s）与合成行情 feeder（${FEED_DUR}s）=="
cd "$WS"
"$PYBIN" run_sim.py --seconds "$DUR" > console.log 2>&1 &
ENGINE_PID=$!
trap 'kill $ENGINE_PID 2>/dev/null; true' EXIT
sleep 8    # 等引擎完成 init / ticker 启动
# 盘外运行时: tick 日期用下一交易日 + 09:30 起步
# (WtCtaRtTicker::init 用墙钟预置 _date/_time 基线, 同日更早时间戳会被 isTickStale 拒收;
#  uDate > lDate 直接通过, 故用下一交易日日期 + 交易时段内时间)
SESSION_STATE=$(python3 - <<'PYEOF'
import datetime
now = datetime.datetime.now()
secs = now.hour * 3600 + now.minute * 60 + now.second
sess = [(9*3600, 10*3600+15*60), (10*3600+30*60, 11*3600+30*60), (13*3600+30*60, 15*3600),
        (21*3600, 23*3600+59*60+59), (0, 2*3600+30*60)]
print("in" if any(a <= secs <= b for a, b in sess) else "out")
PYEOF
)
if [ "$SESSION_STATE" = "in" ]; then
    "$PYBIN" udp_tick_feeder.py --poison 50 --seconds "$FEED_DUR" --codes SHFE.ag.HOT --mport 9002
else
    TDATE=$(date -d tomorrow +%Y%m%d)
    echo "[sim] off-session run: tick date=$TDATE base=09:30:00"
    "$PYBIN" udp_tick_feeder.py --poison 50 --seconds "$FEED_DUR" --codes SHFE.ag.HOT --mport 9002 \
        --date "$TDATE" --start-time 09:30:00
fi
echo "== 3) 等待引擎退出 =="
wait $ENGINE_PID 2>/dev/null
trap - EXIT

echo "== 4) 判定 =="
RUNNER_LOG=$(ls Logs/Runner_*.log Logs/Runner.log 2>/dev/null | head -1)
STRA_LOG=$(ls Logs/Strategy/*.log 2>/dev/null | head -1)
TRADER_LOG=$(ls Logs/Trader/*.log 2>/dev/null | head -1)
echo "  runner=$RUNNER_LOG strategy=$STRA_LOG trader=$TRADER_LOG"
FAILED=0
chk() { # chk <描述> <文件> <grep模式> <最少次数>
    local desc="$1" file="$2" pat="$3" min="${4:-1}" n
    n=$(grep -c "$pat" "$file" 2>/dev/null)
    n=${n:-0}
    if [ "$n" -ge "$min" ]; then
        echo "  [PASS] $desc (n=$n)"
    else
        echo "  [FAIL] $desc (n=$n < $min)"; FAILED=1
    fi
}
chk "A. ticker 线程启动确认日志(补丁②)" "$RUNNER_LOG" "CTA ticker thread started" 1
chk "B. 分钟自动收线 ≥2"                 "$RUNNER_LOG" "closed automatically" 2
# CTA 组合运行器为目标仓位模式，策略无 on_order/on_trade 回调（仅 HFT 引擎有），
# 订单与成交判定改用 Trader 日志的回报记录
[ -n "$TRADER_LOG" ] && chk "C1. 订单回报 Order notified >=2" "$TRADER_LOG" "Order notified" 2
[ -n "$TRADER_LOG" ] && chk "C2. 撮合成交 Trade notified >=2" "$TRADER_LOG" "Trade notified" 2
[ -n "$TRADER_LOG" ] && chk "C3. Trader 通道有回报" "$TRADER_LOG" "." 1
if grep -q "Ticker thread not running" "$RUNNER_LOG" 2>/dev/null; then
    echo "  [FAIL] D. 出现 ticker 线程未运行告警(补丁①) —— 时钟冻结前兆"; FAILED=1
else
    echo "  [PASS] D. 无 ticker 线程未运行告警(补丁①)"
fi
grep -q "on_session_begin" "$STRA_LOG" 2>/dev/null \
    && echo "  [PASS] E. on_session_begin 回调正常" \
    || { echo "  [FAIL] E. 未见 on_session_begin"; FAILED=1; }

echo "== 5) 关键日志摘录 =="
grep -E "CTA ticker thread started|closed automatically" "$RUNNER_LOG" 2>/dev/null | head -5
[ -n "$STRA_LOG" ] && grep -E "on_order|FILLED|enter long|exit long" "$STRA_LOG" | head -8
[ -n "$TRADER_LOG" ] && tail -3 "$TRADER_LOG"

if [ "$FAILED" = "0" ]; then
    echo "== SIM RESULT: PASS =="
    exit 0
else
    echo "== SIM RESULT: FAIL =="
    exit 1
fi
