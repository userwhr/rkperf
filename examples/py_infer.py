#!/usr/bin/env python3
"""用 rknn-toolkit-lite2 跑一次推理，配合 rkperf 观察每次 rknn_run 的耗时和核。

用法:
    sudo env HOME=/home/orangepi \
             PYTHONPATH=/home/orangepi/.local/lib/python3.10/site-packages \
             ~/rkperf/rkperf python3 py_infer.py [model.rknn]

说明:
    rkperf 需要 root，而 pip install --user 装的包在 ~/.local 下，
    sudo 会重置 HOME，所以要显式传 PYTHONPATH（或把包装到系统里）。
"""

import os
import sys

import numpy as np
from rknnlite.api import RKNNLite

MODEL = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/yolo11n-seg-4out.rknn")
RUNS = 5

rknn = RKNNLite()
ret = rknn.load_rknn(MODEL)
print("load_rknn   :", ret)
ret = rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_AUTO)
print("init_runtime:", ret)

img = np.zeros((1, 640, 640, 3), dtype=np.uint8)
for i in range(RUNS):
    outs = rknn.inference(inputs=[img])
    print("run %d -> %s" % (i, [tuple(o.shape) for o in outs]))

rknn.release()
print("done")
