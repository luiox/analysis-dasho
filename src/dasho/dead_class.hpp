/// @file        dead_class.hpp
/// @brief       死类清扫：删掉"生成它的那次变换已经还原掉、再没有活引用"的类。
///
/// 典型对象是 DashO 字符串加密生成的解密器类（`Owner$1` 形态）。调用点折叠成
/// 明文之后，这些类就成了纯粹的混淆残留：留着会让还原产物里凭空多出几个
/// 匿名内部类，反编译出来还得人工判断"这是不是业务代码"。
///
/// ## 为什么不能只数常量池
///
/// libjbc 的常量池是**只追加**的：折叠把 `invokestatic` 换成 `ldc` 之后，原来
/// 那条 `Methodref`（以及它指向的 `Class` 条目）仍留在池里。按 CP 条目计数会
/// 把"池里还有残留条目"误判成"还有引用"，一个也删不掉（实测如此）。
///
/// 因此判定分两步：
///   1. **预筛**（便宜）：该类 CP 里是否出现候选类名。没有则必然无关。
///   2. **定判**（精确）：在预筛命中的类里，逐方法看**活指令**——`classOf`
///      （`new`/`checkcast`/`anewarray`/`instanceof`/`multianewarray`）、
///      `memberOf`（invoke/get/put 的 owner）、`constOf`（`ldc` 的 Class 常量）、
///      异常表 `catch_type`、类头 `super`/`interfaces`、成员声明描述符。
///      全都没有候选名才算死。
///
/// 任一步解不开（方法体 promote 失败）就**整体放弃清扫**：宁可留下残留类，
/// 也不能删掉活类。
///
/// 已知不覆盖的引用形态（都只会导致"该删没删"）：
///   - 反射按名字加载（`Class.forName("Owner$1")` 是字符串常量）；
///   - MANIFEST / `META-INF/services` 里的文本引用。
#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "dasho/class_set.hpp"

namespace dasho {

/// 删除容器里指定 URI 的资源；命中返回 true。
bool eraseResource(mjbase::model::ResourceContainer& container, std::string_view uri);

/// 是否为 DashO 字符串解密器类的**形态**：
///   - 类名形如 `Owner$<数字>`（匿名内部类命名）；
///   - 无字段、无 `<init>`（不是真的匿名类，是纯静态方法壳）；
///   - 全部方法 `static`，且至少一个返回 `String`。
///
/// 四条同时成立才认。DashO 每次构建换实现（`replace`/`copyValueOf` 等），
/// 所以判据只看形态，不看方法名。
bool looksLikeStringDecryptor(const jbc::ir::ClassUnit& unit, std::string_view className);

/// 容器内全部符合解密器形态的类（内部名）。
std::unordered_set<std::string> collectDecryptorClasses(ClassSet& set);

/// 候选类是否**仍被活代码引用**。
///
/// 判定分两步：先用常量池预筛（便宜），再在命中的类里逐方法看活指令（精确）。
/// 任一步解不开（方法体 promote 失败）都返回 true——**保守方向是"当它活着"**：
/// 宁可留下残留类，也不能删掉活类。
bool isCandidateLive(ClassSet& set, std::string_view candidate);

/// 一次清扫的结果。
struct DeadClassSweep
{
    ca::usize deleted        = 0;  ///< 删除的类数
    ca::usize keptReferenced = 0;  ///< 候选里仍有活引用的（保留）
    ca::usize keptMissing    = 0;  ///< 候选在容器里找不到（已被别的 pass 删掉）
    bool      aborted        = false;  ///< 判定失败，整体放弃
};

/// 删除 `candidates` 中没有任何活引用的类资源。
DeadClassSweep eraseUnreferencedClasses(mjbase::model::ResourceContainer&       container,
                                        ClassSet&                               set,
                                        const std::unordered_set<std::string>& candidates);

}  // namespace dasho
