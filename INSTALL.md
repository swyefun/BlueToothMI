# 安装指南（小米手环 11 标准版 / 固件 4.100.179）

产物全部在 **`dist` 分支**（CI 每次构建自动重发），本仓库 main 分支是源码。
dist 内容：

| 文件/目录 | 用途 |
|---|---|
| `out/headset_link_179-0.1.0.canopus` | 模块完整包（tar：manifest + ELF + CMI2 回执） |
| `out/headset_link_179.elf` | 模块 ELF（零导入 ET_REL，已过 verifier） |
| `inbox/headset_link_179.cmi.next` | 设备 inbox 安装文件（回执） |
| `inbox/headset_link_179.ko.next` | 设备 inbox 安装文件（ELF） |
| `installer-watchface/` | Band 11 框架安装器表盘（.179 Supervisor 131,928 B + stage1/2 + boot/profile 资源 + Manager 图标） |

## ⚠️ 先读这个

- `.179` 在仓库记录中有**固化 Supervisor 后循环重启、最终售后换机**的事故（ADR-007，原因未定）。本方案用表盘 **Run** 方式加载（非固化），但同一固件家族的事故记录仍然成立。不要在唯一的、日常使用的设备上首次尝试；确保你手里有可重刷的固件与恢复手段。
- 本模块从未上过真机；装上后它默认 IDLE，不会自己碰蓝牙。
- 模块只实现「连接前端」（扫描/配对/SDP/L2CAP 请求入队），**没有音频**（无 AVDTP/SBC/AVRCP）。
- 安装校验是「回执 ↔ Supervisor 编译常量」比对，都来自 `.179` target，不比对运行中固件的哈希——你的 27 字节绑定跳过补丁不影响安装，也不影响表盘的 `/etc/build.prop` 身份校验（补丁没碰那些字符串）。

## 安装链（4 步）

**第 1 步：装框架安装器表盘。**
把 `installer-watchface/` 根部的 `main.lua` 和所有 `.bin` 打成表盘包（`docs/`、`src/`、`build/` 不参与打包），经你的 rpk 通道（/data 重放链或 AstroBox 表盘安装）装进手环。表盘会读 `/etc/build.prop` 自动选中 `4.100.179` profile。

**第 2 步：点 Run 加载 Supervisor。**
表盘 Run 依次执行 stage0 → stage1 → stage2，Supervisor 常驻后 `/dev/canopus` 注册，Launcher 里出现 Canopus Manager 应用。失败状态可再点 Run 重试；「Clear Env」双击确认后清除 `/data/canopus`。

**第 3 步：模块文件放入 inbox。**
把 `inbox/headset_link_179.cmi.next` 和 `inbox/headset_link_179.ko.next` 放到手环的 `/data/canopus/inbox/`（用你往 /data 写文件的既有通道；这组文件名就是 Supervisor 暂存区约定 `<module_id>.cmi.next` / `<module_id>.ko.next`）。

**第 4 步：导入并启用。**
重启（或重新 Run 安装器）后，Manager 的 inbox 扫描把包导入为**已禁用**槽位（此行为按 p65 `.043` 文档记录，Band 11 上未验证）；在 Manager 里启用该模块。包的 manifest 标记 `reboot_required = true`，启用后重启生效。版本语义是 A/B 分代暂存：更新走下一代，切换发生在下一次加载前，失败自动回退上一代。

## 装上之后

模块加载即停在 `ST_IDLE`，所有固件调用都要显式触发 `canopus_headset_control(cmd, arg)`：
`1` 扫描开始（首次自动注册回调表）→ `8` 读扫描结果 → `3` 设置对端 BDADDR → `4` 配对 → `5` 注册 SDP → `6` 提交 L2CAP 请求（**未验证路径**，见 NOTES）→ `7` 清理。
当前交付**不包含触发端**——没有 Manager 控制页，也没有手机端发送器；`canopus_mod_query` 的状态输出可通过 Manager/QUERY_DEVICE 观察。

## 已知风险与未验证点

- Supervisor 加载失败会在表盘 UI 显示；部分加载后先重启再重试。
- `CMD_CONNECT(6)` 的事件回调（request+12）为空且未注册 AVDTP/AVCTP PSM handler，固件可能空指针调用——**先别发命令 6**。
- Manager 的 inbox 导入、回调分发顺序、stub 返回值约定等在 Band 11 上均无真机证据。
