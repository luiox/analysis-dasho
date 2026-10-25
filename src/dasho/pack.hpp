#pragma once

#include "mjbase/plugin/pack.hpp"

namespace dasho {

/// @brief 家族 pack 入口：返回实现 mjbase::plugin::IPassPack 的 DashO pack。
///        宿主经 mjbase::plugin::installPack(dasho_pack(), registry, config)
///        装配；同源门与 "dasho." 命名空间由 installPack 机械验收。
mjbase::plugin::IPassPack& dasho_pack();

}   // namespace dasho
