# xcp_master_udp.py
# Minimal XCP master for the UDP demo slave.
# Runs a tiny sequence: CONNECT, GET_STATUS, GET_ID,
# SHORT_UPLOAD from an absolute address, SET_MTA + DOWNLOAD + UPLOAD.

import argparse
import socket
import struct
import sys
from time import sleep

XCP_CMD_CONNECT       = 0xFF
XCP_CMD_DISCONNECT    = 0xFE
XCP_CMD_GET_STATUS    = 0xFD
XCP_CMD_GET_ID        = 0xFA
XCP_CMD_SET_MTA       = 0xF6
XCP_CMD_UPLOAD        = 0xF5
XCP_CMD_SHORT_UPLOAD  = 0xF4
XCP_CMD_DOWNLOAD      = 0xF0

def hexdump(prefix, b):
    print(f"{prefix} ({len(b)}): " + " ".join(f"{x:02X}" for x in b))

class XcpUdp:
    def __init__(self, host="127.0.0.1", port=5555, timeout=1.0):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)

    def xfer(self, payload: bytes) -> bytes:
        self.sock.sendto(payload, self.addr)
        data, _ = self.sock.recvfrom(2048)
        return data

    def connect(self, mode=0):
        req = bytes([XCP_CMD_CONNECT, mode & 0xFF])
        res = self.xfer(req)
        hexdump("CONNECT.res", res)
        if len(res) >= 8 and res[0] == 0xFF:
            resources = res[1]
            comm_mode = res[2]
            max_cto   = res[3]
            max_dto   = res[4] | (res[5] << 8)
            print(f"  resources=0x{resources:02X} comm_mode=0x{comm_mode:02X} "
                  f"MAX_CTO={max_cto} MAX_DTO={max_dto} PLver={res[6]} TLver={res[7]}")
        return res

    def get_status(self):
        res = self.xfer(bytes([XCP_CMD_GET_STATUS]))
        hexdump("GET_STATUS.res", res)
        if len(res) >= 8 and res[0] == 0xFF:
            sess = res[1]; prot = res[2]
            sess_id = res[4] | (res[5] << 8)
            print(f"  session=0x{sess:02X} prot_mask=0x{prot:02X} session_id={sess_id}")
        return res

    def get_id(self):
        res = self.xfer(bytes([XCP_CMD_GET_ID]))
        hexdump("GET_ID.res", res)
        if len(res) >= 3 and res[0] == 0xFF:
            mode = res[1]; ln = res[2]
            s = res[3:3+ln].decode(errors="ignore")
            print(f"  ID(mode={mode})='{s}'")
        return res

    def set_mta(self, addr, ext=0):
        req = bytes([XCP_CMD_SET_MTA, ext]) + struct.pack("<I", addr)
        res = self.xfer(req)
        hexdump("SET_MTA.res", res)
        return res

    def upload(self, n):
        req = bytes([XCP_CMD_UPLOAD, n & 0xFF])
        res = self.xfer(req)
        hexdump("UPLOAD.res", res)
        if len(res) >= 1 and res[0] == 0xFF:
            data = res[1:]
            print(f"  data={data.hex(' ')}")
            return data
        return b""

    def short_upload(self, addr, n, ext=0):
        req = bytes([XCP_CMD_SHORT_UPLOAD, n & 0xFF, 0x00, ext]) + struct.pack("<I", addr)
        res = self.xfer(req)
        hexdump("SHORT_UPLOAD.res", res)
        if len(res) >= 1 and res[0] == 0xFF:
            data = res[1:]
            print(f"  data={data.hex(' ')}")
            return data
        return b""

    def download(self, data: bytes):
        n = len(data) & 0xFF
        req = bytes([XCP_CMD_DOWNLOAD, n]) + data
        res = self.xfer(req)
        hexdump("DOWNLOAD.res", res)
        return res

    def disconnect(self):
        res = self.xfer(bytes([XCP_CMD_DISCONNECT]))
        hexdump("DISCONNECT.res", res)
        return res

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5555)
    args = ap.parse_args()

    x = XcpUdp(args.host, args.port)

    print("== CONNECT ==")
    x.connect()

    print("\n== GET_STATUS ==")
    x.get_status()

    print("\n== GET_ID ==")
    x.get_id()

    print("\n== SHORT_UPLOAD addr=0x1000 n=4 ==")
    x.short_upload(0x1000, 4)

    print("\n== SET_MTA 0x2000 + DOWNLOAD [AA BB CC DD] + UPLOAD 4 ==")
    x.set_mta(0x2000)
    x.download(bytes.fromhex("AA BB CC DD"))
    x.upload(4)

    print("\n== DISCONNECT ==")
    x.disconnect()

if __name__ == "__main__":
    try:
        main()
    except socket.timeout:
        print("Timeout waiting for slave response", file=sys.stderr)
        sys.exit(1)
