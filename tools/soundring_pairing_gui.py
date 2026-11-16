"""SoundRing pairing center for Windows. Credentials are held in memory only."""

import base64, hashlib, hmac, ipaddress, json, os, queue, secrets, threading, time, webbrowser
import sys
import tkinter as tk
from tkinter import ttk
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlencode, urlparse, parse_qs
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError
import serial
from serial.tools import list_ports
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

SCOPES = "user-read-playback-state user-modify-playback-state playlist-read-private playlist-read-collaborative user-read-recently-played user-library-read user-library-modify"
BG = "#101310"
PANEL = "#1a201c"
FG = "#f4f7f4"
SUB = "#b0bab2"
GREEN = "#1ed760"
BORDER = "#344139"


class PairError(Exception):
    pass


def discover():
    return [
        p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001
    ]


def req_line(port, expected, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", "replace").strip()
        for prefix in expected:
            if line.startswith(prefix + " "):
                try:
                    return prefix, json.loads(line[len(prefix) + 1 :])
                except (ValueError, TypeError):
                    continue
    raise PairError("等待设备响应超时。请确认设备停在“电脑配对”页面，配对码仍然有效。")


def exchange(port, obj, cmd, timeout=12):
    port.write(
        (json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\r\n").encode(
            "utf-8"
        )
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        prefix, reply = req_line(
            port, ("SROK", "SRERR"), max(0.1, deadline - time.monotonic())
        )
        if prefix == "SRERR":
            reason = reply.get("error", "unknown")
            hints = {
                "bad_code": "配对码不正确，请检查设备屏幕。连续输错 5 次需重新生成。",
                "need_hello": "配对会话已结束，请在设备上重新进入配对页面。",
                "bad_sequence": "配对会话已使用，请退出设备配对页面后重新进入。",
                "bad_cipher": "配对信息已过期，请重新生成配对码。",
                "wifi_rejected": "设备未能保存 Wi-Fi 信息，请检查名称和密码长度。",
                "auth_rejected": "设备未能保存 Spotify 授权，请重新尝试。",
                "line_too_long": "授权信息超过固件可接收的长度。",
            }
            raise PairError(hints.get(reason, "设备拒绝请求：" + str(reason)))
        if reply.get("cmd") == cmd:
            return reply
    raise PairError("设备未确认 " + cmd + " 操作。")


def secure(port, key, cmd, seq, payload):
    iv = os.urandom(12)
    plain = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
        "utf-8"
    )
    if len(plain) > 1536:
        raise PairError("配置数据超过设备的长度限制。")
    encrypted = AESGCM(key).encrypt(iv, plain, f"sr1|{cmd}|{seq}".encode())
    return exchange(
        port,
        {
            "v": 1,
            "cmd": cmd,
            "n": seq,
            "iv": iv.hex(),
            "ct": encrypted[:-16].hex(),
            "tag": encrypted[-16:].hex(),
        },
        cmd,
        30,
    )


def status(port):
    reply = exchange(port, {"v": 1, "cmd": "status"}, "status")
    return dict(
        part.split("=", 1) for part in reply.get("note", "").split(",") if "=" in part
    )


def parse_redirect(value):
    try:
        parsed = urlparse(value)
        if (parsed.scheme != "http" or not parsed.hostname or not parsed.port or
            not ipaddress.IPv4Address(parsed.hostname).is_loopback or
            not parsed.path.startswith("/") or parsed.query or parsed.fragment or
            parsed.username or parsed.password):
            raise ValueError("invalid redirect")
        return parsed
    except ValueError as exc:
        raise PairError("请输入 Spotify 应用中登记的本机 IPv4 回调地址，包含端口和路径。") from exc


def spotify(client_id, redirect_uri):
    redirect = parse_redirect(redirect_uri)
    verifier = base64.urlsafe_b64encode(os.urandom(48)).rstrip(b"=").decode()
    challenge = (
        base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest())
        .rstrip(b"=")
        .decode()
    )
    state = secrets.token_urlsafe(24)
    result = {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            parsed = urlparse(self.path)
            q = parse_qs(parsed.query)
            if parsed.path != redirect.path:
                self.send_error(404)
                return
            if q.get("state", [""])[0] != state:
                self.send_error(400)
                return
            result["error"] = q.get("error", [""])[0]
            result["code"] = q.get("code", [""])[0]
            body = "授权结果已收到。请返回 SoundRing 配对中心继续。".encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    try:
        server = HTTPServer((redirect.hostname, redirect.port), Handler)
    except OSError as exc:
        raise PairError("回调端口不可用，请关闭占用该端口的程序后重试。") from exc
    server.timeout = 1
    params = {
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": redirect_uri,
        "scope": SCOPES,
        "state": state,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
    }
    try:
        if not webbrowser.open(
            "https://accounts.spotify.com/authorize?" + urlencode(params)
        ):
            raise PairError("无法打开浏览器，请检查 Windows 默认浏览器设置后重试。")
        deadline = time.monotonic() + 240
        while not result and time.monotonic() < deadline:
            server.handle_request()
    finally:
        server.server_close()
    if not result:
        raise PairError("等待 Spotify 授权超时，请重新开始。")
    if result.get("error") or not result.get("code"):
        raise PairError("Spotify 授权未完成，请检查账号权限后重试。")
    body = urlencode(
        {
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": redirect_uri,
            "client_id": client_id,
            "code_verifier": verifier,
        }
    ).encode()
    try:
        with urlopen(
            Request(
                "https://accounts.spotify.com/api/token",
                data=body,
                headers={"Content-Type": "application/x-www-form-urlencoded"},
            ),
            timeout=30,
        ) as response:
            token = json.load(response)
    except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
        raise PairError(
            "Spotify 令牌获取失败。请检查网络、Client ID 和应用登记的回调地址。"
        ) from exc
    if not token.get("access_token") or not token.get("refresh_token"):
        raise PairError("Spotify 未返回完整授权信息，请重新授权。")
    return {
        "client_id": client_id,
        "access_token": token["access_token"],
        "refresh_token": token["refresh_token"],
        "scope": token.get("scope", SCOPES),
        "expires_at_ms": int(time.time() * 1000)
        + int(token.get("expires_in", 3600)) * 1000,
    }


class App:
    def __init__(self, window):
        self.w = window
        window.title("SoundRing 配对中心")
        window.geometry("660x880")
        window.minsize(620, 820)
        window.configure(bg=BG)
        self.q = queue.Queue()
        self.busy = False
        self.port = tk.StringVar()
        self.code = tk.StringVar()
        self.ssid = tk.StringVar()
        self.password = tk.StringVar()
        self.client = tk.StringVar()
        self.redirect = tk.StringVar()
        self.want_wifi = tk.BooleanVar(value=True)
        self.want_auth = tk.BooleanVar(value=True)
        self.build()
        self.refresh_ports()
        self.tick()

    def label(self, parent, text, size=10, color=FG, **kw):
        o = tk.Label(
            parent,
            text=text,
            bg=parent["bg"],
            fg=color,
            font=("Microsoft YaHei UI", size),
            anchor="w",
            **kw,
        )
        o.pack(fill="x")
        return o

    def build(self):
        outer = tk.Frame(self.w, bg=BG, padx=24, pady=18)
        outer.pack(fill="both", expand=True)
        self.label(outer, "SOUNDRING  /  配对中心", 17, GREEN)
        self.label(
            outer, "让设备连接 Wi-Fi，并在浏览器完成 Spotify 授权。", 10, SUB, pady=6
        )
        card = tk.Frame(outer, bg=PANEL, padx=18, pady=14)
        card.pack(fill="x", pady=(12, 8))
        self.label(card, "01  连接设备", 12, FG)
        self.label(
            card, "USB 连接设备，在设备上打开“设置 → 电脑配对”。", 9, SUB, pady=4
        )
        row = tk.Frame(card, bg=PANEL)
        row.pack(fill="x", pady=4)
        self.ports = ttk.Combobox(
            row, textvariable=self.port, state="readonly", width=16
        )
        self.ports.pack(side="left")
        tk.Button(row, text="刷新设备", command=self.refresh_ports).pack(
            side="left", padx=8
        )
        self.label(card, "设备上的 8 位配对码", 9, SUB, pady=3)
        self.entry(card, self.code, False)
        card2 = tk.Frame(outer, bg=PANEL, padx=18, pady=12)
        card2.pack(fill="x", pady=8)
        tk.Checkbutton(
            card2,
            text="配置 Wi-Fi",
            variable=self.want_wifi,
            bg=PANEL,
            fg=FG,
            selectcolor=BG,
            activebackground=PANEL,
            activeforeground=FG,
            font=("Microsoft YaHei UI", 11, "bold"),
        ).pack(anchor="w")
        self.label(card2, "Wi-Fi 名称（仅支持 2.4 GHz）", 9, SUB, pady=2)
        self.entry(card2, self.ssid, False)
        self.label(card2, "Wi-Fi 密码（输入时隐藏；开放网络留空）", 9, SUB, pady=2)
        self.entry(card2, self.password, True)
        card3 = tk.Frame(outer, bg=PANEL, padx=18, pady=12)
        card3.pack(fill="x", pady=8)
        tk.Checkbutton(
            card3,
            text="授权 Spotify",
            variable=self.want_auth,
            bg=PANEL,
            fg=FG,
            selectcolor=BG,
            activebackground=PANEL,
            activeforeground=FG,
            font=("Microsoft YaHei UI", 11, "bold"),
        ).pack(anchor="w")
        self.label(
            card3, "Spotify 应用 Client ID（不需要 Client Secret）", 9, SUB, pady=3
        )
        self.entry(card3, self.client, False)
        self.label(card3, "Spotify 应用中登记的本机回调地址", 9, SUB, pady=3)
        self.entry(card3, self.redirect, False)
        self.start = tk.Button(
            outer,
            text="开始配对",
            bg=GREEN,
            fg="#07120b",
            font=("Microsoft YaHei UI", 11, "bold"),
            relief="flat",
            height=2,
            command=self.run,
        )
        self.start.pack(fill="x", pady=(8, 5))
        self.status = tk.Label(
            outer,
            text="准备就绪。可取消勾选不需要更新的项目。",
            bg=BG,
            fg=SUB,
            font=("Microsoft YaHei UI", 9),
            anchor="w",
            wraplength=590,
            justify="left",
        )
        self.status.pack(fill="x")

    def entry(self, parent, var, secret):
        e = tk.Entry(
            parent,
            textvariable=var,
            show="●" if secret else "",
            font=("Microsoft YaHei UI", 10),
            bg=BG,
            fg=FG,
            insertbackground=FG,
            relief="flat",
            highlightthickness=1,
            highlightbackground=BORDER,
        )
        e.pack(fill="x", ipady=4)
        return e

    def refresh_ports(self):
        ports = discover()
        self.ports["values"] = ports
        if len(ports) == 1:
            self.port.set(ports[0])
        elif self.port.get() not in ports:
            self.port.set("")

    def report(self, message):
        self.q.put(("status", message))

    def run(self):
        if self.busy:
            return
        cfg = {
            "port": self.port.get(),
            "code": self.code.get().strip(),
            "wifi": self.want_wifi.get(),
            "auth": self.want_auth.get(),
            "ssid": self.ssid.get(),
            "password": self.password.get(),
            "client": self.client.get().strip(),
            "redirect": self.redirect.get().strip(),
        }
        if not cfg["port"]:
            self.status.config(
                text="请连接设备并点击“刷新设备”，选择串口。", fg="#ffb4a9"
            )
            return
        if (
            len(cfg["code"]) != 8
            or not cfg["code"].isascii()
            or not cfg["code"].isdigit()
        ):
            self.status.config(text="请输入设备屏幕上的 8 位配对码。", fg="#ffb4a9")
            return
        if not cfg["wifi"] and not cfg["auth"]:
            self.status.config(text="请至少选择一个需要配置的项目。", fg="#ffb4a9")
            return
        if cfg["wifi"] and (
            not cfg["ssid"]
            or len(cfg["ssid"].encode()) > 32
            or len(cfg["password"].encode()) > 64
        ):
            self.status.config(
                text="请填写 Wi-Fi 名称（最多 32 字节）和有效密码。", fg="#ffb4a9"
            )
            return
        if cfg["auth"] and (
            len(cfg["client"]) != 32
            or any(c not in "0123456789abcdefABCDEF" for c in cfg["client"])
        ):
            self.status.config(
                text="请输入 Spotify 应用的 32 位 Client ID。", fg="#ffb4a9"
            )
            return
        if cfg["auth"]:
            try:
                parse_redirect(cfg["redirect"])
            except PairError as exc:
                self.status.config(text=str(exc), fg="#ffb4a9")
                return
        self.busy = True
        self.start.config(state="disabled")
        threading.Thread(target=self.worker, args=(cfg,), daemon=True).start()

    def worker(self, cfg):
        port = None
        try:
            self.report("正在连接设备，等待配对广播…")
            port = serial.Serial(
                port=None, baudrate=115200, timeout=0.3, write_timeout=3
            )
            port.port = cfg["port"]
            port.dtr = False
            port.rts = False
            port.open()
            _, frame = req_line(port, ("SRPAIR1",), 20)
            nonce = bytes.fromhex(frame.get("nonce", ""))
            if frame.get("v") != 1 or len(nonce) != 16:
                raise PairError("设备返回了无法识别的配对信息。")
            key = hashlib.pbkdf2_hmac("sha256", cfg["code"].encode(), nonce, 20000, 32)
            exchange(
                port,
                {
                    "v": 1,
                    "cmd": "hello",
                    "mac": hmac.new(key, b"hello", hashlib.sha256).hexdigest(),
                },
                "hello",
            )
            seq = 0
            if cfg["wifi"]:
                self.report("正在发送 Wi-Fi 配置并等待设备联网…")
                seq += 1
                secure(
                    port,
                    key,
                    "wifi",
                    seq,
                    {"ssid": cfg["ssid"], "pass": cfg["password"]},
                )
                connected = False
                for _ in range(20):
                    time.sleep(1.5)
                    s = status(port)
                    if s.get("connected") == "1":
                        connected = True
                        break
                if not connected:
                    raise PairError(
                        "配置已发送，但设备在 30 秒内未取得网络地址。请检查 2.4 GHz 网络、密码和信号。"
                    )
            if cfg["auth"]:
                self.report("请在浏览器中登录 Spotify 并同意授权…")
                token = spotify(cfg["client"], cfg["redirect"])
                self.report("正在安全写入 Spotify 授权…")
                seq += 1
                secure(port, key, "auth", seq, token)
                if status(port).get("auth") != "1":
                    raise PairError("设备未确认保存授权，请重新尝试。")
            try:
                exchange(port, {"v": 1, "cmd": "bye"}, "bye", 4)
            except (PairError, serial.SerialException):
                pass
            self.q.put(("done", "配置完成。设备已确认保存所选项目。"))
        except (PairError, serial.SerialException, OSError, ValueError) as exc:
            self.q.put(("error", str(exc)))
        except Exception:
            self.q.put(
                ("error", "配置过程中发生意外错误，请重新进入设备配对页面后重试。")
            )
        finally:
            if port:
                try:
                    port.close()
                except serial.SerialException:
                    pass
            cfg["password"] = ""
            cfg["code"] = ""

    def tick(self):
        try:
            while True:
                typ, msg = self.q.get_nowait()
                self.status.config(
                    text=msg,
                    fg=GREEN if typ == "done" else "#ffb4a9" if typ == "error" else SUB,
                )
                if typ in ("done", "error"):
                    self.busy = False
                    self.start.config(state="normal")
                    self.password.set("")
                    self.code.set("")
        except queue.Empty:
            pass
        self.w.after(100, self.tick)


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        key = hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 1, 32)
        assert (
            key.hex()
            == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"
        )
        iv = bytes(12)
        assert (
            AESGCM(key).decrypt(iv, AESGCM(key).encrypt(iv, b"test", b"aad"), b"aad")
            == b"test"
        )
        window = tk.Tk()
        window.withdraw()
        app = App(window)
        window.update_idletasks()
        assert app.want_wifi.get() and app.want_auth.get()
        window.destroy()
    else:
        window = tk.Tk()
        App(window)
        window.mainloop()
