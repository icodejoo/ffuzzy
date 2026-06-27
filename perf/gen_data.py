#!/usr/bin/env python3
"""Generate shared benchmark datasets + per-mode query files.

Both the C and Rust benches read these exact files, so the only variable is the
matching engine. Usage: python gen_data.py [N]   (default N=200000)
"""
import os
import random
import sys

N = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
HERE = os.path.dirname(os.path.abspath(__file__))
random.seed(42)

WORDS = ("src lib core util config model view controller service widget render "
         "parser lexer token stream buffer cache index query filter matcher "
         "engine module plugin handler adapter factory builder context session "
         "request response client server socket thread worker queue pool").split()
EXT = "rs dart c h py js ts go java kt txt md json yaml toml".split()
CJK = ("模块 搜索 引擎 用户 数据 中文 东京 北京 服务 组件 视图 控制 缓存 索引 "
       "查询 过滤 匹配 解析 请求 响应 线程 队列 网络 配置 渲染 插件").split()


def ascii_item():
    a, b, c = random.choice(WORDS), random.choice(WORDS), random.choice(WORDS)
    n = random.randint(0, 999)
    e = random.choice(EXT)
    shape = random.randint(0, 3)
    if shape == 0:
        return f"{a}/{b}_{c}/{c}{n}.{e}"
    if shape == 1:
        return f"{a}_{b}_{c}{n}"
    if shape == 2:
        return f"{a}/{b}/{c}.{e}"
    return f"get{b.capitalize()}{c.capitalize()}{n}"


def cjk_item():
    a, b = random.choice(CJK), random.choice(CJK)
    w = random.choice(WORDS)
    n = random.randint(0, 999)
    shape = random.randint(0, 2)
    if shape == 0:
        return f"{a}{b}_{w}{n}"
    if shape == 1:
        return f"{a}/{w}/{b}{n}"
    return f"{a}{b}{w.capitalize()}{n}"


def write_lines(path, lines):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


ascii_data = [ascii_item() for _ in range(N)]
cjk_data = [cjk_item() for _ in range(N)]
write_lines(os.path.join(HERE, "data_ascii.txt"), ascii_data)
write_lines(os.path.join(HERE, "data_cjk.txt"), cjk_data)

# Per-mode queries (a handful each; benches average over them).
write_lines(os.path.join(HERE, "q_fuzzy.txt"),
            ["src", "mod", "cfg", "usrsvc", "rndr", "qfm", "getuser", "tknstrm",
             "parsr", "cachidx"])
write_lines(os.path.join(HERE, "q_prefix.txt"),
            ["src", "lib", "core", "get", "util", "service"])
write_lines(os.path.join(HERE, "q_substring.txt"),
            ["mod", "user", "cache", "token", "service", "render"])
# Word (exact) queries: sample real full items so some hits occur.
write_lines(os.path.join(HERE, "q_word.txt"),
            random.sample(ascii_data, 8))
# CJK fuzzy queries.
write_lines(os.path.join(HERE, "q_cjk.txt"),
            ["搜索", "模块", "用户服务", "数据", "引擎", "中文", "查询过滤"])

print(f"wrote {N} ascii + {N} cjk items and query files to {HERE}")
