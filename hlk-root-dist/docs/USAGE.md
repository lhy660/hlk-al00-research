# HLK-AL00 临时 Root 使用教程

> ⚠️ **使用前必读 —— 本工具是针对特定系统版本的精准利用，只在一组版本上验证过。**
>
> | 项目 | 必须完全一致 | 查看（设置 → 系统 → 关于手机） |
> |---|---|---|
> | 系统版本 | **EMUI 10.1.0.138** | 版本号 |
> | 内核版本 | **4.14.116** | 内核版本 |
> | 安全补丁级别 | **2021年7月1日** | Android 安全补丁程序级别 |
>
> 任何一项不符都可能**无法获得 root，甚至直接死机（kernel panic）**。
> Bootloader 保持锁定，本工具不改系统分区；root 为**临时 root**，重启即消失、
> 开机自动重建。
>
> **本工具必须搭配 SukiSU Ultra 使用**（`bin/SukiSU_v4.2.1_40994-release.apk`），
> root 的授权管理由 SukiSU 完成。

---

## 怎么用（三步）

1. **安装两个 APK**（电脑 adb 或直接传输安装均可）：

   ```sh
   adb reboot                                   # 若设备此前已加载旧模块，必须先重启
   adb wait-for-device
   adb install -r -d bin/hlk-root-guard.apk     # HLKROOT（主程序）
   adb install -r -d bin/SukiSU_v4.2.1_40994-release.apk   # SukiSU Ultra
   adb reboot                                   # 重启，走开机自动链
   ```

2. **装完（或重装）后，在手机上打开一次 HLKROOT，点 `Start now`**。
   这一步激活开机自启——不做的话重启后 root 链不会运行。之后无需再管。

3. **等待开机链自动完成**（开机后 5~20 分钟内，受 EMUI 自启调度影响）：
   自动触发利用链 → 加载两个内核模块 → root 就绪。打开 HLKROOT 首页看到
   「ROOT运行中」即成功；SukiSU 里可以给应用逐个授权 root。

就这么多——**装 APP 就解决了**，日常使用不需要电脑、不需要命令行。

---

## 少数人才需要的两件事

- **Termux 用户**：进 HLKROOT 的「补丁」页点一次「执行补丁」，Termux 的
  `su` / `sudo` / `tsu` 即可用（有「检查补丁」可先验证状态）。重装 Termux
  或重装本 APP 后需要再点一次。
- **想用命令行验证 root**：任意终端执行 `su -c id`，看到 `uid=0` 且
  capability 齐全即正常；`/proc/hlksu_fd9` 可查看模块状态。

## 卸载

直接卸载 HLKROOT 与 SukiSU，重启一次即恢复原状。所有改动都在内存里，
不残留任何系统分区文件。

---

## 更多文档

- 构建环境与顺序、系统依赖：[`../../tools/README-linux.md`](../../tools/README-linux.md)
- 研究过程与漏洞细节：[`RESEARCH_LOG.md`](RESEARCH_LOG.md)（结论速查见包根 `RESEARCH_NOTES.md`）
- 利用链设计：[`mali_lpe_design.md`](mali_lpe_design.md)
- 详细版使用说明（开机链细节、门禁、开关速查、排查）：
  `../history/USAGE-detailed-20261111.md`
