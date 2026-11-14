# BlueToothMI

小米手环 11 标准版（固件 `4.100.179`，`vela_ap.bin`）的 Canopus exact-target 模块：
蓝牙耳机「连接前端」——adapter 检查 → 设备扫描（含结果接收）→ 配对 → 注册 A2DP source / AVRCP control SDP 记录 → 提交 AVDTP/AVCTP L2CAP 连接请求。

**未实现**：AVDTP 信令（DISCOVER / SET_CONFIGURATION / OPEN / START）、SBC 编码、媒体流封装、AVRCP 命令处理——本模块到 `CMD_CONNECT`（通道请求入队）为止，不产生音频。

## 构建

GitHub Actions 自动构建：workflow 检出 [AstralSightStudios/Canopus](https://github.com/AstralSightStudios/Canopus)（pin 在 `f196b20`），把 `modules/headset-link-179` 覆盖进其 `modules/` 目录，用仓库自带 CLI 执行 `canopus build` → `canopus verify` → `canopus package`，产物（ELF + `.canopus` 包）在 Actions Artifacts 下载。

本地构建（Linux/macOS，需 rustup `thumbv8m.main-none-eabi` target + clang/llvm/lld）：

```sh
git clone https://github.com/AstralSightStudios/Canopus.git
cp -r modules/headset-link-179 Canopus/modules/
cd Canopus && cargo build -p canopus-cli
cd modules/headset-link-179
../../target/debug/canopus build && ../../target/debug/canopus package
```

## 模块控制接口

模块加载后默认 IDLE，所有固件调用经 `canopus_headset_control(cmd, arg)` 显式触发：

| cmd | 功能 |
|---|---|
| 1 | 开始扫描（首次自动注册 17-word adapter 回调表；slot 0/1/2 接收 adapter/discovery 状态与扫描结果，其余 stub） |
| 2 | 停止扫描 |
| 3 | 设置对端 BDADDR（6 字节） |
| 4 | 创建配对（classic transport） |
| 5 | 注册 A2DP source + AVRCP SDP 记录 |
| 6 | 提交 AVDTP/AVCTP L2CAP 连接请求（**未验证路径**，见 NOTES） |
| 7 | 清理（注销 SDP / 回调表、停止扫描） |
| 8 | 读取扫描结果（最多 8 条 × 32 字节，按地址去重） |

`canopus_mod_query` 输出：magic、状态机、固件返回码、bond 快照、SDP handle、通道请求入队标记、发现设备数、注册句柄。

## 状态与风险声明

- 构建 / ELF verifier / host 测试 / Unicorn / 真机验证均为 **0 次**。
- 固件符号地址来自对 `4.100.179`（基址 `0x0C0C0000`）的 IDA 逆向与 Canopus 仓库 `.179` target 交叉验证，全部为 restricted/PENDING，无真机证据。
- `CMD_CONNECT` 的 request+12 事件回调当前为空且未注册 AVDTP/AVCTP PSM handler，固件可能空指针调用；回调要求 boot residency 与模块 `removable` 生命周期冲突。细节见 `modules/headset-link-179/NOTES-ida-179.md`。
- `.179` 存在固化后循环重启的仓库事故记录。**不要直接刷入日常使用的设备。**

License: AGPL-3.0-only（与 Canopus 一致）。
