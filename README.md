WebRTC-Native-Manager 高性能远程桌面系统

基于 WebRTC-Native、Boost、MsQuic (QUIC) 与 Interception (驱动级) 构建的下一代 P2P 远程控制方案。

🚀 核心亮点

极致性能: 采用 C++20 协程与 Proactor 模式，结合 WebRTC-Native AV1 编码。

下一代信令: 基于 MsQuic 实现 0-RTT 极速连接与多路复用，告别 TCP 队头阻塞。

系统级操控: Interception 驱动实现键鼠零延迟输入，完美支持 UAC 安全桌面与大型游戏。

真正的 P2P: 最小化服务器依赖，基于 UDP 的点对点直连传输。

🏗️ 系统架构概览
1. 信令服务器 (MsQuic Signaling Server)

核心技术: C++20 Coroutines + Boost.Asio + MsQuic。

并发模型: Proactor IO 模型 + 多通道 Actor 隔离架构，实现无锁高并发。

主要职责: 负责由 QUIC 协议承载的信令交换、SDP/ICE 协商及节点注册。

2. 被控端 (Host/Source)

画面: DXGI 高性能截屏 + WebRTC (AV1) 硬件编码。

输入: Interception 驱动级模拟，绕过 Windows 权限限制 (UAC)。

同步: Windows Hooks 捕获并同步光标状态。

3. 操控端 (Client/Controller)

渲染: WebRTC 解码 + Qt QRHI 高效渲染。

交互: 驱动级输入采集，通过 DataChannel 低延迟传输指令。

🧩 信令服务器架构详解

采用 分层 + 多通道 (Multi-Channel) 设计，确保高负载下的低延迟响应。

整体拓扑
code
Mermaid
download
content_copy
expand_less
graph TD
    Client[客户端/MsQuic] <--> Listener[QUIC Listener :8088]
    Listener --> LB[负载均衡 Load Balancer]
    LB --> Router[通道路由 Global Router]
    
    subgraph "IO Worker Threads (Proactor)"
        Router --> C1[Channel 0 Manager]
        Router --> C2[Channel N Manager]
        C1 --> Logic1[业务逻辑层]
        C2 --> Logic2[业务逻辑层]
    end
    
    Logic1 <--> DB[(MySQL / Cache)]
内部数据流

接入层 (Access): 管理 MsQuic Socket 生命周期，处理 QUIC 流的多路复用。

业务层 (Business): 消息路由与会话状态管理，支持跨通道消息投递 (PostAsyncTask)。

数据层 (Data): 独立的数据库连接池与 LRU 缓存。

🛠️ 功能特性矩阵
🖥️ 视觉与传输

DXGI 高性能采集: 支持 30/60 FPS 动态调整。

AV1 编码: 提供比 H.264 更高的压缩率与画质。

自适应流控: 根据带宽动态调整码率与画质。

MsQuic 传输: 0-RTT 握手，弱网环境下表现优异。

🎮 操控与输入

驱动级输入 (Interception): 0 延迟，支持英雄联盟等 FPS/MOBA 游戏。

UAC 穿透: 完美控制 Windows 安全桌面（登录/UAC 弹窗）。

全功能键鼠: 支持组合键 (Win/Ctrl/Alt)、中键、侧键及滚轮。

状态双向同步: 本地与远程鼠标状态实时一致。

🌐 连接与网络

P2P 直连: 优先尝试局域网/公网直连，STUN/TURN 辅助穿透。

智能重连: 进程守护、断网自动恢复。

多端支持: Windows 完整版 + 移动端 H5 (WebRTC)。

⚡ 协议栈对比：MsQuic vs WebSocket

本系统摒弃了传统的 WebSocket 信令方案，全面拥抱 QUIC 协议。

特性	MsQuic (本系统)	传统 WebSocket	优势分析
连接耗时	0-RTT / 1-RTT	3-RTT (TCP+TLS+WS)	秒开连接，重连极快
多路复用	单连接多流 (Stream)	需建立多个 TCP 连接	避免队头阻塞，资源占用低
弱网表现	优秀 (前向纠错)	较差 (丢包重传慢)	网络切换不断连 (连接迁移)
头部开销	QPACK 压缩	无 / 较大	传输效率更高
📅 平台支持计划

Windows: ✅ 完整支持 (当前核心)

Web/H5: ✅ 支持基础操控

Linux/macOS: 🗓️ 规划中 (基于 MsQuic 跨平台特性)
