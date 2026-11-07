"""雷柏（Rapoo）鼠标电量探测：判定能不能读到、走哪条通道。

背景：
  * 雷柏鼠标没有标准 HID 电量字段（用途页 0x06 / 用途 0x20），所以不吃通用兜底；
  * 游戏系列（VT9 Pro / VT3 Pro）有一条 0xFF00:0x000E 的私有通道，report 0xBA：
        [0] 连接字节  [1] 命令(0xA4 读 / 0xA5 写 / 0xAA 电量)
        [2] 长度      [3..6] 地址(小端)  [7..] 数据
    应答不走输入报告队列，而是 GET_REPORT(Input) —— Windows 对应
    HidD_GetInputReport；应答首字节 0x01 表示已应答，电量帧的第 1 字节是
    标记（1=放电 / 2=充电）、第 2 字节是百分比。
  * 办公系列（M200 等）可能压根不实现这条通道。本脚本就是用来区分这两种情况。

只读：只发文档里的读命令（0xA3 握手 / 0xA4 读寄存器 / 0xAA 读电量），
不写任何配置。

用法：
    python rapoo_probe.py
"""
import ctypes
import sys
import time

import hidprobe as H

hid = H.hid
RAPOO_VID = 0x24AE
CONFIG_PAGE, CONFIG_USAGE = 0xFF00, 0x000E
REPORT_ID = 0xBA


def find_config():
    for r in H.enum_rows():
        if r[0] == RAPOO_VID and r[2] == CONFIG_PAGE and r[3] == CONFIG_USAGE:
            return r
    return None


def frame(conn, cmd, addr=0, length=0):
    f = bytearray(31)
    f[0] = conn
    f[1] = cmd
    f[2] = length
    f[3] = addr & 0xFF
    f[4] = (addr >> 8) & 0xFF
    f[5] = (addr >> 16) & 0xFF
    f[6] = (addr >> 24) & 0xFF
    return bytes([REPORT_ID]) + bytes(f)


def send(h, payload):
    buf = ctypes.create_string_buffer(bytes(payload), 32)
    return bool(hid.HidD_SetOutputReport(h, buf, 32))


def poll(h, tries=20, delay=0.04):
    """GET_REPORT(Input)：雷柏的应答在这里，不在 ReadFile 的队列里。"""
    for _ in range(tries):
        time.sleep(delay)
        buf = ctypes.create_string_buffer(32)
        ctypes.memmove(buf, bytes([REPORT_ID]), 1)
        if hid.HidD_GetInputReport(h, buf, 32):
            data = buf.raw
            if any(data):
                return data
    return None


def show(tag, data):
    if not data:
        print(f"  {tag}: 无应答")
        return None
    off = 1 if data[0] == REPORT_ID else 0
    print(f"  {tag}: {data[:10].hex(' ')}")
    return data, off


def dump_status_feature():
    """0001/0000 上那条 8 字节特性报告：雷柏接收器的状态快照。"""
    row = None
    for r in H.enum_rows():
        if r[0] == RAPOO_VID and r[2] == 0x0001 and r[3] == 0x0000 and r[6] >= 8:
            row = r
            break
    if not row:
        return
    try:
        h = H._open(row[10], access=H.GENERIC_READ)
    except OSError as e:
        print(f"  打开失败: {e}")
        return
    buf = ctypes.create_string_buffer(8)
    ctypes.memmove(buf, bytes([0x05]), 1)
    if hid.HidD_GetFeature(h, buf, 8):
        d = buf.raw
        print(f"  特性报告 0x05: {d.hex(' ')}")
        if len(d) >= 4:
            print(f"    候选解读：{d[1]}% / {d[2] * 0.01:.2f}V / 状态 {d[3]}"
                  "   （未经验证，不要直接当读数用）")
    else:
        print(f"  特性报告 0x05 读取失败 err={ctypes.get_last_error()}")
    H.kernel32.CloseHandle(h)


def main():
    row = find_config()
    if not row:
        print("没找到雷柏的配置接口 0xFF00:0x000E —— 设备没插或不是这一代")
        return 1
    print(f"配置接口: {row[10]}")
    print(f"IN={row[4]} OUT={row[5]} FEA={row[6]}  产品名={row[7]}\n")

    print("状态特性报告（只读）")
    dump_status_feature()

    print("\n私有通道应答测试")
    h = H._open(row[10], access=H.GENERIC_READ | H.GENERIC_WRITE)
    try:
        for tag, payload in (
            ("握手 a3 00", frame(0xA5, 0xA3)),
            ("性能寄存器 0x880", frame(0xA5, 0xA4, 0x880, 4)),
            ("电量 0xAA", frame(0xA5, 0xAA)),
        ):
            if not send(h, payload):
                print(f"  {tag}: 发送失败 err={ctypes.get_last_error()}")
                continue
            got = show(tag, poll(h))
            if tag == "电量 0xAA" and got:
                data, off = got
                marker, pct = data[off + 1], data[off + 2]
                if marker in (1, 2) and pct <= 100:
                    print(f"    -> 电量 {pct}%（{'充电中' if marker == 2 else '放电中'}）")
                else:
                    print(f"    -> 不是电量帧（标记={marker} 值={pct}）")
    finally:
        H.kernel32.CloseHandle(h)

    print("\n结论：若三项都“无应答”，说明这只鼠标的接收器不实现该协议；")
    print("      此时只能看状态特性报告是否随设备状态变化，不能凭固定值当电量。")
    return 0


sys.exit(main())
