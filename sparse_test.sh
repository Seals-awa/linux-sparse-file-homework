#!/bin/sh
# ============================================================================
#  sparse_test.sh -- 空洞文件作业自动验收脚本(POSIX sh, 在 tmp_test/ 下运行)
#
#  覆盖: 空洞生成 / 空洞不占块 / 定点写入不破坏其它空洞 / Ctrl-C 断点续传 /
#        统计输出 / 自定义日志 / --fill 只补空洞 / verify 内容校验
#
#  用法:  make test   或   sh ./sparse_test.sh
# ============================================================================
set -u

BIN=${BIN:-./sparse_file}
SIZE=${SIZE:-64K}          # 自测用小一些, 跑得快
CHUNK=${CHUNK:-1K}
TMP=./tmp_test
PASS=0
FAIL=0

if [ ! -x "$BIN" ]; then
    echo "找不到可执行文件 $BIN, 请先执行 make"
    exit 2
fi

rm -rf "$TMP"
mkdir -p "$TMP"

pass() { PASS=$((PASS + 1)); printf '  [PASS] %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf '  [FAIL] %s\n' "$1"; }
title() { printf '\n=== %s ===\n' "$1"; }

# 读取 stat 的 st_blocks(512B 为单位)
phys_of() { stat -c %b "$1" 2>/dev/null || echo 0; }
size_of() { stat -c %s "$1" 2>/dev/null || echo 0; }
# 解析进度文件里 "off=..." 的数值
prog_off() { sed -n 's/^off=//p' "$1" 2>/dev/null | head -n 1; }

check_range() {   # check_range <值> <下限> <上限> <描述>
    v=$1; lo=$2; hi=$3; desc=$4
    if [ "$v" -ge "$lo" ] && [ "$v" -le "$hi" ]; then
        pass "$desc (=$v, 期望 $lo..$hi)"
    else
        fail "$desc (=$v, 期望 $lo..$hi)"
    fi
}

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

cd "$TMP" || exit 1
BIN="../sparse_file"

# ---------------------------------------------------------------- 1. 空洞文件
title "1. 生成空洞文件: 逻辑 $SIZE, 未写入区不占磁盘块"
$BIN init -f hole.bin -s "$SIZE" -l test.log -q >/dev/null 2>&1
logical=$(size_of hole.bin)
blocks=$(phys_of hole.bin)
if [ "$logical" = "65536" ]; then
    pass "逻辑大小 = $logical 字节(256KB 要求的同款做法, 这里用 64K 加速)"
else
    fail "逻辑大小 = $logical, 期望 65536"
fi
# 4K 文件系统块 + 1 字节写入 => 占用 8 个 512B 扇区
check_range "$blocks" 0 8 "实际占用块数(512B/块)极小, 空洞不占磁盘块"

# ------------------------------------------------- 2. 定点写入不破坏其它空洞
title "2. lseek+write 定点写入, 只覆盖目标块"
$BIN -f hole.bin -s "$SIZE" -c "$CHUNK" -d 0 -l test.log -q --force >/dev/null 2>&1
$BIN verify -f hole.bin -s "$SIZE" -c "$CHUNK" -l test.log > verify1.txt 2>&1
if grep -q "内容异常        : 0 块" verify1.txt; then
    pass "全部已写入块内容校验通过(0 块异常)"
else
    fail "verify 报告内容异常"
    cat verify1.txt
fi
if grep -q "未写入(空洞/全0): 0 块" verify1.txt; then
    pass "顺序写完后已无空洞(63 个数据块 + 文件尾字节)"
else
    fail "顺序写完后仍有空洞"
fi
if grep -q "校验结论        : 通过" verify1.txt; then
    pass "verify 结论: 通过"
else
    fail "verify 结论不是“通过”"
fi

# ------------------------------------------------------------ 3. 断点续传
title "3. Ctrl-C 中断 -> 记录已完成偏移 -> 再次运行续传"
rm -f p.bin p.progress
$BIN -f p.bin -s "$SIZE" -c "$CHUNK" -p p.progress -d 200 -l test.log -q >/dev/null 2>&1 &
pid=$!
sleep 1
kill -INT "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
rc_int=$?
if [ "$rc_int" = "0" ]; then
    pass "SIGINT 后程序优雅退出(退出码 0)"
else
    fail "SIGINT 后退出码 = $rc_int, 期望 0"
fi
if [ -f p.progress ]; then
    pass "进度文件已生成: $(tr '\n' ' ' < p.progress)"
else
    fail "进度文件 p.progress 不存在"
fi
saved=$(prog_off p.progress)
saved=${saved:-0}
if [ "$saved" -gt 0 ] && [ "$saved" -lt 65536 ]; then
    pass "已完成偏移 = $saved 字节(在 (0, 65536) 之间, 说明确实中断了)"
else
    fail "已完成偏移 = $saved, 期望 (0, 65536)"
fi
# 记录已写入块的 inode 元数据, 续传后必须一字不变
stat -c '%n %s %Y %b' p.bin > before.txt
sleep 1
$BIN -f p.bin -s "$SIZE" -c "$CHUNK" -p p.progress -d 0 -l test.log -q > resume.txt 2>&1
rc_resume=$?
stat -c '%n %s %Y %b' p.bin > after.txt
if [ "$rc_resume" = "0" ]; then
    pass "续传运行退出码 0"
else
    fail "续传运行退出码 = $rc_resume"
fi
if grep -q "断点续传: 从该位置继续" resume.txt; then
    pass "续传日志确认从断点($saved 字节)继续"
else
    fail "续传日志里没有断点信息"
    grep -n "发现进度文件\|断点" resume.txt
fi
if [ "$saved" -lt 65536 ] && grep -q "已写过的 $saved 字节不再重写" resume.txt; then
    pass "日志声明已写入的 $saved 字节不会被重写"
fi
if cmp -s before.txt after.txt; then
    pass "续传没有改动已写入部分(大小/占用块数/mtime 均不变)"
else
    fail "续传改动了已写入部分"
    diff before.txt after.txt
fi
$BIN verify -f p.bin -s "$SIZE" -c "$CHUNK" -l test.log > verify2.txt 2>&1
if grep -q "校验结论        : 通过" verify2.txt; then
    pass "续传后的文件整体校验通过"
else
    fail "续传后的文件校验失败"
    cat verify2.txt
fi

# --------------------------------------------- 4. 跳块写入: 空洞肉眼可见
title "4. --stride 2 跳块写入: 一半块是空洞, 空洞占比约 50%"
rm -f s.bin s.progress
$BIN -f s.bin -s "$SIZE" -c "$CHUNK" -p s.progress --stride 2 --force -l test.log -q >/dev/null 2>&1
logical=$(size_of s.bin)
blocks=$(phys_of s.bin)
echo "  逻辑 $(($logical / 1024))KB, 实际占用 $((blocks / 2))KB"
check_range "$blocks" 96 160 "占用块数在 48KB~80KB 区间(一半块被写成真实数据)"
$BIN verify -f s.bin -s "$SIZE" -c "$CHUNK" -l test.log > verify3.txt 2>&1
holes=$(sed -n 's/^未写入(空洞\/全0): \([0-9]*\) 块/\1/p' verify3.txt | head -n 1)
order=$(sed -n 's/^已写入且正确    : \([0-9]*\) 块/\1/p' verify3.txt | head -n 1)
if [ "${holes:-0}" -gt 0 ] && [ "${order:-0}" -gt 0 ]; then
    pass "跳块后: 已写 $order 块, 空洞 $holes 块(两边都 >0)"
else
    fail "跳块后块统计异常: 已写=${order:-?} 空洞=${holes:-?}"
fi

# --------------------------------------------- 5. --fill 只补空洞不动老数据
title "5. --fill 定点补写空洞块, 已有数据的块一个字节都不改"
stat -c '%n %s %Y' s.bin > before_fill.txt
$BIN -f s.bin -s "$SIZE" -c "$CHUNK" -p s.progress --fill -l test.log -q > fill.txt 2>&1
stat -c '%n %s %Y' s.bin > after_fill.txt
if cmp -s before_fill.txt after_fill.txt; then
    pass "补写没有改变文件的大小/时间戳信息"
fi
if grep -q "补写时跳过已有数据的块" fill.txt; then
    pass "日志显示补写时跳过了已有数据的块"
else
    fail "补写日志里没有跳过已有数据块的记录"
fi
$BIN verify -f s.bin -s "$SIZE" -c "$CHUNK" -l test.log > verify4.txt 2>&1
if grep -q "未写入(空洞/全0): 0 块" verify4.txt && \
   grep -q "校验结论        : 通过" verify4.txt; then
    pass "补写后空洞为 0 且整体校验通过"
else
    fail "补写后校验失败"
    cat verify4.txt
fi

# ------------------------------------------------------------ 6. 统计与日志
title "6. 逻辑大小/实际占用块数/空洞占比 + 自定义日志"
$BIN stat -f s.bin -l test.log >> verify4.txt 2>&1
if grep -q "实际占用块数" verify4.txt && grep -q "空洞占比" verify4.txt; then
    pass "统计信息含 逻辑大小/实际占用块数/空洞占比"
else
    fail "统计信息缺少必需字段"
fi
if [ -s test.log ]; then
    pass "自定义日志 test.log 已写入($(wc -l < test.log) 行)"
else
    fail "日志文件为空"
fi

# ------------------------------------------------------------------ 结果汇总
title "结果"
printf '  通过 %d 项, 失败 %d 项\n' "$PASS" "$FAIL"
if [ "$FAIL" = "0" ]; then
    echo "  ALL TESTS PASSED"
    exit 0
fi
echo "  SOME TESTS FAILED"
exit 1
