# C++23 模块与资源所有权

代码路径相对仓库根目录。

## 模块边界与头文件

- import std 后禁止再 include 标准头，C/系统头放 module; 与命名模块声明之间。
  普通 UI .cpp/ui.h 使用哪个标准设施就自行 include 对应头，stderr 需要 <cstdio>。
- JSON 经 import nlohmann.json 使用，禁止 .items() 结构化绑定（代理 get<> 不在
  模块导出集），遍历用迭代器 it.key()/it.value()。

## 命名模块内联

`.cppm` 的命名模块 purview 不使用 `#include` 头文件的全局模块语义。class 体内定义的
成员函数不会自动获得全局模块下的隐式 `inline`；如果成员函数定义留在模块接口中且
希望作为接口内联函数使用，必须显式写 `inline`（`constexpr` / `consteval` 除外）。
普通命名空间函数在头文件和 `.cppm` 中都不会自动 `inline`，不能据此给所有函数加关键字。
`inline` 不是性能保证，实际是否内联需结合 `-O`、LTO/IPO、反汇编和基准确认。重量级
I/O、解析和 JSON 函数默认保留在 `.cpp` 中。

## 资源与 RAII

- **凡是获取后必须释放的东西，一律由 RAII 所有者包裹**，不得在函数体里裸拿
  句柄再靠每条分支手写 `close` / `pclose` / `free` / `join` / `RegCloseKey`。
  覆盖范围：POSIX fd（`open`/`socket`/`accept`）、`FILE*` 与 `popen` 管道、
  平台句柄（Windows `HKEY`/`HANDLE`）、线程、互斥量与锁、临时文件、以及
  HuxerUI 的 `TaskScope` / 订阅 / 动画句柄。
- **裸 `new` / `delete` / `malloc` / `free` 一律禁止**：用 `std::unique_ptr` /
  `std::shared_ptr` / 标准容器；Pimpl 用 `std::unique_ptr<Impl>`（见
  `LocalRouter`、用量账本的 UI 层封装）。
- **锁只用 `std::lock_guard` / `std::unique_lock` / `std::scoped_lock`**，不手写
  `lock()` / `unlock()`；需要条件变量时用 `std::unique_lock` + `wait*`。
- **平台 API 用带自定义 deleter 的 `std::unique_ptr` 或一个极小的 Guard 类**，
  例如 `config.cppm` 的 `UniquePipe`（`popen` → `pclose`）与 `UniqueRegKey`
  （`RegOpenKeyExA` → `RegCloseKey`）；不要为它们引入堆分配以外的运行时开销。
- **平台专用符号必须连 RAII 包装一起关在对应平台分支内编译**：把调用
  POSIX `popen`/`pclose` 的 closer 提到 `#if` 之外会让 MSVC 直接
  `C3861: 'pclose': identifier not found`（它的全局命名空间没有这两个名字），
  而 Linux/macOS 本地编译发现不了——`config.cppm` 的 `UniquePipe` 只在
  `!defined(_WIN32)` 编译就是这个原因。改动跨平台代码后至少确认每个
  `#if` 分支里出现的符号在该平台都存在。
- **获取失败与异常路径都不能泄漏**：先构造所有者，再用 `Valid()` / `operator bool`
  判断；错误分支直接 `return`，由析构负责释放（`src/single_instance.cpp` 的
  `UniqueFd` 是范本）。
- **线程必须由持有者负责停与 join**：持有者析构里 `stop()` + `join()`，或用一个
  析构即停的 RAII 包装；新起的线程不得无人 join。已有范本：
  `LocalRouter::Impl::~Impl`、`single_instance::Coordinator::~Coordinator`、
  测试里的 `FakeUpstream::~FakeUpstream`。线程创建本身失败也要回滚已获取的状态
  （见 `LocalRouter::Impl::start`），不留「已运行但无人监听」的假象。
- **临时文件用守卫**：`<file>.tmp` 这类半成品在成功 `rename` 后显式 `Release()`，
  失败/提前返回/抛异常时由析构删除，不在用户目录留孤儿（`TempFileGuard`，
  见 `src/store.cpp`、`src/mcp.cpp`、`src/skills.cpp`、`src/router.cpp`）。
- **不要用「文档里写着记得关」代替 RAII**：漏掉一条错误分支就是泄漏；新增资源
  类型时先写所有者，再写使用它的代码。

全项目盘点（2026-09 复核）：**所有获取即需释放的资源都有 RAII 所有者**，
新增资源类型必须照此先写所有者再写使用者。

| 资源 | 所有者 | 位置 |
|------|--------|------|
| 堆内存 | `std::unique_ptr<Impl>` / `shared_ptr` / 标准容器（无裸 new/delete） | `router.cpp`、`usage.cpp`、`ui/router_transport.cpp` |
| POSIX fd（open/socket/accept/connect） | `UniqueFd`（析构 `close`，可移动、禁拷贝；`Valid()` 判失败） | `src/single_instance.cpp` |
| `popen` 管道 | `UniquePipe = unique_ptr<FILE, PipeCloser>`（析构 `pclose`） | `src/config.cppm` 的 `systemPrefersDark()` |
| Windows 注册表键 | `UniqueRegKey`（析构 `RegCloseKey`） | `src/config.cppm`、`platform/windows/package/src/app.cpp` |
| 临时文件 `<file>.tmp` | `TempFileGuard`（成功 rename 后 `Release()`，否则析构删除） | `src/store.cpp`、`src/mcp.cpp`、`src/skills.cpp`、`src/router.cpp` |
| SQLite 连接 | `huxerui::sqlite::Database`（可拷贝句柄，单连接串行 worker；持有在统计页组合的 `State` 里，随组合卸载关闭） | `src/ui/stats_page.cpp` |
| 线程 | 持有者析构 `stop()` + `join()`；创建失败回滚绑定状态 | `LocalRouter::Impl`（`src/router.cpp`）、`single_instance::Coordinator`、测试 `FakeUpstream` |
| 锁 / 条件变量 | `std::lock_guard` / `std::unique_lock`（永不手写 lock/unlock） | `router.cpp`、`usage.cpp`、`ui/router_transport.cpp`、`single_instance.cpp` |
| HuxerUI 句柄 / 任务 / 订阅 | SDK 的 `TaskScope`、`ApplicationHandle`、`WindowHandle`、`SystemTrayHandle` 与 `Lifecycle` 返回的 disposer；composable 卸载自动取消 | `src/ui/*.cpp`（`UseTaskScope`、`Lifecycle`、`UseApplication().Clipboard()`） |

要点与易错处：

- 文件流 `std::ifstream` / `std::ofstream` 本身就是 RAII，不要为了「统一」再包一层；
  真正需要守卫的是它们的**副作用残留**（`.tmp` 半成品、备份、锁文件）。
- 平台句柄优先「带自定义 deleter 的 `unique_ptr`」，只有需要 `Out()` 参数风格
  （如 `RegOpenKeyExA` 的 `HKEY*`）才写极小 Guard 类；不要引入堆分配以外开销。
- **平台专用 closer 必须和它的 `#if` 分支绑在一起**：`UniquePipe` 调用的
  `popen`/`pclose` 是 POSIX，MSVC 全局命名空间里没有这两个名字。0.1.37 首轮
  CI 就是把 `PipeCloser` 提到 `#if` 之外，导致 `config.cppm` 在
  `test_store` 里编译失败（`C2039` / `C3861`），两个 Windows job 全挂而
  Linux/macOS 全绿——本地是 Linux 时这类错误只能靠 CI 暴露，改动跨平台
  代码后要人工核对每个 `#if` 分支里的符号在目标平台是否存在。
- 线程创建本身可能抛（资源耗尽）：`LocalRouter::Impl::start` 在 catch 里复位
  `isRunning` / `boundPort` 并 `server.stop()`，避免「报错但端口已占」的假运行态。
- `UniqueFd::Reset(int fd = -1)` 先关旧的再接管新的，`StartOrActivate` 重入安全由此保证。
- 已知的有意例外：`window.OnCloseRequest` 消费关闭、`Lifecycle` 返回清理闭包这类
  回调注册，必须返回可逆的 disposer（如 `single_instance::ClearActivationHandler`），
  不得注册后无人撤销。
