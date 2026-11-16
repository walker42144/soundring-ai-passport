import importlib.util, sys, json, hashlib, hmac, io, socket, threading, urllib.request
from urllib.parse import parse_qs, urlparse, urlencode
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
spec = importlib.util.spec_from_file_location(
    "pair_gui", r"tools\soundring_pairing_gui.py"
)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

# The public project does not embed anyone's redirect URI.
valid_redirect = "http://" + "127.0.0.1" + ":" + "9011" + "/oauth-return"
assert mod.parse_redirect(valid_redirect).port == 9011
for invalid in ("", "https://example.com/auth", "http://localhost:9011/auth",
                "http://127.0.0.1/auth", "http://127.0.0.1:9011/auth?code=x"):
    try:
        mod.parse_redirect(invalid)
    except mod.PairError:
        pass
    else:
        raise AssertionError("invalid redirect accepted")


class Device:
    def __init__(self):
        self.key = hashlib.pbkdf2_hmac(
            "sha256", b"12345678", bytes(range(16)), 20000, 32
        )
        self.pending = []
        self.last = 0
        self.connected = False
        self.auth = False

    def write(self, data):
        obj = json.loads(data)
        cmd = obj["cmd"]
        err = None
        if cmd == "hello":
            if not hmac.compare_digest(
                obj["mac"], hmac.new(self.key, b"hello", hashlib.sha256).hexdigest()
            ):
                err = "bad_code"
        elif cmd in ("wifi", "auth"):
            if obj["n"] != self.last + 1:
                err = "bad_sequence"
            else:
                aad = f"sr1|{cmd}|{obj['n']}".encode()
                ct = bytes.fromhex(obj["ct"]) + bytes.fromhex(obj["tag"])
                data = mod.AESGCM(self.key).decrypt(bytes.fromhex(obj["iv"]), ct, aad)
                assert isinstance(json.loads(data), dict)
                self.last += 1
                self.connected |= cmd == "wifi"
                self.auth |= cmd == "auth"
        note = (
            f"connected={int(self.connected)},time=1,auth={int(self.auth)},ip=-"
            if cmd == "status"
            else "saved"
        )
        response = {"cmd": cmd, "error": err} if err else {"cmd": cmd, "note": note}
        self.pending.append(
            (("SRERR " if err else "SROK ") + json.dumps(response) + "\r\n").encode()
        )
        return len(data)

    def readline(self):
        return self.pending.pop(0) if self.pending else b""


port = Device()
key = port.key
mod.exchange(
    port,
    {
        "v": 1,
        "cmd": "hello",
        "mac": hmac.new(key, b"hello", hashlib.sha256).hexdigest(),
    },
    "hello",
)
mod.secure(port, key, "wifi", 1, {"ssid": "sample", "pass": "sample-pass"})
mod.secure(
    port,
    key,
    "auth",
    2,
    {
        "client_id": "a" * 32,
        "access_token": "a",
        "refresh_token": "r",
        "scope": "x",
        "expires_at_ms": 1,
    },
)
assert mod.status(port)["connected"] == "1" and mod.status(port)["auth"] == "1"
port = Device()
try:
    mod.exchange(port, {"v": 1, "cmd": "hello", "mac": "00" * 32}, "hello")
except mod.PairError as e:
    assert "配对码" in str(e)
else:
    raise AssertionError("bad code accepted")
print("mock pairing wifi + auth + status + bad code: PASS")

# Exercise the configurable loopback callback and PKCE token request end-to-end
# with local mocks; no real Spotify account or device is contacted.
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    callback_port = probe.getsockname()[1]
callback = "http://" + "127.0.0.1:" + str(callback_port) + "/test-return"
original_browser, original_urlopen = mod.webbrowser.open, mod.urlopen
def fake_browser(url):
    params = parse_qs(urlparse(url).query)
    assert params["redirect_uri"][0] == callback
    def deliver():
        address = callback + "?" + urlencode({"state": params["state"][0], "code": "test-code"})
        with urllib.request.urlopen(address, timeout=5) as response:
            assert response.status == 200
    threading.Thread(target=deliver, daemon=True).start()
    return True
def fake_token(request, timeout):
    assert timeout == 30
    body = parse_qs(request.data.decode())
    assert body["redirect_uri"][0] == callback and body["code"][0] == "test-code"
    return io.BytesIO(b'{"access_token":"example","refresh_token":"example","expires_in":3600}')
mod.webbrowser.open, mod.urlopen = fake_browser, fake_token
try:
    assert mod.spotify("a" * 32, callback)["refresh_token"] == "example"
finally:
    mod.webbrowser.open, mod.urlopen = original_browser, original_urlopen
print("configurable callback and PKCE flow: PASS")
import tkinter as tk

w = tk.Tk()
w.withdraw()
app = mod.App(w)
w.update_idletasks()
assert app.want_wifi.get() and app.want_auth.get()
app.run()
assert "串口" in app.status.cget("text") or "配对码" in app.status.cget("text")
w.destroy()
print("GUI construction and empty-field validation: PASS")
