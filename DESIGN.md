# spkg 重构设计文档

> 日期：2026-05-26
> 状态：Phase 1 已实现

## 1. 概述

spkg 是 Sharp 语言的包管理器和构建工具。本次重构的目标是：

1. **声明式构建图** — `Sharp.lua` 只声明"构建什么"，不执行任何编译命令（无副作用）
2. **全平台支持** — 通过 LLVM target triple 支持 macOS、iOS、Windows、Linux、Android
3. **分布式编译预留** — 编译图可序列化，为未来 distcc 式分布式编译打基础
4. **增量编译** — 检测 `.sp` 文件修改时间，只编译变更的源文件
5. **类 Makefile 的 cflags/ldflags** — 灵活表达任意编译器和链接器参数

---

## 2. 架构

```
┌─────────────────────────────────────────────┐
│              spkg (C + Lua 5.4)              │
│                                              │
│  main.c          解析 CLI 参数，注入 b 上下文 │
│  native.c        系统级 C 函数绑定            │
│  spkg_init.lua   CLI 命令分发                │
│  spkg_build.lua  执行编译图（本地 or 分布式）  │
│  spkg_fetch.lua  依赖拉取                    │
│  spkg_lock.lua   Sharp.lock 管理             │
│  spkg_resolve.lua 依赖解析                   │
└─────────────────────┬───────────────────────┘
                      │ 加载
          ┌───────────┴────────────┐
          │    Sharp.lua (用户)     │
          │  b:add_executable{...} │
          │  b:install(artifact)    │
          └────────────────────────┘
```

### 核心分离

| 层级 | 文件 | 职责 |
|------|------|------|
| **声明层** | `Sharp.lua` | 纯函数，声明工件（artifact）和源文件，无副作用 |
| **执行层** | `spkg_build.lua` | 接收声明，生成编译图，执行编译任务 |
| **系统层** | `native.c` | 文件操作、进程执行、平台检测等系统级 API |

---

## 3. Sharp.lua 规范

### 3.1 注入的构建上下文 `b`

`spkg` 在加载 `Sharp.lua` 之前，会注入一个全局对象 `b`（Build Context），提供声明式 API：

```lua
-- b 的方法
b:get_target()           → string          -- 当前 target triple，如 "x86_64-pc-linux-gnu"
b:get_optimize()         → string          -- 优化级别：Debug, ReleaseSafe, ReleaseFast, ReleaseSmall
b:get_verbose()          → bool            -- 是否详细输出

b:add_executable(opts)   → Artifact         -- 声明可执行文件
b:add_static_library(opts) → Artifact       -- 声明静态库
b:add_shared_library(opts) → Artifact       -- 声明动态库

b:install(artifact)       → void            -- 标记为安装目标
b:dependency(name)        → Artifact        -- 获取已声明的工件（用于依赖链接）
```

### 3.2 Artifact 对象

```lua
-- 创建工件
local exe = b:add_executable({
    name = "myapp",
})

-- Artifact 的方法
artifact:add_source(source_spec)     -- 添加源文件
artifact:add_include(dir)            -- 添加头文件/模块搜索路径 (-I)
artifact:add_cflags(flag, ...)       -- 添加额外编译标志
artifact:add_ldflags(flag, ...)      -- 添加额外链接标志
artifact:link_library(lib)           -- 链接库 (-l)
artifact:link_artifact(other)        -- 链接另一个 Artifact
```

### 3.3 Source Spec

```lua
-- 简写：单个文件
artifact:add_source("src/main.sp")

-- 结构体：单个文件 + 自定义标志
artifact:add_source({
    file = "src/platform.sp",
    cflags = {"-DLINUX"},
    include = {"include/linux"},
})

-- 结构体：glob 模式 + 自定义标志
artifact:add_source({
    file = "src/features/**/*.sp",
    cflags = {"-DFEATURE_X"},
    include = {"src/features/include"},
})
```

### 3.4 完整示例

```lua
-- Sharp.lua
local target   = b:get_target()
local optimize = b:get_optimize()

-- ── 可执行文件 ──
local exe = b:add_executable({
    name = "myapp",
})
exe:add_source("src/main.sp")
exe:add_source({
    file = "src/network.sp",
    cflags = {"-DWITH_NETWORK"},
})
exe:add_source({
    file = "src/platform.sp",
    cflags = {"-DLINUX"},
    include = {"include/linux"},
})
exe:add_include("include")
exe:add_include("src")
exe:link_library("pthread")
exe:link_library("dl")
b:install(exe)

-- ── 静态库 ──
local mathlib = b:add_static_library({
    name = "mathlib",
})
mathlib:add_source("src/math/basic.sp")
mathlib:add_source({
    file = "src/math/advanced.sp",
    cflags = {"-DOPTIMIZE"},
})
b:install(mathlib)

-- ── 可执行文件链接静态库 ──
exe:link_artifact(mathlib)
```

### 3.5 多 Target 示例

```lua
-- Sharp.lua 通过命令行 --target 指定当前构建的 target
-- spkg build --target x86_64-w64-mingw32

local target = b:get_target()

local exe = b:add_executable({ name = "myapp" })
exe:add_source("src/main.sp")

-- 根据 target 添加平台特定源文件
if target:match("linux") then
    exe:add_source({
        file = "src/platform_linux.sp",
        cflags = {"-DLINUX"},
    })
    exe:link_library("pthread")
elseif target:match("mingw") or target:match("windows") then
    exe:add_source({
        file = "src/platform_win.sp",
        cflags = {"-DWIN32"},
    })
    exe:link_library("ws2_32")
elseif target:match("apple") or target:match("darwin") then
    exe:add_source({
        file = "src/platform_macos.sp",
        cflags = {"-DMACOS"},
    })
end

b:install(exe)
```

---

## 4. 编译图（Build Graph）

### 4.1 生成过程

`spkg_build.lua` 执行 `Sharp.lua` 后，收集所有 `b:install()` 标记的 Artifact，生成编译图：

```lua
build_graph = {
    artifacts = {
        {
            name = "myapp",
            type = "exe",            -- exe | staticlib | sharedlib
            target = "x86_64-pc-linux-gnu",
            optimize = "ReleaseFast",
            compile_tasks = {        -- 编译任务列表
                {
                    source = "src/main.sp",
                    output = "build/myapp/main.o",
                    cflags = {"-O2", "-Iinclude", "-Isrc"},
                },
                {
                    source = "src/network.sp",
                    output = "build/myapp/network.o",
                    cflags = {"-O2", "-DWITH_NETWORK", "-Iinclude", "-Isrc"},
                },
                {
                    source = "src/platform.sp",
                    output = "build/myapp/platform.o",
                    cflags = {"-O2", "-DLINUX", "-Iinclude", "-Isrc", "-Iinclude/linux"},
                },
            },
            link_step = {
                inputs = {
                    "build/myapp/main.o",
                    "build/myapp/network.o",
                    "build/myapp/platform.o",
                    "build/mathlib/libmathlib.a",  -- 来自 link_artifact
                },
                output = "build/myapp",
                ldflags = {"-lpthread", "-ldl"},
            },
        },
        {
            name = "mathlib",
            type = "staticlib",
            compile_tasks = { ... },
            link_step = {
                inputs = { "build/mathlib/basic.o", "build/mathlib/advanced.o" },
                output = "build/mathlib/libmathlib.a",
                command = "ar",  -- staticlib 用 ar 打包
            },
        },
    }
}
```

### 4.2 编译任务的可序列化性

每个 `compile_tasks[]` 条目是纯数据，可直接序列化（JSON / Lua 表）发送到远程编译节点：

```json
{
  "source": "src/main.sp",
  "output": "build/myapp/main.o",
  "target": "x86_64-pc-linux-gnu",
  "optimize": "ReleaseFast",
  "cflags": ["-O2", "-Iinclude"],
  "needs": []
}
```

这是分布式编译的核心基础：未来只需替换"任务执行器"，把本地 `sharpc` 调用改为通过网络发送任务到远程节点即可。

---

## 5. 增量编译

### 5.1 原理

对每个编译任务，比较源文件和输出文件的修改时间：

- 输出文件 `.o` 不存在 → **必须编译**
- 源文件 `.sp` 比 `.o` 新 → **必须编译**
- 任何被 `#include` 或 `import` 的文件比 `.o` 新 → **必须编译**（未来实现依赖追踪）
- 否则 → **跳过**

### 5.2 native.c 新增函数

```c
// spkg.get_mtime(path) → number | nil
// 返回文件最后修改时间（Unix 时间戳），文件不存在返回 nil
static int n_get_mtime(lua_State *L);

// spkg.current_platform() → string
// 返回当前宿主平台的 target triple
// 如 "x86_64-pc-linux-gnu", "arm64-apple-darwin", "x86_64-w64-mingw32"
static int n_current_platform(lua_State *L);
```

### 5.3 Lua 层实现

```lua
-- spkg_build.lua
function M.needs_compile(source, output)
    local src_mtime = spkg.get_mtime(source)
    if not src_mtime then return false end

    local out_mtime = spkg.get_mtime(output)
    if not out_mtime then return true end  -- 输出不存在

    return src_mtime > out_mtime
end
```

---

## 6. Target Triple 支持

### 6.1 常见 Target

| 平台 | Target Triple | 说明 |
|------|--------------|------|
| Linux x86_64 | `x86_64-pc-linux-gnu` | |
| Linux ARM64 | `aarch64-pc-linux-gnu` | |
| macOS Intel | `x86_64-apple-darwin` | |
| macOS ARM | `arm64-apple-darwin` | |
| Windows | `x86_64-pc-windows-msvc` | MSVC ABI |
| Windows MinGW | `x86_64-w64-mingw32` | GNU ABI |
| iOS | `arm64-apple-ios` | |
| iOS Simulator | `arm64-apple-ios-simulator` | |
| Android ARM64 | `aarch64-linux-android` | |
| Android x86_64 | `x86_64-linux-android` | |

### 6.2 命令行指定

```bash
spkg build --target x86_64-pc-linux-gnu
spkg build --target x86_64-w64-mingw32
spkg build --target arm64-apple-darwin
```

### 6.3 默认 Target

未指定 `--target` 时，使用 `spkg.current_platform()` 返回的当前平台 target triple。

---

## 7. CLI 命令

```
spkg init                        创建 Sharp.lua
spkg build                       构建当前平台 target
spkg build --target <triple>     构建指定 target
spkg build --all                 构建所有配置的 targets
spkg build --optimize ReleaseFast
spkg run                         构建并运行
spkg run --target <triple>       构建指定 target 并运行（需要兼容层）
spkg clean                       清理 build/ 目录
spkg add <name>                  添加依赖
spkg help                        帮助
```

---

## 8. native.c 变更清单

### 8.1 新增函数

| 函数 | 说明 |
|------|------|
| `spkg.get_mtime(path)` | 返回文件修改时间戳，不存在返回 nil |
| `spkg.current_platform()` | 返回当前平台的 target triple |

### 8.2 `spkg.find_sharpc()` 跨平台增强

当前只搜索 Linux 路径，需要扩展：

```c
// macOS
"../build/sharpc"
"../sharpc"

// Windows (通过 zig cc)
"sharpc.exe"

// 环境变量 SHARPC 优先级最高（所有平台）
```

### 8.3 `spkg.find_zigcc()` 跨平台增强

```c
// macOS ARM 需要检测 zig 架构支持
// Windows 需要检测 zig cc 是否可用
```

### 8.4 完整 spkg 模块 API 列表

| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `spkg.run_cmd(cmd)` | `{ok, out, code}` | 执行 shell 命令 |
| `spkg.file_exists(path)` | `bool` | 文件是否存在 |
| `spkg.dir_exists(path)` | `bool` | 目录是否存在 |
| `spkg.mkdir_p(path)` | `bool` | 递归创建目录 |
| `spkg.glob(pattern)` | `{...}` | 文件匹配（支持 `**`） |
| `spkg.read_file(path)` | `string \| nil` | 读取文件内容 |
| `spkg.write_file(path, s)` | `bool` | 写入文件内容 |
| `spkg.find_sharpc()` | `string \| nil` | 查找 sharpc 编译器 |
| `spkg.find_zigcc()` | `string \| nil` | 查找 zig cc |
| `spkg.home_dir()` | `string` | 用户主目录 |
| `spkg.cwd()` | `string` | 当前工作目录 |
| `spkg.get_mtime(path)` | `number \| nil` | 文件修改时间戳 |
| `spkg.current_platform()` | `string` | 当前平台 target triple |

---

## 9. 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `native.c` | 修改 | 新增 `get_mtime`、`current_platform`；跨平台适配 |
| `spkg.h` | 修改 | 更新注释文档 |
| `main.c` | 修改 | 解析 `--target`、`--optimize`、`--verbose` 参数；注入 `b` 上下文 |
| `spkg_init.lua` | 修改 | 适配新 CLI 参数；更新 `init` 生成的 Sharp.lua 模板 |
| `spkg_build.lua` | 重写 | 实现 Build 上下文、Artifact 对象、编译图生成、增量编译、执行器 |
| `spkg_fetch.lua` | 修改 | 适配新格式 |
| `spkg_lock.lua` | 保持 | 无需变更 |
| `spkg_resolve.lua` | 保持 | 无需变更 |
| `CMakeLists.txt` | 可能修改 | 如果新增脚本文件需更新 |

---

## 10. 分布式编译路线图（Phase 2）

当前阶段（Phase 1）只实现本地编译，但编译图的序列化能力为 Phase 2 打基础。

### 10.1 Phase 2 架构

```
┌─────────────────────────────────────┐
│           spkg (coordinator)         │
│                                      │
│  Sharp.lua → Build Graph → Tasks     │
│  │                                   │
│  ├─[task1]─→ node1:10080 ──→ .o     │
│  ├─[task2]─→ node2:10080 ──→ .o     │
│  ├─[task3]─→ node3:10080 ──→ .o     │
│  │                                   │
│  └── 收集 .o → 本地链接 → 最终产物    │
└─────────────────────────────────────┘

┌──────────────────────────┐
│  spkg-node (编译节点)     │
│                          │
│  接收 task → sharpc → .o │
│  返回 .o 文件             │
└──────────────────────────┘
```

### 10.2 序列化格式

```json
{
  "compile_tasks": [
    {
      "source_content": "<base64 encoded .sp file>",
      "source_name": "src/main.sp",
      "output_name": "build/main.o",
      "cflags": ["-O2", "-Iinclude"],
      "target": "x86_64-pc-linux-gnu"
    }
  ],
  "compiler": "sharpc"
}
```

---

## 11. 目录结构（产物）

```
project/
├── Sharp.lua              # 构建配置（声明式，类似 build.zig）
├── SharpDeps.lua          # 依赖声明（类似 build.zig.zon）
├── Sharp.lock             # 依赖锁定
├── src/
│   ├── main.sp
│   └── ...
├── build/
│   ├── myapp/             # 按工件名分目录
│   │   ├── main.o
│   │   ├── network.o
│   │   ├── platform.o
│   │   └── myapp          # 最终产物
│   └── mathlib/
│       ├── basic.o
│       ├── advanced.o
│       └── libmathlib.a
└── spkg_packages/         # 依赖缓存
    └── ...
```

---

## 12. 依赖管理

### SharpDeps.lua

```lua
-- SharpDeps.lua
return {
    { name = "sharp-lib",    version = "1.0.0" },
    { name = "sharp-logger", version = "*" },
}
```

### 命令

| 命令 | 说明 |
|------|------|
| `spkg add <name> [version]` | 添加依赖到 SharpDeps.lua |
| `spkg remove <name>` | 移除依赖 |
| `spkg list` | 列出依赖状态 |

### Sharp.lock

```lua
-- generated by spkg
return {
  ["sharp-lib"] = {
    source = "https://gitee.com/sharp-libs/sharp-lib.git",
    version = "1.0.0",
    tag = "v1.0.0",
    commit = "abc123def"
  }
}
```


