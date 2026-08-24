# dsh-pet-standalone 🐾

一个独立的 Windows 桌面宠物：双击 exe 启动，像 QQ 宠物一样趴在桌面上。

- **严格单文件**：默认配置 + 91 个透明动画全部内嵌在 exe 里，无需任何外部文件或运行时
- **原画质**：640×360 @ 24fps，与上游插件所见一致
- **原生 Win32**（C++17），内存解码、零落盘、仅依赖 Windows 系统库
- **托盘菜单**：退出 / 重启 / 显示隐藏 / 鼠标穿透 / 开机启动（左键右键均可呼出）
- **交互**：拖拽（含副屏）、点击反应、屏幕漫游、待机 / 转向 / 动作权重调度

## 快速开始

1. 从 [Releases](https://github.com/masknull/dsh-pet-standalone/releases) 下载 exe（约 65 MiB）。
2. 双击运行：宠物出现在屏幕右下角。
3. 右键宠物或托盘图标 → 菜单操作。

## 素材来源与授权

91 个桌宠动画素材来自上游开源项目 [PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)（MIT License），即其 `dsh-pet/assets/thumb/` 目录下的透明动画。上游素材生成链（AI 提示词 → 绿幕视频 → 透明动画）见该项目仓库。

本项目按上游 MIT 许可自由使用这些素材；衍生作品请保留上游版权声明。

## 构建

### GitHub Actions（推荐）

推送到本仓库后自动触发 `.github/workflows/build-standalone.yml`：

1. 下载上游素材 → 双流 VP9 转换 → MinGW-w64 交叉编译 → 全量自检
2. 自动递增版本号 → 发布 GitHub Release（附 exe）

### 本地构建

前置：MinGW g++、Python3、ffmpeg（含 libvpx）、libvpx 静态库、上游素材目录。

```powershell
$env:VPX_ROOT      = '<libvpx 静态库目录>'
$env:FFMPEG        = '<ffmpeg.exe>'
$env:DSH_WEBM_SRC  = '<上游 dsh-pet 的 assets/thumb 目录>'
.\build.ps1
```

## 目录结构

```
src/          C++17 + Win32 源码
scripts/      构建管线（双流转换 / 资源生成）
assets/       配置与动画清单（素材由构建生成）
samples/      外部配置示例
build.ps1     本地一键构建
.github/      CI 自动构建 + Release
```

## 排障

```powershell
.\dsh-pet-standalone.exe --selftest    # 自检
$env:DSH_PET_DIAG='1'; .\dsh-pet-standalone.exe   # 日志 → %TEMP%\dsh-pet-diag.log
```

## 致谢与许可

- 行为逻辑与素材来源：[PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)（MIT）
- 本项目 [LICENSE](LICENSE)：MIT（保留上游版权声明）