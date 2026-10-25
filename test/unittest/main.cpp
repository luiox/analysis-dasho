/// @file        main.cpp
/// @brief       dasho 单测入口。
#include <gtest/gtest.h>

#include "test_helper.hpp"

int main(int argc, char** argv)
{
    // 注册 dasho 子项目根（.project_root_file）：test_helper 资源访问
    // 统一入口；当前测试语料在仓外 lab 目录，缺失时用例自行 GTEST_SKIP。
    test::setup("analysis-dasho");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
