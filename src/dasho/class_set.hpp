/// @file        class_set.hpp
/// @brief       容器级 pass 的共用底座：类装载索引、纯度判定、写回（含帧再生）。
///
/// DashO 的各变换面（字符串加密、控制流注入、注入式检查）都以「整 jar 的类
/// 集合」为作用域：解密器与调用点常分居不同类、匿名内部类，跨类解析不可避。
/// 本单元把三件重复工作收口：
///   1. 从容器装载全部 ClassResource 成会话，建 内部名 → 会话 索引；
///   2. 纯方法判定（无字段写/无同步/调用面全纯）——解释器只对纯方法递归，
///      保证任何求值都不会改变程序语义；
///   3. 写回：lower + A1 帧链（Strict 重算 + SMT 再生）+ writeClass +
///      replaceResource。GlobalPass 自持写回，必须自己接帧链，否则改了栈形
///      的方法会留下过期 StackMapTable。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

namespace dasho {

/// 待写回的方法。
struct DirtyMethod
{
    ca::usize idx = 0;
    /// 是否做了**指令增删**（整段折叠）。true 时既有 StackMapTable 的目标
    /// 会悬垂，必须在 lower 后按新布局再生帧；false（原地改指令，布局仅因
    /// LDC→LDC_W 可能位移）沿用原帧的 relocation 结果即可。
    bool structural = false;
};

/// 一个已装载的类资源（会话 + 原始 URI + 待写回方法表）。
struct LoadedClass
{
    std::string                   name;      ///< 类内部名
    std::string                   uri;       ///< 容器 URI（写回键）
    mjbase::model::ClassResource* resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
    std::vector<DirtyMethod>      dirty;     ///< 已改动、待写回的方法
};

/// 类集合：装载 + 索引 + 纯度缓存 + 写回。
class ClassSet
{
public:
    /// 从容器装载全部 class 资源；非 class 资源忽略。
    void load(mjbase::model::ResourceContainer& container);

    std::vector<LoadedClass>&       classes() noexcept { return classes_; }
    const std::vector<LoadedClass>& classes() const noexcept { return classes_; }

    /// 内部名 → 会话；未装载返回 nullptr。
    jbc::ir::ClassSession* find(std::string_view internalName) const;

    /// 类表视图（解释器的类解析入口）。
    const std::unordered_map<std::string, jbc::ir::ClassSession*>& map() const noexcept
    {
        return byName_;
    }

    /// 纯方法判定：无字段读写、无同步、无 athrow、调用面全部纯或为内建纯方法。
    /// 递归判定带环检测（环 → 不纯）。
    bool isPure(jbc::ir::ClassSession& session, ca::usize methodIdx);

    /// 写回全部 dirty 类：lower → 帧链 → writeClass → replaceResource。
    /// @return 成功写回的类数；失败类保持原样并记 warn。
    ca::usize commit(mjbase::model::ResourceContainer& container);

private:
    enum class Purity : ca::u8 { Unknown, InProgress, Pure, Impure };

    bool computePurity(jbc::ir::ClassSession& session, ca::usize methodIdx);

    std::vector<LoadedClass>                                classes_;
    std::unordered_map<std::string, jbc::ir::ClassSession*> byName_;
    std::unordered_map<const jbc::ir::ClassSession*, std::unordered_map<ca::usize, Purity>>
        purity_;
};

}   // namespace dasho
