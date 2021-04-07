# BlockServerSimulator
#### 介绍
用于观察WorkSteal和短任务优先的实际效果。
尚未实现 fio_plugin
#### 软件架构
- main.cc： BlockServer reactor 主体代码
- client.cc：客户端示例代码
- bs_ipc_msg.h：消息定义  
#### 编译过程
0. 安装 gflags
1. 下载并编译 spdk-21.01.1
2. 按需修改 Makefile 里的 spdk 路径
3. make
#### 发行说明
v0.01 
- ./a.out -help 显示 server 帮助
- ./b.out -help 显示 client 帮助
