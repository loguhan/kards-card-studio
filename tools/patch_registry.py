"""
patch_registry.py -- 给 writer 产出的 AssetRegistry.bin 补回 name 哈希表。

writer（UAssetRegistry / 卡编译器共用）会把 name 哈希表整个写成 0，
并重建 name 表顺序。本脚本：
  1. 从游戏原版注册表建 name -> hash 映射
  2. 按名字把 hash 填回 writer 的输出
  3. 恢复 hashVersion 字段
  4. 报告填不上的新名字

用法: patch_registry.py <原版.bin> <writer输出.bin> <输出.bin>
"""

import struct
import sys

sys.stdout.reconfigure(encoding="utf-8")


def parse(p):
    d = bytearray(open(p, "rb").read())
    count, nsb = struct.unpack_from("<ii", d, 24)
    hash_version = struct.unpack_from("<Q", d, 32)[0]
    hashes_off = 40
    headers_off = hashes_off + count * 8
    blob_off = headers_off + count * 2
    names = []
    q = blob_off
    for i in range(count):
        h = struct.unpack_from(">H", d, headers_off + i * 2)[0]
        wide = bool(h & 0x8000)
        ln = h & 0x7FFF
        if wide:
            names.append(d[q:q + ln * 2].decode("utf-16-le", "replace"))
            q += ln * 2
        else:
            names.append(d[q:q + ln].decode("utf-8", "replace"))
            q += ln
    assert q - blob_off == nsb, "name blob size mismatch"
    hashes = list(struct.unpack_from("<%dQ" % count, d, hashes_off))
    return d, count, nsb, hash_version, hashes_off, headers_off, blob_off, names, hashes


def main(argv):
    orig_path, writer_path, out_path = argv[0], argv[1], argv[2]

    o_d, o_c, o_nsb, o_hv, o_hoff, o_hdro, o_blob, o_names, o_hashes = parse(orig_path)
    w_d, w_c, w_nsb, w_hv, w_hoff, w_hdro, w_blob, w_names, w_hashes = parse(writer_path)

    print("原版   : count=%d hashVersion=0x%016X" % (o_c, o_hv))
    print("writer : count=%d hashVersion=0x%016X" % (w_c, w_hv))

    name_to_hash = dict(zip(o_names, o_hashes))
    filled, missing = 0, []
    for i, n in enumerate(w_names):
        h = name_to_hash.get(n)
        if h is None:
            missing.append(n)
            h = 0
        else:
            filled += 1
        struct.pack_into("<Q", w_d, w_hoff + i * 8, h)

    struct.pack_into("<Q", w_d, 32, o_hv)

    print("填回哈希 : %d / %d" % (filled, w_c))
    print("查不到的新名字 (%d): %s" % (len(missing), missing))
    print("hashVersion 恢复为 0x%016X" % o_hv)

    # 校验：哈希区不再是全零
    seg = bytes(w_d[w_hoff:w_hoff + w_c * 8])
    zeros = seg.count(0)
    print("哈希区零字节: %d / %d (%.1f%%)" % (zeros, len(seg), 100 * zeros / len(seg)))

    open(out_path, "wb").write(bytes(w_d))
    print("已写出:", out_path, len(w_d), "字节")


if __name__ == "__main__":
    main(sys.argv[1:])
