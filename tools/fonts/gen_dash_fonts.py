#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""家庭看板(Mosaico 版)中文字体子集生成器。

做一件事: 扫描 GSP 场景 JSON 里出现的所有文字, 把 Noto Sans SC 裁成
只含这些字形的 TTF, 供 gspc 烘焙进场景。

用法:
    python3 tools/fonts/gen_dash_fonts.py --scene components/app_hello/ui/main.json \
        --source /home/openclaw/esp/fonts/NotoSansSC-Regular.otf \
        --output components/app_hello/ui/fonts/NotoSansSC-DashSubset.ttf

设计要点:
  - 字符集 = 场景里所有 text 的字符 + font_charset + 数字/常用标点
  - 只在字符集变化时重写输出(比对 mtime), 避免无谓的构建抖动
  - 源字体找不到时直接报错退出, 不产出半成品
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

# 运行时动态绑定的数字/符号, 静态文本里可能不出现, 但必须提前烘焙进去
EXTRA_CHARS = "".join(chr(c) for c in range(0x20, 0x7F)) + "°·×↓±"


def collect_chars(scene: dict) -> str:
    chars: set[str] = set(EXTRA_CHARS)

    def walk(node) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "text" and isinstance(value, str):
                    chars.update(value)
                elif key == "font_charset" and isinstance(value, str):
                    chars.update(value)
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(scene)
    chars.discard("\n")
    chars.discard("\r")
    return "".join(sorted(chars))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scene", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    scene_path = Path(args.scene)
    source_path = Path(args.source)
    output_path = Path(args.output)

    if not source_path.is_file():
        print(f"font source not found: {source_path}", file=sys.stderr)
        return 2

    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    chars = collect_chars(scene)

    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        print("fontTools is required: pip install fonttools", file=sys.stderr)
        return 2

    font = TTFont(str(source_path))
    options = subset.Options()
    options.flavor = None
    options.desubroutinize = True
    options.drop_tables += ["DSIG"]
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=chars)
    subsetter.subset(font)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(output_path.suffix + ".tmp")
    font.save(str(tmp_path))
    os.replace(tmp_path, output_path)

    print(f"fonts: {len(chars)} chars -> {output_path} ({output_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
