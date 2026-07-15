# webrtc-signal-server

## 一、整体架构图

```mermaid
graph TB
    subgraph Client["客户端"]
        WS["WebSocket 客户端"]
        HTTP["HTTP 客户端"]
    end

    subgraph Network["网络层 - AsioProactors"]
        direction LR
        AP["AsioProactors<br/>多线程 IO 上下文池"]
        AP --> IOC1["io_context 0"]
        AP --> IOC2["io_context 1"]
        AP --> IOCn["io_context N"]
    end

    subgraph Server["信令服务层"]
        direction TB
        WSS["WebRTCSignalServer<br/>主服务入口"]
        WSS --> LB["负载均衡器<br/>round-robin"]
        WSS --> QC["TaskChannel<br/>全局任务队列"]
        
        LB --> MGR1["WebRTCSignalManager 0"]
        LB --> MGR2["WebRTCSignalManager 1"]
        LB --> MGRn["WebRTCSignalManager N"]
    end

    subgraph Manager["每个 Manager 内部"]
        direction LR
        MGR["WebRTCSignalManager"]
        MGR --> ACC["Acceptor<br/>TCP/WebSocket"]
        MGR --> HTTPACC["HttpAcceptor"]
        MGR --> LS["WebRTCLogicSystem"]
        MGR --> SOCK["WebRTCSignalSocket<br/>连接管理"]
        MGR --> MAP["webrtcSocketMap<br/>accountId → Socket"]
        MGR --> ROUTE["actorMappingIndex<br/>跨 Manager 路由缓存"]
    end

    subgraph Logic["逻辑处理层"]
        direction TB
        LS --> HANDLERS["Handler 注册表<br/>requestType → 处理函数"]
        LS --> HTTPHANDLERS["HttpHandler 注册表<br/>URL → 处理函数"]
        LS --> TASKQ["本地任务队列<br/>localTaskQueueSize"]
        LS --> MYSQL["WebRTCMysqlManagerPools"]
    end

    subgraph Infra["基础设施层"]
        direction LR
        MYSQL --> POOL["MySQL 连接池<br/>concurrent_channel"]
        POOL --> CONN1["WebRTCMysqlManager 0"]
        POOL --> CONN2["WebRTCMysqlManager 1"]
        
        LOG["异步日志系统<br/>AsioConcurrentQueue"]
        CFG["ConfigManager<br/>配置管理"]
        TCMALLOC["TCMalloc<br/>内存管理"]
    end

    subgraph Storage["存储"]
        DB[("MySQL 数据库")]
    end

    WS --> WSS
    HTTP --> WSS
    WSS --> AP
    AP --> Manager
    Manager --> Logic
    Logic --> Infra
    CONN1 --> DB
    CONN2 --> DB
```

---

## 二、核心业务流程 - 连接与消息处理

```mermaid
sequenceDiagram
    autonumber
    participant Client as WebSocket 客户端
    participant Acceptor as Acceptor
    participant Socket as WebRTCSignalSocket
    participant Manager as WebRTCSignalManager
    participant Logic as WebRTCLogicSystem
    participant TaskQ as TaskChannel
    participant Handlers as Handler 注册表
    participant MySQL as MySQL 连接池
    participant Target as 目标 Socket

    %% 连接建立阶段
    rect rgb(240, 248, 255)
        Note over Client,Target: 1. 连接建立与 WebSocket 握手
        Client->>Acceptor: TCP 连接
        Acceptor->>Socket: 创建 Socket
        Socket->>Socket: SSL 握手 (可选)
        Socket->>Socket: WebSocket 握手（包含账户校验）
        Socket->>Manager: 注册到 webrtcSocketMap
        Note right of Socket: 握手阶段即完成鉴权，无需额外注册超时
    end

    %% 注册阶段
    rect rgb(255, 248, 240)
        Note over Client,Target: 2. 注册 (requestType=0)
        Client->>Socket: {"requestType":0, "accountId":"userA"}
        Socket->>Logic: postTaskAsync(packet)
        Logic->>Handlers: webrtcHandlers[0]
        Handlers->>Socket: 设置 accountId, isRegistered=true
        Handlers->>Manager: webrtcSocketMap[accountId]=socket
        Handlers->>Manager: 全局注册 actorSocketMappingIndex
        Socket-->>Client: {"state":200, "message":"Register Successful"}
    end

    %% 消息转发阶段
    rect rgb(240, 255, 240)
        Note over Client,Target: 3. 消息转发 (requestType=1-7)
        Client->>Socket: {"requestType":1, "accountId":"userA", "targetId":"userB", ...}
        Socket->>Logic: postTaskAsync(packet)
        Logic->>TaskQ: 检查负载阈值
        alt 负载 < threshold
            Logic->>Handlers: 本地执行 forwardHandler
        else 负载 >= threshold
            Logic->>TaskQ: 入队全局任务队列
            TaskQ->>Handlers: 异步执行
        end
        Handlers->>Handlers: 解析 targetId
        alt targetId 在本 Manager
            Handlers->>Manager: webrtcSocketMap.find(targetId)
            Handlers->>Target: asyncWrite(转发消息)
        else targetId 在其他 Manager
            Handlers->>Manager: actorMappingIndex 查路由
            Handlers->>Manager: postTaskAsync(targetChannelIndex)
            Manager->>Target: 跨 Manager 转发
        end
        Target-->>Target: 响应 200 转发成功
    end

    %% 断开连接
    rect rgb(255, 240, 240)
        Note over Client,Target: 4. 断开清理
        Client->>Socket: 连接断开 / requestType=4
        Socket->>Manager: removeConnection(accountId, sessionId)
        Manager->>Manager: 从 webrtcSocketMap 移除
        Manager->>Manager: 全局索引清理
        Manager->>Socket: closeEvent()
        Socket->>Socket: 关闭队列、定时器、TCP 连接
    end
```

---

## 三、异步任务调度机制

```mermaid
flowchart TB
    subgraph Input["任务入口"]
        WS_REQ["WebSocket 消息"]
        HTTP_REQ["HTTP 请求"]
    end

    subgraph LogicSystem["WebRTCLogicSystem"]
        direction TB
        ENTER["postTaskAsync()<br/>postHttpTaskAsync()"]
        ENTER --> CHECK{"taskQueueSize<br/>>= threshold ?"}
        
        CHECK -->|否| LOCAL["本地执行<br/>localTaskQueueSize++"]
        CHECK -->|是| CHECK_LOCAL{"localTaskQueueSize<br/>>= threshold ?"}
        
        CHECK_LOCAL -->|否| LOCAL
        CHECK_LOCAL -->|是| GLOBAL["全局任务队列<br/>TaskChannel.enqueue()"]
        
        LOCAL --> EXEC["co_spawn 执行"]
        EXEC --> DONE["完成后<br/>localTaskQueueSize--"]
        DONE --> TRIGGER{"localTaskQueueSize<br/>== asyncThreshold ?"}
        TRIGGER -->|是| START["asyncTaskExecute()<br/>启动消费协程"]
        
        GLOBAL --> QUEUE["TaskChannel<br/>moodycamel队列 + channel"]
        QUEUE --> CONSUME["主循环消费"]
        CONSUME --> SPAWN["co_spawn 执行"]
    end

    subgraph Overload["过载保护"]
        GLOBAL --> FULL{"队列已满?"}
        FULL -->|是| REJECT["返回 503<br/>Service Busy"]
    end

    subgraph QueueConsumer["队列消费协程"]
        START --> LOOP["while asyncEvents"]
        LOOP --> DEQ["dequeue()"]
        DEQ -->|有任务| RUN["co_await func()"]
        DEQ -->|nullopt| EXIT["退出"]
        RUN --> CHECK_EXIT{"local queue depth<br/>>= exitThreshold ?"}
        CHECK_EXIT -->|是| PAUSE["暂停消费<br/>交由本地执行"]
        CHECK_EXIT -->|否| LOOP
    end

    WS_REQ --> ENTER
    HTTP_REQ --> ENTER
```

---

## 四、功能模块总览

```mermaid
graph LR
    subgraph Core["核心功能"]
        REG["用户注册<br/>requestType=0"]
        FWD["消息转发<br/>requestType=1-3,6-7"]
        CLOSE["断开连接<br/>requestType=4"]
        PING["心跳保活"]
    end

    subgraph HTTP["HTTP 管理接口"]
        OVERVIEW["/api/v1/managers/overview<br/>总览所有 Manager"]
        STAT["/api/v1/managers/stat<br/>查询指定 Manager 详情"]
        AUTH["Bearer Token 鉴权"]
    end

    subgraph Infra2["基础设施"]
        MYSQL_POOL["MySQL 连接池<br/>自动重连 + 心跳"]
        ASYNC_LOG["异步日志系统<br/>分级写入文件/控制台"]
        CONFIG["配置文件热加载"]
        TASK_SCHED["自适应任务调度<br/>本地/全局队列切换"]
    end

    subgraph Reliability["可靠性机制"]
        HANDSHAKE["WebSocket 握手校验<br/>鉴权前置"]
        KEEPALIVE["TCP Keep-Alive<br/>跨平台支持"]
        OVERLOAD_PROT["过载保护<br/>503 响应"]
        ROUTE_CACHE["路由缓存<br/>减少跨 Manager 查询"]
    end

    Core --> Infra2
    HTTP --> Infra2
    Infra2 --> Reliability
```

---

## 五、跨 Manager 路由机制

```mermaid
sequenceDiagram
    autonumber
    participant SrcSock as 源 Socket (Manager-A)
    participant SrcMgr as Manager-A
    participant LogicA as LogicSystem-A
    participant GlobalIdx as actorSocketMappingIndex
    participant DstMgr as Manager-B
    participant DstSock as 目标 Socket (Manager-B)

    Note over SrcSock,DstSock: 消息转发：accountId → targetId

    SrcSock->>LogicA: postTaskAsync(packet)
    LogicA->>LogicA: forwardHandler 处理

    alt 检查本地缓存 actorMappingIndex
        LogicA->>SrcSock: actorMappingIndex.find(targetId)
        alt 缓存命中
            LogicA->>DstMgr: 直接 postTaskAsync(cachedIndex)
            DstMgr->>DstSock: 查找并转发
        end
    end

    alt 本地未命中
        LogicA->>GlobalIdx: 查询全局注册表
        alt targetId 在全局索引中
            GlobalIdx-->>LogicA: 返回 targetChannelIndex
            LogicA->>DstMgr: postTaskAsync(targetChannelIndex)
            DstMgr->>DstSock: 查找并转发
            DstSock-->>SrcSock: 更新 actorMappingIndex
        else targetId 未注册
            GlobalIdx-->>LogicA: 404 Not Found
            LogicA-->>SrcSock: 返回 404 错误
        end
    end

    Note over SrcSock,DstSock: 每个 Socket 维护 actorMappingIndex<br/>作为跨 Manager 路由缓存
```

---
1. **多 Proactor 模型**：每个 IO 线程独立运行 `io_context`，通过 `AsioProactors` 管理线程池
2. **两级任务队列**：本地队列 + 全局队列，根据负载动态切换，防止单点瓶颈
3. **跨 Manager 路由**：`actorSocketMappingIndex` 全局索引 + 本地缓存，减少跨线程查询
4. **C++20 协程**：大量使用 `boost::asio::awaitable`，代码可读性强
5. **连接池**：MySQL 连接池带自动重连和心跳，`ScopedMysqlConnection` RAII 自动归还
