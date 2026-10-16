# ============================================================================
#  Linux 文件 IO 作业: 空洞文件 + 模拟下载定点写入
#  make 使用说明:
#     make            编译程序
#     make run        编译并跑一遍“默认 256KB 顺序写入”演示
#     make demo       演示跳块(留空洞)写入 + 空洞统计
#     make test       执行完整自测脚本(sparse_test.sh)
#     make clean      清理可执行文件/日志/演示产物
#     make help       显示帮助
# ============================================================================

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -O2 -g -std=gnu99
CPPFLAGS += -D_FILE_OFFSET_BITS=64
LDFLAGS  ?=
LDLIBS   ?=

TARGET   := sparse_file
SRCS     := $(wildcard *.c)
OBJS     := $(SRCS:.c=.o)
DEPS     := $(OBJS:.o=.d)

# 演示用参数(可通过命令行覆盖, 例如: make run SIZE=512K DELAY=50)
SIZE     ?= 256K
CHUNK    ?= 1K
DELAY    ?= 0
FILE     ?= demo.bin

.PHONY: all run demo stride fill verify test clean distclean help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "==> 编译完成: ./$(TARGET)  (试试 ./$(TARGET) --help)"

# 头文件依赖自动生成
%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

# 顺序写入演示(模拟下载)
run: $(TARGET)
	./$(TARGET) -f $(FILE) -s $(SIZE) -c $(CHUNK) -d $(DELAY)

# 跳块写入: 只写偶数块, 其余保持空洞, 直观看到“空洞不占磁盘块”
stride: $(TARGET)
	./$(TARGET) -f demo_sparse.bin -s $(SIZE) -c $(CHUNK) --stride 2 --force

# 补写空洞: 只动空洞块, 已有数据的块不碰
fill: $(TARGET)
	./$(TARGET) -f demo_sparse.bin -s $(SIZE) -c $(CHUNK) --fill

# 校验 + 空洞统计
verify: $(TARGET)
	./$(TARGET) verify -f $(FILE) -s $(SIZE) -c $(CHUNK)

demo: $(TARGET)
	@echo "===== 1. 创建 256KB 空洞文件 ====="
	./$(TARGET) init -f demo_hole.bin -s $(SIZE)
	@echo "===== 2. 跳块写入, 观察空洞占比 ====="
	./$(TARGET) -f demo_sparse.bin -s $(SIZE) -c $(CHUNK) --stride 2 --force
	@echo "===== 3. 只补写空洞块 ====="
	./$(TARGET) -f demo_sparse.bin -s $(SIZE) -c $(CHUNK) --fill
	@echo "===== 4. 校验 + 最终统计 ====="
	./$(TARGET) verify -f demo_sparse.bin -s $(SIZE) -c $(CHUNK)
	@echo "===== 5. 系统工具交叉验证 ====="
	ls -ls demo_hole.bin demo_sparse.bin
	du -h demo_hole.bin demo_sparse.bin

test: $(TARGET)
	@sh ./sparse_test.sh

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)
	rm -f demo.bin demo_hole.bin demo_sparse.bin
	rm -f demo.bin.tmp demo_hole.bin.tmp demo_sparse.bin.tmp
	rm -f *.progress *.progress.tmp *.log
	rm -rf ./tmp_test

distclean: clean
	@echo "==> 已恢复干净目录"

help:
	@echo "可用目标: all run stride fill verify demo test clean distclean help"
	@echo "可覆盖变量: CC CFLAGS SIZE CHUNK DELAY FILE"
