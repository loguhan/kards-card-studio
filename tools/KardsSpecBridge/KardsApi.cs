using System.Text.Json.Nodes;

namespace KardsSpecBridge;

/// <summary>
/// KARDS 游戏 API 目录 —— 从 <c>-Mode=Dump</c> 的反射结果生成，
/// 供 C# 写卡时查询真实函数名、引脚名与出参名。
///
/// 用法（推荐先 dump 再写卡）：
/// <code>
///   pwsh .\BuildUeCard.ps1 -DumpApi /Script/kards.CardFunctionsStub -WorkRoot ...\dump
///   pwsh .\BuildUeCard.ps1 -DumpApi /Script/kards.BaseCardObject   -WorkRoot ...\dump
/// </verbatim>
/// </summary>
public static class KardsApi
{
    /// <summary>
    /// 从 dump.json 生成 C# 常量表（复制粘贴进卡包，或直接引用本类）。
    /// </summary>
    public static string GenerateCatalog(string dumpJsonPath, string className)
    {
        var text = File.ReadAllText(dumpJsonPath);
        var root = JsonNode.Parse(text)!.AsObject();
        var functions = root["functions"]!.AsArray();

        var sb = new System.Text.StringBuilder();
        sb.AppendLine($"// 自动生成自 {Path.GetFileName(dumpJsonPath)}");
        sb.AppendLine($"// 类: {className}");
        sb.AppendLine();
        sb.AppendLine($"public static class {className}Api");
        sb.AppendLine("{");
        sb.AppendLine($"    public const string Owner = \"{className}\";");
        sb.AppendLine();

        foreach (var f in functions
                     .Select(x => x!.AsObject())
                     .OrderBy(x => x["name"]!.GetValue<string>()))
        {
            var name = f["name"]!.GetValue<string>();
            var pure = f["blueprint_pure"]?.GetValue<bool>() ?? false;
            var callable = f["blueprint_callable"]?.GetValue<bool>() ?? false;
            if (!callable) continue;

            var ins = new List<string>();
            var outs = new List<string>();
            foreach (var p in f["params"]!.AsArray().Select(x => x!.AsObject()))
            {
                var pn = p["name"]!.GetValue<string>();
                var isOut = p["out"]?.GetValue<bool>() ?? false;
                if (isOut) outs.Add(pn);
                else ins.Add(pn);
            }

            sb.AppendLine($"    /// <summary>{(pure ? "pure" : "impure")}  in=[{string.Join(", ", ins)}]  out=[{string.Join(", ", outs)}]</summary>");
            sb.AppendLine($"    public const string {name} = \"{name}\";");
        }

        sb.AppendLine("}");
        return sb.ToString();
    }
}
