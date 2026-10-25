-- dasho：DashO 混淆家族的 deobf 插件仓（mjbase pass-pack + 独立 CLI）。
-- 依赖形态：静态链接 mjbase（源码级 dev 依赖，MJBASE_DIR 指向含 plugin
-- 接口的 mjbase 检出；mjbase 发版含接口后可切包消费）。各仓自组 CLI，
-- 无宿主进程、无动态库边界，ABI 面不存在。
--
-- 依赖红线：本仓只 include mjbase 公开头 + 自身代码；旧栈与产品线内容
-- 不入仓（含测试与语料）。

local mjbase_dir = os.getenv("MJBASE_DIR")
                  or path.join(os.scriptdir(), "..", "mjbase-plugin-api")
mjbase_dir = path.absolute(mjbase_dir)
if not os.isfile(path.join(mjbase_dir, "mjbase", "xmake.lua")) then
    raise("MJBASE_DIR 无效（缺 mjbase/xmake.lua）：" .. mjbase_dir)
end

-- 版本单源必须先于 libjbc.lua include（其内联包定义拼接这两个全局量）；
-- 数值与 mjbase 根 xmake 保持同一字面量。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"

set_project("analysis-dasho")
set_xmakever("2.9.3")
set_allowedplats("windows", "linux", "macosx")
set_defaultplat("windows")
set_defaultarchs("x64")
set_languages("c99", "cxx17")
add_rules("mode.debug", "mode.release")
set_warnings("all")

if is_plat("windows") then
    set_runtimes("MT")
    set_toolchains("msvc")
    add_cxflags("/GR-", {tools = "cl"})
    add_cxflags("/EHa", {tools = "cl"})
    add_cxflags("/wd4541", {tools = "cl"})
    set_exceptions("none")
else
    add_cxxflags("-fno-rtti")
end

add_defines("CA_COMPILE_LOG_LEVEL=0")

-- 以下包清单随 mjbase 根 xmake 同步（版本/嵌套钉定口径见彼处注释）。
includes(path.join(mjbase_dir, "xmake", "options.lua"))
includes(path.join(mjbase_dir, "xmake", "packages", "libjbc.lua"))
includes(path.join(mjbase_dir, "xmake", "libca_rules.lua"))
add_repositories("mjbase-repo " .. path.join(mjbase_dir, "xmake", "repo"))
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libarchive 3.8.8", {configs = {openssl3 = false}})
add_requires("zlib")

add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
add_requires("libjbc 0.4.15")
add_requires("libjstr 0.0.3")
add_requires("libjbytecode 0.0.3")
add_requires("libjzip 0.0.3")
add_requires("libgarena 0.0.3")
add_requires("libjclassfile 0.0.2")
add_requires("libjstd 0.0.1")
add_requires("libjdk_finder 0.1.0")
add_requires("libjhasm 0.0.1")
add_requireconfs("libjbc.libca",                         {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjstr.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libca",                   {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libjstr.libca",           {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libgarena.libca",         {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjzip.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libgarena.libca",                      {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjclassfile.libca",                  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjclassfile.libjstr.libca",          {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjstd.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjdk_finder.libca",                  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libca",                       {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})

if has_config("with_tests") then
    add_requires("gtest")
end

-- binary 静态库链接闭包（mjbase 根同名函数的副本；MSVC 顺序不敏感，
-- GNU ld 消费库在前）。
function mjbase_binary_link_closure(...)
    local libs = {...}
    local unpack = table.unpack or unpack
    add_links(unpack(libs))
    if is_plat("linux") then
        add_linkgroups(unpack(libs))
    end
end

includes(path.join(mjbase_dir, "mjbase", "xmake.lua"))
includes(path.join(mjbase_dir, "test_helper", "xmake.lua"))

target("dasho")
    set_kind("static")
    add_deps("mjbase")
    add_packages("libjbc", "libca", {public = true})
    add_files("src/dasho/**.cpp")
    add_includedirs("src", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("dasho_cli")
    set_kind("binary")
    add_deps("dasho", "mjbase")
    add_packages("libjbc", "libca")
    add_files("src/cli/**.cpp")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("dasho_unittest")
    set_kind("binary")
    if not has_config("with_tests") then
        set_enabled(false)
    end
    add_deps("dasho", "mjbase", "test_helper")
    add_packages("gtest", "libjbc", "libca", "libjdk_finder")
    add_files("test/unittest/**.cpp")
    add_includedirs("test/unittest")
    -- 测试夹具暂跨仓引用 mjbase 测试目录（structural_pass_util 等）；
    -- 稳定后 SDK 化或最小化复制，消掉这条。
    add_includedirs(path.join(mjbase_dir, "mjbase", "test", "unittest"))
    set_rundir("$(projectdir)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
