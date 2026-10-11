"""
fix_registry_class.py -- 修掉卡编译器注册表条目里缺失的 AssetClass。

用法:
    fix_registry_class.py <List导出的JSON> <输出JSON> <卡名>

原理:
    KardsCardCompiler 依赖的 UAssetRegistry 不序列化 FAssetData::AssetClass，
    所以合并后新卡的条目 AssetClass 为空。空的 AssetClass 会让
    FAssetData::GetAsset() 解析不出资产 → 卡进不了卡池。
    真卡的条目是 /Script/Engine.Blueprint 与 /Script/Engine.BlueprintGeneratedClass，
    这里照抄。
"""

import json
import sys

sys.stdout.reconfigure(encoding="utf-8")


def main(argv):
    src, out, name = argv[0], argv[1], argv[2]

    assets = json.load(open(src, encoding="utf-16"))["Assets"]

    def find(n):
        for e in assets:
            if e.get("AssetName") == n:
                return e
        raise SystemExit("not found: " + n)

    bp = find(name)
    gc = find(name + "_C")

    bp["AssetClass"] = "/Script/Engine.Blueprint"
    gc["AssetClass"] = "/Script/Engine.BlueprintGeneratedClass"

    # 导出会把非 ASCII 标题搞坏，顺手修回来
    title = 'NSLOCTEXT("kards_mod", "%s_title", "\u53d8\u8282\u70ae\u7ec4")' % name
    for e in (bp, gc):
        for t in e.get("TagsAndValues", []):
            if t["TagName"] == "title":
                t["TagValue"] = title

    json.dump({"Assets": [bp, gc]}, open(out, "w", encoding="utf-8-sig"),
              ensure_ascii=False, indent=2)

    tags = {t["TagName"]: t["TagValue"] for t in bp.get("TagsAndValues", [])}
    print("写出:", out)
    print("  ", bp["AssetName"], "->", bp["AssetClass"], " cardSet =", tags.get("cardSet"))
    print("  ", gc["AssetName"], "->", gc["AssetClass"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
