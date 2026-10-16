/*
 * sparse_file.c -- 空洞(Hole/Sparse)文件生成 + 模拟下载的实时定点写入
 * ---------------------------------------------------------------------------
 * Linux 文件 IO 课程作业
 *
 * 1) 以 256KB(默认)为逻辑大小创建空洞文件: 用 lseek() 定位到文件末尾之后,
 *    内核只更新 inode 的 i_size, 不为其分配数据块 => 未写入区不占磁盘块。
 * 2) 模拟下载: 命令行传参指定目标文件/大小/块大小/延迟, 循环用
 *    lseek(fd, offset, SEEK_SET) + write(fd, chunk, CHUNK) 定点写入,
 *    每次只覆盖 1KB, 其余空洞完全不受影响(且 i_size 只增不减)。
 * 3) 断点续传: 每写完一块就把“已完成偏移”原子地写进进度文件;
 *    Ctrl-C(SIGINT) 被捕获后落地进度并优雅退出; 再次运行自动从断点继续。
 * 4) 统计输出: 逻辑大小 / 实际占用块数 / 空洞占比, 与自写日志一起打印。
 *
 * 编译: make          (等价: gcc -Wall -Wextra -O2 -o sparse_file sparse_file.c)
 * 用法: ./sparse_file --help
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ======================= 常量与全局状态 ======================= */

#define DEFAULT_SIZE      (256LL * 1024)   /* 逻辑大小: 256KB */
#define DEFAULT_CHUNK     1024             /* 每次定点写入 1KB */
#define DEFAULT_DELAY_MS  0                /* 块间延迟(毫秒), 模拟慢速下载 */
#define DEFAULT_TARGET    "sparse_download.bin"
#define DEFAULT_PROGRESS  "download.progress"
#define DEFAULT_LOGFILE   "sparse_file.log"
#define SECTOR_SIZE       512              /* st_blocks 的单位固定是 512B */
#define PROGRESS_TMP_TAIL ".tmp"

#define BUFSZ             1024
#define PROGRESS_BUFSZ    4096

/* 数据块头部: 标记 + 偏移 + 总长 + 块长, 便于 verify 逐块校验 */
#define MARK_SIZE 8
#define HEAD_SIZE (MARK_SIZE + 8 + 8 + 8)

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

static const char *const g_level_name[] = { "DEBUG", "INFO", "WARN", "ERROR" };

/* 程序级全局状态(信号处理器与各函数共享的少量变量) */
static volatile sig_atomic_t g_stop_flag = 0;     /* 是否收到退出信号 */
static const char *g_stop_signame = "SIGINT";     /* 触发退出的信号名 */
static volatile sig_atomic_t g_want_stats = 0;    /* SIGUSR1: 打印统计 */
static int g_log_fd = -1;                         /* 日志文件 fd(自写日志) */
static pid_t g_pid = 0;                           /* 缓存 pid, 避免信号里调用 */

static long long g_logical_size = DEFAULT_SIZE;   /* 目标逻辑大小 */
static long long g_chunk_size = DEFAULT_CHUNK;    /* 单次写入块大小 */
static long long g_delay_ms = DEFAULT_DELAY_MS;   /* 块间延迟 */
static long long g_stride = 1;                    /* 块间步长(1=连续下载, 2=跳块) */
static int g_quiet = 0;                           /* 1: 不打印每块进度 */
static int g_fill = 0;                            /* 1: 补写所有空洞块 */
static int g_do_fsync = 1;                        /* 1: 每块写完 fsync */
static int g_force = 0;                           /* 1: 忽略旧进度重新开始 */

static long long g_run_written = 0;               /* 本次运行写入字节 */
static long long g_run_chunks = 0;                /* 本次运行写入块数 */

/* ======================= 自写日志模块 ======================= */

/*
 * 自写日志: 不依赖 syslog, 直接以 O_APPEND 打开日志文件,
 * 每条日志用一次 write() 追加一行 "[时间.毫秒] [级别] [pid] 消息",
 * 同时镜像到标准输出, 便于前台演示。
 */
static void log_init(const char *path)
{
    if (path != NULL && path[0] != '\0' && strcmp(path, "-") != 0) {
        g_log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_log_fd < 0) {
            fprintf(stderr, "[WARN ] 打开日志文件 %s 失败: %s(日志仅输出到终端)\n",
                    path, strerror(errno));
        }
    }
}

static void log_close(void)
{
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

static void write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void log_msg(log_level_t lv, const char *fmt, ...)
{
    char line[BUFSZ];
    char out[BUFSZ + 256];
    char ts[64];
    struct tm tmv;
    struct timespec now_ts;
    time_t now;
    va_list ap;
    int n;
    int m;

    if (lv < LOG_DEBUG) {
        lv = LOG_DEBUG;
    }
    if (lv > LOG_ERROR) {
        lv = LOG_ERROR;
    }

    clock_gettime(CLOCK_REALTIME, &now_ts);
    now = now_ts.tv_sec;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;          /* 超长消息截断 */
    }

    m = snprintf(out, sizeof(out), "[%s.%03ld] [%-5s] [pid:%ld] %.*s\n",
                 ts, now_ts.tv_nsec / 1000000L, g_level_name[lv],
                 (long)g_pid, n, line);
    if (m > 0) {
        write_all(STDOUT_FILENO, out, (size_t)m);
        if (g_log_fd >= 0) {
            write_all(g_log_fd, out, (size_t)m);
        }
    }
}

/* ======================= 通用小工具 ======================= */

/* 解析 "256K" / "1M" / "128k" / "65536"(字节) */
static int parse_size(const char *s, long long *out)
{
    char *copy;
    char *end = NULL;
    long long val;
    size_t len;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    copy = strdup(s);
    if (copy == NULL) {
        return -1;
    }
    for (char *p = copy; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    len = strlen(copy);
    if (len > 0 && copy[len - 1] == 'b') {   /* 允许 "256kb" */
        copy[len - 1] = '\0';
    }

    errno = 0;
    val = strtoll(copy, &end, 10);
    if (errno != 0 || end == copy || val < 0) {
        free(copy);
        return -1;
    }
    if (*end != '\0') {                      /* 后面只允许一个单位字符 */
        if (end[1] != '\0') {
            free(copy);
            return -1;
        }
        switch (*end) {
        case 'k': val *= 1024LL; break;
        case 'm': val *= 1024LL * 1024LL; break;
        case 'g': val *= 1024LL * 1024LL * 1024LL; break;
        default:
            free(copy);
            return -1;
        }
    }
    free(copy);
    if (val <= 0) {
        return -1;
    }
    *out = val;
    return 0;
}

/* 读取进度文件里 "field=value" 的整数值 */
static int read_size_field(const char *path, const char *field, long long *out)
{
    FILE *fp;
    char line[512];
    size_t flen = strlen(field);

    *out = 0;
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *nl;
        if (strncmp(line, field, flen) != 0 || line[flen] != '=') {
            continue;
        }
        nl = strchr(line + flen + 1, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        *out = strtoll(line + flen + 1, NULL, 10);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

/*
 * 解析进度文件。
 * found=0 表示“没有进度(第一次运行)”; 返回 -1 表示文件存在但不可用。
 */
static int parse_progress(const char *path, long long *off, long long *total,
                          long long *chunk, long long *stride, int *found)
{
    struct stat st;

    *off = 0;
    *total = 0;
    *chunk = -1;
    if (stride != NULL) {
        *stride = -1;
    }
    *found = 0;

    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return 0;                       /* 正常: 第一次运行 */
        }
        if (errno == EACCES) {
            log_msg(LOG_WARN, "进度文件 %s 不可读(%s), 视为无进度",
                    path, strerror(errno));
            return 0;
        }
        log_msg(LOG_ERROR, "stat 进度文件 %s 失败: %s", path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        log_msg(LOG_ERROR, "进度文件 %s 不是普通文件, 拒绝使用", path);
        return -1;
    }

    (void)read_size_field(path, "off", off);
    (void)read_size_field(path, "total", total);
    (void)read_size_field(path, "chunk", chunk);
    if (stride != NULL) {
        (void)read_size_field(path, "stride", stride);
    }
    *found = 1;
    return 0;
}

/*
 * 更新“已完成偏移”: 写临时文件 + fsync + rename() 原子替换,
 * 保证任何时刻磁盘上的进度文件都是一份完整可用的记录。
 */
static int write_progress(const char *tmp, const char *dst, long long off,
                          long long total, long long chunk, long long stride,
                          int complete)
{
    char buf[512];
    int fd;
    int n;
    int rc = 0;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_msg(LOG_ERROR, "创建进度文件 %s 失败: %s", tmp, strerror(errno));
        return -1;
    }

    n = snprintf(buf, sizeof(buf),
                 "state=%s\noff=%lld\ntotal=%lld\nchunk=%lld\nstride=%lld\n",
                 complete ? "complete" : "incomplete", off, total, chunk, stride);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        rc = -1;
    } else if (write(fd, buf, (size_t)n) != (ssize_t)n) {
        log_msg(LOG_ERROR, "写进度文件 %s 失败: %s", tmp, strerror(errno));
        rc = -1;
    } else if (fsync(fd) != 0) {
        log_msg(LOG_WARN, "fsync 进度文件失败: %s", strerror(errno));
    }
    close(fd);

    if (rc == 0 && rename(tmp, dst) != 0) {
        log_msg(LOG_ERROR, "rename %s -> %s 失败: %s", tmp, dst, strerror(errno));
        rc = -1;
    }
    return rc;
}

/* 把偏移格式化成 "12.50KB(12800 字节)" 便于阅读 */
static void format_offset(long long off, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%.2fKB(%lld 字节)", (double)off / 1024.0, off);
}

/*
 * 在 [scan_from, total) 中计算本次运行要处理的块数:
 *   fill 模式   : 从 k 开始的每一块都算(把剩下所有空洞都补上)
 *   stride 模式 : 只算 idx = k, k+stride, k+2*stride, ...
 */
static long long count_blocks(long long scan_from, long long total,
                              long long chunk, long long stride)
{
    long long k;
    long long n;

    if (scan_from < 0 || scan_from >= total) {
        return 0;
    }
    k = (scan_from + chunk - 1) / chunk;        /* 向上对齐到块边界 */
    n = (total + chunk - 1) / chunk;            /* 总块数(允许最后一块不满) */
    if (g_fill) {
        return (n > k) ? (n - k) : 0;
    }
    if (stride < 1) {
        stride = 1;
    }
    if (k >= n) {
        return 0;
    }
    return (n - 1 - k) / stride + 1;
}

/*
 * 在 [from, total) 中找下一个“按 chunk 对齐且尚未写入”的偏移。
 * stride=1 时它就是 from 本身(连续下载); stride>1 时会跳过中间的块,
 * 被跳过的那部分保持空洞; fill 模式则一路扫过去, 把空洞块都写上。
 */
static long long next_block_offset(long long from, long long total,
                                   long long chunk, long long stride)
{
    long long idx;
    long long n;

    if (from >= total) {
        return total;
    }
    idx = (from + chunk - 1) / chunk;          /* 向上对齐到块边界 */
    if (g_fill) {
        if (idx * chunk >= total) {
            return total;
        }
        return idx * chunk;                    /* 逐块扫描, 空洞判断交给调用者 */
    }
    if (stride < 1) {
        stride = 1;
    }
    n = idx / stride + 1;                      /* 跳到第 n 个步长块 */
    return n * stride * chunk;
}

/* 用 djb2 散列生成确定性的“下载数据”填充字节 */
static unsigned char pattern_byte(long long off, long long chunk, long long idx)
{
    unsigned long long h = 5381ULL;
    unsigned char tmp[24];
    unsigned char *p = tmp;
    size_t i;

    memset(tmp, 0, sizeof(tmp));
    memcpy(tmp + 0, &off, sizeof(off));
    memcpy(tmp + 8, &chunk, sizeof(chunk));
    memcpy(tmp + 16, &idx, sizeof(idx));
    for (i = 0; i < sizeof(tmp); i++) {
        h = ((h << 5) + h) + (unsigned long long)p[i];
    }
    return (unsigned char)(h & 0xFFu);
}

/* 构造一个数据块: 头部记录 offset/chunk/总长, 其余用散列填充(非 0, 便于校验) */
static void build_chunk(unsigned char *buf, long long off,
                        long long total, long long chunk)
{
    long long i;
    unsigned char *p = buf;

    memcpy(p, "SPARSEv1", MARK_SIZE);
    p += MARK_SIZE;
    memcpy(p, &off, sizeof(off));       p += sizeof(off);
    memcpy(p, &chunk, sizeof(chunk));   p += sizeof(chunk);
    memcpy(p, &total, sizeof(total));   p += sizeof(total);

    for (i = 0; i < chunk - HEAD_SIZE; i++) {
        buf[HEAD_SIZE + i] = pattern_byte(off, chunk, i);
    }
}

/* 校验读回的块内容与写入时是否一致 */
static int chunk_matches(const unsigned char *buf, long long off,
                         long long total, long long chunk)
{
    long long i;
    const unsigned char *p = buf;

    if (memcmp(p, "SPARSEv1", MARK_SIZE) != 0) {
        return 0;
    }
    p += MARK_SIZE;
    if (memcmp(p, &off, sizeof(off)) != 0) {
        return 0;
    }
    p += sizeof(off);
    if (memcmp(p, &chunk, sizeof(chunk)) != 0) {
        return 0;
    }
    p += sizeof(chunk);
    if (memcmp(p, &total, sizeof(total)) != 0) {
        return 0;
    }

    for (i = 0; i < chunk - HEAD_SIZE; i++) {
        if (buf[HEAD_SIZE + i] != pattern_byte(off, chunk, i)) {
            return 0;
        }
    }
    return 1;
}

/* ======================= 统计输出(要求 4) ======================= */

/*
 * 逻辑大小 : stat.st_size          —— 文件“看起来”有多大
 * 实际占用 : stat.st_blocks * 512  —— 真正分配的磁盘块(1 块 = 512B)
 * 空洞占比 : 1 - 实际占用 / 逻辑大小
 */
static void report_stats(const char *path, time_t t_start)
{
    struct stat st;
    long long logical;
    long long physical;
    long long hole;
    double hole_ratio;

    if (stat(path, &st) != 0) {
        log_msg(LOG_ERROR, "stat %s 失败: %s", path, strerror(errno));
        return;
    }

    logical = (long long)st.st_size;
    physical = (long long)st.st_blocks * SECTOR_SIZE;
    hole = (logical > physical) ? (logical - physical) : 0;
    hole_ratio = (logical > 0) ? (double)hole * 100.0 / (double)logical : 0.0;

    log_msg(LOG_INFO, "================ 空洞文件统计 ================");
    log_msg(LOG_INFO, "文件路径        : %s", path);
    log_msg(LOG_INFO, "文件系统块大小  : %ld 字节 (st_blksize, 空洞最小粒度)",
            (long)st.st_blksize);
    log_msg(LOG_INFO, "逻辑大小        : %lld 字节 = %.2fKB (%.3fMB)",
            logical, (double)logical / 1024.0,
            (double)logical / (1024.0 * 1024.0));
    log_msg(LOG_INFO, "实际占用块数    : %lld 块(512B/块) = %lld 字节 = %.2fKB",
            (long long)st.st_blocks, physical, (double)physical / 1024.0);
    log_msg(LOG_INFO, "空洞大小        : %lld 字节 = %.2fKB", hole, (double)hole / 1024.0);
    log_msg(LOG_INFO, "空洞占比        : %.2f%%", hole_ratio);
    if (hole == 0) {
        log_msg(LOG_INFO, "说明            : 当前逻辑区域已被写满, 没有空洞;"
                          " 可用 --stride 2 跳块写入来观察空洞, 或用 init 建全新空洞文件");
    }
    log_msg(LOG_INFO, "inode 信息      : 链接数=%lu, 权限=%04o, 设备号=%lu",
            (unsigned long)st.st_nlink, (unsigned int)(st.st_mode & 07777u),
            (unsigned long)st.st_dev);
    if (t_start != (time_t)-1) {
        time_t now = time(NULL);
        log_msg(LOG_INFO, "本次耗时        : %lld 秒, 本次写入 %lld 字节 / %lld 块",
                (long long)(now - t_start), g_run_written, g_run_chunks);
    }
    log_msg(LOG_INFO, "=============================================");
}

/* ======================= 信号处理(要求 3) ======================= */

/*
 * 信号处理器里只做“置标志 + 异步安全地写一行提示”, 真正的收尾
 * (fsync、落地进度文件、打印统计)全部留给主循环, 避免在信号里调用
 * 非异步安全的函数。
 */
static void on_signal(int signo)
{
    /* 只调用异步信号安全的函数: write() 追加一行提示, 主循环随后收尾 */
    static const char msg_int[] = "\n[信号] 收到 SIGINT, 正在保存进度并准备安全退出...\n";
    static const char msg_term[] = "\n[信号] 收到 SIGTERM, 正在保存进度并准备安全退出...\n";
    const char *msg = msg_int;
    size_t len;

    if (signo == SIGINT) {
        g_stop_signame = "SIGINT";
    } else if (signo == SIGTERM) {
        g_stop_signame = "SIGTERM";
        msg = msg_term;
    } else {
        g_stop_signame = "SIGUSR1";
    }
    g_stop_flag = 1;
    len = strlen(msg);          /* strlen 是异步信号安全的 */
    {
        ssize_t r = write(STDOUT_FILENO, msg, len);
        (void)r;
    }
}

static void on_sigterm(int signo)
{
    on_signal(signo);
}

static void on_sigusr1(int signo)
{
    static const char msg[] = "\n[信号] 收到 SIGUSR1, 将打印当前统计信息\n";

    (void)signo;
    g_want_stats = 1;
    {
        ssize_t r = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        (void)r;
    }
}

static void install_signals(void)
{
    struct sigaction sa;
    struct sigaction ign;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            /* 不用 SA_RESTART: 让 write/nanosleep 可被打断 */
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        log_msg(LOG_WARN, "注册 SIGINT 处理器失败: %s", strerror(errno));
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        log_msg(LOG_WARN, "注册 SIGTERM 处理器失败: %s", strerror(errno));
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        log_msg(LOG_WARN, "注册 SIGUSR1 处理器失败: %s", strerror(errno));
    }

    /* 后台运行时忽略 SIGPIPE, 避免管道关闭直接杀死进程 */
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &ign, NULL);
}

/* 可被信号打断的毫秒睡眠(被打断后继续睡剩余时间) */
static void msleep(long ms)
{
    struct timespec req;

    if (ms <= 0) {
        return;
    }
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR || g_stop_flag) {
            return;
        }
    }
}

/* ======================= 帮助信息 ======================= */

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
"空洞(Sparse)文件生成 + 模拟下载定点写入工具              v1.0\n"
"\n"
"用法:\n"
"  %s [write]  [选项]      生成空洞文件并模拟下载写入(默认子命令)\n"
"  %s init     [选项]      只创建空洞文件, 不写数据\n"
"  %s verify   [选项]      校验已写入数据 + 打印空洞统计\n"
"  %s stat     [选项]      只打印逻辑大小/实际占用块数/空洞占比\n"
"  %s reset    [选项]      删除目标文件与进度文件(重新开始)\n"
"\n"
"选项(命令行传参):\n"
"  -f, --file <路径>      目标文件(默认: %s)\n"
"  -s, --size <大小>      逻辑大小, 支持 256K/1M/128K/65536(默认: 256K)\n"
"  -c, --chunk <大小>     单次定点写入块大小(默认: 1K, 至少 %d 字节)\n"
"  -p, --progress <路径>  进度文件, 保存“已完成偏移”(默认: %s)\n"
"  -l, --log <路径>       日志文件(默认: %s; '-' 表示只输出终端)\n"
"  -d, --delay <毫秒>     每块之间延迟, 模拟慢速下载(默认: 0)\n"
"      --stride <N>       步长: 每 N 块写 1 块(默认 1=连续下载; 2=跳块留空洞)\n"
"      --fill             只补写文件里还没写过的空洞块(可与 --stride 2 配合)\n"
"  -q, --quiet            不打印每块进度, 只保留日志\n"
"      --no-fsync         每块写完后不 fsync(默认 fsync, 断电更安全)\n"
"      --force            忽略旧进度文件, 从头开始写\n"
"  -h, --help             显示本帮助\n"
"\n"
"示例:\n"
"  make && ./sparse_file -f demo.bin -s 256K -c 1K -d 200   # 每块 200ms, 中途 Ctrl-C\n"
"  ./sparse_file -f demo.bin -d 200                         # 再次运行: 从断点继续\n"
"  ./sparse_file verify -f demo.bin                         # 校验 + 空洞统计\n"
"  ./sparse_file -f demo.bin --stride 2                     # 跳块写: 立刻看到大比例空洞\n"
"  ./sparse_file -f demo.bin --fill                         # 只补空洞, 不动已写块\n"
"  ls -ls demo.bin ; du -h demo.bin ; stat demo.bin         # 交叉验证空洞\n",
            argv0, argv0, argv0, argv0, argv0,
            DEFAULT_TARGET, HEAD_SIZE, DEFAULT_PROGRESS, DEFAULT_LOGFILE);
}

/* ======================= 选项解析 ======================= */

typedef struct {
    const char *target;
    const char *progress;
    const char *logfile;
    int help;
    int error;
} opts_t;

static void opts_default(opts_t *o)
{
    o->target = DEFAULT_TARGET;
    o->progress = DEFAULT_PROGRESS;
    o->logfile = DEFAULT_LOGFILE;
    o->help = 0;
    o->error = 0;
}

/*
 * 解析选项。任何子命令都接受同一套选项, 因此统一处理:
 * 位置参数(不以 '-' 开头)当作目标文件。
 */
static void parse_opts(int argc, char **argv, opts_t *o)
{
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(a, "-f") == 0 || strcmp(a, "--file") == 0) {
            if (val == NULL) { o->error = 1; return; }
            o->target = val;
            i++;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--size") == 0) {
            if (val == NULL || parse_size(val, &g_logical_size) != 0) {
                fprintf(stderr, "无效的逻辑大小: %s\n", val ? val : "(缺失)");
                o->error = 1;
                return;
            }
            i++;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--chunk") == 0) {
            if (val == NULL || parse_size(val, &g_chunk_size) != 0) {
                fprintf(stderr, "无效的块大小: %s\n", val ? val : "(缺失)");
                o->error = 1;
                return;
            }
            i++;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--progress") == 0) {
            if (val == NULL) { o->error = 1; return; }
            o->progress = val;
            i++;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--log") == 0) {
            if (val == NULL) { o->error = 1; return; }
            o->logfile = val;
            i++;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--delay") == 0) {
            if (val == NULL) { o->error = 1; return; }
            g_delay_ms = strtoll(val, NULL, 10);
            if (g_delay_ms < 0) {
                g_delay_ms = 0;
            }
            i++;
        } else if (strcmp(a, "--stride") == 0) {
            if (val == NULL) { o->error = 1; return; }
            g_stride = strtoll(val, NULL, 10);
            if (g_stride < 1) {
                g_stride = 1;
            }
            i++;
        } else if (strcmp(a, "--fill") == 0) {
            g_fill = 1;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(a, "--no-fsync") == 0) {
            g_do_fsync = 0;
        } else if (strcmp(a, "--force") == 0) {
            g_force = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            o->help = 1;
        } else if (strcmp(a, "-") == 0) {
            /* 允许 " -l - " 这种“只输出终端”的写法, 单独一个 '-' 忽略 */
        } else if (a[0] != '-') {
            o->target = a;                       /* 位置参数: 目标文件 */
        } else {
            fprintf(stderr, "未知选项: %s(试用 --help)\n", a);
            o->error = 1;
            return;
        }
    }
}

/* ======================= 子命令: init ======================= */

/* 用 lseek 越界 + 写 1 字节, 把 i_size 撑到逻辑大小, 中间全部是空洞 */
static int create_hole_file(const char *path, long long size)
{
    int fd;

    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        log_msg(LOG_ERROR, "open %s 失败: %s", path, strerror(errno));
        return -1;
    }
    if (lseek(fd, (off_t)(size - 1), SEEK_SET) < 0) {
        log_msg(LOG_ERROR, "lseek(%s, %lld) 失败: %s", path, size - 1, strerror(errno));
        close(fd);
        return -1;
    }
    if (write(fd, "", 1) != 1) {
        log_msg(LOG_ERROR, "写文件最后一个字节失败: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (fsync(fd) != 0) {
        log_msg(LOG_WARN, "fsync %s 失败: %s", path, strerror(errno));
    }
    close(fd);
    return 0;
}

static int cmd_init(const opts_t *o)
{
    log_msg(LOG_INFO, "init: 创建空洞文件 %s, 逻辑大小 %lld 字节 (%.2fKB)",
            o->target, g_logical_size, (double)g_logical_size / 1024.0);

    if (create_hole_file(o->target, g_logical_size) != 0) {
        return 1;
    }

    log_msg(LOG_INFO, "init: 完成 —— 只写了文件最后一个字节, 其余 %lld 字节都是空洞",
            g_logical_size - 1);
    report_stats(o->target, (time_t)-1);
    return 0;
}

/* ======================= 子命令: write ======================= */

/*
 * cmd_write: 模拟下载的核心。
 *
 *   默认 stride=1 : 从“已完成偏移”开始, 每次 lseek 到 off 再 write 一块,
 *                   写满一块就把 off 落进进度文件 —— 就是顺序下载。
 *   stride>1      : 每 N 块写 1 块, 中间跳过的块保持空洞, 用来直观演示
 *                   “空洞不占磁盘块”(也证明写入不会破坏其它区域)。
 *   --fill        : 只找“还是全 0 的空洞块”来写, 已有数据的块一个字节都不动。
 *
 *   三种模式共用同一套“断点续传”逻辑: 已处理的偏移保存到进度文件,
 *   Ctrl-C 中断后再次运行即从该偏移继续。
 */
static int cmd_write(const opts_t *o)
{
    char tmp_path[PROGRESS_BUFSZ];
    const char *progress = o->progress;
    const char *target = o->target;
    unsigned char *buf = NULL;
    long long prev_off = 0;
    long long prev_total = 0;
    long long prev_chunk = -1;
    long long prev_stride = -1;
    long long saved_off = 0;
    long long scan_from = 0;
    long long first = 0;
    long long off = 0;
    long long total_blocks = 0;
    long long done_blocks = 0;
    long long skipped_blocks = 0;
    long long done = 0;
    long long written_run = 0;
    long long chunks_run = 0;
    int found = 0;
    int resuming = 0;
    int finished = 0;
    int exhausted = 0;
    int fd = -1;
    int rc = 0;
    int interrupted = 0;
    struct stat st;
    time_t t_start = time(NULL);
    char offbuf[64];

    /* 块大小合法性检查 */
    if (g_chunk_size < HEAD_SIZE) {
        fprintf(stderr, "块大小至少需要 %d 字节(存放块头)\n", HEAD_SIZE);
        return 2;
    }
    if (g_chunk_size > g_logical_size) {
        g_chunk_size = g_logical_size;
    }
    if (g_logical_size % g_chunk_size != 0) {
        log_msg(LOG_WARN, "逻辑大小 %lld 不是块大小 %lld 的整数倍, 最后一块按剩余长度写",
                g_logical_size, g_chunk_size);
    }
    if (g_fill) {
        g_stride = 1;               /* 补空洞模式按块扫描, 不再跳块 */
    }

    /* 进度文件临时路径 */
    if (strlen(progress) + strlen(PROGRESS_TMP_TAIL) + 1 > sizeof(tmp_path)) {
        fprintf(stderr, "进度文件路径过长\n");
        return 2;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s%s", progress, PROGRESS_TMP_TAIL);

    g_pid = getpid();
    install_signals();

    /* --- 1. 读取“已完成偏移”, 决定断点 --- */
    if (g_force) {
        (void)parse_progress(progress, &prev_off, &prev_total, &prev_chunk,
                             &prev_stride, &found);
        if (found) {
            log_msg(LOG_WARN, "--force: 忽略已存在的进度文件 %s, 从偏移 0 重新开始", progress);
        }
        found = 0;
        prev_off = 0;
    } else if (parse_progress(progress, &prev_off, &prev_total, &prev_chunk,
                              &prev_stride, &found) != 0) {
        return 1;
    }

    /* 最终采用的起始偏移 */
    if (!found || prev_off < 0 || prev_off > g_logical_size) {
        if (found) {
            log_msg(LOG_WARN, "进度文件中的偏移 %lld 非法, 从 0 重新开始", prev_off);
        }
        resuming = 0;
        scan_from = 0;
    } else {
        resuming = 1;
        scan_from = prev_off;
    }
    saved_off = resuming ? prev_off : 0;

    log_msg(LOG_INFO, "================ 开始模拟下载 ================");
    log_msg(LOG_INFO, "目标文件 %s", target);
    log_msg(LOG_INFO, "逻辑大小 %lld 字节 (%.2fKB), 块大小 %lld 字节, 块间延迟 %lld ms",
            g_logical_size, (double)g_logical_size / 1024.0, g_chunk_size, g_delay_ms);
    if (g_fill) {
        log_msg(LOG_INFO, "写入模式 fill: 只补写文件中的空洞块, 已有数据的块不碰");
        if (g_stride > 1) {
            log_msg(LOG_WARN, "--fill 已把步长重置为 1(逐块扫描空洞)");
        }
    } else if (g_stride > 1) {
        log_msg(LOG_INFO, "写入模式 sparse(跳块): 每 %lld 块写 1 块, 其余块保持空洞",
                g_stride);
    } else {
        log_msg(LOG_INFO, "写入模式 sequential: 每次只写 offset..offset+%lld-1, 其余区域保持空洞",
                g_chunk_size);
    }

    if (resuming) {
        if (prev_total > 0 && prev_total != g_logical_size) {
            log_msg(LOG_WARN, "进度文件记录的总大小 %lld 与本次 %lld 不一致, 从头开始",
                    prev_total, g_logical_size);
            resuming = 0;
            scan_from = 0;
            saved_off = 0;
        }
        if (prev_chunk > 0 && prev_chunk != g_chunk_size) {
            log_msg(LOG_WARN, "进度文件记录的块大小 %lld 与本次 %lld 不一致, 从头开始",
                    prev_chunk, g_chunk_size);
            resuming = 0;
            scan_from = 0;
            saved_off = 0;
        }
        if (resuming && prev_stride > 0 && prev_stride != g_stride && !g_fill) {
            log_msg(LOG_WARN, "进度文件记录的步长 %lld 与本次 %lld 不一致, 进度偏移仅作参考",
                    prev_stride, g_stride);
        }
    }

    if (resuming) {
        format_offset(saved_off, offbuf, sizeof(offbuf));
        log_msg(LOG_INFO, "发现进度文件 %s: 已完成/已处理到偏移 %s", progress, offbuf);
        log_msg(LOG_INFO, "断点续传: 从该位置继续, 已写过的 %lld 字节不再重写", saved_off);
    } else {
        log_msg(LOG_INFO, "没有可用进度文件: 从头开始写第 0 块");
    }

    /* --- 2. 打开(必要时创建)目标文件 --- */
    fd = open(target, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        log_msg(LOG_ERROR, "open %s 失败: %s", target, strerror(errno));
        return 1;
    }
    if (fstat(fd, &st) != 0) {
        log_msg(LOG_ERROR, "fstat %s 失败: %s", target, strerror(errno));
        close(fd);
        return 1;
    }

    /* 文件比目标小: 先撑出空洞(只写最后一个字节) */
    if ((long long)st.st_size < g_logical_size) {
        log_msg(LOG_INFO, "文件当前逻辑大小 %lld < 目标 %lld, 先用 lseek+write 撑出空洞",
                (long long)st.st_size, g_logical_size);
        if (create_hole_file(target, g_logical_size) != 0) {
            close(fd);
            return 1;
        }
        if (fstat(fd, &st) != 0) {
            log_msg(LOG_ERROR, "fstat %s 失败: %s", target, strerror(errno));
            close(fd);
            return 1;
        }
    }

    /* 断点不能超过文件真实大小, 否则会“凭空”多出空洞 */
    if (scan_from > (long long)st.st_size) {
        log_msg(LOG_WARN, "进度偏移 %lld 超过文件真实大小 %lld, 回退到文件末尾",
                scan_from, (long long)st.st_size);
        scan_from = (long long)st.st_size;
    }

    /* --- 3. 计算本次要处理的块集合 --- */
    first = next_block_offset(scan_from, g_logical_size, g_chunk_size, g_stride);
    if (first < scan_from) {
        first = next_block_offset(scan_from, g_logical_size, g_chunk_size, 1);
    }
    total_blocks = count_blocks(first, g_logical_size, g_chunk_size, g_stride);
    off = first;

    if (total_blocks <= 0 || first >= g_logical_size) {
        format_offset(saved_off, offbuf, sizeof(offbuf));
        log_msg(LOG_INFO, "没有需要处理的块(已完成偏移 %s, 逻辑大小 %lld), 无需继续",
                offbuf, g_logical_size);
        report_stats(target, t_start);
        close(fd);
        return 0;
    }

    format_offset(first, offbuf, sizeof(offbuf));
    log_msg(LOG_INFO, "本次计划处理 %lld 块, 起始偏移 %s, 结束偏移 %lld",
            total_blocks, offbuf, g_logical_size);
    log_msg(LOG_INFO, "运行中按 Ctrl-C 可中断; 进度实时保存到 %s", progress);
    log_msg(LOG_INFO, "-----------------------------------------------");

    buf = malloc((size_t)g_chunk_size);
    if (buf == NULL) {
        log_msg(LOG_ERROR, "malloc %lld 字节失败: %s", g_chunk_size, strerror(errno));
        close(fd);
        return 1;
    }

    /* --- 4. 定点写入主循环: lseek + write, 只覆盖选中的块 --- */
    while (off >= 0 && off < g_logical_size) {
        long long cur = g_chunk_size;
        long long want;

        if (g_stop_flag) {
            interrupted = 1;
            break;
        }
        if (g_want_stats) {
            g_want_stats = 0;
            report_stats(target, t_start);
        }

        if (off + cur > g_logical_size) {
            cur = g_logical_size - off;
        }
        build_chunk(buf, off, g_logical_size, cur);

        if (g_fill) {
            long long jump_to = -1;
            int is_hole = 0;

            /* 补空洞前先确认这一块确实是空洞(全 0); 有数据就原样跳过 */

#ifdef SEEK_DATA
            /*
             * lseek(fd, off, SEEK_DATA) 直接跳到下一个“有数据”的偏移:
             * 若返回值 > off, 说明 [off, 返回值) 整段都是空洞;
             * 返回 -1/ENXIO 表示 off 之后已没有数据(剩下全是空洞)。
             * 这样补空洞时不必逐块读一遍已经写好的数据(只需落笔点预览 1 字节,
             * 该位置本来就是 0, 写它不会改变文件内容与空洞分布)。
             */
            {
                off_t dataoff = lseek(fd, (off_t)off, SEEK_DATA);
                if (dataoff < 0 && errno == ENXIO) {
                    dataoff = (off_t)g_logical_size;
                }
                if (dataoff >= 0 && (long long)dataoff > off &&
                    dataoff < (off_t)g_logical_size) {
                    log_msg(LOG_DEBUG, "SEEK_DATA: [%lld, %lld) 是空洞, 可整段跳过有数据的区域",
                            off, (long long)dataoff);
                    if (lseek(fd, (off_t)off, SEEK_SET) < 0 || write(fd, buf, 1) != 1) {
                        log_msg(LOG_WARN, "空洞预览写失败: %s", strerror(errno));
                    }
                    jump_to = (long long)dataoff;
                }
            }
#endif
            if (jump_to < 0) {
                ssize_t r = pread(fd, buf, (size_t)cur, (off_t)off);
                if (r == (ssize_t)cur) {
                    long long k;
                    is_hole = 1;
                    for (k = 0; k < cur; k++) {
                        if (buf[k] != 0) {
                            is_hole = 0;
                            break;
                        }
                    }
                } else if (r < 0) {
                    log_msg(LOG_ERROR, "pread 偏移 %lld 失败: %s", off, strerror(errno));
                    rc = 1;
                    break;
                }
            }

            if (jump_to >= 0) {
                /* 这一整段都已有数据: 原样跳过, 一个字节都不动 */
                skipped_blocks += (jump_to - off + g_chunk_size - 1) / g_chunk_size;
                off = jump_to;
            } else if (!is_hole) {
                /* 本块已有数据: 跳过 */
                skipped_blocks++;
                off = next_block_offset(off + cur, g_logical_size, g_chunk_size, 1);
            } else {
                build_chunk(buf, off, g_logical_size, cur);  /* pread 覆盖了 buf, 重建数据 */
            }

            if (jump_to >= 0 || !is_hole) {
                if (off < 0) {
                    off = g_logical_size;
                }
                if (off >= g_logical_size) {
                    exhausted = 1;
                    break;
                }
                if (g_stop_flag) {
                    interrupted = 1;
                    break;
                }
                msleep(g_delay_ms);
                continue;
            }
        }

        /* 定点: 直接跳到 off, 中间的空洞一个字节都不碰 */
        if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
            log_msg(LOG_ERROR, "lseek 到 %lld 失败: %s", off, strerror(errno));
            rc = 1;
            break;
        }
        want = cur;

        {
            ssize_t w = write(fd, buf, (size_t)cur);
            if (w != (ssize_t)cur) {
                if (w < 0) {
                    if (errno == EINTR || g_stop_flag) {
                        interrupted = 1;        /* 本块没写进去, 下次重写 */
                        break;
                    }
                    log_msg(LOG_ERROR, "write 偏移 %lld 失败: %s", off, strerror(errno));
                    rc = 1;
                    break;
                }
                /* 部分写入: 只累计真正写成功的字节, 本块下次再补 */
                log_msg(LOG_WARN, "write 偏移 %lld 只写入 %lld/%lld 字节, 下次继续",
                        off, (long long)w, cur);
                written_run += (long long)w;
                want = 0;
            }
        }

        if (want == cur) {
            written_run += cur;
            chunks_run++;
            done_blocks++;
        }

        if (g_do_fsync && fsync(fd) != 0) {
            log_msg(LOG_WARN, "fsync 失败: %s", strerror(errno));
        }

        off = next_block_offset(off + cur, g_logical_size, g_chunk_size, g_stride);
        if (off < 0) {
            off = g_logical_size;           /* 本模式已无块可写 */
        }
        if (off >= g_logical_size) {
            exhausted = 1;                  /* 本轮的块都已写完 */
        }

        /* 每处理完一块就落地“已完成偏移”(原子替换进度文件) */
        if (write_progress(tmp_path, progress, off, g_logical_size,
                           g_chunk_size, g_stride, 0) != 0) {
            rc = 1;
            break;
        }

        if (!g_quiet || chunks_run <= 3 || exhausted) {
            format_offset(off, offbuf, sizeof(offbuf));
            log_msg(LOG_INFO, "  已处理 %6.2f%% (%lld/%lld 块), 下一偏移 %s",
                    (double)done_blocks * 100.0 / (double)total_blocks,
                    done_blocks, total_blocks, offbuf);
        }

        if (g_stop_flag) {
            interrupted = 1;
            break;
        }
        msleep(g_delay_ms);
    }

    done = (saved_off > off) ? saved_off : off;
    if (off >= g_logical_size) {
        done = g_logical_size;
    }

    /* --- 5. 收尾: 区分“本轮全部写完”/“被中断”/“出错了” --- */
    if (rc == 0 && exhausted) {
        finished = 1;
        if (fsync(fd) != 0) {
            log_msg(LOG_WARN, "最终 fsync 失败: %s", strerror(errno));
        }
        /*
         * 只有“顺序下载/补空洞”才算整个文件做完; 跳块模式只是本轮写完,
         * 文件里还留着空洞, 所以状态仍是 incomplete, 偏移保留以便续传。
         */
        if (write_progress(tmp_path, progress,
                           (g_stride > 1) ? done : g_logical_size,
                           g_logical_size, g_chunk_size, g_stride,
                           (g_stride > 1) ? 0 : 1) != 0) {
            rc = 1;
        }
        log_msg(LOG_INFO, "-----------------------------------------------");
        if (g_fill) {
            log_msg(LOG_INFO, "补写完成: 本次填入 %lld 块空洞, 跳过已有数据 %lld 块",
                    chunks_run, skipped_blocks);
        } else if (g_stride > 1) {
            log_msg(LOG_INFO, "跳块写入完成: 本次 %lld 块; 跳过的块仍是空洞(不占磁盘块)",
                    chunks_run);
            log_msg(LOG_INFO, "如需把文件补满, 运行: ./sparse_file --fill -f %s", target);
        } else {
            log_msg(LOG_INFO, "下载完成: 逻辑 %lld 字节已全部写好, 本次 %lld 块",
                    g_logical_size, chunks_run);
        }
    } else if (interrupted) {
        done = (saved_off > off) ? saved_off : off;
        if (fsync(fd) != 0) {
            log_msg(LOG_WARN, "中断前 fsync 失败: %s", strerror(errno));
        }
        if (write_progress(tmp_path, progress, done, g_logical_size,
                           g_chunk_size, g_stride, 0) != 0) {
            rc = 1;
        }
        format_offset(done, offbuf, sizeof(offbuf));
        log_msg(LOG_WARN, "==> 收到 %s, 断点已保存: 已完成偏移 %s", g_stop_signame, offbuf);
        log_msg(LOG_WARN, "==> 再次运行同样的命令即可从该偏移继续(已写部分不重写)");
        rc = 0;                 /* 中断续传属于正常流程, 不算失败 */
    } else if (rc == 0) {
        done = (saved_off > off) ? saved_off : off;
    }

    g_run_written = written_run;
    g_run_chunks = chunks_run;

    free(buf);
    if (close(fd) != 0) {
        log_msg(LOG_WARN, "close %s 失败: %s", target, strerror(errno));
    }

    log_msg(LOG_INFO, "本次运行: 起始偏移 %lld -> 完成偏移 %lld, 写入 %lld 字节 / %lld 块%s",
            first, done, written_run, chunks_run, finished ? " (已完成)" : "");
    if (skipped_blocks > 0) {
        log_msg(LOG_INFO, "补写时跳过已有数据的块: %lld 块", skipped_blocks);
    }
    report_stats(target, t_start);
    log_msg(LOG_INFO, "================ 运行结束 (rc=%d) ================", rc);
    return rc;
}

/* ======================= 子命令: verify ======================= */

/* 逐块读回, 统计: 已写入且校验通过 / 全 0 的空洞块 / 内容异常块 */
static int cmd_verify(const opts_t *o)
{
    struct stat st;
    unsigned char *buf = NULL;
    long long off = 0;
    long long good = 0;
    long long hole_chunks = 0;
    long long bad = 0;
    long long bad_first = -1;
    int fd;
    int rc = 0;

    fd = open(o->target, O_RDONLY);
    if (fd < 0) {
        log_msg(LOG_ERROR, "open %s 失败: %s", o->target, strerror(errno));
        return 1;
    }
    if (fstat(fd, &st) != 0) {
        log_msg(LOG_ERROR, "fstat %s 失败: %s", o->target, strerror(errno));
        close(fd);
        return 1;
    }
    if ((long long)st.st_size == 0) {
        log_msg(LOG_ERROR, "文件 %s 长度为 0, 无法校验", o->target);
        close(fd);
        return 1;
    }
    if ((long long)st.st_size % g_chunk_size != 0) {
        log_msg(LOG_WARN, "文件大小 %lld 不是块大小 %lld 的整数倍, 按剩余长度校验",
                (long long)st.st_size, g_chunk_size);
    }

    buf = malloc((size_t)g_chunk_size);
    if (buf == NULL) {
        log_msg(LOG_ERROR, "malloc 失败: %s", strerror(errno));
        close(fd);
        return 1;
    }

    while (off < (long long)st.st_size) {
        long long cur = g_chunk_size;
        ssize_t r;
        long long k;
        int all_zero = 1;

        if (off + cur > (long long)st.st_size) {
            cur = (long long)st.st_size - off;
        }
        r = pread(fd, buf, (size_t)cur, (off_t)off);
        if (r != (ssize_t)cur) {
            log_msg(LOG_ERROR, "pread 偏移 %lld 失败: %s", off,
                    r < 0 ? strerror(errno) : "读到的字节数不足");
            rc = 1;
            break;
        }

        for (k = 0; k < cur; k++) {
            if (buf[k] != 0) {
                all_zero = 0;
                break;
            }
        }

        if (all_zero) {
            hole_chunks++;                  /* 空洞区域读出来是全 0 */
        } else if (cur >= HEAD_SIZE &&
                   chunk_matches(buf, off, (long long)st.st_size, cur)) {
            good++;
        } else {
            bad++;
            if (bad_first < 0) {
                bad_first = off;
            }
        }
        off += cur;
    }

    free(buf);
    close(fd);

    log_msg(LOG_INFO, "================ 校验结果 ================");
    log_msg(LOG_INFO, "文件            : %s", o->target);
    log_msg(LOG_INFO, "逻辑大小        : %lld 字节", (long long)st.st_size);
    log_msg(LOG_INFO, "校验块大小      : %lld 字节", g_chunk_size);
    log_msg(LOG_INFO, "已写入且正确    : %lld 块", good);
    log_msg(LOG_INFO, "未写入(空洞/全0): %lld 块", hole_chunks);
    log_msg(LOG_INFO, "内容异常        : %lld 块%s", bad,
            bad_first >= 0 ? "(首个异常偏移见下)" : "");
    if (bad_first >= 0) {
        log_msg(LOG_ERROR, "首个内容异常偏移: %lld", bad_first);
    }
    log_msg(LOG_INFO, "校验结论        : %s", (bad == 0 && rc == 0) ? "通过" : "失败");
    report_stats(o->target, (time_t)-1);
    return (bad == 0 && rc == 0) ? 0 : 1;
}

/* ======================= 子命令: stat ======================= */

static int cmd_stat(const opts_t *o)
{
    report_stats(o->target, (time_t)-1);
    return 0;
}

/* ======================= 子命令: reset ======================= */

static int cmd_reset(const opts_t *o)
{
    if (unlink(o->target) == 0) {
        log_msg(LOG_INFO, "已删除目标文件 %s", o->target);
    } else if (errno != ENOENT) {
        log_msg(LOG_ERROR, "删除 %s 失败: %s", o->target, strerror(errno));
        return 1;
    } else {
        log_msg(LOG_INFO, "目标文件 %s 不存在, 跳过", o->target);
    }

    if (unlink(o->progress) == 0) {
        log_msg(LOG_INFO, "已删除进度文件 %s", o->progress);
    } else if (errno != ENOENT) {
        log_msg(LOG_ERROR, "删除 %s 失败: %s", o->progress, strerror(errno));
        return 1;
    } else {
        log_msg(LOG_INFO, "进度文件 %s 不存在, 跳过", o->progress);
    }
    return 0;
}

/* ======================= 入口 ======================= */

int main(int argc, char **argv)
{
    opts_t o;
    const char *cmd = "write";
    int arg_start = 1;
    int rc;

    g_pid = getpid();
    opts_default(&o);

    if (argc > 1 && argv[1][0] != '-') {
        cmd = argv[1];
        arg_start = 2;
    }

    if (strcmp(cmd, "write") != 0 && strcmp(cmd, "run") != 0 &&
        strcmp(cmd, "init") != 0 && strcmp(cmd, "verify") != 0 &&
        strcmp(cmd, "stat") != 0 && strcmp(cmd, "reset") != 0 &&
        strcmp(cmd, "help") != 0) {
        fprintf(stderr, "未知子命令: %s\n\n", cmd);
        usage(stderr, argv[0]);
        return 2;
    }

    parse_opts(argc - arg_start, argv + arg_start, &o);
    if (o.error) {
        fprintf(stderr, "参数错误, 试用 %s --help\n", argv[0]);
        return 2;
    }
    if (o.help || strcmp(cmd, "help") == 0) {
        usage(stdout, argv[0]);
        return 0;
    }

    /* 除 help 外都写日志; reset 也写, 方便追溯 */
    log_init(o.logfile);

    if (strcmp(cmd, "init") == 0) {
        rc = cmd_init(&o);
    } else if (strcmp(cmd, "verify") == 0) {
        rc = cmd_verify(&o);
    } else if (strcmp(cmd, "stat") == 0) {
        rc = cmd_stat(&o);
    } else if (strcmp(cmd, "reset") == 0) {
        rc = cmd_reset(&o);
    } else {
        rc = cmd_write(&o);
    }

    log_close();
    return rc;
}
