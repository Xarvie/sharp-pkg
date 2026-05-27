# spkg 设计文档

> Sharp Package Manager & Build System
> 日期：2026-05-26
> 状态：Phase 1-4 全部完成，代码审查零警告

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
| **Phase 2** | 内容寻址编译缓存（local）、Custom Step、spkg cache 命令 | ✅ |
| **Phase 3** | 分布式编译（spkg-node + coordinator） | ✅ |
| **Phase 4** | 彩色诊断、测试框架 | ✅ |

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

## 16. 分布式编译（Phase 3）

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

### 16.2 核心原则

- **coordinator 无状态**：本地 spkg 负责 DAG 解析、任务分发、结果收集
- **node 无状态**：spkg-node 只接收编译任务，返回 .o，不保存状态
- **本地优先**：本地有空闲 worker 时本地编译，否则分发到远程
- **纯 C 实现**：spkg-node 是独立 C 程序，无外部依赖（仅用 BSD sockets）
- **HTTP/1.0**：轻量协议，无 TLS（内网使用），无 JSON 依赖（手动解析）

### 16.3 spkg-node CLI

```bash
spkg-node --listen 0.0.0.0:10080 --max-jobs 4 --sharpc /usr/bin/sharpc
```

| 参数 | 说明 |
|------|------|
| `--listen <addr:port>` | 监听地址（默认 `0.0.0.0:10080`） |
| `--max-jobs <N>` | 最大并发编译数（默认 4） |
| `--sharpc <path>` | sharpc 路径（默认从 PATH 查找） |
| `--cache-dir <path>` | 节点本地缓存目录 |

### 16.4 通信协议

**任务请求（POST /compile）：**

```http
POST /compile HTTP/1.0
Content-Type: application/json
Content-Length: 256

{
  "source": "src/main.sp",
  "source_hash": "sha256:abc123...",
  "cflags": ["-O2", "-Iinclude"],
  "target": "x86_64-pc-linux-gnu",
  "optimize": "ReleaseFast",
  "cache_key": "fnv1a:1234567890abcdef"
}
```

> `source` 可以是文件路径（节点本地有源码）或 base64 编码的源码内容。
> 分布式场景下 coordinator 先发送源码，后续同文件用 hash 引用。

**任务响应：**

```http
HTTP/1.0 200 OK
Content-Type: application/json

{
  "status": "ok",
  "output": "<base64 .o content>",
  "depfile": "build/main.o: src/main.sp src/include/header.sp\n",
  "cached": false
}
```

**错误响应：**

```http
HTTP/1.0 500 Internal Server Error
Content-Type: application/json

{
  "status": "error",
  "code": 1,
  "stderr": "error: cannot find module 'foo'"
}
```

**健康检查（GET /health）：**

```http
GET /health HTTP/1.0

HTTP/1.0 200 OK
Content-Type: application/json
{"status": "ok", "jobs": 2, "max_jobs": 4}
```

### 16.5 native.c 新增 API（coordinator 侧）

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.http_post(url, body)` | `{ok, body, code}` | HTTP POST 请求 |
| `spkg.http_get(url)` | `{ok, body, code}` | HTTP GET 请求 |

### 16.6 spkg_build.lua 集成

```lua
-- 分布式编译入口
function M.execute_distributed(nodes, verbose, max_jobs)
    -- 1. 构建 DAG（与本地相同）
    -- 2. 对每个编译任务：
    --    a. 检查本地缓存
    --    b. 缓存未命中 → 选择空闲节点
    --    c. 发送源码 + cflags → 远程编译
    --    d. 接收 .o → 写入本地
    -- 3. 本地链接
end
```

### 16.7 节点注册与发现

| 方式 | 说明 |
|------|------|
| 手动配置 | `spkg_nodes.json` 文件列出节点地址 |
| 环境变量 | `SPKG_NODES=node1:10080,node2:10080` |
| mDNS 发现 | 可选，Phase 3+ |

### 16.8 安全考虑

- **内网使用**：无 TLS，假设运行在可信网络
- **命令隔离**：节点不执行任意命令，只执行 `sharpc` 编译
- **资源限制**：`--max-jobs` 限制并发，防止节点过载
- **超时**：HTTP 请求默认 60s 超时

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

---

## 18. 开发记录与代码审查

### 18.1 Phase 完成时间线

| 日期 | Phase | 内容 |
|------|-------|------|
| 2026-05-26 | Phase 1 | ✅ 声明式构建图、全平台 target、增量编译 |
| 2026-05-26 | Phase 1.5 | ✅ 头文件依赖追踪、Windows 一等公民、并行编译、DAG |
| 2026-05-26 | Phase 2 | ✅ 内容寻址缓存、Custom Step、cache CLI |
| 2026-05-26 | Phase 3 | ✅ 分布式编译（spkg-node + coordinator + HTTP client） |
| 2026-05-26 | Phase 4 | ✅ 彩色诊断输出、测试框架（b:add_test + spkg test） |

### 18.2 代码审查修复记录（2026-05-26）

**审查标准**: 零警告、零妥协、-Wall -Wextra -Wformat-truncation=2 -Wuse-after-free=1

| # | 文件 | 严重性 | 问题描述 | 修复方案 |
|---|------|--------|----------|----------|
| 1 | spkg_init.lua | 高 | `spkg_cmd_run()` 函数缺失，`spkg run` 命令失效 | 重新添加函数，支持 `--dist` 分布式模式 |
| 2 | spkg_init.lua | 低 | `spkg_cmd_run()` 重复定义（两处） | 删除重复定义 |
| 3 | native.c | 高 | `n_find_sharpc` 格式字符串 `%c` 类型不匹配 | 改用 `memcpy` 手动拼接路径，消除 snprintf |
| 4 | native.c | 高 | `n_find_sharpc` snprintf truncation warning | `memcpy` + `need > PATH_MAX` 边界检查 |
| 5 | native.c | 高 | `n_custom_exec` snprintf truncation warning | `full_cmd` 增大到 8192，添加 `snprintf` 返回值检查 |
| 6 | native.c | 高 | `cache_path_for` snprintf truncation warning | 重写为 `memcpy` 手动拼接，返回 NULL on overflow |
| 7 | native.c | 高 | `cache_get/cache_put` cached_file truncation | 新增 `cache_file_path()` helper + 溢出检查 |
| 8 | native.c | 高 | `cache_stats` cmd/stats_file truncation | `cache_file_path()` + `PATH_MAX+64` 精确分配 |
| 9 | native.c | 中 | `cache_clear` cmd truncation | `PATH_MAX+64/32` 精确分配 |
| 10 | native.c | 中 | `n_mkdir_p` cmd truncation | `PATH_MAX+32` 精确分配 |
| 11 | native.c | 中 | `n_remove` cmd truncation | `PATH_MAX+64/32` 精确分配 |
| 12 | native.c | 中 | `build_http_post/get` `%.*s` truncation | 限制 `host.len` 到 255 字节 |
| 13 | native.c | 中 | `color_code_count` 未使用变量 | 删除 |
| 14 | native.c | 高 | `n_start_cmd` Windows GetTempPathA 未检查返回值 | 添加返回值检查，失败时返回错误 |
| 15 | native.c | 中 | `n_start_cmd` Windows GetTempFileNameA 未检查返回值 | 添加返回值检查 |
| 16 | node.c | 高 | `compile_task` 使用 `rand()` 生成临时文件名，可能冲突 | 替换为 `mkstemp`(POSIX) / `GetTempFileNameA`(Windows) |
| 17 | node.c | 高 | `compile_task` Windows GetTempPathA 未检查返回值 | 添加返回值检查 |
| 18 | node.c | 高 | HTTP response snprintf truncation warning | 手动 `memcpy` 构建 JSON，完全避免 snprintf |
| 19 | node.c | 高 | `odata` use-after-free | 添加 `odata = NULL` 置空 |

**验证命令**:
```bash
cmake -B build -DCMAKE_C_FLAGS="-Wall -Wextra -Wformat-truncation=2 -Wuse-after-free=1"
cmake --build build 2>&1 | grep -c "warning:"
# → 0（零警告）
```

### 18.3 关键设计决策

1. **路径拼接零 snprintf**: 所有涉及 PATH_MAX 的路径拼接统一使用 `memcpy` 手动实现，完全消除编译器 truncation 警告
2. **临时文件安全**: POSIX 使用 `mkstemp()`，Windows 使用 `GetTempFileNameA()`，杜绝 `rand()` 竞态条件
3. **动态响应构建**: HTTP 大响应（base64 编码的 .o 文件）使用精确 `malloc` + `memcpy`，避免固定大小缓冲区
4. **snprintf 返回值检查**: 所有必要的 snprintf 调用都检查返回值，截断时返回错误而非静默失败
5. **sharpc 集成**: sharpc 二进制已集成到 `sharpc/bin/sharpc`，spkg 可通过相对路径自动定位，也可设 `SHARPC` 环境变量

### 18.4 第二轮审查修复记录（2026-05-26）

**审查重点**: 内存安全、use-after-free、资源泄漏、错误处理完整性、第三方库使用安全

| # | 文件:行 | 严重性 | 问题描述 | 修复方案 |
|---|---------|--------|----------|----------|
| 1 | native.c:181/197 | 严重 | `n_run_cmd` use-after-free: realloc 失败后 `out` 已 free 但 `lua_pushstring(L, out)` 直接使用 | `out = NULL` + `lua_pushstring(L, out ? out : "")` |
| 2 | native.c:691/706 | 严重 | `n_wait_task` Windows use-after-free | 同上 |
| 3 | native.c:807/822 | 严重 | `n_wait_task` POSIX use-after-free | 同上 |
| 4 | native.c:1524/1540 | 严重 | `n_custom_exec` use-after-free | 同上 |
| 5 | node.c:227 | 严重 | malloc 失败时未发送 HTTP 错误响应，客户端永久挂起 | 发送 500 错误响应 |
| 6 | node.c:260 | 严重 | resp malloc 失败时未发送 HTTP 错误响应 | 发送 500 错误响应 + 修复 b64 泄漏 |
| 7 | node.c:232-233 | 严重 | Base64 固定 131KB 缓冲区，大文件编码静默失败 | 改为动态分配 `((osize+2)/3)*4+1` + 检查返回值 |
| 8 | native.c:378 | 中等 | `n_read_file` fread 返回值未检查 | 检查 nread == sz，否则返回 nil |
| 9 | native.c:393 | 中等 | `n_write_file` fwrite 返回值未检查，磁盘满时不报错 | 检查 nwrote == len |
| 10 | native.c:1217 | 中等 | `copy_file` fwrite 未检查，缓存可能损坏 | 检查每次 fwrite 返回值，失败时删除部分文件 |
| 11 | node.c:332 | 低 | `--max-jobs` atoi 未校验，传入 0 导致 DoS | 限制范围 1-64 |
| 12 | native.c:1317 | 低 | `fscanf` 未检查返回值 | 检查 fscanf == 2，失败时重置 hit/miss |
| 13 | native.c:1104-1108 | 中等 | `n_colorize` 固定 4096 缓冲区可能截断 | 改为动态分配 `malloc(text_len + code_len + reset_len + 1)` |
| 14 | native.c:140,152-153 | 中等 | `find_zig_near_exe` 使用 strcpy/strcat | 改用 memcpy + 编译时已知长度 |

### 18.5 sharpc 集成修复记录（2026-05-27）

**背景**: sharpc 二进制已集成到 `sharp-pkg/sharpc/bin/sharpc`，行为与 gcc 一致（默认 compile+link，`-c` 只编译不链接）

| # | 文件 | 问题描述 | 修复方案 |
|---|------|----------|----------|
| 1 | spkg_init.lua | `ok_msg()` 等辅助函数定义在 `spkg_main()` 内部，`spkg_cmd_init()` 无法访问 | 移到模块级别（文件顶部） |
| 2 | native.c | `n_find_sharpc` 搜索路径不包含 `sharpc/bin/sharpc` | 添加 `../../sharpc/bin/sharpc`（最高优先级） |
| 3 | spkg_build.lua | `compile_task_cmd` 缺少 `-c` 参数，sharpc 默认 compile+link 产生可执行文件而非 .o | 添加 `-c` 参数（gcc 兼容行为） |
| 4 | spkg_build.lua | `gsub("%.o$", "%.d")` 替换字符串中 `%` 是 Lua 模式字符 | 改为 `gsub("%.o$", ".d")` |
| 5 | spkg_build.lua | 指纹不包含构建系统版本，`-c` 参数添加后旧指纹仍然匹配 | 添加 `BUILD_SYSTEM_VERSION = "spkg-v4"` 到指纹前缀 |

