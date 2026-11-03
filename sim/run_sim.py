# -*- coding: utf-8 -*-
"""
run_sim —— wtpy CTA 模拟引擎入口（sim/run_sim.sh 编排，照生产 run.py 建模）

与生产 money_cta_secbar5m3/run.py 的区别：
  1. 策略换 SimProbe（探针：订阅+心跳+第2根bar开1手/第5根平，触发 mocker 成交）；
  2. 行情走 ParserUDP（合成 tick 灌入），交易走 TraderMocker（本地撮合），
     不依赖任何外网前置；
  3. wtpy 包与原生库由 run_sim.sh 组装到本目录（libs 与生产部署 zip 同源），
     PYTHONPATH 指向本目录，不污染 conda 环境。

用法（一般由 run_sim.sh 调用，也可手动）：
    python3 run_sim.py --seconds 150
"""
import argparse
import os
import sys
import time

HERE = os.path.abspath(os.path.dirname(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)

from wtpy import WtEngine, EngineType
from SimProbe import StraSimProbe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=150, help="运行时长（秒），到点自动退出")
    ap.add_argument("--code", default="SHFE.ag.HOT")
    ap.add_argument("--name", default="sim_probe")
    args = ap.parse_args()

    env = WtEngine(EngineType.ET_CTA)
    env.init(HERE + "/", "config.yaml")
    env.add_cta_strategy(StraSimProbe(args.name, args.code))
    env.run(True)

    print("[run_sim] engine started, will exit in %ds" % args.seconds)
    deadline = time.time() + args.seconds
    while time.time() < deadline:
        time.sleep(1)
    print("[run_sim] time up, force exit")
    os._exit(0)   # 日志 sink 均为 async:false，逐条落盘，强退安全


if __name__ == "__main__":
    main()
