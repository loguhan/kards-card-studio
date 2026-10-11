using UAssetAPI;
using UAssetAPI.ExportTypes;
using UAssetAPI.UnrealTypes;
using UAssetAPI.Unversioned;
using UAssetAPI.PropertyTypes.Objects;
using UAssetAPI.PropertyTypes.Structs;

// 用法:
//   KardsTable dump  <uasset> <usmap> [前N行]
//   KardsTable add   <uasset> <usmap> <行名> <列=值>... <输出uasset>
//   KardsTable clone <uasset> <usmap> <模板行> <新行名> <输出uasset>

if (args.Length < 3)
{
    Console.WriteLine("KardsTable dump  <uasset> <usmap> [N]");
    Console.WriteLine("KardsTable add   <uasset> <usmap> <rowName> <col=value>... <out.uasset>");
    Console.WriteLine("KardsTable clone <uasset> <usmap> <templateRow> <newRowName> <out.uasset>");
    return 1;
}

string cmd = args[0];
string assetPath = args[1];
string usmapPath = args[2];

var mappings = new Usmap(usmapPath);
var asset = new UAsset(assetPath, EngineVersion.VER_UE5_6, mappings);
Console.WriteLine($"asset={assetPath} exports={asset.Exports.Count}");

if (cmd == "exports")
{
    Console.WriteLine($"{"#",-4}{"Type",-28}{"ObjectName",-46}Outer");
    for (int i = 0; i < asset.Exports.Count; i++)
    {
        var e = asset.Exports[i];
        string outer = e.OuterIndex.IsNull() ? "<pkg>" : e.OuterIndex.ToString();
        Console.WriteLine($"{i,-4}{e.GetType().Name,-28}{e.ObjectName,-46}{outer}");
    }
    Console.WriteLine($"\n-- 包内是否有裸名对象 --");
    string want = Path.GetFileNameWithoutExtension(assetPath);
    bool found = false;
    for (int i = 0; i < asset.Exports.Count; i++)
    {
        var e = asset.Exports[i];
        if (e.OuterIndex.IsNull())
            Console.WriteLine($"  顶层导出: {e.ObjectName}  ({e.GetType().Name})");
        if (e.ObjectName.ToString() == want) found = true;
    }
    Console.WriteLine($"  包内存在名为 '{want}' 的对象: {found}");
    return 0;
}

if (cmd == "props")
{
    // KardsTable props <uasset> <usmap> <exportName>
    string exportName = args[3];
    NormalExport target = null;
    for (int i = 0; i < asset.Exports.Count; i++)
    {
        if (asset.Exports[i] is NormalExport ne &&
            (ne.ObjectName.ToString() == exportName || ne.ObjectName.ToString().Contains(exportName)))
        { target = ne; break; }
    }
    if (target == null)
    {
        Console.WriteLine("找不到导出 " + exportName + "；可用:");
        foreach (var e in asset.Exports) Console.WriteLine("   " + e.GetType().Name + "  " + e.ObjectName);
        return 4;
    }
    Console.WriteLine($"导出 {target.ObjectName}  属性数={target.Data.Count}");
    foreach (var f in target.Data)
    {
        string val = f switch
        {
            IntPropertyData x => x.Value.ToString(),
            Int64PropertyData x => x.Value.ToString(),
            BoolPropertyData x => x.Value.ToString(),
            FloatPropertyData x => x.Value.ToString(),
            StrPropertyData x => x.Value?.Value ?? "",
            NamePropertyData x => x.Value?.Value?.ToString() ?? "",
            EnumPropertyData x => x.Value?.Value?.ToString() ?? "",
            _ => ""
        };
        string extra = "";
        if (f is ArrayPropertyData ap)
        {
            extra = $" len={ap.Value?.Length ?? 0} elemType={ap.ArrayType?.ToString() ?? "?"}";
            if (ap.Value != null)
                foreach (var el in ap.Value.Take(6))
                    extra += $"\n        [{el.GetType().Name}] {Trunc(el.ToString())}";
        }
        Console.WriteLine($"   {f.Name,-34} {f.GetType().Name,-22} {val}{extra}");
    }
    return 0;
}

if (cmd == "setprop")
{
    // KardsTable setprop <uasset> <usmap> <exportName> <prop=value>... <out.uasset>
    string exportName = args[3];
    string outPath = args[args.Length - 1];
    var edits = new Dictionary<string, string>();
    for (int i = 4; i < args.Length - 1; i++)
    {
        var kv = args[i].Split('=', 2);
        if (kv.Length == 2) edits[kv[0]] = kv[1];
    }

    NormalExport target = null;
    for (int i = 0; i < asset.Exports.Count; i++)
    {
        if (asset.Exports[i] is NormalExport ne &&
            (ne.ObjectName.ToString() == exportName || ne.ObjectName.ToString().Contains(exportName)))
        { target = ne; break; }
    }
    if (target == null)
    {
        Console.WriteLine("找不到导出 " + exportName + "；可用:");
        foreach (var e in asset.Exports) Console.WriteLine("   " + e.GetType().Name + "  " + e.ObjectName);
        return 4;
    }
    Console.WriteLine($"目标导出: {target.ObjectName}  属性数={target.Data.Count}");

    int applied = 0;
    var seen = new HashSet<string>();
    foreach (var f in target.Data)
    {
        string key = f.Name.Value.Value.ToString();
        if (!edits.TryGetValue(key, out var v)) continue;
        seen.Add(key);
        bool ok = false;
        switch (f)
        {
            case BoolPropertyData bp:
                bp.Value = v.Equals("true", StringComparison.OrdinalIgnoreCase) || v == "1";
                ok = true; break;
            case IntPropertyData ip:
                ip.Value = int.Parse(v); ok = true; break;
            case Int64PropertyData i64:
                i64.Value = long.Parse(v); ok = true; break;
            case FloatPropertyData fp:
                fp.Value = float.Parse(v); ok = true; break;
            case StrPropertyData sp:
                sp.Value = new FString(v); ok = true; break;
            case NamePropertyData np:
                np.Value = new FName(asset, v); ok = true; break;
        }
        Console.WriteLine($"   {(ok ? "OK " : "跳过")} {key} ({f.GetType().Name}) <- {v}");
        if (ok) applied++;
    }

    // CDO 只序列化非默认值，所以缺失的属性要【新增】
    foreach (var kv in edits)
    {
        if (seen.Contains(kv.Key)) continue;
        var name = new FName(asset, kv.Key);
        PropertyData added = null;
        if (kv.Value.StartsWith("enumarray:", StringComparison.Ordinal))
        {
            // 形如 enumarray:ERegisteredCardFunction=OnOtherCardPlayedFromHand,OnStartofTurn
            var spec = kv.Value.Substring("enumarray:".Length).Split('=', 2);
            string enumType = spec[0];
            var entries = spec.Length > 1 ? spec[1].Split(',') : Array.Empty<string>();
            var arr = new ArrayPropertyData(name) { ArrayType = new FName(asset, "EnumProperty") };
            arr.Value = entries.Select(en => (PropertyData)new EnumPropertyData(new FName(asset, "None"))
            {
                EnumType = new FName(asset, enumType),
                Value = new FName(asset, en.Contains("::") ? en : enumType + "::" + en)
            }).ToArray();
            added = arr;
        }
        else if (kv.Value.Equals("true", StringComparison.OrdinalIgnoreCase) || kv.Value.Equals("false", StringComparison.OrdinalIgnoreCase))
            added = new BoolPropertyData(name) { Value = kv.Value.Equals("true", StringComparison.OrdinalIgnoreCase) };
        else if (int.TryParse(kv.Value, out var iv))
            added = new IntPropertyData(name) { Value = iv };
        if (added == null) { Console.WriteLine($"   新增失败 {kv.Key}"); continue; }
        target.Data.Add(added);
        Console.WriteLine($"   NEW {kv.Key} ({added.GetType().Name}) <- {kv.Value}");
        applied++;
    }
    Console.WriteLine($"应用了 {applied} 个改动");
    asset.Write(outPath);
    Console.WriteLine("已写出 " + outPath);
    return 0;
}

DataTableExport table = null;
for (int i = 0; i < asset.Exports.Count; i++)
{
    if (asset.Exports[i] is DataTableExport dt) { table = dt; Console.WriteLine($"DataTableExport @ {i}: {asset.Exports[i].ObjectName}"); break; }
}
if (table == null)
{
    Console.WriteLine("没有 DataTableExport；导出类型:");
    foreach (var e in asset.Exports) Console.WriteLine($"   {e.GetType().Name}  {e.ObjectName}");
    return 2;
}

var rows = table.Table.Data;
Console.WriteLine($"行数 = {rows.Count}");
if (rows.Count > 0)
{
    var r0 = rows[0];
    Console.WriteLine($"第一行: {r0.Name}  字段数={r0.Value.Count}");
    foreach (var f in r0.Value)
        Console.WriteLine($"    {f.Name,-28} {f.GetType().Name,-26} {Trunc(f.ToString())}");
}

if (cmd == "dump")
{
    int n = args.Length > 3 ? int.Parse(args[3]) : 5;
    Console.WriteLine($"\n--- 前 {n} 行 ---");
    foreach (var r in rows.Take(n))
        Console.WriteLine($"  {r.Name}");
    return 0;
}

if (cmd == "clone")
{
    string template = args[3];
    string newName = args[4];
    string outPath = args[5];
    var src = rows.FirstOrDefault(r => r.Name.Value.Value.ToString() == template);
    if (src == null) { Console.WriteLine($"找不到模板行 {template}"); return 3; }
    var copy = DeepCopyRow(asset, src, newName);
    rows.Add(copy);
    Console.WriteLine($"已克隆 {template} -> {newName}，现在 {rows.Count} 行");
    asset.Write(outPath);
    Console.WriteLine($"已写出 {outPath}");
    return 0;
}

if (cmd == "add")
{
    string outPath = args[args.Length - 1];
    string rowName = args[3];
    var edits = new Dictionary<string, string>();
    for (int i = 4; i < args.Length - 1; i++)
    {
        var kv = args[i].Split('=', 2);
        if (kv.Length == 2) edits[kv[0]] = kv[1];
    }

    var copy = DeepCopyRow(asset, rows[0], rowName);
    foreach (var f in copy.Value)
    {
        string key = f.Name.Value.Value.ToString();
        if (!edits.TryGetValue(key, out var v)) continue;
        Console.WriteLine($"  设 {key} = {v}");
        switch (f)
        {
            case IntPropertyData ip: ip.Value = int.Parse(v); break;
            case StrPropertyData sp: sp.Value = new FString(v); break;
            case NamePropertyData np: np.Value = new FName(asset, v); break;
            default: Console.WriteLine($"    (类型 {f.GetType().Name} 暂不支持)"); break;
        }
    }
    rows.Add(copy);
    Console.WriteLine($"已添加 {rowName}，现在 {rows.Count} 行");
    asset.Write(outPath);
    Console.WriteLine($"已写出 {outPath}");
    return 0;
}

Console.WriteLine($"未知命令 {cmd}");
return 9;

// 手工深拷贝一行（JSON 往返会因为 FNameJsonConverter 需要 UAsset 实例而失败）
static StructPropertyData DeepCopyRow(UAsset asset, StructPropertyData src, string newName)
{
    var row = new StructPropertyData(new FName(asset, newName));
    try { row.StructType = src.StructType; } catch { }
    try { row.StructGUID = src.StructGUID; } catch { }
    var list = new List<UAssetAPI.PropertyTypes.Objects.PropertyData>();
    foreach (var f in src.Value)
    {
        var n = f.Name;
        UAssetAPI.PropertyTypes.Objects.PropertyData nf = f switch
        {
            StrPropertyData s => new StrPropertyData(n) { Value = s.Value },
            IntPropertyData i => new IntPropertyData(n) { Value = i.Value },
            Int64PropertyData i64 => new Int64PropertyData(n) { Value = i64.Value },
            BoolPropertyData b => new BoolPropertyData(n) { Value = b.Value },
            NamePropertyData nm => new NamePropertyData(n) { Value = nm.Value },
            FloatPropertyData fl => new FloatPropertyData(n) { Value = fl.Value },
            _ => null
        };
        if (nf == null) { Console.WriteLine($"    !! 无法拷贝字段 {n} 类型 {f.GetType().Name}"); continue; }
        list.Add(nf);
    }
    row.Value = list;
    return row;
}

static string Trunc(string s) => s == null ? "" : (s.Length > 90 ? s.Substring(0, 90) + "..." : s);
