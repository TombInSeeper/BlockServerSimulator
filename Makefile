CC=gcc
CCpp=g++
Q=@

#Build Type
ver=release

CFLAGS= -D_GNU_SOURCE -fPIC -Wall -std=gnu11 -march=native -fno-strict-aliasing
CPPFLAGS= -D_GNU_SOURCE -fPIC -Wall -std=gnu++11 -march=native -fno-strict-aliasing

ifeq ($(ver), debug)
CFLAGS +=-g -O0  
CPPFLAGS +=-g -O0  
else
CFLAGS += -O3
CPPFLAGS += -O3
endif


#SPDK 项目目录
SPDK_DIR= /home/wuyue/spdk

#FIO 项目目录
FIO_DIR= /home/wuyue/fio


#适用于 SPDK-21.01
DPDK_LIB = -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf -lrte_mempool_ring -lrte_pci
DPDK_LIB += -lrte_bus_pci -lrte_kvargs -lrte_vhost -lrte_net -lrte_hash -lrte_telemetry
DPDK_LIB += -lrte_cryptodev -lrte_power -lrte_rcu

SPDK_BDEV_MOUDLES = -lspdk_bdev_malloc -lspdk_bdev_null -lspdk_bdev_delay -lspdk_bdev_nvme

SPDK_LIB = $(SPDK_BDEV_MOUDLES) \
	-lspdk_accel -lspdk_accel_ioat -lspdk_ioat\
	-lspdk_event_bdev -lspdk_event_accel -lspdk_event_vmd -lspdk_event_sock -lspdk_bdev \
	-lspdk_event -lspdk_thread -lspdk_util -lspdk_conf -lspdk_trace \
	-lspdk_log -lspdk_json -lspdk_jsonrpc -lspdk_rpc -lspdk_sock -lspdk_nvme\
	-lspdk_notify -lspdk_vmd -lspdk_env_dpdk \
	$(DPDK_LIB) 

DPDK_INC_DIR= $(SPDK_DIR)/dpdk/build/include
DPDK_LIB_DIR= $(SPDK_DIR)/dpdk/build/lib
SPDK_INC_DIR= $(SPDK_DIR)/include
SPDK_LIB_DIR= $(SPDK_DIR)/build/lib

SPDK_LINK_FLAGS= -L$(DPDK_LIB_DIR) -L$(SPDK_LIB_DIR) \
	-Wl,--whole-archive,-Bstatic $(SPDK_LIB) 

SYS_LIBS= -lnuma -luuid -lpthread -ldl -lrt -lpmem 

CPP_LIBS = -lgflags

#英特尔存储软件加速库
SYS_LIBS+= -lisal
SYS_LINK_FLAGS= -Wl,--no-whole-archive,-Bdynamic $(SYS_LIBS)

INC_FLAGS= -I. -I$(SPDK_INC_DIR) -I$(FIO_DIR) -I$(DPDK_INC_DIR)


####

MAKEFLAGS += --no-print-directory

C_SRCS +=$(wildcard *.c)
CPP_SRCS +=$(wildcard *.cc)

OBJS = $(C_SRCS:.c=.o)
OBJS += $(CPP_SRCS:.cc=.o)

DEPFLAGS = -MMD -MP -MF $*.d.tmp

# Compile first input $< (.c) into $@ (.o)
COMPILE_C=\
	$(Q)echo "  CC [$(ver)] $@";\
	$(CC) -o $@ $(INC_FLAGS) $(DEPFLAGS)  $(CFLAGS) -c $< && \
	mv -f $*.d.tmp $*.d && touch -c $@

COMPILE_CPP=\
	$(Q)echo "  C++ [$(ver)] $@";\
	$(CCpp) -o $@ $(INC_FLAGS) $(DEPFLAGS)  $(CPPFLAGS) -c $< && \
	mv -f $*.d.tmp $*.d && touch -c $@


# Link $^ and $(LIBS) into $@ (app/lib)
LINK_C_APP=\
	$(Q)echo "  LINK_APP [$(ver)] $@"; \
	$(CC) -o $@ $(INC_FLAGS) $(CFLAGS)  $^ $(SPDK_LINK_FLAGS) $(SYS_LINK_FLAGS)

LINK_C_SHARED_LIB=\
	$(Q)echo "  LINK_SHARED_LIB [$(ver)] $@"; \
	$(CC) -shared -o $@ $(INC_FLAGS) $(CFLAGS) $^ $(SPDK_LINK_FLAGS) $(SYS_LINK_FLAGS)

LINK_CPP_APP=\
	$(Q)echo "  LINK_APP [$(ver)] $@"; \
	$(CCpp) -o $@ $(INC_FLAGS) $(CFLAGS)  $^ $(SPDK_LINK_FLAGS) $(SYS_LINK_FLAGS) $(CPP_LIBS)

LINK_CPP_SHARED_LIB=\
	$(Q)echo "  LINK_SHARED_LIB [$(ver)] $@"; \
	$(CCpp) -shared -o $@ $(INC_FLAGS) $(CFLAGS) $^ $(SPDK_LINK_FLAGS) $(SYS_LINK_FLAGS) $(CPP_LIBS)



ALL = a.out b.out

.PHONY: all clean 


all : $(ALL)

a.out : main.o 
	$(LINK_CPP_APP)
b.out : client.o
	$(LINK_CPP_APP)

# 	echo $(SPDK_DIR)
# 	echo $(C_SRCS)
# 	echo $(CFLAGS)

%.o: %.c %.d
	$(COMPILE_C)

%.o: %.cc %.d
	$(COMPILE_CPP)


%d: ;

.PRECIOUS: $(OBJS)

-include $(OBJS:.o=.d)

clean:
	@rm -rf *.o *.d $(ALL)