#!/bin/bash
# =========================================
# mca_uinput 自动化测试示例脚本
# 演示单点点击、长按、滑动 + 多点 MT 操作
# =========================================

DEVICE="/dev/input/event1"   # 替换为板子的触摸设备
UINPUT_BIN="/home/root/mca_uinput" # mca_uinput 可执行文件路径

echo "=== 单点点击测试 ==="
$UINPUT_BIN -d $DEVICE tap 200 150 100
sleep 0.2
$UINPUT_BIN -d $DEVICE longpress 300 200 2000
sleep 0.2

echo "=== 单点滑动测试 ==="
$UINPUT_BIN -d $DEVICE swipe 100 100 400 300 500 20
sleep 0.5

echo "=== 多点 MT 测试 ==="
# 两指同时按下
$UINPUT_BIN -d $DEVICE mt-down 0 150 150
$UINPUT_BIN -d $DEVICE mt-down 1 300 150
sleep 0.2

# 两指移动
for i in {0..10}; do
    x0=$((150 + i*5))
    y0=$((150 + i*3))
    x1=$((300 - i*5))
    y1=$((150 + i*3))
    $UINPUT_BIN -d $DEVICE mt-move 0 $x0 $y0
    $UINPUT_BIN -d $DEVICE mt-move 1 $x1 $y1
    sleep 0.05
done

# 两指抬起
$UINPUT_BIN -d $DEVICE mt-up 0
$UINPUT_BIN -d $DEVICE mt-up 1
sleep 0.2

echo "=== 测试完成 ==="
