#!/usr/bin/env python3
"""rknn_names.py —— 从 .rknn 文件里扫出层名表，不用跑推理、不用 root、不用 SDK。

原理
----
转换器把每层写成一条 NUL 结尾的 ASCII 字符串 "OpType:Name"，放在模型的元数据段。
模型是 flatbuffer，vector 是倒着构建的，所以这些字符串在文件里的物理顺序是
层序的逆序 —— 反转之后就是运行时的 ID 顺序（1..N）。

已验证（与 rknn_query(RKNN_QUERY_PERF_DETAIL) 逐行比对，100% 一致）：
    yolov8s-2out.rknn   125 层
    yolov5s-640-640.rknn 86 层
    mobilenet_v1.rknn    34 层

已知限制
--------
* 只给"ID + OpType + Name"，没有每层耗时（那要 runtime）。
* 加密过的 .rknn 扫不出来（字符串被加密）。
* 名字里的字符集是白名单猜的（字母数字和 _ / . # -），遇到新字符需要放宽。
* 从 TF 转来的模型会多出 "Relu6__11:0" 这类变量名，用"OpType 含 __ 就丢掉"过滤。
* 这里的 OpType 是**名字前缀**（转换器记的原始算子），不一定等于运行时表里
  的 OpType 列：融合层的名字仍是 Conv，运行时列写的是 ConvExSwish / ConvAdd。
  也就是说 Conv 68 = ConvExSwish 53 + Conv 11 + ConvAdd 4。

用法
----
    ./rknn_names.py model.rknn               # ID / OpType / Name
    ./rknn_names.py model.rknn --csv         # id,op_type,name
    ./rknn_names.py model.rknn --names-only  # 只打印名字
    ./rknn_names.py model.rknn --raw         # 不过滤、不反转，看原始命中
"""
import argparse
import re
import sys

# "OpType:Name" —— OpType 只含字母数字下划线；Name 允许字母数字和 _ / . # -
NAME_RE = re.compile(rb"^[A-Za-z][A-Za-z0-9_]{1,31}:[A-Za-z0-9_/.#\-]{1,255}$")


def scan(path, keep_order=False, raw=False):
    with open(path, "rb") as f:
        data = f.read()

    hits = []
    pos = 0
    for chunk in data.split(b"\0"):
        if 3 <= len(chunk) <= 300 and NAME_RE.match(chunk):
            op, _, name = chunk.decode("ascii").partition(":")
            # TF 变量名（Relu6__11:0）不是层名：OpType 里不会有连续两个下划线
            if raw or "__" not in op:
                hits.append((pos, op, name))
        pos += len(chunk) + 1

    if not keep_order and not raw:
        hits.reverse()
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--csv", action="store_true", help="输出 id,op_type,name")
    ap.add_argument("--names-only", action="store_true", help="只打印 OpType:Name")
    ap.add_argument("--keep-order", action="store_true", help="保持文件物理顺序（不反转）")
    ap.add_argument("--raw", action="store_true", help="不做任何过滤/反转")
    ap.add_argument("-q", "--quiet", action="store_true", help="不打印表头/统计")
    args = ap.parse_args()

    hits = scan(args.model, args.keep_order, args.raw)
    if not hits:
        print("no layer name found (encrypted model?)", file=sys.stderr)
        return 2

    if args.csv:
        print("id,op_type,name")
        for i, (_, op, name) in enumerate(hits, 1):
            print(f"{i},{op},{name}")
    elif args.names_only:
        for _, op, name in hits:
            print(f"{op}:{name}")
    else:
        w = max(2, len(str(len(hits))))
        if not args.quiet:
            print(f"# {args.model}: {len(hits)} layers")
        print(f"{'ID':>{w}}  {'OpType':<16} Name")
        for i, (_, op, name) in enumerate(hits, 1):
            print(f"{i:>{w}}  {op:<16} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
