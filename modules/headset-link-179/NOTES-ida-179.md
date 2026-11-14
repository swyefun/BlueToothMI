# headset-link-179 交接备忘（IDA / band11std .179）

基线：commit `f196b20`；IDA 打开 `band11std` patched `vela_ap.bin`，基址 `0x0C0C0000`。
本轮只做只读核实与注释/文档增补，未改 `targets/`、`sdk/`、`.github/workflows/`，未 push。

## 1. Rust 工具链是不是用于构建 —— 是，且只用于构建/验证

| 位置 | 用途 |
|---|---|
| `scripts/ci.sh:16,20,27,28,30,33` | `cargo test --workspace`、`cargo build -p canopus-cli`、`target generate --all --check`、`cargo check` 各 target 生成物与 private 后端 |
| `scripts/ci.sh:80-83` | gate_5：`no-heap-counter` 交叉编译成 ELF + verifier + 真机 ELF loader 加载测试 |
| `scripts/tests/band11_private_backend.py:25,31` | 用 `cargo` / `rustc` 编译测试桩供 Unicorn 跑固件函数体，本质仍是构建 |
| `.github/workflows/ci.yml` | 已有 `macos-latest` + `ubuntu-latest` 两个 job，`dtolnay/rust-toolchain@stable` + `thumbv8m.main-none-eabi`；macOS `brew install lld`，Linux `apt clang lld llvm` |

逆向/`fw-match`/IDA 侧不依赖 Rust 工具链。

**本机限制**：`cargo` / `rustc` / `canopus` 均不在 PATH；`cli/canopus/src/main.rs:3-4` 在原生 Windows 直接 `compile_error!`；`wsl.exe` 被本机安全策略禁止启动且不可绕过。所以本机无法编译，构建必须放到 Ubuntu 或 GitHub Actions。

## 2. 模块在构建链里的位置

- `Cargo.toml` 末尾有空 `[workspace]`，是**独立 crate**，不在根 `Cargo.toml` 的 `members` 里 → `cargo test --workspace` 不会构建它。
- 构建入口是 `canopus build` / `canopus package --build`；CLI `cli/canopus/src/project.rs:104-107` 注入 `device` feature 与 `.179` 的 target feature。
- 现有 CI 的 gate_5 只覆盖 `no-heap-counter` 和所有 `canopus new` 模板，**不包含本模块**。要进 GitHub Actions，需新增一个 job/step 显式跑下面的 build + verify（工具链已现成）。

## 3. IDA 核实结果（本轮新增）

| 项 | 结果 |
|---|---|
| `bt_l2cap_connect` `0xc86d468` | 反编译为 `sub_C861768(MEM[0x200BE740], sub_C86F9B0, sub_C856DE0, request, 1)`。`sub_C861768` 分配 20 字节 work node：owner+0、request+4、handler+8、cancel+12、prio+16 —— 只是异步入队，不建链 |
| `l2cap_connect_handler` `0xc86f9b0` | 取 `flags@+8` bit0 → `v4`；`*(req+23)=2-v4`；**`*(req+12)` 为 0 时用 `sub_C867C80(PSM@+2, v4+2)` 填**；`sub_C86BD54(peer, req+12, PSM, 1)` 建 channel；`*(u16*)req = *(u16*)(ch+116)` 写回 CID；`(*(cb)(ch+20))(2, req)` 触发 CONFIRM |
| `sub_C86BD54` `0xc86bd54` | `*(ch+20)=a2`（回调）、`*(ch+112)=PSM`、`*(ch+116)=CID`、`*(ch+120)=flag`；channel 由 `sub_C8610A4(124)` 从内部池分配，**不是** `bt_alloc` |
| `0xc86eb0c` | `docs/band11-bluetooth-139.md:36` 记为「单独分配 120 字节 completion event」。在本 `.179` IDB 中 `is_code=False`、`FUNCATTR_START=BADADDR`；邻近函数起点为 `0xc86eac0` / `0xc86eb9a`。L2CAP 区 (`0xc86a000..0xc872000`) 未发现对 `bt_alloc(0xc3514ec)` 的直接调用 → **该地址不可直接复用到 .179**，completion event 的 CID+108 / MTU+72 释放逻辑也未验证 |
| 音频字符串 | `AVDTP signaling hannel recognized scid = {%d:%d}` @`0xcb34837`、`A2DP_STATE_PLAYING` @`0xcb7825e`、`a2dp_connection_state_callback` @`0xca4be34`、`avrcp_event_register` @`0xca461cd`、`AVCTP/AVRCP/HFP` PSM 列表 @`0xcb37bc4`；这些字符串在当前 IDB 中 xrefs 均为 0，未自动识别代码引用 |
| 生成物常量 | `.179` generated 已含 `l2cap_register_psm_handler` @`0xc8686c0`、`l2cap_connect_handler` @`0xc86f9b0`、`bt_free` @`0xc34cc7c` |

## 4. 对模块的直接影响（比已知泄漏更严重）

1. **`request+12` 是事件回调函数指针**，不是可选上下文。`queue_connect()` 把 68 字节全清零后直接 `bt_l2cap_connect`，固件会走 `sub_C867C80(PSM, ...)` 回退，要求 AVDTP(0x19)/AVCTP(0x17) 的 PSM handler 已注册。仓库内没有对应调用，因此 `CMD_CONNECT` 是未验证路径，可能空指针调用。
2. 回调签名按反编译推断为 `extern "C" fn(i32, *mut u8)`，首个事件是 `kind=2 CONFIRM`、参数为原始 request、CID 在 request+0。`band11.rs` 头部注释要求 published callbacks 具备 boot residency，而 `Canopus.toml` 是 `lifecycle = "removable"` —— 装回调前须先解决卸载问题。
3. `docs/band11-bluetooth-139.md` 的 completion/confirm 所有权规则来自 `.139`，在 `.179` 未验证，不能照抄释放逻辑。
4. `CMD_SCAN_START` / `CMD_SET_PEER` / `CMD_PAIR` / `CMD_REGISTER_SDP` 不经过上述未验证路径；`CMD_CONNECT` 及其之后无任何设备证据。已把这段风险写进 `src/lib.rs` 头部注释。

## 5. 换到 Ubuntu 后

```sh
rustup target add thumbv8m.main-none-eabi
sudo apt-get install -y clang lld llvm
cargo build -p canopus-cli
target/debug/canopus doctor
cd modules/headset-link-179
../../target/debug/canopus build          # 或 canopus package --build
../../target/debug/canopus verify <elf> --target xiaomi-band-11-4.100.179 --targets-dir ../../targets
```

参照范本：`sdk/rust/examples/no-heap-counter/build.sh`（cargo → C ctor shim → `ld.lld -r` → verifier）。
GitHub Actions 侧：在 `ci.yml` 的 linux job 里复用同一套 Rust/clang/lld 步骤，追加上述 `build` + `verify`；模块不在 workspace，不会被现有 `cargo test --workspace` 覆盖。

## 7. 扫描结果接收（第 7 轮新增实现）

**目标**：让 `CMD_SCAN_START` 之后模块能收到扫描到的设备，`CMD_SCAN_RESULTS`（cmd 8）取出列表。

### 本轮 IDA 新核实的固件语义

| 项 | 证据 |
|---|---|
| `bt_adapter_register` `0xc46a302` 是**追加式**注册 | 反编译：adapter+120 为空时 `sub_C4666D4(6)` 建 registration 并 `sub_C466740(reg, 0, callbacks)`；非空时直接再 `sub_C466740` —— 多个 callbacks 表可共存，不会顶掉系统表 |
| `sub_C466740` `0xc466740` | `bt_alloc(8)` 节点（cookie+表指针），挂入 registration 的列表，`++计数`；容量满或列表操作失败返回 0 |
| 注册握手 | `sub_C46A18C(adapter, buf, 49)`：先写 command 49，读 **708 字节** IPC envelope（与 `docs/band11-bluetooth-139.md:15` 一致），校验字节 `buf[12]` 非零则 `sub_C46732C` 回滚并返回 0。注册失败自动清理 |
| `bt_adapter_unregister` `0xc46a36c` | generated 已有 callable；teardown 时撤销本模块的 registration |
| 回调分发函数 | **未找到**。`0xc523570` 虽读 adapter 附近但实为设备侧 IPC 请求路径，不是观察者分发 |

### 模块侧设计（防御性，不依赖未知分发语义）

- 17-word 表全部非空：slot 0/1/2 = `on_adapter_state` / `on_discovery_state` / `on_discovery_result`，其余 14 个 slot 统一 stub（AAPCS 两个寄存器参数，多余参数忽略，r0=0）。**不存在空指针 slot**。
- slot 5（pair request）保持 stub：不干预配对决策，系统表（仍在观察者列表中）继续应答。
- `on_discovery_result` 只写静态缓冲（`AtomicU8` 数组，8 条 × 32 字节：addr6+rssi+保留+CoD4+name20，按地址去重），**不调用任何固件函数**（除 `discovery_name` 读名），避免回调上下文里的锁/分配风险。
- `scan_start`：首次注册（REGISTRATION=0 时），失败返回 `E_IO`；每次扫描清空缓冲。
- `teardown`：`bt_adapter_unregister(adapter, reg)`，防悬空回调。
- `CMD_SCAN_RESULTS`（cmd 8）：arg 为 256 字节输出缓冲，返回条数；query 尾部新增 found_count、registration、adapter_state、discovery_state 四项。

### 仍未验证（新）

- 分发模式是广播还是首个处理者；stub 返回 0 的约定（例如 bond/scan-mode slot 是否把返回值当结果）。
- 17-slot 顺序沿用 `.139` ABI 注记（`CALLBACK_*` 常量），`.179` 未单独核对。
- `discovery_name(result, 24)` 的读界是猜测值（头 12 字节 + 12 字节名字），固件实际分配的结果缓冲大小未知。
- 注册握手（708 字节 IPC 同步读写）在控制调用所在任务上下文是否安全，文档明言「不能据此保证无死锁」。
- 回调表地址 `| 1`（Thumb callable 位）按 generated 约定填写；固件若将该字当数据指针解引用则不成立——未验证。

## 8. 仍未解决（汇总）

构建已于 CI 闭环（github.com/swyefun/BlueToothMI，run 43542452980）：canopus build + ELF verifier + package 全部通过，修复了两处编译错误（回调表 fn-ptr→usize 的 E0605 与 static const-eval 限制，改为运行时填表）。host 测试、Unicorn、真机仍为 0 次；v0.2.0 增加 Manager UI 控制页（ADR-CAN-010：activate 注册 `ctx->ui->set_entry`，单页树：状态行模块/错误/配对/对端/扫描结果 + 操作行开始扫描/停止扫描/注册 SDP/配对第一台/复位；`CMD_CONNECT` 与 UI 选 peer 的 `CMD_PAIR` 刻意不上 UI）；AVDTP 信令、SBC、媒体包、音频源、AVRCP、L2CAP 完成回调/PSM 注册未实现；符号仍 restricted/PENDING、`capabilities = []`；§7 列出的分发语义各项未验证。`.179` 有固化后循环重启的事故记录，勿直接刷日常设备。
