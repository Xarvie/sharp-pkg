# spkg 设计文档

> 日期：2026-05-26
> 状态：Phase 1 + 1.5 已实现

## 1. 概述

spkg 是 Sharp 语言的包管理器和构建工具。

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 声明式构建图、全平台 target、增量编译、cflags/ldflags | ✅ |
| Phase 1.5 | 头文件依赖追踪、Windows 支持、错误提示改进 | ✅ |
| Phase 2 | 分布式编译 | 规划中 |

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
| **声明层** | `Sharp.lua` | 纯函数，声明工件和源文件，无副作用 |
| **执行层** | `spkg_build.lua` | 接收声明，生成编译图，执行编译任务 |
| **系统层** | `native.c` | 文件操作、进程执行、平台检测等系统级 API |

---

## 3. Sharp.lua 规范

### 3.1 注入的构建上下文 `b`

```lua
-- b 的方法
b:get_target()           → string          -- 当前 target triple
b:get_optimize()         → string          -- 优化级别：Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
b:get_verbose()          → bool            -- 是否详细输出

b:add_executable(opts)   → Artifact         -- 声明可执行文件
b:add_static_library(opts) → Artifact       -- 声明静态库
b:add_shared_library(opts) → Artifact       -- 声明动态库

b:install(artifact)       → void            -- 标记为安装目标
b:dependency(name)        → Artifact | nil  -- 获取已声明的工件
```

### 3.2 Artifact 对象

```lua
local exe = b:add_executable({ name = "myapp" })

exe:add_source("src/main.sp")                  -- 简写
exe:add_source({ file = "src/lib.sp" })        -- 结构体
exe:add_source({
    file = "src/features/**/*.sp",             -- glob 模式
    cflags = {"-DFEATURE_X"},
    include = {"src/features/include"},
})

exe:add_include("include")
exe:add_include("src")
exe:add_cflags("-Wall", "-Wextra")
exe:add_ldflags("-pthread")
exe:link_library("pthread")
exe:link_library("dl")
exe:link_artifact(mathlib)                     -- 链接另一个 Artifact
b:install(exe)
```

### 3.3 Source Spec

```lua
-- 字符串：等价于 { file = "..." }
artifact:add_source("src/main.sp")

-- 结构体
{
    file = "src/platform.sp",     -- 单个文件或 glob 模式
    cflags = {"-DLINUX"},         -- 仅此文件的额外编译标志
    include = {"include/linux"},  -- 仅此文件的额外包含路径
}
```

### 3.4 多 Target 示例

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

## 4. 编译图（Build Graph）

### 4.1 生成过程

`spkg_build.lua` 执行 `Sharp.lua` 后，收集所有 `b:install()` 标记的 Artifact，生成纯数据的编译图：

```lua
build_graph = {
    target   = "x86_64-pc-linux-gnu",
    optimize = "ReleaseFast",
    artifacts = {
        {
            name          = "myapp",
            type          = "exe",
            target        = "x86_64-pc-linux-gnu",
            compile_tasks = {
                {
                    source = "src/main.sp",
                    output = "build/myapp/main.o",
                    cflags = {"-O2", "-Iinclude", "-Isrc"},
                },
                -- ...
            },
            link_step = {
                inputs  = { "build/myapp/main.o", "build/myapp/network.o", "build/mathlib/libmathlib.a" },
                output  = "build/myapp",
                ldflags = {"-lpthread", "-ldl"},
            },
        },
    },
}
```

### 4.2 产物类型

| type | 链接命令 | 输出路径 |
|------|---------|----------|
| `exe` | `zig cc ... -o <output>` | `build/<name>/<name>` |
| `staticlib` | `zig ar rcs <output> ...` | `build/<name>/lib<name>.a` |
| `sharedlib` | `zig cc ... -o <output> -shared` | `build/<name>/lib<name>.so` |

---

## 5. 增量编译

### 5.1 原理

- `.o` 不存在 → **编译**
- `.sp` 比 `.o` 新 → **编译**
- 任何 `#include` 的头文件比 `.o` 新 → **编译**（通过 `.d` 依赖文件）
- 源文件被删除 → **清理残留 `.o` 和 `.d`**
- 否则 → **跳过**

### 5.2 依赖文件生成

编译时自动加 `-MMD -MF <output>.d` 生成 Makefile 风格的 `.d` 依赖文件：

```
build/myapp/main.o: src/main.sp src/include/header.sp
```

### 5.3 native.c 相关函数

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.get_mtime(path)` | `number \| nil` | 文件修改时间戳（纳秒精度） |
| `spkg.current_platform()` | `string` | 当前平台 target triple |

---

## 6. Target Triple 支持

| 平台 | Target Triple |
|------|--------------|
| Linux x86_64 | `x86_64-pc-linux-gnu` |
| Linux ARM64 | `aarch64-pc-linux-gnu` |
| macOS Intel | `x86_64-apple-darwin` |
| macOS ARM | `arm64-apple-darwin` |
| Windows MSVC | `x86_64-pc-windows-msvc` |
| Windows MinGW | `x86_64-w64-mingw32` |
| iOS | `arm64-apple-ios` |
| iOS Simulator | `arm64-apple-ios-simulator` |
| Android ARM64 | `aarch64-linux-android` |
| Android x86_64 | `x86_64-linux-android` |

### 6.1 命令行

```bash
spkg build --target x86_64-pc-linux-gnu
spkg build --optimize ReleaseFast
spkg build --verbose
spkg build --all                 # 构建所有 targets（Phase 1 占位）
```

---

## 7. CLI 命令

```
spkg init                        创建 Sharp.lua + SharpDeps.lua
spkg build                       构建当前 target
spkg build --target <triple>     构建指定 target
spkg build --optimize <level>    Debug | ReleaseSafe | ReleaseFast | ReleaseSmall
spkg build --verbose             详细输出
spkg build --all                 构建所有 targets
spkg run                         构建 + 运行
spkg add <name> [version]        添加依赖
spkg remove <name>               移除依赖
spkg list                        列出依赖状态
spkg clean                       清理 build/, spkg_packages/, Sharp.lock
spkg help                        帮助
```

---

## 8. 依赖管理

### 8.1 SharpDeps.lua

```lua
-- SharpDeps.lua（类似 build.zig.zon）
return {
    { name = "sharp-lib",    version = "1.0.0" },
    { name = "sharp-logger", version = "*" },
}
```

### 8.2 Sharp.lock

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

### 8.3 传递依赖

`spkg_fetch.fetch_recursive()` 会递归解析每个依赖自身的 `SharpDeps.lua`，用 visited 集合防止循环依赖。

---

## 9. native.c 完整 API

| 函数 | 返回 | 说明 |
|------|------|------|
| `spkg.run_cmd(cmd)` | `{ok, out, code}` | 执行 shell 命令 |
| `spkg.file_exists(path)` | `bool` | 文件是否存在 |
| `spkg.dir_exists(path)` | `bool` | 目录是否存在 |
| `spkg.mkdir_p(path)` | `bool` | 递归创建目录 |
| `spkg.glob(pattern)` | `{...}` | 文件匹配（支持 `**`） |
| `spkg.read_file(path)` | `string \| nil` | 读取文件内容 |
| `spkg.write_file(path, s)` | `bool` | 写入文件内容 |
| `spkg.find_sharpc()` | `string \| nil` | 查找 sharpc 编译器 |
| `spkg.find_zigcc()` | `string \| nil` | 查找 zig |
| `spkg.home_dir()` | `string` | 用户主目录 |
| `spkg.cwd()` | `string` | 当前工作目录 |
| `spkg.get_mtime(path)` | `number \| nil` | 文件修改时间戳 |
| `spkg.current_platform()` | `string` | 当前平台 target triple |

### 9.1 编译器查找优先级

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

### 9.2 Windows 支持

- 文件操作：`GetFileAttributesA`
- 路径分隔符：自动处理 `/` 和 `\`
- `mtime`：`FILETIME` → Unix 时间戳转换
- `mkdir_p`：`CreateDirectoryA` 递归创建
- `glob`：`dir /s /b` 命令
- `home_dir`：`USERPROFILE` 环境变量

---

## 10. 目录结构

```
project/
├── Sharp.lua              # 构建配置（声明式）
├── SharpDeps.lua          # 依赖声明
├── Sharp.lock             # 依赖锁定
├── src/
│   ├── main.sp
│   └── include/
│       └── header.sp
├── build/
│   ├── myapp/
│   │   ├── main.o
│   │   ├── main.d          # 头文件依赖（自动生成）
│   │   └── myapp           # 最终产物
│   └── mathlib/
│       ├── basic.o
│       ├── basic.d
│       └── libmathlib.a
└── spkg_packages/          # 依赖缓存
    └── sharp-lib/
```

---

## 11. 分布式编译路线图（Phase 2）

### 11.1 架构

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

### 11.2 通信协议（草案）

**任务请求：**

```json
{
  "id": "task-001",
  "type": "compile",
  "source": "src/main.sp",
  "output": "build/myapp/main.o",
  "target": "x86_64-pc-linux-gnu",
  "optimize": "ReleaseFast",
  "cflags": ["-O2", "-Iinclude", "-Isrc"],
  "needs_depfile": true
}
```

**任务响应：**

```json
{
  "id": "task-001",
  "status": "ok",
  "output": "<base64 encoded .o file>",
  "depfile": "build/myapp/main.o: src/main.sp src/include/header.sp\n"
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

### 11.3 序列化格式（Lua 表）

编译任务本身是纯数据，可以直接序列化：

```lua
{
    id        = "task-001",
    type      = "compile",
    source    = "src/main.sp",
    output    = "build/myapp/main.o",
    target    = "x86_64-pc-linux-gnu",
    optimize  = "ReleaseFast",
    cflags    = {"-O2", "-Iinclude", "-Isrc"},
    depfile   = "build/myapp/main.d",
}
```

### 11.4 增量编译在分布式场景下的处理

分布式编译需要本地维护 `.d` 依赖文件（记录头文件依赖关系），用于判断是否需要重新编译：

1. 本地检查 `.sp` 和 `.d` 文件的 mtime
2. 如果需要编译，发送任务到远程节点
3. 远程节点返回 `.o` 和 `.d` 文件内容
4. 本地写入 `.o` 和 `.d` 文件
5. 下次增量检查时使用本地的 `.d` 文件

### 11.5 spkg-node 设计（未来）

- 简单的 HTTP 服务器
- 端点：`POST /compile` 接收任务
- 执行 `sharpc` 编译
- 返回 `.o` 文件内容
- 可配置：最大并发数、支持的 target 列表
