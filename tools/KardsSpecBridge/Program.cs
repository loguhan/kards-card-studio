using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using KardsCardCompiler.Core;
using KardsSpecBridge;

namespace KardsSpecBridge;

/// <summary>
/// KardsSpecBridge —— C# 写卡 → KardsCardBuilder spec 的桥接器。
///
/// 用法：
///   KardsSpecBridge &lt;pack.dll|pack.cs&gt; &lt;out.spec.json&gt; [options.json]
///
/// 它做三件事：
///   1. 加载你写的 ICardPackDefinition / ICardBlueprintDefinition 程序集
///   2. 把卡片定义 + 蓝图函数 IR 组装成 spec JSON
///   3. 交给 BuildUeCard.ps1 走 UE 侧 8 步（拼 K2 节点 → 编译 → Strip → 打包 → 部署）
///
/// 结果：你只写 C#，产出的卡与手拼节点<b>完全等价</b>（同一份 spec、同一个插件）。
/// </summary>
public static class Program
{
    public static int Main(string[] args)
    {
        if (args.Length < 2 || args[0] is "-h" or "--help")
        {
            PrintUsage();
            return args.Length < 2 ? 2 : 0;
        }

        var packPath = args[0];
        var outPath = args[1];
        var optPath = args.Length > 2 ? args[2] : null;

        try
        {
            // ── 1. 参数 ────────────────────────────────────────────
            var opt = LoadOptions(optPath);

            // ── 2. 加载卡包程序集 ──────────────────────────────────
            var asm = LoadAssembly(packPath);

            var packDefs = FindImplementations<ICardPackDefinition>(asm);
            if (packDefs.Count == 0)
                throw new InvalidOperationException(
                    $"{packPath} 里找不到 ICardPackDefinition 实现。");

            var bpDefs = FindImplementations<ICardBlueprintDefinition>(asm).FirstOrDefault();
            var specDefs = FindImplementations<ISpecBlueprintDefinition>(asm).FirstOrDefault();

            Console.WriteLine($"  卡包实现 : {packDefs.Count} 个");
            Console.WriteLine($"  蓝图实现 : {(bpDefs is null ? "(无)" : bpDefs.GetType().Name)}");
            Console.WriteLine($"  原生spec : {(specDefs is null ? "(无)" : specDefs.GetType().Name)}");

            // ── 3. 逐卡翻译 ────────────────────────────────────────
            var specs = new List<(string Name, JsonObject Spec)>();

            foreach (var pack in packDefs)
            {
                foreach (var card in pack.DefineCards())
                {
                    var cardIr = card.ToIr();

                    // 每张卡用它自己的名字推导 package / assetName（除非 options 显式覆盖）
                    var cardOpt = CloneForCard(opt, cardIr);

                    var fns = bpDefs?.DefineBlueprintFunctions(cardIr)
                              ?? Array.Empty<BlueprintFunctionIr>();
                    var members = bpDefs?.DefineBlueprintMembers(cardIr)
                                  ?? Array.Empty<BlueprintMemberDefinition>();
                    var specSet = specDefs?.DefineSpecGraphs(cardIr);

                    var spec = SpecAssembler.Assemble(card, fns, members, cardOpt, specSet);
                    specs.Add((cardIr.Name, spec));

                    Console.WriteLine(
                        $"  翻译 : {cardIr.Name,-44} IR函数 {fns.Count} 个, " +
                        $"原生图 {specSet?.Graphs.Count ?? 0} 个, " +
                        $"变量 {spec["variables"]!.AsArray().Count} 个");
                }
            }

            if (specs.Count == 0)
                throw new InvalidOperationException("卡包没有产出任何卡。");

            // ── 4. 写出 ────────────────────────────────────────────
            // 单卡：直接写 outPath；多卡：每张卡写 <outPath 去扩展名>.<卡名>.json
            var jsonOpts = new JsonSerializerOptions
            {
                WriteIndented = true,
                Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
                TypeInfoResolver = new System.Text.Json.Serialization.Metadata.DefaultJsonTypeInfoResolver()
            };

            if (specs.Count == 1)
            {
                WriteSpec(outPath, specs[0].Spec, jsonOpts);
                Console.WriteLine($"  spec  : {Path.GetFullPath(outPath)}");
            }
            else
            {
                var dir = Path.GetDirectoryName(Path.GetFullPath(outPath))!;
                var stem = Path.GetFileNameWithoutExtension(outPath);
                Directory.CreateDirectory(dir);

                var manifest = new JsonArray();
                foreach (var (name, spec) in specs)
                {
                    var p = Path.Combine(dir, $"{stem}.{name}.json");
                    WriteSpec(p, spec, jsonOpts);
                    manifest.Add(p);
                    Console.WriteLine($"  spec  : {p}");
                }

                // 顺带写一个清单，方便脚本批量驱动
                var manifestPath = Path.Combine(dir, $"{stem}.manifest.json");
                File.WriteAllText(manifestPath,
                    new JsonObject { ["specs"] = manifest }.ToJsonString(jsonOpts));
                Console.WriteLine($"  清单  : {manifestPath}");
            }

            Console.WriteLine($"  完成  : {specs.Count} 张卡");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine($"[KardsSpecBridge] 失败: {ex.GetType().Name}");
            Console.Error.WriteLine($"  {ex.Message}");
            if (ex.InnerException is not null)
                Console.Error.WriteLine($"  内层: {ex.InnerException.Message}");
            return 1;
        }
    }

    // ───────────────────────────────────────────────────────────── 助手

    private static void PrintUsage()
    {
        Console.WriteLine("""
            KardsSpecBridge —— C# 写卡 → KardsCardBuilder spec 桥接器

            用法:
              KardsSpecBridge <pack.dll> <out.spec.json> [options.json]

            参数:
              pack.dll      编译好的卡包程序集（含 ICardPackDefinition /
                            ICardBlueprintDefinition 实现）
              out.spec.json 输出 spec；多张卡时自动拆成 <stem>.<卡名>.json
                            + <stem>.manifest.json
              options.json  可选，流水线参数（见下方字段）

            options.json 字段:
              Package         资产包路径，如 /Game/Blueprints/Cards/Mods/Soviet/units/x
              AssetName       资产名
              ParentClass     默认 /Script/kards.BaseCardObject
              HostRegistry    原版 AssetRegistry.bin 路径
              BaseCatalog     原版 deckCodeIDsTable2 路径
              FilterPath      默认 /Game/Blueprints/Cards/Mods
              PakName         输出 pak 名（须以 _P.pak 结尾）
              DeckCode        卡组码，如 VA
              CardId          目录 ID

            产出后交给：BuildUeCard.ps1 -Spec <out.spec.json> -Force
            """);
    }

    private static BridgeOptions LoadOptions(string? optPath)
    {
        var opt = new BridgeOptions();

        if (optPath is not null)
        {
            if (!File.Exists(optPath))
                throw new FileNotFoundException($"options.json 不存在: {optPath}");

            opt = JsonSerializer.Deserialize<BridgeOptions>(
                      File.ReadAllText(optPath),
                      new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
                  ?? throw new InvalidOperationException("options.json 解析为空");

            opt.BaseDirectory = Path.GetDirectoryName(Path.GetFullPath(optPath));
        }

        // 未显式给出基础数据路径时，按 BaseDir / 环境变量推导
        opt.ResolveDefaults(FindRepoRoot());

        if (string.IsNullOrWhiteSpace(opt.HostRegistry))
            throw new InvalidOperationException(
                "无法确定 AssetRegistry.bin 路径。请任选其一：\n" +
                "  1. options.json 里写 \"HostRegistry\": \"<路径>\"\n" +
                "  2. options.json 里写 \"BaseDir\": \"<含 AssetRegistry.bin 的目录>\"\n" +
                "  3. 设环境变量 KARDS_BASE_DIR\n" +
                "  4. 把 AssetRegistry.bin 放到 <仓库根>\\_work\\base\\");

        return opt;
    }

    /// <summary>从当前程序集位置向上找仓库根（含 .git 或 config.example.json）。</summary>
    private static string? FindRepoRoot()
    {
        var dir = AppContext.BaseDirectory;
        for (var i = 0; i < 8 && !string.IsNullOrEmpty(dir); i++)
        {
            if (File.Exists(Path.Combine(dir, "config.example.json")) ||
                Directory.Exists(Path.Combine(dir, ".git")))
                return dir;
            dir = Path.GetDirectoryName(dir.TrimEnd(Path.DirectorySeparatorChar)) ?? "";
        }
        return null;
    }

    private static Assembly LoadAssembly(string packPath)
    {
        var full = Path.GetFullPath(packPath);
        if (!File.Exists(full))
            throw new FileNotFoundException($"卡包程序集不存在: {full}");

        // 从卡包所在目录解析依赖（KardsCardCompiler.Core 等）
        var dir = Path.GetDirectoryName(full)!;
        AppDomain.CurrentDomain.AssemblyResolve += (_, e) =>
        {
            var simple = new AssemblyName(e.Name).Name + ".dll";
            var candidate = Path.Combine(dir, simple);
            return File.Exists(candidate) ? Assembly.LoadFrom(candidate) : null;
        };

        // 预先把 Core 加载进来（桥接器自身也引用它，确保同版本）
        var coreDll = Path.Combine(dir, "KardsCardCompiler.Core.dll");
        if (File.Exists(coreDll)) Assembly.LoadFrom(coreDll);

        return Assembly.LoadFrom(full);
    }

    private static List<T> FindImplementations<T>(Assembly asm)
    {
        var iface = typeof(T);
        var result = new List<T>();

        foreach (var t in asm.GetTypes())
        {
            if (t.IsAbstract || t.IsInterface || !iface.IsAssignableFrom(t)) continue;
            if (t.GetConstructor(Type.EmptyTypes) is null) continue;

            if (Activator.CreateInstance(t) is T inst) result.Add(inst);
        }
        return result;
    }

    /// <summary>
    /// options 里没写 package/assetName 时，按卡名推导到 Mods 目录下。
    /// 阵营取自卡定义，落到 /Game/Blueprints/Cards/Mods/&lt;Faction&gt;/units/&lt;name&gt;。
    /// </summary>
    private static BridgeOptions CloneForCard(BridgeOptions opt, CardIr card)
    {
        var clone = new BridgeOptions
        {
            Package = opt.Package,
            AssetName = opt.AssetName,
            ParentClass = opt.ParentClass,
            HostRegistry = opt.HostRegistry,
            BaseCatalog = opt.BaseCatalog,
            CatalogPathInPak = opt.CatalogPathInPak,
            FilterPath = opt.FilterPath,
            PakName = opt.PakName,
            DeckCode = opt.DeckCode,
            CardId = opt.CardId,
            AssetPathInPak = opt.AssetPathInPak,
            BaseDirectory = opt.BaseDirectory
        };

        // 未显式指定 → 按卡名 + 阵营推导
        if (string.IsNullOrWhiteSpace(clone.AssetName))
        {
            clone.AssetName = card.Name;
            var kindSeg = card.Kind == CardKind.Unit ? "units" : "events";
            clone.Package =
                $"/Game/Blueprints/Cards/Mods/{card.Faction}/{kindSeg}/{card.Name}";
            clone.PakName = $"KardsDIY_{card.Name}_ue_P.pak";
        }
        else if (string.IsNullOrWhiteSpace(clone.Package))
        {
            clone.Package = $"/Game/Blueprints/Cards/Mods/{(int)card.Faction}/{card.Name}";
        }

        if (string.IsNullOrWhiteSpace(clone.PakName))
            clone.PakName = $"KardsDIY_{clone.AssetName}_ue_P.pak";

        if (clone.CardId == 0) clone.CardId = card.Id;
        if (string.IsNullOrWhiteSpace(clone.DeckCode)) clone.DeckCode = card.DeckCodeId;

        return clone;
    }

    private static void WriteSpec(string path, JsonObject spec, JsonSerializerOptions opts)
    {
        var full = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        File.WriteAllText(full, spec.ToJsonString(opts));
    }
}
