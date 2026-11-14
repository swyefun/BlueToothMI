# 安装指南（小米手环 11 标准版 / 固件 4.100.179）

产物全部在 **`dist` 分支**（CI 每次构建自动重发），本仓库 main 分支是源码。
dist 内容：

| 文件/目录 | 用途 |
|---|---|
| `module-installer-watchface/build/headset-link-prod.zip` | **模块安装器表盘包（推荐入口）**：内嵌模块 ELF + CMI2 回执，装进手环后自动完成写 inbox + 提交安装 |
| `module-installer-watchface/` | 安装器表盘源文件（main.lua + 2 个 .bin 资源，与 zip 内容一致） |
| `out/headset_link_179-0.2.0.canopus` | 模块完整包（tar：manifest + ELF + CMI2 回执；手机侧格式，`.179` 无投递通道，仅存档用） |
| `inbox/headset_link_179.cmi.next` | 设备 inbox 安装文件（回执）——仅当你有裸写 `/data` 文件的通道时才用 |
| `inbox/headset_link_179.ko.next` | 设备 inbox 安装文件（ELF） |
| `installer-watchface/` | Band 11 框架安装器表盘（.179 Supervisor 131,928 B + stage1/2 + boot/profile 资源 + Manager 图标） |

## ⚠️ 先读这个

- `.179` 在仓库记录中有**固化 Supervisor 后循环重启、最终售后换机**的事故（ADR-007，原因未定）。本方案用表盘 **Run** 方式加载（非固化），但同一固件家族的事故记录仍然成立。不要在唯一的、日常使用的设备上首次尝试；确保你手里有可重刷的固件与恢复手段。
- 本模块从未上过真机；装上后它默认 IDLE，不会自己碰蓝牙。
- 模块只实现「连接前端」（扫描/配对/SDP/L2CAP 请求入队），**没有音频**（无 AVDTP/SBC/AVRCP）。
- 安装校验是「回执 ↔ Supervisor 编译常量」比对，都来自 `.179` target，不比对运行中固件的哈希——你的 27 字节绑定跳过补丁不影响安装，也不影响表盘的 `/etc/build.prop` 身份校验（补丁没碰那些字符串）。

## 安装链

**第 1 步：装框架安装器表盘（Supervisor）。**
把 `installer-watchface/` 根部的 `main.lua` 和所有 `.bin` 打成表盘包（`docs/`、`src/`、`build/` 不参与打包），经你的 rpk 通道装进手环。表盘会读 `/etc/build.prop` 自动选中 `4.100.179` profile。已装过可跳过。

**第 2 步：点 Run 加载 Supervisor。**
表盘 Run 依次执行 stage0 → stage1 → stage2，Supervisor 常驻后 `/dev/canopus` 注册，Launcher 里出现 Canopus Manager 应用，同时注册安装入口 `/canopus/install`。

**第 3 步：装模块安装器表盘（推荐，无需任何传文件工具）。**
把 `module-installer-watchface/build/headset-link-prod.zip` 经同一条 rpk 通道装进手环，打开运行。表盘会自动完成整个安装并逐步显示状态：

1. 读 `/etc/build.prop` 的 `ro.build.id`，匹配 `4.100.179`
2. 写 `/data/canopus/inbox/headset_link_179.cmi.next` + `.ko.next`（写后回读校验）
3. 向 `/canopus/install` 提交 CPC2 INSTALL（token = `headset_link_179`）
4. 显示 Supervisor 回复：新槽位 → `COMPLETED`（模块已导入、保持禁用）；更新 → `REBOOT_REQUIRED`（重启后切换到新代）

替代路径（仅当已有裸写 `/data` 文件的工具）：手动把 `inbox/` 下两个 `.next` 放进 `/data/canopus/inbox/` 后，仍需运行一次安装器表盘提交 INSTALL——Band 11 的 Supervisor 只认 `/canopus/install` 的安装请求，光放文件不会被导入。

**第 4 步：启用模块。**
按表盘提示重启，打开 Canopus Manager → 模块 `headset_link_179`（v0.2.0）→ 启用 → 再重启生效。版本语义是 A/B 分代暂存：更新走下一代，切换发生在下一次加载前，失败自动回退上一代。

## 装上之后

模块 activate 时向 Manager 注册「**耳机连接**」控制页（v0.2.0）：Manager → 模块详情 → **打开界面**。页面提供：

- 状态行：模块状态 / 最近错误 / 配对状态 / 对端地址 / 扫描结果（含前 3 台设备名）
- 操作行：开始扫描 / 停止扫描 / 注册 SDP / 配对第一台（与扫描结果第一台建 bond）/ 复位

底层命令对应 `canopus_headset_control`：`1` 扫描开始（首次自动注册回调表）→ `8` 读扫描结果 → `3` 设置对端 BDADDR → `4` 配对 → `5` 注册 SDP → `6` 提交 L2CAP 请求（**未验证路径**，UI 刻意不暴露）→ `7` 清理。

## 已知风险与未验证点

- Supervisor 加载失败会在表盘 UI 显示；部分加载后先重启再重试。
- `CMD_CONNECT(6)` 的事件回调（request+12）为空且未注册 AVDTP/AVCTP PSM handler，固件可能空指针调用——**UI 与命令面都不暴露命令 6**。
- Manager 的「打开界面」入口、inbox 安装器在 Band 11 `.179` 上均无真机证据（`.155/.179` 的返回手势支持与 `/canopus/install` 路径检查有上游文档与 `.139` 固件证据背书）；安装器若失败，UI 会显示失败步骤与 Supervisor diagnostic。
