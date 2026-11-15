#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""nsc_unpack.py — 独立 CLI：把 NSC 包 (.nsc/.ensc/.uns/.unse/.nus4 及 .zip 系)
递归解包成 MO2 mod 目录。

这是 NSC C++ MO2 插件 (nsc_plugin) 调用的「解包重活」——它不依赖 mobase /
MO2 插件框架，只做纯粹的归档解包，因此 C++ 插件可以用 QProcess 安全地调用它，
而不会把 Python 插件框架带进进程、也不会拉起第二个 MO2。

用法:
    python nsc_unpack.py "<archive>" "<out_mod_dir>"
    -> 解包所有文件(含嵌套压缩包)到 out_mod_dir，并写入 nsc_mod.json

注意：不写游戏根目录，只在给定的 MO2 mod 目录里落地。
"""
from __future__ import annotations

import json
import shutil
import sys
import zipfile
from pathlib import Path

# 内层可能继续是压缩容器的扩展名；.nsc 本身已是最外层，递归时不再二次识别为嵌套。
NESTED = {".zip", ".nsc", ".ensc", ".uns", ".unse", ".nus4", ".7z", ".rar"}


def _extract(archive: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    # 第一层
    with zipfile.ZipFile(archive) as z:
        z.extractall(out)
    # 递归把内层嵌套压缩包继续解平到同一棵树
    for p in list(out.rglob("*")):
        if p.is_file() and p.suffix.lower() in NESTED and p.suffix.lower() != ".nsc":
            sub = p.parent / (p.stem + ".d")
            sub.mkdir(exist_ok=True)
            try:
                with zipfile.ZipFile(p) as z2:
                    z2.extractall(sub)
                for f in list(sub.rglob("*")):
                    if f.is_file():
                        dest = p.parent / f.relative_to(sub)
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        if not dest.exists():
                            shutil.move(str(f), str(dest))
                p.unlink()
            except zipfile.BadZipFile:
                pass  # 不是 zip 系，原样保留
            finally:
                shutil.rmtree(sub, ignore_errors=True)


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: nsc_unpack.py <archive> <out_mod_dir>", file=sys.stderr)
        return 2
    archive = Path(sys.argv[1])
    out = Path(sys.argv[2])
    try:
        _extract(archive, out)
    except zipfile.BadZipFile:
        print(f"[nsc_unpack] not a zip-like archive: {archive}", file=sys.stderr)
        return 1
    except FileNotFoundError:
        print(f"[nsc_unpack] archive not found: {archive}", file=sys.stderr)
        return 1

    meta = {"kind": "replace", "name": out.name, "version": "", "globalOverrides": False}
    (out / "nsc_mod.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"[nsc_unpack] extracted {archive.name} -> {out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
