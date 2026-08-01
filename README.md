# LanDrop

一个轻量的局域网文件传输工具,客户端 + 服务端,界面参考 FileZilla / UltraFTP 的双栏浏览 + 传输队列模式。用 Qt6 + C++17 编写,macOS / Windows / Linux 跨平台。

主要场景:在自己的多台设备(比如一台 Mac、一台 Windows 笔记本)之间,通过局域网稳定、可视化地互传文件,不依赖公网、不依赖第三方网盘。

## 下载

去 [Releases](https://github.com/DavidRedVest/LanDrop/releases) 页面下载对应平台的压缩包/安装包(macOS 是 `.dmg`,Windows 是 `.zip`,Linux 是 `.tar.gz`)。每个平台都分别打包了服务端和客户端(Linux 是两个二进制一起打包)。

- **macOS**:打开 `.dmg`,把 `LanDrop 服务端.app` / `LanDrop 客户端.app` 拖进"应用程序"即可,已经打包好 Qt 运行库,不需要额外安装 Qt。
- **Windows**:解压 `.zip` 后直接运行 `.exe`,同目录下已经带好了所需的 Qt DLL。
- **Linux**:解压 `.tar.gz` 后需要自行通过发行版包管理器装好 Qt6 运行库(比如 `sudo apt install qt6-base-dev` 或对应发行版的等价包),再运行里面的二进制文件。

## 功能特性

- **双栏文件浏览器**:左边本地、右边远程,同一套界面和操作(浏览目录、新建文件夹、重命名、删除)。
- **传输队列可视化**:进度条、速度、剩余时间(ETA)、状态,支持暂停 / 继续 / 取消 / 清除已完成。
- **断点续传**:传输中断后重新发起会自动从已传输的字节位置继续,不用重头再传。
- **失败自动重试**:网络抖动导致的传输失败会自动退避重试(有次数上限,超过后停止并可手动重试)。
- **完整性校验**:每次传输完成后用 SHA-256 校验文件内容,校验失败会自动清理并要求重传。
- **站点管理**:保存常用的连接信息(主机、端口、用户名),一键连接。
- **账号安全**:服务端密码使用加盐 SHA-256 哈希存储,不落地明文。

## 当前进度

**第一阶段(自有协议,设备间局域网互传)已完成并可用**:服务端(`landrop_server`)+ 客户端(`landrop_client`)之间通过一套自定义二进制协议(非标准 FTP)通信,双方都是本项目自己实现的程序。

尚未实现(规划中,不在当前版本范围内):

- 标准 FTP 协议支持(连接/托管第三方 FTP 服务器,如 NAS)
- SFTP 客户端支持(基于系统自带 SSH,如 macOS/Linux 的 OpenSSH、Windows 的 OpenSSH 可选功能)
- 局域网自动发现设备

## 构建

### 依赖

- CMake 3.16+
- 支持 C++17 的编译器
- Qt6(Core、Network、Widgets 组件)

### 编译

```bash
cmake -B build -DCMAKE_PREFIX_PATH="<你的 Qt6 安装路径>"
cmake --build build
```

macOS 用 Homebrew 装的 Qt6 举例:

```bash
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
```

编译产物是 `landrop_server` 和 `landrop_client`。在 macOS 上因为是 `MACOSX_BUNDLE`,实际可执行文件在 `build/landrop_server.app/Contents/MacOS/landrop_server`(直接 `open build/landrop_server.app` 更方便);Windows/Linux 上就是普通的 `build/landrop_server(.exe)`。

### 发布 / CI

`.github/workflows/release.yml` 会在 macOS / Windows / Linux 三个平台上分别编译并打包(macOS 用 `macdeployqt` 打成 `.dmg`,Windows 用 `windeployqt` 打成 `.zip`,Linux 打成 `.tar.gz`)。推送形如 `v1.0.0` 的 tag 会自动创建一个 GitHub Release 并附上三个平台的包;也可以在 Actions 页面手动触发(`workflow_dispatch`)只生成构建产物、不发布 Release,用来验证构建是否正常。

## 使用方法

1. **在提供文件的一端**运行 `landrop_server`:选择根目录、添加一个用户(用户名/密码),点"启动服务"。默认控制端口 2121,数据端口是控制端口+1(2122)。
2. **在需要访问文件的一端**运行 `landrop_client`:点工具栏的"连接..."按钮打开站点管理,填入服务端的局域网 IP、端口、用户名密码,点"连接"。
3. 连接成功后,右侧远程面板会显示服务端根目录。选中文件后右键菜单里有"上传到远程当前目录" / "下载到本地当前目录",底部的传输队列会显示实时进度。

两台设备可以互为客户端和服务端——都装上这两个程序,各自按需启动服务或发起连接即可。

## 架构简述

- `common/` — 客户端和服务端共用的协议编解码(`protocol.h/.cpp`)和工具函数(`utils.h/.cpp`)。
- `server/` — 服务端:`FTPServer` 管理连接和用户,`Session` 处理单个客户端的控制通道,`FileManager` 负责所有文件系统操作(带路径校验,防止目录穿越),`DataChannelServer` 负责实际的文件字节收发。
- `client/` — 客户端:`Connection` 是控制通道的封装,`TransferQueue` 是传输队列引擎(限并发、自动重试、断点续传),`FileBrowserPanel`/`TransferWidget`/`MainWindow` 是界面部分。

协议采用控制通道(登录、浏览、文件管理)+ 数据通道(实际传输字节)分离的设计,类似经典 FTP 的思路,但是自定义的二进制帧格式,不是标准 FTP 协议。

更详细的实现细节(协议字段布局、已知的坑等)见仓库里的开发笔记。
