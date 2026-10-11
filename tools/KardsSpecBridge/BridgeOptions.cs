using System.Text.Json.Serialization;

namespace KardsSpecBridge;

/// <summary>
/// 流水线参数 —— 对应 spec 的 <c>pipeline</c> 段。
/// 可写在 options.json 里（第 3 个命令行参数）。
/// </summary>
public sealed class BridgeOptions
{
    /// <summary>资产包路径，形如 /Game/Blueprints/Cards/Mods/Soviet/units/&lt;name&gt;。</summary>
    public string Package { get; set; } = "";

    /// <summary>资产名（= package 末段）。</summary>
    public string AssetName { get; set; } = "";

    public string ParentClass { get; set; } = "/Script/kards.BaseCardObject";

    /// <summary>
    /// 原版 AssetRegistry.bin（从游戏主 pak 提取）。
    ///
    /// 留空时由 <see cref="ResolveDefaults"/> 按
    /// <c>KARDS_BASE_DIR</c> 环境变量 / 仓库根 <c>_work\base\</c> 推导。
    /// </summary>
    public string HostRegistry { get; set; } = "";

    /// <summary>
    /// 客户端卡目录表路径（不含扩展名）。
    /// 留空时同样由 <see cref="ResolveDefaults"/> 推导。
    /// </summary>
    public string BaseCatalog { get; set; } = "";

    /// <summary>
    /// 基础数据目录 —— 存放 <c>AssetRegistry.bin</c> 与 <c>deckCodeIDsTable2*</c>。
    ///
    /// 优先级：本属性 &gt; 环境变量 <c>KARDS_BASE_DIR</c> &gt; <c>&lt;仓库根&gt;\_work\base</c>。
    /// </summary>
    public string? BaseDir { get; set; }

    /// <summary>未显式给出时，按 BaseDir 推导出 HostRegistry / BaseCatalog。</summary>
    public void ResolveDefaults(string? repoRoot = null)
    {
        var dir = BaseDir;
        if (string.IsNullOrWhiteSpace(dir))
            dir = Environment.GetEnvironmentVariable("KARDS_BASE_DIR");
        if (string.IsNullOrWhiteSpace(dir) && repoRoot is not null)
            dir = Path.Combine(repoRoot, "_work", "base");

        if (!string.IsNullOrWhiteSpace(dir))
        {
            if (string.IsNullOrWhiteSpace(HostRegistry))
                HostRegistry = Path.Combine(dir, "AssetRegistry.bin");
            if (string.IsNullOrWhiteSpace(BaseCatalog))
                BaseCatalog = Path.Combine(dir, "deckCodeIDsTable2");
        }
    }

    public string CatalogPathInPak { get; set; } = "kards/Content/Structs/deckCodeIDsTable2";

    public string FilterPath { get; set; } = "/Game/Blueprints/Cards/Mods";

    public string PakName { get; set; } = "";

    public string DeckCode { get; set; } = "";

    public int CardId { get; set; }

    /// <summary>pak 内资产路径前缀，默认由 Package 推导。</summary>
    public string? AssetPathInPak { get; set; }

    /// <summary>相对 spec 文件所在目录的解析基准（内部用）。</summary>
    [JsonIgnore] public string? BaseDirectory { get; set; }
}
