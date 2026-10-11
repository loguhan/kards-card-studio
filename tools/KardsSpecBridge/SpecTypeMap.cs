using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// <see cref="BlueprintTypeRef"/> → KardsCardBuilder 的引脚类型词表。
///
/// 词表来源：CardSpec.cpp 的 <c>PinTypeFromSpec</c>（:318-380），实测有效取值：
///   bool byte int int64 float double string name text
///   enum:/Script/..  struct:/Script/..  object:/Script/..  class:/Script/..
///   interface:/Script/..
/// 容器前缀：array:  set:
///
/// 这是整个桥接器的地基 —— 类型串写错，插件会直接硬失败（不静默降级）。
/// </summary>
public static class SpecTypeMap
{
    /// <summary>标量 / 对象 / 枚举 → 引脚类型串（不含容器前缀）。</summary>
    public static string ToPinType(BlueprintTypeRef t) => t.Kind switch
    {
        BlueprintValueKind.Int32 => "int",
        BlueprintValueKind.Boolean => "bool",
        BlueprintValueKind.Float => "float",
        BlueprintValueKind.Name => "name",
        BlueprintValueKind.String => "string",
        BlueprintValueKind.Text => "text",

        // Byte 带 EnumType 时是"枚举值"，否则是裸 byte
        BlueprintValueKind.Byte => t.NativeName is null
            ? "byte"
            : $"enum:{Norm(t.NativePackage!)}.{t.NativeName}",

        BlueprintValueKind.Object => $"object:{Norm(t.NativePackage!)}.{t.NativeName}",
        BlueprintValueKind.Enum => $"enum:{Norm(t.NativePackage!)}.{t.NativeName}",

        // IR 里唯一的数组种类，spec 里是 array:int
        BlueprintValueKind.Int32Array => "array:int",

        _ => throw new NotSupportedException($"unsupported BlueprintValueKind {t.Kind}")
    };

    /// <summary>带容器前缀的引脚类型串。asArray=false 时等价于 <see cref="ToPinType"/>。</summary>
    public static string ToPinType(BlueprintTypeRef t, bool asArray) =>
        asArray ? "array:" + ToPinType(t) : ToPinType(t);

    /// <summary>
    /// 变量声明的类型串。IR 没有"对象数组"这个 kind，
    /// 但 spec 支持 —— 这是路线 B 相对路线 A 的核心增益之一，
    /// 由调用方通过 <paramref name="forceArray"/> 显式声明。
    /// </summary>
    public static string DeclType(BlueprintTypeRef t, bool forceArray = false) =>
        ToPinType(t, forceArray);

    /// <summary>把 "/Script/kards" 或裸 "kards" 统一成 "/Script/kards"。</summary>
    public static string Norm(string pkg) =>
        pkg.StartsWith("/Script/", StringComparison.Ordinal) ? pkg : "/Script/" + pkg;

    /// <summary>对象/枚举的完整类路径，形如 /Script/kards.BaseCardObject。</summary>
    public static string ClassPath(string package, string name) =>
        $"{Norm(package)}.{name}";

    /// <summary>把 BlueprintTypeRef 描述的类转成完整路径（用于 call 的 owner）。</summary>
    public static string ClassPath(BlueprintTypeRef t) =>
        t.NativeName is null
            ? throw new InvalidOperationException("type has no native class")
            : ClassPath(t.NativePackage!, t.NativeName);
}
