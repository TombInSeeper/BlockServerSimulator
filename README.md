# BlockServerSimulator
#### 介绍
模拟 Block Server 服务行为。
#### 软件架构
main.cc： BlockServer reactor 主体代码
client.cc：客户端示例代码
bs_ipc_msg.h：一些消息定义  
#### 编译过程
0. 安装 gflags
1. 下载并编译 spdk-21.01.1
2. 按需修改 Makefile 里的 spdk 路径
3. make
#### 使用说明
v0.01 有许多硬编码逻辑以源代码为准
./a.out -help 显示 server 帮助
./b.out -help 显示 client 帮助
