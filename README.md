# sharp-pkg

**Sharp Package Manager & Build System** — Sharp 语言的一等构建工具与包管理器。

核心哲学：声明优先、确定性构建、零浪费编译、Windows 一等公民、分布式就绪。

- **C 核心，Lua 声明** — Lua 仅用于用户声明构建图（类似 Zig 的 `build.zig`），所有执行逻辑在 C 层
- **增量编译** — 基于 mtime + depfile + 命令指纹的三重检测，只编译必要的内容
- **内容寻址缓存** — 相同输入产生相同输出，跨项目复用编译产物
- **分布式编译** — 原生支持多节点分布式构建，自动打包头文件依赖
- **跨平台** — Linux / macOS / Windows 原生支持，10 种 target triple

---

## 目录

- [安装与构建](#安装与构建)
- [快速开始](#快速开始)
- [CLI 命令参考](#cli-命令参考)
- [config.spkg 构建配置](#configspkg-构建配置)
- [构建上下文 b API](#构建上下文-b-api)
- [Artifact API](#artifact-api)
- [Source Spec](#source-spec)
- [Custom Step 自定义步骤](#custom-step-自定义步骤)
- [测试框架](#测试框架)
- [依赖管理](#依赖管理)
- [增量编译](#增量编译)
- [编译缓存](#编译缓存)
- [分布式编译](#分布式编译)
- [Target Triple 支持](#target-triple-支持)
- [项目目录结构](#项目目录结构)
- [编译器查找](#编译器查找)
- [环境变量](#环境变量)
- [spkg-node 编译节点](#spkg-node-编译节点)
- [构建选项](#构建选项)
- [依赖可见性](#依赖可见性)
- [许可证](#许可证)

---

## 安装与构建

### 前置依赖

- CMake ≥ 3.14
- C 编译器（GCC / Clang / MSVC）
- Lua 5.4（已内嵌，无需单独安装）
- libgit2（预编译到 `libgit2/build/libgit2.a`）

### 构建 spkg

```bash
# 1. 构建 libgit2（首次）
cd sharp-pkg/libgit2
cmake -B build -DBUILD_CLI=OFF
cmake --build build

# 2. 构建 spkg
cd sharp-pkg/spkg
cmake -B build
cmake --build build

# 构建产物：
#   sharp-pkg/spkg/build/spkg        — 主程序
#   sharp-pkg/spkg/build/spkg-node   — 分布式编译节点
```

### 安装

```bash
cd sharp-pkg/spkg/build
cmake --install . --prefix /usr/local
# 安装 spkg 和 spkg-node 到 /usr/local/bin
```

---

## 快速开始

```bash
# 1. 初始化项目
spkg init

# 2. 编写源码
mkdir -p src
echo 'fn main() { print("Hello, Sharp!") }' > src/main.ce

# 3. 构建
spkg build

# 4. 运行
spkg run

# 5. 添加依赖
spkg add sharp-lib 1.0.0

# 6. 构建 + 运行测试
spkg test
```

---

## CLI 命令参考

```
spkg init                       创建 config.spkg + SharpDeps.lua
spkg build                      构建当前 target
spkg build --target <triple>    交叉编译到指定 target
spkg build --optimize <level>   设置优化级别
spkg build --verbose            详细输出模式
spkg build --all                构建所有 targets
spkg build --jobs <N>           并行编译数（默认 1）
spkg build --no-cache           禁用编译缓存
spkg build --dist               启用分布式编译
spkg run                        构建 + 运行第一个可执行文件
spkg test                       构建 + 运行所有测试
spkg add <name> [version]       添加依赖到 SharpDeps.lua
spkg remove <name>              从 SharpDeps.lua 移除依赖
spkg update                     更新依赖到最新版本
spkg list                       列出依赖及其状态
spkg info                       显示项目信息和依赖树
spkg clean                      清理 build/、spkg_packages/、Sharp.lock
spkg cache --stats              查看编译缓存统计
spkg cache --clear              清空编译缓存
spkg help                       显示帮助信息
```

### 优化级别

| 级别 | 编译标志 | 说明 |
|------|---------|------|
| `Debug` | `-O0` | 默认，无优化，保留调试信息 |
| `ReleaseSafe` | `-O1` | 优化 + 安全检查 |
| `ReleaseFast` | `-O2` | 最大优化速度 |
| `ReleaseSmall` | `-Os` | 优化体积 |

---

## config.spkg 构建配置

`config.spkg` 是项目的构建配置文件（类似 Zig 的 `build.zig`），使用 Lua 编写。spkg 会注入全局构建上下文 `b`，用户通过 `b` 声明工件、源文件和依赖关系。

### 最小示例

```lua
local exe = b:add_executable({ name = "myapp" })
exe:add_source("src/main.ce")
exe:add_include("src")
b:install(exe)
```

### 多工件示例

```lua
local target = b:get_target()

-- 静态库
local mathlib = b:add_static_library({ name = "mathlib" })
mathlib:add_source({ file = "src/math.ce" })
mathlib:add_include("src/include")
b:install(mathlib)

-- 可执行文件
local exe = b:add_executable({ name = "myapp" })
exe:add_source("src/main.ce")
exe:add_include("src")
exe:add_include("src/include")
exe:link_artifact(mathlib)
b:install(exe)
```

### 跨平台条件编译

```lua
local target = b:get_target()

local exe = b:add_executable({ name = "myapp" })
exe:add_source("src/main.ce")

if target:match("linux") then
    exe:add_source({ file = "src/platform_linux.ce", cflags = {"-DLINUX"} })
    exe:link_library("pthread")
elseif target:match("mingw") or target:match("windows") then
    exe:add_source({ file = "src/platform_win.ce", cflags = {"-DWIN32"} })
    exe:link_library("ws2_32")
elseif target:match("apple") or target:match("darwin") then
    exe:add_source({ file = "src/platform_macos.ce", cflags = {"-DMACOS"} })
end

b:install(exe)
```

---

## 构建上下文 b API

`b` 是 spkg 注入到 `config.spkg` 的全局构建上下文对象。

### 环境查询

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `b:get_target()` | `string` | 当前构建目标 triple（如 `x86_64-pc-linux-gnu`） |
| `b:get_optimize()` | `string` | 优化级别：`Debug` / `ReleaseSafe` / `ReleaseFast` / `ReleaseSmall` |
| `b:get_verbose()` | `bool` | 是否启用详细输出 |
| `b:get_jobs()` | `int` | 并行编译数 |
| `b:get_host()` | `string` | 宿主机平台 triple |

### 声明工件

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `b:add_executable(opts)` | `Artifact` | 声明可执行文件 |
| `b:add_static_library(opts)` | `Artifact` | 声明静态库 |
| `b:add_shared_library(opts)` | `Artifact` | 声明动态库 |

`opts` 参数：`{ name = "工件名" }`

### 其他方法

| 方法 | 返回类型 | 说明 |
|------|---------|------|
| `b:install(artifact)` | `Artifact` | 标记为默认构建目标 |
| `b:dependency(name)` | `Artifact | nil` | 获取已声明的工件 |
| `b:add_custom_step(opts)` | `CustomStep` | 添加自定义构建步骤 |
| `b:add_test(opts)` | `Artifact` | 声明测试工件 |

### 构建选项

```lua
b:option("enable_ssl", {
    description = "Enable SSL support",
    default     = false,
})
```

在命令行中使用：`spkg build --enable_ssl=true`

通过 `b.options.enable_ssl` 访问（惰性解析）。

### Target 解析

`b.target` 和 `b.host` 是已解析的 target 表：

```lua
b.target.raw    -- 完整 triple 字符串
b.target.arch   -- 架构（如 x86_64, aarch64）
b.target.vendor -- 厂商（如 pc, apple）
b.target.os     -- 操作系统（如 linux, darwin, windows）
b.target.abi    -- ABI（如 gnu, msvc, android）
```

---

## Artifact API

```lua
local exe = b:add_executable({ name = "myapp" })
```

### 源文件管理

```lua
-- 简写：字符串路径
exe:add_source("src/main.ce")

-- 结构体：支持 glob 模式和文件级编译选项
exe:add_source({
    file    = "src/features/**/*.ce",
    cflags  = {"-DFEATURE_X"},
    include = {"src/features/include"},
    define  = {"VERSION=1.0"},
})
```

### 编译选项

```lua
-- 添加包含路径
exe:add_include("include")
exe:add_include("src")

-- 添加编译标志
exe:add_cflags("-Wall", "-Wextra")

-- 添加宏定义（自动加 -D 前缀）
exe:add_define("DEBUG", "VERSION=2")

-- 添加链接标志
exe:add_ldflags("-pthread")
```

### 链接

```lua
-- 链接系统库
exe:link_library("pthread")
exe:link_library("dl")

-- 链接另一个 Artifact（自动 DAG 排序）
exe:link_artifact(mathlib)

-- 带可见性的链接
exe:link_artifact(mathlib, { visibility = "public" })
```

### 运行时配置

```lua
-- 设置 spkg run 时传递的参数
exe:set_run_args("--verbose", "--port=8080")
```

---

## Source Spec

`add_source` 接受字符串或表：

```lua
-- 字符串：等价于 { file = "..." }
artifact:add_source("src/main.ce")

-- 结构体
artifact:add_source({
    file    = "src/platform.ce",       -- 单个文件或 glob 模式（支持 **）
    cflags  = {"-DLINUX"},             -- 仅此文件的额外编译标志
    include = {"include/linux"},       -- 仅此文件的额外包含路径
    define  = {"VERSION=1.0"},         -- 宏定义（自动加 -D 前缀）
})
```

glob 模式示例：
- `"src/**/*.ce"` — 递归匹配 src 下所有 .ce 文件
- `"src/*.ce"` — 匹配 src 目录下的 .ce 文件

---

## Custom Step 自定义步骤

用于代码生成、资源编译等自定义构建步骤。

```lua
local proto_gen = b:add_custom_step({
    name    = "generate_proto",
    command = {"protoc", "--lua_out=build/proto", "src/proto/*.proto"},
    inputs  = {"src/proto/user.proto", "src/proto/message.proto"},
    outputs = {"build/proto/user_pb.lua", "build/proto/message_pb.lua"},
})

-- 使用自定义步骤的输出作为源文件
exe:add_source("build/proto/user_pb.lua")
exe:add_source("build/proto/message_pb.lua")
```

Custom Step 的执行逻辑：
1. 如果任何 output 不存在 → 执行
2. 如果任何 input 比 output 新 → 执行
3. 否则 → 跳过

---

## 测试框架

```lua
-- 声明测试工件
local test_exe = b:add_test({
    name = "myapp_test",
})
test_exe:add_source("tests/**/*.ce")
test_exe:link_artifact(myapp)
```

或使用已有工件：

```lua
local test_exe = b:add_test({
    artifact = existing_artifact,
})
```

运行测试：

```bash
spkg test
```

测试运行器会：
1. 构建所有测试工件
2. 依次执行每个测试
3. 输出 `[test]` 和 `[pass]` 状态
4. 所有测试通过时输出 `spkg: all tests passed.`

---

## 依赖管理

### SharpDeps.lua — 依赖声明

```lua
return {
    { name = "sharp-lib",    version = "1.0.0" },
    { name = "sharp-logger", version = "*" },
}
```

| 字段 | 说明 |
|------|------|
| `name` | 依赖包名称 |
| `version` | 版本号，`"*"` 表示最新版 |

### Sharp.lock — 依赖锁定（自动生成）

```lua
return {
  sharp-lib = {
    source  = "https://gitee.com/sharp-libs/sharp-lib.git",
    version = "1.0.0",
    tag     = "v1.0.0",
    commit  = "abc123def"
  }
}
```

### 依赖解析顺序

1. 检查 `Sharp.lock` — 如果已锁定，使用锁定的版本
2. 检查 `~/.sharp/config.spkm` — 用户自定义源配置
3. 使用默认源 — `https://gitee.com/sharp-libs/{name}.git`

### 自定义源配置

在 `~/.sharp/config.spkm` 中配置：

```lua
return {
    source = {
        default = "https://gitee.com/sharp-libs/{name}.git",
        ["my-private-lib"] = "https://github.com/me/my-private-lib.git",
    }
}
```

### 传递依赖

spkg 会递归解析每个依赖自身的 `SharpDeps.lua`，使用 visited 集合防止循环依赖。

### CLI 操作

```bash
# 添加依赖
spkg add sharp-lib 1.0.0

# 移除依赖（同时清理 spkg_packages/ 中的缓存）
spkg remove sharp-lib

# 列出依赖及状态
spkg list
# 输出：
#   sharp-lib (1.0.0) [installed]
#   sharp-logger (*) [missing]

# 更新依赖到最新版本
spkg update

# 查看项目信息和依赖树
spkg info
# 输出：
#   Project:
#     name:     myapp
#     target:   x86_64-pc-linux-gnu
#     optimize: Debug
#
#   Dependencies (2):
#     [1] sharp-lib (1.0.0) [installed] @abc123de
#       └─ sharp-logger (*) 
#     [2] sharp-logger (*) [missing]
```

---

## 增量编译

spkg 使用三重检测机制实现精确的增量编译：

| 条件 | 动作 |
|------|------|
| `.o` 不存在 | **编译** |
| `.ce` 比 `.o` 新 | **编译** |
| 任何 `#include` 的头文件比 `.o` 新 | **编译**（通过 `.d` 依赖文件） |
| 编译命令（cflags）改变 | **编译**（命令指纹对比） |
| 源文件被删除 | **清理残留 `.o` 和 `.d`** |
| 否则 | **跳过** |

### 依赖文件

编译时自动加 `-MMD -MF <output>.d` 生成 Makefile 风格的依赖文件：

```
build/myapp/main.o: src/main.ce src/include/header.he
```

### 命令指纹

为每个编译任务生成 FNV-1a 64-bit 哈希指纹，检测编译参数变化：

```
fingerprint = hash("spkg-v4" + "|" + sorted_cflags)
```

指纹存储在 `.o.fp` 文件中，下次构建时对比，不同则重新编译。

---

## 编译缓存

### 原理

参考 sccache/mozilla 设计，缓存编译结果：

```
缓存键 = hash(cflags_fingerprint + source_fingerprint)
缓存值 = .o 文件内容
```

### 缓存流程

```
编译请求 → 检查缓存
   │
   ├─ 命中 → 直接使用缓存的 .o
   │
   └─ 未命中 → 编译 → 存储到缓存 → 输出 .o
```

### 缓存目录

- 本地缓存：`$HOME/.spkg-cache/`
- 布局：`$HOME/.spkg-cache/<key>/output.o`
- 统计文件：`$HOME/.spkg-cache/.stats`

### CLI 操作

```bash
# 查看缓存统计
spkg cache --stats
# 输出：
#   Cache statistics:
#     entries:  42
#     hits:     128
#     misses:   42
#     size:     3.2 MB

# 清空缓存
spkg cache --clear

# 构建时禁用缓存
spkg build --no-cache
```

---

## 分布式编译

spkg 原生支持分布式编译，将编译任务分发到远程节点执行。

### 架构

```
┌─────────────────────────────────────────┐
│            spkg (coordinator)            │
│                                          │
│  config.spkg → DAG → 拓扑排序 → 任务队列  │
│  │                                       │
│  ├─[task1]─→ node1:10080 → .o           │
│  ├─[task2]─→ node2:10080 → .o           │
│  │                                       │
│  └── 收集 .o → 本地链接 → 最终产物        │
└─────────────────────────────────────────┘
```

### 节点配置

**方式一：环境变量**

```bash
export SPKG_NODES="node1:10080,node2:10080,node3:10080"
spkg build --dist
```

**方式二：配置文件** `spkg_nodes.json`

```json
["node1:10080", "node2:10080", "node3:10080"]
```

### 头文件上下文打包

分布式编译时，spkg 自动收集源文件引用的所有头文件，打包到编译请求中发送给远程节点：

1. 解析源码中 `#include "..."` / `#include <...>` 指令
2. 在 `-I` 目录和源文件目录中查找头文件
3. 递归收集嵌套头文件（深度限制 10 层）
4. 将头文件内容打包到 JSON 请求的 `headers` 字段

### 通信协议

**编译请求（POST /compile）：**

```json
{
  "source": "源码内容",
  "cflags": ["-O2", "-Iinclude"],
  "optimize": "ReleaseFast",
  "headers": [
    {"path": "lib/calc.h", "content": "头文件内容"}
  ]
}
```

**编译响应：**

```json
{
  "status": "ok",
  "output": "<base64 编码的 .o 文件>",
  "depfile": "依赖文件内容",
  "cached": false
}
```

**健康检查（GET /health）：**

```json
{"status": "ok", "active": 2, "max_jobs": 4}
```

### 容错与重试

- Round-robin 重试：如果当前节点失败，自动尝试下一个节点
- 所有节点失败时报告错误
- 链接步骤始终在本地执行

---

## Target Triple 支持

| 平台 | Target Triple | 状态 |
|------|--------------|------|
| Linux x86_64 | `x86_64-pc-linux-gnu` | ✅ |
| Linux ARM64 | `aarch64-pc-linux-gnu` | ✅ |
| Linux ARMv7 | `armv7l-pc-linux-gnueabihf` | ✅ |
| macOS Intel | `x86_64-apple-darwin` | ✅ |
| macOS ARM | `arm64-apple-darwin` | ✅ |
| Windows MSVC | `x86_64-pc-windows-msvc` | ✅ |
| Windows MinGW | `x86_64-w64-mingw32` | ✅ |
| iOS | `arm64-apple-ios` | ✅ |
| iOS Simulator | `arm64-apple-ios-simulator` | ✅ |
| Android ARM64 | `aarch64-linux-android` | ✅ |
| Android x86_64 | `x86_64-linux-android` | ✅ |

### 产物命名

| 类型 | POSIX | Windows |
|------|-------|---------|
| 可执行文件 | `myapp` | `myapp.exe` |
| 静态库 | `libmyapp.a` | `myapp.lib` |
| 动态库 | `libmyapp.so` | `myapp.dll` |

---

## 项目目录结构

```
project/
├── config.spkg              # 构建配置（声明式，必需）
├── SharpDeps.lua            # 依赖声明
├── Sharp.lock               # 依赖锁定（自动生成）
├── spkg_nodes.json          # 分布式节点配置（可选）
├── src/
│   ├── main.ce
│   └── include/
│       └── header.he
├── build/                   # 构建产物（自动生成）
│   ├── myapp/
│   │   ├── main.o
│   │   ├── main.o.d         # 头文件依赖
│   │   ├── main.o.fp        # 命令指纹
│   │   └── myapp            # 最终可执行文件
│   └── mathlib/
│       ├── basic.o
│       ├── basic.o.d
│       └── libmathlib.a
└── spkg_packages/           # 依赖缓存
    └── sharp-lib/

$HOME/.spkg-cache/           # 全局编译缓存
├── a1b2c3d4e5f6.../
│   └── output.o
└── .stats
```

---

## 编译器查找

### sharpc 查找优先级

1. `SHARPC` 环境变量
2. `{bin}/../../sharpc/bin/sharpc`
3. `{bin}/../../../build/sharpc`
4. `{bin}/../../build/sharpc`
5. `{bin}/../build/sharpc`
6. `{bin}/../sharpc`

### zig cc 查找优先级

1. `ZIGCC` 环境变量
2. `{bin}/zig`（POSIX）或 `{bin}/zig.exe`（Windows）
3. `{bin}/../zig/zig`（sharp 发行目录布局）
4. PATH 中的 `zig`

---

## 环境变量

| 变量 | 说明 |
|------|------|
| `SHARPC` | 指定 sharpc 编译器路径 |
| `ZIGCC` | 指定 zig 编译器路径 |
| `SPKG_NODES` | 分布式编译节点列表（逗号分隔，如 `node1:10080,node2:10080`） |
| `HOME` | 用户主目录（缓存和配置位置） |

---

## spkg-node 编译节点

`spkg-node` 是独立的分布式编译服务器，纯 C 实现，无外部依赖。

### 启动

```bash
spkg-node --listen 0.0.0.0:10080 --max-jobs 4 --sharpc /usr/bin/sharpc
```

### 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--listen <addr:port>` | `http://0.0.0.0:10080` | 监听地址 |
| `--max-jobs <N>` | `4` | 最大并发编译数（范围 1-64） |
| `--sharpc <path>` | 自动检测 | sharpc 编译器路径 |
| `-h, --help` | — | 显示帮助 |

### HTTP 路由

| 路由 | 方法 | 说明 |
|------|------|------|
| `/compile` | POST | 提交编译任务 |
| `/health` | GET | 健康检查 |

### 安全考虑

- **内网使用**：无 TLS，假设运行在可信网络
- **命令隔离**：节点只执行 `sharpc` 编译，不执行任意命令
- **资源限制**：`--max-jobs` 限制并发，防止过载
- **超时**：编译任务默认 120 秒超时
- **请求大小**：限制 10 MB

---

## 构建选项

在 `config.spkg` 中声明用户可配置的构建选项：

```lua
b:option("enable_ssl", {
    description = "Enable SSL support",
    default     = false,
})

b:option("log_level", {
    description = "Logging verbosity",
    default     = "info",
})
```

在命令行中传递选项：

```bash
spkg build --enable_ssl=true
spkg build --log_level=debug
```

在 `config.spkg` 中通过 `b.options` 访问：

```lua
if b.options.enable_ssl then
    exe:add_define("ENABLE_SSL")
    exe:link_library("ssl")
end
```

---

## 依赖可见性

链接 Artifact 时可指定可见性，语义与 CMake 的 PUBLIC/PRIVATE/INTERFACE 一致：

```lua
-- public（默认）：链接 + 编译标志传播到消费者
exe:link_artifact(mathlib, { visibility = "public" })

-- private：仅用于构建此工件，不传播
exe:link_artifact(internal_lib, { visibility = "private" })

-- interface：传播到消费者但不用于自身构建
exe:link_artifact(header_only, { visibility = "interface" })
```

可见性影响：
- **public**：依赖的 include 目录和 cflags 传播到消费者
- **private**：依赖的 include 目录和 cflags 不传播
- **interface**：依赖的 include 目录和 cflags 传播但不用于自身

---

## 许可证

MIT License — Copyright (c) 2026 Xarvie
