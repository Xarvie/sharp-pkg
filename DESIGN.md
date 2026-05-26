# spkg 设计文档

> Sharp Package Manager & Build System
> 日期：2026-05-26
> 状态：Phase 1 + 1.5 已实现

## 1. 愿景

**成为 Sharp 语言的一等构建工具，达到或超越 Ninja/Bazel/Zig build 级别的工程品质。**

核心哲学：
- **声明优先** — 用户描述"要什么"，spkg 决定"怎么做"
- **确定性构建** — 相同输入 → 相同输出，可复现
- **零浪费编译** — 只做必要的编译，不多不少
- **Windows 一等公民** — 不是"支持"，而是原生一等平台
- **分布式就绪** — 架构设计原生支持分布式扩展
- **C 核心，Lua 声明** — Lua 仅用于用户声明构建图（类似 zig 的 build.zig），所有执行逻辑在 C 层

---

## 2. 路线图

| Phase | 内容 | 状态 |
|-------|------|------|
| **Phase 1** | 声明式构建图、全平台 target、增量编译、cflags/ldflags | ✅ |
| **Phase 1.5** | 头文件依赖追踪、Windows 一等公民、并行编译、DAG 拓扑排序、命令指纹 | ✅ |
| **Phase 2** | 内容寻址编译缓存（local）、Custom Step、spkg cache 命令 | 开发中 |
| **Phase 3** | 分布式编译（spkg-node + coordinator） | 规划中 |
| **Phase 4** | 彩色诊断、测试框架 | 规划中 |

---

## 3. 架构

```
┌────────────────────────────────────────────────────────────────┐
│                        spkg (C + Lua 5.4)                       │
│                                                                  │
│  ┌───────────┐  ┌───────────┐  ┌──────────────────────────┐    │
│  │  main.c   │  │ native.c  │  │   Lua Runtime (5.4)      │    │
│  │ CLI/Args  │  │ Core      │  │                          │    │
│  │           │  │ Engine    │  │ spkg_init.lua            │    │
│  │           │  │           │  │   (CLI dispatch)          │    │
│  │           │  │ • Compile │  │                          │    │
│  │           │  │ • Link    │  │ spkg_build.lua           │    │
│  │           │  │ • Cache   │  │   (b API: user declares)  │    │
│  │           │  │ • DAG     │  │                          │    │
│  │           │  │ • Custom  │  │ spkg_fetch.lua           │    │
│  │           │  │ Step      │  │   (dependency fetch)      │    │
│  │           │  │           │  │                          │    │
│  │           │  │           │  │ spkg_lock.lua            │    │
│  │           │  │           │  │   (lockfile management)   │    │
│  └───────────┘  └───────────┘  └──────────────────────────┘    │
└────────────────────────┬────────────────────────────────────────┘
                         │ 加载
              ┌──────────┴──────────┐
              │   Sharp.lua (用户)   │
              │ b:add_executable{}  │
              │ b:install(artifact) │
              └─────────────────────┘
```

### 三层分离

| 层级 | 文件 | 职责 | 原则 |
|------|------|------|------|
| **声明层** | `Sharp.lua` | 用户声明工件、源文件、依赖关系 | 纯函数，无副作用 |
| **执行层** | `native.c` | 编译、链接、缓存、DAG、Custom Step | **所有核心逻辑在 C** |
| **系统层** | `native.c` | 文件操作、进程执行、平台检测 | 跨平台封装（POSIX + Windows） |

> **关键设计原则**：Lua 只负责 Sharp.lua 的用户声明 API（类似 zig 的 build.zig）。
> 编译、链接、缓存、DAG 排序、并行调度等所有执行逻辑都在 native.c 实现，Lua 只通过 C 绑定调用。

---

## 4. Sharp.lua 规范

### 4.1 构建上下文 `b`

```lua
-- 查询构建环境
b:get_target()            → string          -- target triple (x86_64-pc-linux-gnu)
b:get_optimize()          → string          -- Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
b:get_verbose()           → bool            -- 详细输出模式
b:get_jobs()              → int             -- 并行编译数
b:get_host()              → string          -- 宿主机平台 triple

-- 声明工件
b:add_executable(opts)    → Artifact        -- 可执行文件
b:add_static_library(opts) → Artifact       -- 静态库
b:add_shared_library(opts) → Artifact       -- 动态库

-- 自定义构建步骤（Phase 2）
b:add_custom_step(opts)   → CustomStep     -- 代码生成、资源编译等

-- 获取已声明工件
b:dependency(name)        → Artifact | nil

-- 安装目标
b:install(artifact)       → void            -- 标记为默认构建目标
b:install(artifact, path) → void            -- 安装到指定路径
```

### 4.2 Artifact API

```lua
local exe = b:add_executable({ name = "myapp" })

-- 源文件管理
exe:add_source("src/main.sp")                     -- 简写
exe:add_source({
    file    = "src/features/**/*.sp",             -- glob 模式
    cflags  = {"-DFEATURE_X"},
    include = {"src/features/include"},
})

-- 编译选项（全局）
exe:add_include("include")
exe:add_include("src")
exe:add_cflags("-Wall", "-Wextra")
exe:add_ldflags("-pthread")

-- 链接
exe:link_library("pthread")                        -- 系统库
exe:link_library("dl")
exe:link_artifact(mathlib)                         -- 链接另一个 Artifact（自动排序）

-- 运行时配置
exe:set_run_args("--verbose", "--port=8080")       -- run 时传递参数

b:install(exe)
```

### 4.3 Source Spec

```lua
-- 字符串：等价于 { file = "..." }
artifact:add_source("src/main.sp")

-- 结构体
{
    file    = "src/platform.sp",       -- 单个文件或 glob 模式
    cflags  = {"-DLINUX"},             -- 仅此文件的额外编译标志
    include = {"include/linux"},       -- 仅此文件的额外包含路径
    define  = {"VERSION=1.0"},         -- 宏定义
}
```

### 4.4 Custom Step（Phase 2）

```lua
-- 代码生成示例：protobuf 编译
local proto_gen = b:add_custom_step({
    name    = "generate_proto",
    command = {"protoc", "--lua_out=build/proto", "src/proto/*.proto"},
    inputs  = {"src/proto/user.proto", "src/proto/message.proto"},
    outputs = {"build/proto/user_pb.lua", "build/proto/message_pb.lua"},
})

exe:add_source("build/proto/user_pb.lua")
exe:add_source("build/proto/message_pb.lua")
```

### 4.5 多 Target 示例

```lua
local target = b:get_target()

local exe = b:add_executable({ name = "myapp" })
exe:add_source("src/main.sp")

if target:match("linux") then
    exe:add_source({ file = "src/platform_linux.sp", cflags = {"-DLINUX"} })
    exe:link_library("pthread")
elseif target:match("mingw") or target:match("windows") then
    exe:add_source({ file = "src/platform_win.sp", cflags = {"-DWIN32"} })
    exe:link_library("ws2_32")
elseif target:match("apple") or target:match("darwin") then
    exe:add_source({ file = "src/platform_macos.sp", cflags = {"-DMACOS"} })
end

b:install(exe)
```

---

## 5. 编译图（Build Graph）

### 5.1 DAG 结构

`spkg_build.lua` 执行 `Sharp.lua` 后，生成 **有向无环图（DAG）** 形式的编译图：

```lua
build_graph = {
    target   = "x86_64-pc-linux-gnu",
    optimize = "ReleaseFast",
    artifacts = {
        {
            name = "myapp",
            type = "exe",
            target = "x86_64-pc-linux-gnu",
            dependencies = {"mathlib"},          -- 依赖其他 artifact
            compile_tasks = {
                {
                    source = "src/main.sp",
                    output = "build/myapp/main.o",
                    cflags = {"-O2", "-Iinclude", "-Isrc"},
                    depfile = "build/myapp/main.o.d",
                },
            },
            link_step = {
                inputs  = { "build/myapp/main.o", "build/mathlib/libmathlib.a" },
                output  = "build/myapp",
                ldflags = {"-lpthread", "-ldl"},
            },
        },
        {
            name = "mathlib",
            type = "staticlib",
            dependencies = {},                    -- 无依赖
            compile_tasks = { ... },
            link_step = { ... },
        },
    },
}
```

### 5.2 拓扑排序

Sharp.lua 声明完成后，spkg_build.lua 将构建图序列化并传递给 native.c 的 C 引擎。
C 层执行 DAG 拓扑排序（Kahn 算法），确定编译顺序：
1. `mathlib` 无依赖 → 最先编译
2. `myapp` 依赖 `mathlib` → `mathlib` 完成后再链接

### 5.3 产物类型

| type | 链接命令 | 输出路径 |
|------|---------|----------|
| `exe` | `zig cc ... -o <output>` | `build/<name>/<name>`（POSIX） / `build/<name>/<name>.exe`（Windows） |
| `staticlib` | `zig ar rcs <output> ...` | `build/<name>/lib<name>.a`（POSIX） / `build/<name>/<name>.lib`（Windows） |
| `sharedlib` | `zig cc ... -o <output> -shared` | `build/<name>/lib<name>.so`（POSIX） / `build/<name>/<name>.dll`（Windows） |

---

## 6. 增量编译

### 6.1 原理

| 条件 | 动作 |
|------|------|
| `.o` 不存在 | **编译** |
| `.sp` 比 `.o` 新 | **编译** |
| 任何 `#include` 的头文件比 `.o` 新 | **编译**（通过 `.d` 依赖文件） |
| 编译命令（cflags）改变 | **编译**（命令指纹对比） |
| 源文件被删除 | **清理残留 `.o` 和 `.d`** |
| 否则 | **跳过** |

### 6.2 依赖文件生成

编译时自动加 `-MMD -MF <output>.d` 生成 Makefile 风格的 `.d` 依赖文件：

```
build/myapp/main.o: src/main.sp src/include/header.sp
```

### 6.3 命令指纹（Command Fingerprint）

为每个编译任务生成哈希指纹，检测编译参数变化：

```lua
fingerprint = sha256(cflags_str .. include_str .. target .. optimize)
-- 存储在 .o 旁边的 .fingerprint 文件
-- 下次构建时对比指纹，不同则重新编译
```

### 6.4 native.c 相关函数

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.get_mtime(path)` | `number | nil` | 文件修改时间戳（秒级精度） |
| `spkg.current_platform()` | `string` | 当前平台 target triple |
| `spkg.fingerprint(data)` | `string` | SHA-256 哈希（Phase 2） |

---

## 7. 内容寻址编译缓存（Phase 2）

### 7.1 原理

参考 sccache/mozilla 设计，缓存编译结果：

```
缓存键 = hash(source_file + cflags + compiler_version + target)
缓存值 = .o 文件内容
```

### 7.2 缓存流程

```
编译请求 → 计算缓存键 → 查找缓存
   │                         │
   ├─ 命中 ───────────→ 直接使用缓存的 .o
   │
   └─ 未命中 ──→ 编译 ──→ 存储到缓存 ──→ 输出 .o
```

### 7.3 缓存后端

| 后端 | 说明 | 适用场景 |
|------|------|----------|
| `local` | 本地目录 `$HOME/.spkg-cache` | 个人开发 |
| `http` | HTTP 远程缓存服务器 | 团队协作 |

### 7.4 缓存清理

- LRU 策略，超过配额自动清理最久未使用的条目
- `spkg cache --clear` 手动清理
- `spkg cache --stats` 查看缓存命中率

---

## 8. Target Triple 支持

### 8.1 平台矩阵

| 平台 | Target Triple | 状态 |
|------|--------------|------|
| Linux x86_64 | `x86_64-pc-linux-gnu` | ✅ |
| Linux ARM64 | `aarch64-pc-linux-gnu` | ✅ |
| macOS Intel | `x86_64-apple-darwin` | ✅ |
| macOS ARM | `arm64-apple-darwin` | ✅ |
| Windows MSVC | `x86_64-pc-windows-msvc` | ✅ |
| Windows MinGW | `x86_64-w64-mingw32` | ✅ |
| iOS | `arm64-apple-ios` | ✅ |
| iOS Simulator | `arm64-apple-ios-simulator` | ✅ |
| Android ARM64 | `aarch64-linux-android` | ✅ |
| Android x86_64 | `x86_64-linux-android` | ✅ |

### 8.2 命令行

```bash
spkg build --target x86_64-pc-linux-gnu
spkg build --optimize ReleaseFast
spkg build --verbose
spkg build --all                 # 构建所有 targets
spkg build --jobs 8              # 并行编译数
```

---

## 9. 并行编译

### 9.1 Worker Pool 模型

```
┌──────────────────────────────────────────────┐
│              Scheduler                        │
│                                               │
│  编译任务队列: [T1, T2, T3, T4, T5, ...]      │
│         │                                     │
│         ▼                                     │
│  ┌─────┬─────┬─────┬─────┐                   │
│  │ W1  │ W2  │ W3  │ W4  │  (N = --jobs)     │
│  └─────┴─────┴─────┴─────┘                   │
│         │                                     │
│         ▼                                     │
│  任意 Worker 完成 → 立即从队列取下一个任务     │
└──────────────────────────────────────────────┘
```

### 9.2 实现机制

| 平台 | 进程创建 | 非阻塞等待 |
|------|---------|-----------|
| **Windows** | `CreateProcessA` | `WaitForSingleObject(hProcess, 0)` |
| **POSIX** | `fork` + `exec` | `waitpid(pid, &status, WNOHANG)` |

### 9.3 DAG 感知调度

- 拓扑排序确定编译顺序
- 同一层级（无相互依赖）的任务并行执行
- 依赖满足后立即启动，不等待整个层级完成

---

## 10. 安全执行

### 10.1 命令执行安全

**问题**：`/bin/sh -c "cmd"` 或 `cmd.exe /c "cmd"` 存在命令注入风险

**解决方案**（Phase 2）：使用 argv 数组直接执行，不经过 shell

```c
// POSIX: execvp(argv[0], argv)
// Windows: CreateProcessA(NULL, cmd_line, ...)
// 其中 cmd_line 从 argv 数组安全构建
```

### 10.2 Response File（Windows）

Windows 命令行长度限制（~32KB），使用 response file 解决：

```
zig cc @build/myapp/response.rsp
```

response.rsp 内容：
```
-O2 -Iinclude -Isrc -DWIN32 src/main.o src/network.o -o build/myapp.exe
```

---

## 11. CLI 命令

```
spkg init                        创建 Sharp.lua + SharpDeps.lua
spkg build                       构建当前 target
spkg build --target <triple>     构建指定 target
spkg build --optimize <level>    Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
spkg build --verbose             详细输出
spkg build --all                 构建所有 targets
spkg build --jobs <N>            并行编译数
spkg build --no-cache            禁用缓存编译（Phase 2）
spkg run                         构建 + 运行
spkg test                        构建 + 运行测试（Phase 4）
spkg add <name> [version]        添加依赖
spkg remove <name>               移除依赖
spkg list                        列出依赖状态
spkg clean                       清理 build/, spkg_packages/, Sharp.lock
spkg cache --stats               缓存统计（Phase 2）
spkg cache --clear               清理缓存（Phase 2）
spkg help                        帮助
```

---

## 12. 依赖管理

### 12.1 SharpDeps.lua

```lua
-- SharpDeps.lua（类似 build.zig.zon）
return {
    { name = "sharp-lib",    version = "1.0.0" },
    { name = "sharp-logger", version = "*" },
}
```

### 12.2 Sharp.lock

```lua
-- generated by spkg
return {
  ["sharp-lib"] = {
    source  = "https://gitee.com/sharp-libs/sharp-lib.git",
    version = "1.0.0",
    tag     = "v1.0.0",
    commit  = "abc123def"
  }
}
```

### 12.3 传递依赖

`spkg_fetch.fetch_recursive()` 会递归解析每个依赖自身的 `SharpDeps.lua`，用 visited 集合防止循环依赖。

---

## 13. native.c 完整 API

### 13.1 文件系统

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.file_exists(path)` | `bool` | 文件是否存在 |
| `spkg.dir_exists(path)` | `bool` | 目录是否存在 |
| `spkg.mkdir_p(path)` | `bool` | 递归创建目录 |
| `spkg.glob(pattern)` | `{...}` | 文件匹配（支持 `**`） |
| `spkg.read_file(path)` | `string | nil` | 读取文件内容 |
| `spkg.write_file(path, s)` | `bool` | 写入文件内容 |
| `spkg.get_mtime(path)` | `number | nil` | 文件修改时间戳 |
| `spkg.remove(path)` | `bool` | 删除文件或目录（Phase 2） |

### 13.2 进程执行

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.run_cmd(cmd)` | `{ok, out, code}` | 执行 shell 命令（阻塞） |
| `spkg.start_cmd(cmd)` | `task_id | nil, err` | 启动异步任务 |
| `spkg.wait_task(task_id)` | `{ok, out, code} | nil` | 检查任务完成状态（非阻塞） |
| `spkg.execv(argv)` | `{ok, out, code}` | 直接执行，不经过 shell（Phase 2） |

### 13.3 编译缓存（Phase 2）

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.cache_init()` | `bool` | 初始化缓存目录（`$HOME/.spkg-cache`） |
| `spkg.cache_get(key)` | `bool` | 缓存命中 → 复制 .o 到 output_path |
| `spkg.cache_put(key, file_path)` | `bool` | 将编译产物存入缓存 |
| `spkg.cache_stats()` | `table` | `{hit, miss, size, count}` |
| `spkg.cache_clear()` | `bool` | 清空缓存 |

### 13.4 Custom Step（Phase 2）

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.custom_needs_run(inputs, outputs)` | `bool` | 判断是否需要执行自定义步骤 |
| `spkg.custom_exec(command, workdir)` | `{ok, out, code}` | 执行自定义命令（argv 数组直接执行） |

### 13.5 工具查找

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.find_sharpc()` | `string | nil` | 查找 sharpc 编译器 |
| `spkg.find_zigcc()` | `string | nil` | 查找 zig 编译器 |
| `spkg.home_dir()` | `string` | 用户主目录 |
| `spkg.cwd()` | `string` | 当前工作目录 |
| `spkg.current_platform()` | `string` | 当前平台 target triple |
| `spkg.fingerprint(data)` | `string` | SHA-256 哈希（Phase 2） |

### 13.6 编译器查找优先级

**sharpc:**
1. `SHARPC` 环境变量
2. `{bin}/../../../build/sharpc`
3. `{bin}/../../build/sharpc`
4. `{bin}/../build/sharpc`
5. `{bin}/../sharpc`

**zig:**
1. `ZIGCC` 环境变量
2. `{bin}/zig`（POSIX）或 `{bin}/zig.exe`（Windows）
3. `{bin}/../zig/zig`（sharp 发行目录布局）
4. PATH 中的 `zig`

### 13.7 Windows 支持（一等公民）

| 功能 | 实现 |
|------|------|
| 文件操作 | `GetFileAttributesA` / `CreateFileA` |
| 路径处理 | 统一使用 `/`，不做转换 |
| mtime | `FILETIME` → Unix 时间戳转换 |
| mkdir_p | `CreateDirectoryA` 递归创建 |
| glob | `dir /s /b` 命令 |
| home_dir | `USERPROFILE` 环境变量 |
| 进程执行 | `CreateProcessA` + `WaitForSingleObject` |
| 临时文件 | `GetTempPathA` + `GetTempFileNameA` |
| 路径长度 | `MAX_PATH` → `\\?\` 前缀支持长路径（Phase 2） |

---

## 14. 目录结构

```
project/
├── Sharp.lua              # 构建配置（声明式）
├── SharpDeps.lua          # 依赖声明
├── Sharp.lock             # 依赖锁定（自动生成）
├── src/
│   ├── main.sp
│   └── include/
│       └── header.sp
├── build/
│   ├── myapp/
│   │   ├── main.o
│   │   ├── main.o.d        # 头文件依赖（自动生成）
│   │   ├── main.o.fingerprint  # 命令指纹（Phase 2）
│   │   └── myapp           # 最终产物
│   └── mathlib/
│       ├── basic.o
│       ├── basic.o.d
│       └── libmathlib.a
└── spkg_packages/          # 依赖缓存
    └── sharp-lib/

$HOME/.spkg-cache/          # 全局编译缓存（Phase 2）
├── a1b2c3d4e5f6.../
│   ├── output.o
│   └── metadata.json
└── ...
```

---

## 15. Windows 一等公民设计

### 15.1 路径处理

**统一使用 `/` 分隔符，不做平台转换。**

```lua
-- 用户输入（所有平台统一）
exe:add_source("src/main.sp")
exe:add_include("include")

-- spkg 内部也统一使用 /
-- Windows 上 zig cc / 现代 Windows API 完全接受 /c/path/to/file
-- 只有极少数场景（cmd.exe 内置命令）需要 \，届时局部处理
```

这简化了实现，消除了路径转换的 bug 风险，与 zig 的设计哲学一致。

### 15.2 产物命名

| 类型 | POSIX | Windows |
|------|-------|---------|
| 可执行文件 | `myapp` | `myapp.exe` |
| 静态库 | `libmyapp.a` | `myapp.lib` |
| 动态库 | `libmyapp.so` | `myapp.dll` |

### 15.3 并行编译实现

```c
// Windows
STARTUPINFOA si = { sizeof(si) };
PROCESS_INFORMATION pi = { 0 };
si.dwFlags = STARTF_USESHOWWINDOW;
si.wShowWindow = SW_HIDE;
CreateProcessA(NULL, full_cmd, NULL, NULL, FALSE,
               CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
CloseHandle(pi.hThread);
// WaitForSingleObject(pi.hProcess, 0) 非阻塞检查

// POSIX
pid_t pid = fork();
if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd, NULL); }
// waitpid(pid, &status, WNOHANG) 非阻塞检查
```

### 15.4 长路径支持（Phase 2）

Windows 默认 MAX_PATH = 264 字符限制，通过 `\\?\` 前缀支持长路径：

```c
// Windows: \\?\C:\very\long\path\...
// 自动在 mkdir_p/glob 等操作中添加前缀
```

---

## 16. 分布式编译路线图（Phase 3）

### 16.1 架构

```
┌─────────────────────────────────────────────────┐
│               spkg (coordinator)                 │
│                                                  │
│  Sharp.lua → DAG → 拓扑排序 → 任务队列            │
│  │                                                │
│  ├─[task1]─→ node1:10080 ──→ .o (缓存命中?)       │
│  ├─[task2]─→ node2:10080 ──→ .o                  │
│  ├─[task3]─→ node3:10080 ──→ .o                  │
│  │                                                │
│  └── 收集 .o → 本地链接 → 最终产物                  │
└─────────────────────────────────────────────────┘

┌──────────────────────────────────┐
│  spkg-node (编译节点)             │
│                                  │
│  接收 task → 检查缓存 → sharpc   │
│  返回 .o 文件 + 元数据            │
└──────────────────────────────────┘
```

### 16.2 通信协议

**任务请求：**
```json
{
  "id": "task-001",
  "type": "compile",
  "source": "src/main.sp",
  "target": "x86_64-pc-linux-gnu",
  "optimize": "ReleaseFast",
  "cflags": ["-O2", "-Iinclude", "-Isrc"],
  "needs_depfile": true,
  "cache_key": "sha256:abc123..."
}
```

**任务响应：**
```json
{
  "id": "task-001",
  "status": "ok",
  "output": "<base64 encoded .o file>",
  "depfile": "build/myapp/main.o: src/main.sp src/include/header.sp\n",
  "cached": false
}
```

**错误响应：**
```json
{
  "id": "task-001",
  "status": "error",
  "code": 1,
  "stderr": "error: cannot find module 'foo'"
}
```

### 16.3 增量编译在分布式场景下

1. 本地检查 `.sp` 和 `.d` 文件的 mtime + 命令指纹
2. 如果需要编译，计算缓存键
3. 检查本地/远程缓存
4. 缓存未命中 → 发送任务到远程节点
5. 远程节点返回 `.o` 和 `.d` 文件内容
6. 本地写入 `.o` 和 `.d` 文件，更新缓存
7. 下次增量检查时使用本地的 `.d` 文件

### 16.4 spkg-node 设计

- 轻量级 HTTP 服务器
- 端点：`POST /compile` 接收任务
- 执行 `sharpc` 编译
- 返回 `.o` 文件内容
- 可配置：最大并发数、支持的 target 列表
- 本地缓存：`$HOME/.spkg-node-cache`

---

## 17. 未来增强（Phase 4）

### 17.1 彩色诊断输出

```
error: src/main.sp:42:10: expected ';' after expression
    let x = foo()
             ^
             ;
```

### 17.2 测试框架

```lua
-- 在 Sharp.lua 中声明测试
local test_exe = b:add_executable({ name = "myapp_test" })
test_exe:add_source("tests/**/*.sp")
test_exe:link_artifact(myapp)
b:install(test_exe)

-- spkg test 命令自动发现并运行测试
```

