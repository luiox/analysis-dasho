# 分析工具链(mjt-mcp)使用说明

本仓的 pass 判据都是先在 Python 里做原型、验证通过后才落 C++ 的。原型跑在 `mjt-mcp` 上——一个打包好的字节码读写 SDK,可以直接改指令、改异常表、写回并重算帧。

本文记录**怎么用它**,以及踩过的坑。工具本体在仓外的 `studio/tools/mjt-mcp`(本地路径;剩余新版本仍在开发中)。

## 1. 为什么要有原型层

| | C++ 侧 | Python 原型 |
|---|---|---|
| 改一行到能跑 | `xmake build` ≈ 6s | 直接执行 |
| 跑一遍全样本 | 分钟级 | 秒级 |
| 调试手段 | `CA_LOG_WARN` + 重编译 | `print` |

`dasho.Checks` 的判据改了三轮(第一次 hammock 表述太严、handler 分组、全锚点覆盖),全是在 Python 里收敛的。C++ 侧只在算法定型后写了一遍。反过来做的话,每轮 6 秒编译 + 分钟级回归,三轮就是半小时。

原型不是一次性脚本,留在 `findings/run-lab/checks/proto_checks_strip.py` 当**参考实现**:C++ 里哪条判据看不懂,去原型里找对应函数。

## 2. 调用方式

```sh
mjt-mcp.exe --script proto_checks_strip.py <arg1> <arg2>
```

`--script` 之后的参数原样进 `sys.argv[1:]`。脚本里 `import mjt` 和 `import mjt_util` 都能直接用(与 exe 同目录)。

## 3. API 面

引用有两种元组,别混:

| 名称 | 形状 | 例 |
|---|---|---|
| `cref` | `(handle, "pkg/Class")` | `(0, "com/example/Foo")` |
| `mref` | `(handle, "pkg/Class", "name", "(I)V")` | `(0, "com/example/Foo", "run", "()V")` |

| 调用 | 说明 |
|---|---|
| `mjt.open_jar(path)` | 返回 handle |
| `mjt.close_container(h)` | |
| `mjt.class_names(h)` | 类名列表(斜杠分隔) |
| `mjt.methods(cref)` | **必须传元组**,不能拆成两个参数 |
| `mjt.insns(mref)` | 指令列表 |
| `mjt.insn_detail(mref, uid)` | 单条指令的细节(操作数、常量池解析结果) |
| `mjt.try_catches(mref)` | 异常表 |
| `mjt.remove_insn(...)` / `mjt.remove_try_catch(...)` | |
| `mjt.write_back(cref, compute_frames=True)` | 写回,可选重算 StackMapTable |
| `mjt.export_jar(h, path)` | |

## 4. 坑

1. **`open_container` / `methods(h, cls)` 不存在**。正确是 `open_jar` 和 `methods((h, cls))`。传错形态报 `TypeError: methods(): incompatible function arguments`,信息很不直观。先读 `mjt_util.py`,别猜。
2. **Foos 路径必须用盘符形式**。Python 的 `zipfile` 不认 MSYS 的 `/d/...` 形式,会 `FileNotFoundError`。传 `C:/...`(正斜杠可以,`/d/` 前缀不行)。
3. **`remove_try_catch` 要在删指令之前调**。先删指令会让表项按标签匹配落空,留下指向已删 pc 的陈旧项——导出成功、校验器拒。这与 C++ 侧 `dasho.Checks` 的三步顺序同源(见 [checks-strip-pass.md](checks-strip-pass.md) §5)。
4. **`write_back(compute_frames=True)` 只给可达的分支目标发帧**。方法里留死块就会 `VerifyError: Expecting a stack map frame`。原型和 C++ 都踩过,解法都是先清死块。

## 5. 与本仓 pass 的对应

原型只用来**定型判据**,不承担交付:

| 原型 | 落地 pass | 保留原因 |
|---|---|---|
| `findings/run-lab/checks/proto_checks_strip.py` | `dasho.Checks` | 算法参考实现 |

原型产物(`proto-all.jar`)也做过独立的逐类加载校验,这是判据正确性的第二份证据——与 C++ 实现互相印证,而不是"C++ 跑通了就算对"。

## 6. 两种接入方式

同一份二进制有两种用法，**能力面不一样**：

| 方式 | 入口 | 能力 |
|---|---|---|
| 脚本宿主 | `mjt-mcp.exe --script x.py` | `mjt` SDK 全量（读写指令、异常表、帧），但每个动作都要自己写代码 |
| MCP 服务器 | `mjt-mcp.exe --stdio` | 一套现成的分析工具面（见下），适合交互式勘察 |

MCP 面的工具（比 SDK 高一层，自带工程/缓存/台账）：

| 工具 | 职责 |
|---|---|
| `project_open` / `project_close` / `project_info` | 分析工程生命周期；`project_info` 报容器、revision、缓存健康、反编译后端 |
| `container_mount` | 挂载支撑容器（support/library/runtime） |
| `class_query` | 单类/资源/成员精确查询（九种 kind） |
| `search` | classes / members / strings / numbers / instructions 五族，支持 `contains`/`exact`/`prefix`/`regex` 与结构化操作数过滤 |
| `xref_query` | 引用图：指向/发出/调用点/层次/override |
| `decompile` | 反编译类或定位方法（异步 job + 条目级缓存） |
| `edit_apply` / `edit_history` | 编辑唯一入口（`dry_run` 预览 + 乐观 revision 控制）与台账（savepoint/restore/undo/redo） |
| `project_export` | 编辑态物化为派生容器（原子，不碰原容器） |

注册到客户端（`~/.workbuddy-ai/mcp.json`）：

```json
{
  "mcpServers": {
    "mjt-mcp": {
      "command": "C:/path/to/mjt-mcp/mjt-mcp.exe",
      "args": ["--stdio"]
    }
  }
}
```

两点注意：

1. **写完配置不会自动生效**。客户端在连接建立时缓存 `serverInfo` 与工具清单，需要在连接器管理页右上角的"自定义连接器"入口点一次信任，或重启客户端。信任之后**当前会话**就能直接看到工具面，不必重启（实测：写配置时工具不可见，点完信任后同一会话内可见）。
2. **就地替换 exe 后，在途会话感知不到**。`serverInfo.version` 形如 `0.1.0+build.<UTC 编译时间戳>`，可用它核对在途进程是不是新构建——旧进程的时间戳不变，重连后变化即生效。

纯 MCP 模式对 Python **零加载期依赖**（exe 不在加载期链接 `python314.dll`），只有 `--script` 模式在初始化时才装载解释器；脚本模式的查找顺序是 `MJT_PYTHON_HOME` → `<exe 目录>/python` → 编译期缺省路径。

### 6.1 实际调用顺序（几个容易踩的参数约束）

`project_open` 返回 `project_id`，后续所有工具都要带它。临时工程（不传 `project_dir`）在 `project_close` 时删掉 bundle，勘察用这个就够；需要保留编辑态才传 `project_dir`。

```text
project_open(path="<jar>")                                  → project_id
class_query(project_id, kind="list", limit=50)              # 枚举类；不要用 search 代替
class_query(project_id, kind="class", class="Foo")       # 结构 + 方法 + 行号表
decompile(project_id, class="Foo")                       # 整类
decompile(project_id, owner="Foo", name="<init>", descriptor="()V")   # 单方法
project_close(project_id)
```

- **`search(kind="classes")` 必须给 `query`**，空 query 报 `INVALID_ARGUMENT`。只想枚举时用 `class_query(kind="list")`。
- **`decompile` 定位方法时 `name` 和 `descriptor` 都必填**，只给 `name` 报 `name and descriptor are required`。描述符从 `class_query(kind="class")` 的 `methods[].descriptor` 抄。
- `class_query(kind="class")` 的 `lines[]` 是**指令下标 → 源行号**的映射表，块重排/jumbling 之后它会明显乱序，可直接当作"这个方法的线性结构被破坏过"的证据。

### 6.2 两条通道的分工

`--script` 适合批量、可重复、要落盘的改写（本仓的 pass 实现与回归都走它）；MCP 面适合交互式勘察与交叉验证。**结论级判断建议两条通道都过一遍**——[cleanup-pass.md](cleanup-pass.md) §2 那条"Vineflower 能还原 CFR 还原不了的 jumbled 嵌套循环"就是用 MCP 的 `decompile(owner/name/descriptor)` 独立复核的：`Foo.<init>`（行号表乱序）在 CFR 下报 `Unable to fully structure code`，Vineflower 输出完整的双层 `for`。
