# LanDrop

一个轻量的局域网文件传输工具,客户端 + 服务端,界面参考 FileZilla / UltraFTP 的双栏浏览 + 传输队列模式。用 Qt6 + C++17 编写,macOS / Windows / Linux 跨平台。底层是标准 **FTP 协议(RFC 959)**,核心网络逻辑完全不依赖 Qt。

主要场景:在自己的多台设备(比如一台 Mac、一台 Windows 笔记本)之间,通过局域网稳定、可视化地互传文件,不依赖公网、不依赖第三方网盘;因为走的是标准 FTP,也可以直接用同一个客户端连接局域网内其它标准 FTP 服务器(比如 NAS),不局限于两台都装了 LanDrop 的设备。

## 下载

去 [Releases](https://github.com/DavidRedVest/LanDrop/releases) 页面下载对应平台的压缩包(macOS/Windows/Linux 都是 `.zip`/`.tar.gz`,不需要"安装"——解压后直接运行里面的可执行文件即可,不会往系统里写任何东西)。每个平台都分别打包了服务端和客户端(Linux 是两个二进制一起打包)。

- **macOS**:解压后直接运行 `LanDrop-Server-macOS` / `LanDrop-Client-macOS` 里的 `.app`,已经打包好 Qt 运行库,不需要额外安装 Qt。

  ⚠️ **首次打开会被 Gatekeeper 拦截**,提示"已损坏,无法打开"或"无法验证开发者"——这是因为本项目没有购买 Apple 开发者证书($99/年)对 App 做官方签名+公证,不是文件真的坏了。解决方法二选一:
  1. 在 Finder 里**右键点击** App → 选择"打开" → 弹窗里再点一次"打开"(只有首次需要这样,之后双击就正常了)。
  2. 或者在终端执行 `xattr -cr "LanDrop 客户端.app"`(把下载时系统加上的隔离标记去掉)。

- **Windows**:解压 `.zip` 后直接运行 `.exe`,同目录下已经带好了所需的 Qt DLL。Windows SmartScreen 可能会提示"未知发布者",点"更多信息" → "仍要运行"即可(原因同上,没有付费的代码签名证书)。
- **Linux**:解压 `.tar.gz` 后需要自行通过发行版包管理器装好 Qt6 运行库(比如 `sudo apt install qt6-base-dev` 或对应发行版的等价包),再运行里面的二进制文件。

## 功能特性

- **标准 FTP 协议(RFC 959)**:不是自造协议,可以连接任意标准 FTP 服务器(比如 NAS),不局限于两台都装了 LanDrop 的设备。
- **双栏文件浏览器**:左边本地、右边远程,同一套界面和操作(浏览目录、新建文件夹、重命名、删除)。
- **文件夹上传/下载**:选中的不只是文件,也可以是整个文件夹——客户端会自动递归遍历、在远程/本地重建出一致的目录结构,不是被拍平成一堆文件。
- **局域网自动发现**:客户端的站点管理对话框会自动列出局域网内正在广播的 LanDrop 服务端,双击即可填入连接信息,不需要手动记 IP。
- **传输队列可视化**:方向、文件名、百分比数字、进度条、速度、剩余时间(ETA)、状态,支持暂停 / 继续 / 取消 / 清除已完成。
- **断点续传**:传输中断后重新发起会自动从已传输的字节位置继续(标准 FTP 的 `REST` 命令),不用重头再传。
- **失败自动重试**:网络抖动导致的传输失败会自动线性退避重试(有次数上限,超过后停止并可手动重试)。
- **站点管理**:保存常用的连接信息(主机、端口、用户名),一键连接。
- **账号安全**:服务端密码使用加盐 SHA-256 哈希存储,不落地明文。

**没有的功能**(明确排除,不是漏做):

- **SFTP** 客户端支持——完全不同的协议(走 SSH),引入加密库的工程量和收益不成比例,详见开发笔记。
- **FTPS**(`AUTH TLS`,RFC 4217)——目前只支持明文 FTP,连接强制要求 TLS 的服务器会失败(报 `USER rejected: Use AUTH first` 之类的错误)。支持它需要引入 TLS 库,目前评估后暂缓,详见开发笔记。
- **标准 FTP 本身没有内建传输后完整性校验**这回事——不是 LanDrop 没做,是协议本身没有这个机制;对第三方 FTP 服务器没有办法绕过,对 LanDrop 自己的服务端未来可能加一个非标准扩展。

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

`.github/workflows/release.yml` 会在 macOS / Windows / Linux 三个平台上分别编译并打包(macOS 用 `macdeployqt` 打包 Qt 运行库后临时签名再压成 `.zip`,Windows 用 `windeployqt` 打成 `.zip`,Linux 打成 `.tar.gz`)。推送形如 `v1.0.0` 的 tag 会自动创建一个 GitHub Release 并附上三个平台的包;也可以在 Actions 页面手动触发(`workflow_dispatch`)只生成构建产物、不发布 Release,用来验证构建是否正常。

## 使用方法

1. **在提供文件的一端**运行 `landrop_server`:选择根目录、添加一个用户(用户名/密码),勾选"允许被局域网自动发现"(可选),点"启动服务"。
2. **在需要访问文件的一端**运行 `landrop_client`:点工具栏的"连接..."按钮打开站点管理。如果服务端开了局域网发现,直接在"局域网发现"面板里双击找到的设备即可自动填好地址;否则手动填入服务端的局域网 IP、端口、用户名密码,点"连接"。
3. 连接成功后,右侧远程面板会显示服务端根目录。选中文件或文件夹后右键菜单里有"上传到远程当前目录" / "下载到本地当前目录"(文件夹会递归处理),底部的传输队列会显示实时进度。

两台设备可以互为客户端和服务端——都装上这两个程序,各自按需启动服务或发起连接即可;也可以用 `landrop_client` 连接局域网里任何一台标准 FTP 服务器(比如 NAS),不要求对方也是 LanDrop。

## 架构简述

- `core/` —— 核心网络逻辑,**完全不依赖 Qt**,只用标准 C++17 + 原生 OS socket API。真实的 RFC 959 FTP 客户端(`core/ftp/ftp_client.*`)和服务端(`core/ftp/ftp_server.*` + `ftp_session.*`)状态机、路径安全(`core/ftp/ftp_paths.*`)、客户端并发传输池(`core/ftp/ftp_transfer_manager.*`)、局域网自动发现(`core/discovery/lan_discovery.*`,自定义 UDP 广播)都在这里。
- `client/`、`server/` —— GUI 层,唯一用到 Qt 的地方。`Connection`/`TransferQueue`/`FTPServer` 都是薄包装,内部委托给 `core/` 对应的类,通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 把后台线程的结果安全转发到 UI 线程。`MainWindow`/`FileBrowserPanel`/`TransferWidget`/`SiteManagerDialog`/`ServerWindow`/`FolderTransferCoordinator`(文件夹上传/下载的递归遍历/建目录逻辑)是界面和交互部分。
- `common/` —— 客户端/服务端共用、协议无关的工具函数(`utils.h/.cpp`:密码哈希、文件哈希、人类可读格式化)。

核心与界面严格分层是这个项目的一条硬约束:`core/` 里的任何改动都要用 `otool -L`(或等价工具)确认没有意外链接进 Qt。更详细的分阶段实现历史、已知限制、协议设计细节见仓库里的 `需求书.md` 和给 Claude Code 用的 `CLAUDE.md`。
