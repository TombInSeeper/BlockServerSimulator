# BlockServerSimulator
#### 介绍
用于观察WorkSteal和短任务优先的实际效果。
支持 fio_plugin。
#### 软件架构
- main.cc： BlockServer reactor 主体代码
- client.cc：客户端示例代码
- bs_ipc_msg.h：消息定义  
#### 编译过程
0. 安装 gflags,gperftools
1. clone 并编译 [spdk](https://github.com/spdk/spdk.git)(tag:21.01.1)，编译安装 spdk 附带的 intel-isal 库
2. clone、编译并安装 [fio](git://git.kernel.dk/fio.git)
3. 按需修改 Makefile 里的 spdk 和 fio 路径
4. make
#### 发行说明
v0.01 
- ./a.out -help 显示 server 帮助
- ./b.out -help 显示 client 帮助
