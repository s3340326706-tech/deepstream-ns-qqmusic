#!/usr/bin/env python3
"""打一个可以发出去的发布包。

产出 dist/Deepstream_v<版本>_Switch.zip，结构：

    Deepstream_v0.9.1/
    ├── switch/                 ← 整个文件夹拖到 SD 卡根目录
    │   ├── deepstream.nro
    │   └── se_sources/qqmusic.json
    ├── tools/qq-cookie/        ← 电脑上取的 Cookie 小工具（不进 SD 卡）
    │   ├── Get-QQCookie.bat
    │   ├── Get-QQCookie.ps1
    │   └── README.md
    ├── QUICKSTART.md
    ├── README.md
    ├── CHANGELOG.md
    ├── LICENSE
    ├── THIRD_PARTY_NOTICES.md
    └── account.example.json

刻意不包含 account.json —— 那是账号凭据，只属于使用者自己。

用法：
    python scripts/make_release.py [--nro app/build-nx/deepstream.nro]
"""

import argparse
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")

# 包内路径 ← 仓库内路径
LAYOUT = [
    ("switch/deepstream.nro", "app/build-nx/deepstream.nro"),
    ("switch/se_sources/qqmusic.json", "app/resources/sources/qqmusic.json"),
    ("tools/qq-cookie/Get-QQCookie.bat", "tools/qq-cookie/Get-QQCookie.bat"),
    ("tools/qq-cookie/Get-QQCookie.ps1", "tools/qq-cookie/Get-QQCookie.ps1"),
    ("tools/qq-cookie/README.md", "tools/qq-cookie/README.md"),
    ("QUICKSTART.md", "QUICKSTART.md"),
    ("README.md", "README.md"),
    ("CHANGELOG.md", "CHANGELOG.md"),
    ("LICENSE", "LICENSE"),
    ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md"),
    ("account.example.json", "account.example.json"),
]

# 绝不允许进包的文件（防止手滑把凭据打进去）
# 注意别写成裸 "cookie"：工具目录名里本来就有 cookie，会把整包判死
FORBIDDEN = re.compile(
    r"(^|/)account\.json$|\.bak$|\.cookie$|(^|/)cookie[^/]*\.txt$|\.log$|src-private/",
    re.I,
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nro", default="app/build-nx/deepstream.nro")
    args = ap.parse_args()

    # 版本号从 CMakeLists 读，别在两处各写一遍
    cmake = open(os.path.join(ROOT, "app", "CMakeLists.txt"), encoding="utf-8").read()
    ver = "{}.{}.{}".format(
        *(re.search(r'set\(VERSION_%s "(\d+)"\)' % k, cmake).group(1)
          for k in ("MAJOR", "MINOR", "ALTER"))
    )

    plan = [(dst, args.nro if src.endswith("deepstream.nro") else src)
            for dst, src in LAYOUT]

    missing = [src for _, src in plan if not os.path.isfile(os.path.join(ROOT, src))]
    if missing:
        print("缺少文件，先构建：")
        for m in missing:
            print("   ", m)
        return 1

    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, "Deepstream_v{}_Switch.zip".format(ver))
    top = "Deepstream_v{}".format(ver)

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dst, src in plan:
            arc = "{}/{}".format(top, dst)
            if FORBIDDEN.search(arc):
                print("拒绝打包（疑似凭据）:", arc)
                return 2
            z.write(os.path.join(ROOT, src), arc)

    # 打出来再独立核一遍：包里绝不能出现凭据特征
    with zipfile.ZipFile(out) as z:
        names = z.namelist()
        leak = [n for n in names if FORBIDDEN.search(n)]
        nro = z.read("{}/switch/deepstream.nro".format(top))
    for pat in (b"uin=o", b"qm_keyst=@", b"qqmusic_key=@"):
        if pat in nro:
            print("nro 内发现凭据特征:", pat)
            return 3

    size = os.path.getsize(out) / 1024 / 1024
    print("版本   :", ver)
    print("输出   :", out)
    print("大小   : %.2f MB" % size)
    print("文件数 :", len(names))
    if leak:
        print("!! 疑似泄漏:", leak)
        return 3
    print("凭据检查: 通过（无 account.json，nro 内无 cookie 特征）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
