# Linux 文件 IO 作业 —— 空洞(Sparse)文件 + 模拟下载的定点写入

> 用文件 IO 系统调用(`open/lseek/write/read/fsync/rename/stat`)做一个
> **逻辑 256KB 的空洞文件**, 再用**命令行传参**模拟“边下载边实时修改”:
> 每次只写 1KB, 其余区域保持空洞; 中途 `Ctrl-C` 记录“已完成偏移”,
> 下次运行**从断点继续**; 最后打印 **逻辑大小 / 实际占用块数 / 空洞占比**。

---

## 一、实验要求对照

| 题目要求 | 本程序的实现 | 对应文件/函数 |
| --- | --- | --- |
| 1. 生成空洞文件, 逻辑 256KB, 未写入区不占磁盘块 | `lseek(fd, size-1, SEEK_SET)` + `write(fd,"",1)`, 只写最后 1 字节, 中间 i_size 撑大但不分配块 | `sparse_file.c: create_hole_file()` / `cmd_init()` |
| 2. `lseek`+`write` 定点写入, 不破坏其余空洞 | 循环内 `lseek(fd, off, SEEK_SET)` 后只 `write` 一个 `chunk`, 跳到下一块再写, 中间空洞一个字节都不碰 | `sparse_file.c: cmd_write()` 主循环 |
| 3. 记录“已完成偏移”, Ctrl-C 后断点继续 | 每写完一块就把偏移原子写入 `download.progress`(`.tmp` + `fsync` + `rename`); `SIGINT/SIGTERM` 处理器置标志, 主循环收尾落盘; 再次运行读该文件续传 | `on_signal()` / `write_progress()` / `parse_progress()` |
| 4. 打印逻辑大小 / 实际占用块数 / 空洞占比 | `stat()` 取 `st_size` 与 `st_blocks`(单位 512B), 空洞占比 = 1 - 实际/逻辑 | `report_stats()` |
| 5. 含 README | 本文件 | `README.md` |
| 6. Makefile 制作 | `make / run / demo / stride / fill / verify / test / clean / help` | `Makefile` |
| 7. 自写日志 | 不使用 syslog, 自己以 `O_APPEND` 打开日志文件, 每条一次 `write()` 追加 `[时间.毫秒] [级别] [pid] 消息`, 同时镜像到终端 | `log_init()/log_msg()/log_close()` |
| 8. 写入 Linux作业2 文件夹 | 全部源码、Makefile、README、自测脚本都在本目录 | 本目录 |

额外加的: `verify` 子命令(逐块读回校验内容 + 统计空洞)、`--stride` 跳块写入
(直观演示“空洞不占磁盘块”)、`--fill` 只补空洞(证明定点写入不碰其它区域)、
`SIGUSR1` 运行时打印统计、`sparse_test.sh` 自动验收脚本。

---

## 二、原理: 为什么“空洞”不占磁盘块

Linux 的 ext2/3/4、XFS、btrfs 等文件系统支持 **稀疏文件(sparse file)**。
文件大小(`inode.i_size`)和“占用了多少磁盘块”是两件独立的事:

1. 普通写入 `write(fd, buf, n)` 会把数据落到文件系统的数据块上;
2. 而 **`lseek(fd, 很大偏移, SEEK_SET)` 只是移动文件读写位置**,
   只要后面写入的位置超过当前文件尾, 内核就把 `i_size` 改成新位置,
   **中间没有写过的区域不会被分配数据块**, 读出来一律是 0 —— 这就是“空洞”。
3. 空洞所在的最后一个数据块里那 1 个字节(`write(fd,"",1)`)仍然会占一个块,
   所以用 `stat` 看是“占 1 个块, 逻辑 256KB”, 用 `du` 看几乎不占空间。

```
逻辑大小 (st_size)            = 262144 字节       ← 文件“看起来”是 256KB
实际占用 (st_blocks * 512)    ≈ 4096 字节 (1 块)  ← 真正从磁盘要走的
空洞大小 (st_size - 实际占用) ≈ 258048 字节
空洞占比                      ≈ 98.44%
```

**文件系统块大小的意义**: 空洞的最小粒度是文件系统的块(通常 4KB)。
写 1KB 数据实际上会让整个 4KB 块被分配。所以用 `-c 1K` 连续写 4 次,
就把同一个 4KB 块写满了 —— 这也是 `--stride 2`(每 2 块写 1 块)能明显
看出空洞、而连续写看不出空洞的原因。

---

## 三、文件清单

| 文件 | 说明 |
| --- | --- |
| `sparse_file.c` | 全部实现: 空洞创建 + 定点写入 + 断点续传 + 统计 + 自写日志 |
| `Makefile` | 编译与演示目标 |
| `README.md` | 本文件 |
| `sparse_test.sh` | 自动验收脚本(`make test` 调用) |
| `demo*.bin` / `*.progress` / `*.log` | 运行后生成的演示数据、进度文件、日志(`make clean` 清除) |

---

## 四、编译与运行

```bash
# 编译
make                     # 等价于 gcc -Wall -Wextra -O2 -g -std=gnu99 -o sparse_file sparse_file.c

# 查看用法
./sparse_file --help
```

### 4.1 命令行参数(要求“命令行传参”)

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `-f, --file <路径>` | 目标文件 | `sparse_download.bin` |
| `-s, --size <大小>` | 逻辑大小, 支持 `256K`/`1M`/`128K`/`65536` | `256K` |
| `-c, --chunk <大小>` | 单次定点写入的块大小(至少 32 字节, 要放块头) | `1K` |
| `-p, --progress <路径>` | 进度文件(记录“已完成偏移”) | `download.progress` |
| `-l, --log <路径>` | 日志文件, `-` 表示只输出终端 | `sparse_file.log` |
| `-d, --delay <毫秒>` | 每块之间的延迟, 模拟慢速网络 | `0` |
| `--stride <N>` | 每 N 块写 1 块(2 = 跳块留空洞) | `1` |
| `--fill` | 只补写还是 0 的空洞块, 已有数据的块不动 | 关 |
| `-q, --quiet` | 不打印每块进度 | 关 |
| `--no-fsync` | 每块写完后不 fsync | 关(默认 fsync) |
| `--force` | 忽略旧进度文件, 从头开始 | 关 |

子命令: `write`(默认)、`init`、`verify`、`stat`、`reset`、`help`。

### 4.2 完整演示(建议按顺序执行)

```bash
# ① 生成 256KB 空洞文件: 未写入区不占磁盘块
./sparse_file init -f demo.bin -s 256K
ls -ls demo.bin          # 第一列是占用的 1KB 块数, 会很小
du -h demo.bin           # 占用 4.0K, 而逻辑大小是 256K

# ② 模拟下载: 每块 200ms, 方便中途 Ctrl-C
./sparse_file -f demo.bin -s 256K -c 1K -d 200
#    ... 进度走到一半时按 Ctrl-C ...
#    ==> 收到 SIGINT, 断点已保存: 已完成偏移 xxKB

cat download.progress    # state=incomplete / off=xxx / total=262144 ...

# ③ 再次运行同一条命令: 从断点继续, 前面的数据不会被重写
./sparse_file -f demo.bin -s 256K -c 1K -d 200
#    日志会显示 “发现进度文件 ... 断点续传: 从该位置继续”

# ④ 校验内容 + 看最终统计
./sparse_file verify -f demo.bin -s 256K -c 1K
```

### 4.3 直观演示“空洞不占磁盘块”(跳块写入 + 只补空洞)

```bash
# 只写 0,2,4,... 号块, 奇数块保持空洞 => 立刻能看到约一半是空洞
./sparse_file -f demo_sparse.bin -s 256K -c 1K --stride 2 --force
#  打印: 逻辑大小 256.00KB / 实际占用块数 ... / 空洞占比 xx%

# 只补空洞: 已有数据的块一个字节都不动(证明定点写入不破坏其它区域)
./sparse_file -f demo_sparse.bin -s 256K -c 1K --fill

# 或者一条命令跑完整演示
make demo
```

### 4.4 自动验收

```bash
make test       # 运行 sparse_test.sh, 逐项检查 8 个要求
```

---

## 五、核心实现片段

### 5.1 生成空洞(只写最后 1 字节)

```c
fd = open(path, O_WRONLY | O_CREAT, 0644);
lseek(fd, size - 1, SEEK_SET);   /* 越过文件尾, 不分配数据块 */
write(fd, "", 1);                /* 只写最后一个字节, i_size = size */
fsync(fd);
```

### 5.2 定点写入(不破坏其它空洞)

```c
for (off = 断点; off < 逻辑大小; off += chunk) {
    lseek(fd, off, SEEK_SET);            /* 定点: 直接跳到 off */
    write(fd, chunk_buf, chunk);         /* 只覆盖这一块 */
    write_progress(off + chunk);         /* 立刻记录“已完成偏移” */
}
```

每个数据块都带一个 32 字节的块头(`"SPARSEv1"` + 偏移 + 块长 + 总长),
其余字节用根据偏移算出的散列填充, 这样 `verify` 能逐块判断
“这块是写过的(内容正确)/ 还是空洞(全 0)/ 还是被写坏了”。

### 5.3 断点续传与 Ctrl-C

```c
/* 信号处理器只做两件事: 置标志 + 异步安全地写一行提示 */
static void on_signal(int signo) { g_stop_flag = 1; write(1, "...", n); }

/* 主循环检测到标志后: fsync + 落地进度 + 打印断点, 然后正常返回 0 */
if (interrupted) {
    fsync(fd);
    write_progress(progress_tmp, progress, done, total, chunk, stride, 0);
}
```

进度文件的写入是 **“写 `.tmp` → `fsync` → `rename()` 原子替换”**,
所以即使写到一半被 `kill -9` 或掉电, 磁盘上的 `download.progress`
也总是一份完整的记录, 不会出现半截数据导致续传偏移错乱。

```
$ cat download.progress
state=incomplete      # complete 表示整个文件已写完
off=53248             # 已完成偏移(断点), 下次从这里继续
total=262144          # 逻辑大小
chunk=1024            # 块大小
stride=1              # 步长
```

### 5.4 统计(逻辑大小 / 实际占用块数 / 空洞占比)

```c
stat(path, &st);
逻辑大小   = st.st_size;
实际占用   = st.st_blocks * 512;            /* st_blocks 的单位固定是 512B */
空洞大小   = 逻辑大小 - 实际占用;
空洞占比   = 空洞大小 * 100.0 / 逻辑大小;
```

### 5.5 自写日志

```c
g_log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
/* 每条日志一次 write() 追加, 同时镜像到 stdout */
write_all(STDOUT_FILENO, line, len);
write_all(g_log_fd,      line, len);
```

日志格式(`sparse_file.log`):

```
[2024-05-20 15:04:05.123] [INFO ] [pid:12345] ================ 开始模拟下载 ================
[2024-05-20 15:04:05.124] [INFO ] [pid:12345] 逻辑大小 262144 字节 (256.00KB), 块大小 1024 字节, 块间延迟 200 ms
[2024-05-20 15:04:05.325] [INFO ] [pid:12345]   已处理   0.39% (1/256 块), 下一偏移 1.00KB(1024 字节)
[2024-05-20 15:04:05.987] [WARN ] [pid:12345] ==> 收到 SIGINT, 断点已保存: 已完成偏移 5.00KB(5120 字节)
```

---

## 六、结果验证方法(可与系统工具交叉核对)

```bash
./sparse_file init -f demo.bin -s 256K
stat -c '逻辑=%s 字节, 占用=%b 块(512B)' demo.bin   # 逻辑 262144, 占用 8 块 = 4KB
du -h   demo.bin                                   # 4.0K
ls -ls  demo.bin                                   # 第一列 4 (KB)
du --apparent-size -h demo.bin                     # 256K (逻辑大小)
```

- **`stat` 的 `st_blocks`** 是内核给出的“真实占用块数”, 与程序打印的一致;
- `du` 走的就是 `st_blocks`, 所以 `du` 远小于文件大小, 直接证明“空洞不占磁盘块”;
- `du --apparent-size` 显示逻辑大小, 与 `st_size` 一致。

---

## 七、常见问题与说明

1. **连续写完 256KB 后空洞占比是 0%?**
   正常。跳块(`--stride 2`)才留空洞; 顺序写入会把每个 4KB 文件系统块填满。
   想看空洞请用 `init` 建新文件, 或用 `--stride 2` / 写到一半 `Ctrl-C`。
2. **`st_blocks` 为什么按 512B 计?**
   POSIX 规定 `st_blocks` 的单位是 512 字节, 与文件系统块大小(4KB)无关,
   程序里用 `st_blocks * 512` 换算成字节。
3. **跳块写入时 `实际占用` 可能大于逻辑大小?**
   会出现。每 1KB 数据落在独立的 4KB 块里(中间隔着空洞, 无法合并),
   128 个块 × 4KB = 512KB > 256KB; `st_blocks` 可以大于 `st_size`,
   这是稀疏文件的正常表现, 程序会把空洞占比下限截到 0%。
4. **在 tmpfs/内存文件系统上测试?**
   也支持空洞, 但 `du` 统计的是页(page)占用, 数值会与磁盘文件系统略有差异。
5. **中断后想从头开始?**
   `./sparse_file reset -f demo.bin` 删除数据文件与进度文件, 或加 `--force`。
6. **块大小能不能小于 32 字节?**
   不行, 每块要放 32 字节块头, 程序会拒绝并提示最小块大小。

---

## 八、知识点小结

- 文件 IO 系统调用: `open/close/read/write/pread/lseek/fsync/ftruncate/stat/unlink/rename`
- **稀疏文件与空洞**: `i_size` 与数据块分配解耦; `lseek` 越界不分配块
- **定点写入**: `lseek(SEEK_SET)` 与当前偏移无关, 只覆盖目标区间
- **`SEEK_DATA`/`SEEK_HOLE`**: 内核直接告诉程序哪段是空洞, 补空洞时可跳过已有数据
- **信号与可重入**: `SIGINT/SIGTERM/SIGUSR1` + `sigaction`, 只调用异步信号安全函数
- **崩溃一致性**: “临时文件 + fsync + rename” 实现原子更新进度
- **工程化**: Makefile 多目标、自动依赖(`-MMD -MP`)、shell 自动验收脚本
