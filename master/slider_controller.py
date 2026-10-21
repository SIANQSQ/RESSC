"""Four-axis HTTP GET desktop controller (Python standard library only)."""
import argparse
import json
import math
import queue
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
from urllib.error import HTTPError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen


def base_url(value):
    value = value.strip().rstrip("/")
    if "://" not in value:
        value = "http://" + value
    parsed = urlsplit(value)
    if (parsed.scheme not in ("http", "https") or not parsed.hostname or
            parsed.username or parsed.password or parsed.query or parsed.fragment or
            parsed.path not in ("", "/")):
        raise ValueError("请输入设备 IP 或根地址，例如 http://192.168.1.100")
    parsed.port  # Validate port syntax too.
    return value


def move_params(cm, rpm):
    if len(cm) != 4 or any(not math.isfinite(v) or not 0 <= v <= 10 for v in cm):
        raise ValueError("四个位置都必须在 0～10 cm 内")
    if not math.isfinite(rpm) or not 1 <= rpm <= 300:
        raise ValueError("转速必须为 1～300 RPM，并符合设备脉冲频率限制")
    return dict({"m" + str(i + 1): format(v * 10, ".6f") for i, v in enumerate(cm)},
                rpm=format(rpm, ".6f"))


def axes_from(status):
    axes = sorted(status["motors"], key=lambda m: m["motor"])
    if len(axes) != 4 or [m["motor"] for m in axes] != [1, 2, 3, 4]:
        raise ValueError("设备状态必须包含 M1～M4")
    for axis in axes:
        length, ppr = float(axis["length_mm"]), float(axis["ppr"])
        if not math.isfinite(length) or length <= 0 or not math.isfinite(ppr) or ppr <= 0:
            raise ValueError("设备行程或细分参数无效")
        for key in ("position_mm", "target_mm"):
            if axis.get(key) is not None and not math.isfinite(float(axis[key])):
                raise ValueError("设备位置反馈无效")
    return axes


def can_control(status):
    return (not status.get("auto_active", False) and
            all(m.get("homed") is True and m.get("position_mm") is not None and
                not m.get("fault") for m in axes_from(status)))


class HttpClient:
    def __init__(self, base):
        self.base = base_url(base)

    def get(self, path, params=None):
        url = self.base + path + ("?" + urlencode(params) if params else "")
        request = Request(url, headers={"Cache-Control": "no-cache"})
        try:
            with urlopen(request, timeout=2) as response:
                return response.status, json.load(response)
        except HTTPError as error:
            return error.code, json.load(error)


class DemoClient:
    """In-memory stage simulation. Never opens a network connection."""
    def __init__(self):
        self.position, self.target = [0.0] * 4, [0.0] * 4
        self.last = time.monotonic()
        self.request_id = 0

    def get(self, path, params=None):
        now = time.monotonic()
        distance = (now - self.last) * 2  # 2 mm/s for preview.
        self.last = now
        for i in range(4):
            delta = self.target[i] - self.position[i]
            self.position[i] += max(-distance, min(distance, delta))
        if path == "/api/status":
            return 200, {"auto_active": False, "self_test": "disabled", "motors": [
                {"motor": i + 1, "position_mm": self.position[i], "target_mm": self.target[i],
                 "homed": True, "home_state": "ready", "fault": "", "ppr": 1600,
                 "length_mm": 100, "moving": abs(self.position[i] - self.target[i]) > 0.001}
                for i in range(4)]}
        if path == "/api/result":
            return 200, {"pending": False, "ok": True, "message": "演示指令已执行"}
        if path == "/api/move_all":
            self.target = [float(params["m" + str(i + 1)]) for i in range(4)]
        elif path == "/api/stop":
            self.target = self.position.copy()
        else:
            return 404, {"message": "unknown endpoint"}
        self.request_id += 1
        return 202, {"accepted": True, "id": self.request_id}


class Controller:
    """Used only by the worker thread; one active command and one latest target."""
    def __init__(self, client, emit, clock=time.monotonic):
        self.client, self.emit, self.clock = client, emit, clock
        self.desired = None
        self.inflight = None
        self.status = None
        self.not_before = 0

    def submit(self, positions, rpm):
        move_params(positions, rpm)
        self.desired = (tuple(positions), rpm)
        self.emit("message", "已保存最新目标；运动中将等待四轴空闲")

    def accept(self, action, params, command=None):
        code, body = self.client.get("/api/" + action, params)
        if code != 202 or not body.get("accepted") or "id" not in body:
            raise ValueError("指令未确认接收：" + str(body.get("message", code)))
        self.inflight = (body["id"], action, command, self.clock())
        self.emit("message", "停止请求已发送" if action == "stop" else "四轴目标已发送，正在确认执行结果")

    def stop(self):
        # STOP supersedes any local target and previous result wait.
        self.desired = None
        self.accept("stop", {})

    def step(self):
        code, body = self.client.get("/api/status")
        if code != 200:
            raise ValueError("状态不可用：" + str(body.get("message", code)))
        axes = axes_from(body)
        self.status = body
        self.emit("status", body)
        if body.get("auto_active") or not can_control(body):
            self.desired = None  # Never execute an old target automatically after reset/fault.
        if self.inflight:
            request_id, action, sent, started = self.inflight
            code, result = self.client.get("/api/result", {"id": request_id})
            if code == 202:
                if self.clock() - started > 6:
                    raise ValueError("执行结果超时；指令可能已执行，请重新连接查状态，勿重复发送")
                return
            self.inflight = None
            self.not_before = self.clock() + 0.25  # Wait for a fresh firmware snapshot.
            if code == 409 and action == "move_all":
                if self.desired is None and can_control(body):
                    self.desired = sent
                self.emit("message", "设备忙，等待当前运动完成")
            elif code != 200 or not result.get("ok"):
                self.desired = None
                self.emit("message", "设备拒绝指令：" + str(result.get("message", code)))
            else:
                self.emit("stopped" if action == "stop" else "message",
                          "停止指令已执行" if action == "stop" else "定位指令已启动，等待位置反馈")
            return
        if (self.desired is None or self.clock() < self.not_before or
                not can_control(body) or any(m["moving"] for m in axes)):
            return
        positions, rpm = self.desired
        params = move_params(positions, rpm)
        if any(cm * 10 > float(m["length_mm"]) for cm, m in zip(positions, axes)):
            self.desired = None
            self.emit("message", "目标超出设备实际行程，未发送")
            return
        maximum_rpm = min(120000 / float(m["ppr"]) for m in axes)
        if rpm > maximum_rpm + 1e-9:
            self.desired = None
            self.emit("message", "当前细分允许的最高转速为 %.3f RPM，请调低转速" % maximum_rpm)
            return
        self.desired = None
        self.accept("move_all", params, (positions, rpm))


class Worker(threading.Thread):
    def __init__(self, events, demo=False):
        super().__init__(daemon=True)
        self.events, self.demo = events, demo
        self.commands = queue.Queue()
        self.closed = threading.Event()
        self.controller = None
        self.session = 0

    def emit(self, kind, value):
        self.events.put((self.session, kind, value))

    def run(self):
        while not self.closed.is_set():
            try:
                actions = []
                while True:
                    try:
                        actions.append(self.commands.get_nowait())
                    except queue.Empty:
                        break
                # A stop drops all slider targets still queued locally.
                if any(a[0] == "stop" for a in actions):
                    actions = [a for a in actions if a[0] != "move"]
                for action, value in actions:
                    if action == "connect":
                        host, self.session = value
                        client = DemoClient() if self.demo else HttpClient(host)
                        self.controller = Controller(client, self.emit)
                        self.emit("message", "正在读取设备状态…")
                    elif action == "disconnect":
                        self.session = value
                        self.controller = None
                        self.emit("disconnected", "已断开；设备上已经启动的运动会继续")
                    elif self.controller and action == "move":
                        self.controller.submit(*value)
                    elif self.controller and action == "stop":
                        self.controller.stop()
                    elif self.controller and action == "clear":
                        self.controller.desired = None
                if self.controller:
                    self.controller.step()
            except Exception as error:
                # Never replay motion after an uncertain network outcome.
                self.controller = None
                self.emit("disconnected", "连接/请求失败：%s。已清除待发目标，请重新连接确认位置。" % error)
            self.closed.wait(0.2)


class SliderApp:
    BG, CARD, TEXT, MUTED = "#eef3f9", "#ffffff", "#14243a", "#64748b"

    def __init__(self, root, initial_host, demo=False):
        self.root, self.demo = root, demo
        self.events = queue.Queue()
        self.worker = Worker(self.events, demo)
        self.worker.start()
        self.connected = self.ready = self.synced = False
        self.session = 0
        self.current = [None] * 4
        self.axes = None
        self.debounce = None
        self.updating = False
        root.title("四轴滑台控制台" + (" · 演示模式" if demo else ""))
        root.geometry("1040x850")
        root.minsize(900, 800)
        root.configure(bg=self.BG)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background=self.BG)
        style.configure("TLabel", background=self.BG, foreground=self.TEXT, font=("Segoe UI", 10))
        style.configure("TButton", font=("Segoe UI", 10), padding=(12, 7))
        style.configure("TCheckbutton", background=self.BG, font=("Segoe UI", 10))
        style.configure("Horizontal.TScale", troughcolor="#dde7f2", background=self.CARD)
        outer = ttk.Frame(root, padding=24)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="四轴滑台控制台", font=("Segoe UI", 23, "bold")).pack(anchor="w")
        ttk.Label(outer, text="0–10 cm 绝对定位  /  四轴 GET 指令  /  实时位置反馈",
                  foreground=self.MUTED).pack(anchor="w", pady=(5, 15))
        connection = ttk.Frame(outer)
        connection.pack(fill="x")
        ttk.Label(connection, text="设备地址").pack(side="left")
        self.host = tk.StringVar(value=initial_host)
        self.host_entry = ttk.Entry(connection, textvariable=self.host, width=31)
        self.host_entry.pack(side="left", padx=8)
        if demo:
            self.host_entry.configure(state="disabled")
        self.connect_btn = ttk.Button(connection, text="连接设备", command=self.connect)
        self.connect_btn.pack(side="left")
        ttk.Button(connection, text="断开", command=self.disconnect).pack(side="left", padx=6)
        ttk.Label(connection, text="转速 RPM").pack(side="left", padx=(12, 5))
        self.rpm = tk.StringVar(value="75")
        self.rpm_entry = ttk.Entry(connection, textvariable=self.rpm, width=7)
        self.rpm_entry.pack(side="left")
        options = ttk.Frame(outer)
        options.pack(fill="x", pady=(13, 7))
        self.auto = tk.BooleanVar(value=False)
        ttk.Checkbutton(options, text="拖动后自动发送（等待空闲，仅保留最新目标）",
                        variable=self.auto, command=self.auto_changed).pack(side="left")
        self.sync_axes = tk.BooleanVar(value=False)
        ttk.Checkbutton(options, text="四轴同步拖动（四个目标设为相同位置）",
                        variable=self.sync_axes, command=self.sync_changed).pack(side="left", padx=(18, 0))
        self.state_text = tk.StringVar(value="演示模式：不连接真实设备" if demo else "未连接 · 请输入 ESP32 的 IP 地址")
        ttk.Label(outer, textvariable=self.state_text, foreground="#1768ae").pack(anchor="w", pady=(0, 10))
        self.targets, self.position_text, self.row_states = [], [], []
        self.canvases, self.scales, self.spins = [], [], []
        self.controls = []
        for index in range(4):
            self.make_axis(outer, index)
        actions = ttk.Frame(outer)
        actions.pack(fill="x", pady=(10, 5))
        self.send_btn = ttk.Button(actions, text="发送四轴目标", command=self.send_targets)
        self.send_btn.pack(side="left")
        self.zero_btn = ttk.Button(actions, text="四轴回到 0 cm", command=self.move_zero)
        self.zero_btn.pack(side="left", padx=8)
        self.stop_btn = tk.Button(actions, text="停止全部运动", command=self.stop,
                                  bg="#c73546", fg="white", activebackground="#a52535",
                                  font=("Segoe UI", 11, "bold"), relief="flat", padx=18, pady=8)
        self.stop_btn.pack(side="right")
        self.controls += [self.send_btn, self.zero_btn]
        self.message = tk.StringVar(value="连接后同步设备当前位置；仅显示位置，不会自动发送初始目标。")
        ttk.Label(outer, textvariable=self.message, wraplength=970, foreground=self.MUTED).pack(anchor="w", pady=(8, 4))
        self.url_text = tk.StringVar(value="")
        ttk.Entry(outer, textvariable=self.url_text, state="readonly").pack(fill="x", pady=4)
        ttk.Label(outer, text="位置来自步数推算。每次上电/复位前请手动放回原点；断开或关闭窗口不会停止已启动的运动。",
                  foreground=self.MUTED, wraplength=970).pack(anchor="w", pady=(5, 0))
        self.enable_controls(False)
        self.stop_btn.configure(state="disabled")
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.receive_events)
        if demo:
            root.after(150, self.connect)

    def make_axis(self, parent, index):
        card = tk.Frame(parent, bg=self.CARD, padx=16, pady=9)
        card.pack(fill="x", pady=4)
        header = tk.Frame(card, bg=self.CARD)
        header.pack(fill="x")
        tk.Label(header, text="M%d  滑台 %d" % (index + 1, index + 1), bg=self.CARD,
                 fg=self.TEXT, font=("Segoe UI", 12, "bold")).pack(side="left")
        state = tk.StringVar(value="等待连接")
        self.row_states.append(state)
        tk.Label(header, textvariable=state, bg=self.CARD, fg=self.MUTED).pack(side="left", padx=15)
        feedback = tk.StringVar(value="当前位置  — cm")
        self.position_text.append(feedback)
        tk.Label(header, textvariable=feedback, bg=self.CARD, fg="#087f9c",
                 font=("Segoe UI", 11, "bold")).pack(side="right")
        canvas = tk.Canvas(card, height=28, bg=self.CARD, highlightthickness=0)
        canvas.pack(fill="x", pady=(1, 0))
        self.canvases.append(canvas)
        canvas.bind("<Configure>", lambda e, i=index: self.draw_axis(i))
        row = tk.Frame(card, bg=self.CARD)
        row.pack(fill="x")
        tk.Label(row, text="0 cm", bg=self.CARD, fg=self.MUTED).pack(side="left")
        target = tk.DoubleVar(value=0)
        self.targets.append(target)
        scale = ttk.Scale(row, from_=0, to=10, variable=target,
                          command=lambda v, i=index: self.target_changed(i, v))
        scale.pack(side="left", fill="x", expand=True, padx=8)
        self.scales.append(scale)
        tk.Label(row, text="10 cm   目标", bg=self.CARD, fg=self.MUTED).pack(side="left")
        spin = ttk.Spinbox(row, from_=0, to=10, increment=0.01, format="%.2f", width=7,
                           textvariable=target, command=lambda i=index: self.target_changed(i))
        spin.pack(side="left", padx=(7, 3))
        tk.Label(row, text="cm", bg=self.CARD, fg=self.MUTED).pack(side="left")
        spin.bind("<Return>", lambda e, i=index: self.target_changed(i))
        spin.bind("<FocusOut>", lambda e, i=index: self.target_changed(i))
        self.spins.append(spin)
        self.controls += [scale, spin]

    def enable_controls(self, enabled):
        self.ready = enabled
        for control in self.controls:
            control.configure(state="normal" if enabled else "disabled")

    def connect(self):
        try:
            host = base_url(self.host.get())
        except ValueError as error:
            messagebox.showerror("设备地址", str(error))
            return
        self.connected = self.synced = False
        self.axes = None
        self.cancel_debounce()
        self.enable_controls(False)
        self.state_text.set("正在连接…")
        self.session += 1
        self.connect_btn.configure(state="disabled", text="连接中…")
        self.host_entry.configure(state="disabled")
        self.worker.commands.put(("connect", (host, self.session)))

    def disconnect(self):
        self.cancel_debounce()
        self.enable_controls(False)
        self.session += 1
        self.worker.commands.put(("disconnect", self.session))

    def cancel_debounce(self):
        if self.debounce:
            self.root.after_cancel(self.debounce)
            self.debounce = None

    def auto_changed(self):
        self.cancel_debounce()
        # Discard local pending targets when automatic sending is turned off.
        if not self.auto.get():
            self.worker.commands.put(("clear", None))

    def sync_changed(self):
        self.message.set("四轴同步已开启：下次拖动或修改任一目标，四轴将设为相同位置。"
                         if self.sync_axes.get() else "四轴同步已关闭：可独立调整每个目标。")

    def target_changed(self, index, value=None):
        if self.updating:
            return
        try:
            v = float(value) if value is not None else self.targets[index].get()
            if not math.isfinite(v) or not 0 <= v <= 10:
                raise ValueError("目标必须在 0～10 cm 内")
            affected = range(4) if self.sync_axes.get() else (index,)
            self.updating = True
            try:
                for i in affected:
                    self.targets[i].set(round(v, 2))
                    self.draw_axis(i)
            finally:
                self.updating = False
            self.preview_url()
            self.cancel_debounce()
            if self.auto.get() and self.ready:
                self.debounce = self.root.after(350, self.send_targets)
        except (ValueError, tk.TclError) as error:
            self.message.set("位置输入无效：%s" % error)

    def values(self):
        return [float(v.get()) for v in self.targets], float(self.rpm.get())

    def preview_url(self):
        try:
            values, rpm = self.values()
            self.url_text.set(base_url(self.host.get()) + "/api/move_all?" + urlencode(move_params(values, rpm)))
        except (ValueError, tk.TclError):
            self.url_text.set("请输入 0～10 cm 的目标和有效转速")

    def send_targets(self):
        self.cancel_debounce()
        if not self.ready:
            self.message.set("请先连接设备，并等待自检结束、原点有效")
            return
        try:
            values, rpm = self.values()
            move_params(values, rpm)
        except (ValueError, tk.TclError) as error:
            self.message.set(str(error))
            return
        self.worker.commands.put(("move", (values, rpm)))
        self.preview_url()

    def move_zero(self):
        self.updating = True
        for target in self.targets:
            target.set(0)
        self.updating = False
        for i in range(4):
            self.draw_axis(i)
        self.send_targets()  # Moves to existing zero; never sends software ZERO.

    def stop(self):
        self.cancel_debounce()
        self.worker.commands.put(("stop", None))
        self.message.set("正在请求停止；网络 STOP 不等于硬件急停")

    def draw_axis(self, index):
        canvas = self.canvases[index]
        width = max(120, canvas.winfo_width())
        left, right = 12, width - 12
        canvas.delete("all")
        canvas.create_line(left, 15, right, 15, fill="#dce6f0", width=6)
        for k in range(11):
            x = left + (right - left) * k / 10
            canvas.create_line(x, 9, x, 21, fill="#c1cddd")
        position = self.current[index]
        if position is not None:
            x = left + (right - left) * min(10, max(0, position)) / 10
            canvas.create_line(left, 15, x, 15, fill="#22a4bb", width=6)
            canvas.create_rectangle(x - 7, 6, x + 7, 24, fill="#087f9c", outline="")
        try:
            x = left + (right - left) * self.targets[index].get() / 10
            canvas.create_line(x, 2, x, 27, fill="#f59e0b", width=2)
        except tk.TclError:
            pass

    def update_status(self, status):
        axes = axes_from(status)
        self.axes = axes
        self.connected = True
        self.connect_btn.configure(state="disabled", text="已连接")
        self.host_entry.configure(state="disabled")
        ready = can_control(status)
        idle = not any(m["moving"] for m in axes)
        self.stop_btn.configure(state="normal")
        self.enable_controls(ready and (self.synced or idle))
        if status.get("auto_active"):
            label = "上电自检中 · " + str(status.get("self_test", ""))
        elif not ready:
            label = "原点无效或设备故障 · 请检查设备状态"
        else:
            label = "正在运动" if any(m["moving"] for m in axes) else "已连接 · 四轴空闲"
        maximum = min(120000 / float(m["ppr"]) for m in axes)
        self.state_text.set(("演示模式 · " if self.demo else "") + label + " · 最高 %.3f RPM" % maximum)
        self.updating = True
        for i, axis in enumerate(axes):
            value = axis.get("position_mm")
            self.current[i] = None if value is None else float(value) / 10
            self.position_text[i].set("当前位置  %s cm" % ("—" if value is None else "%.3f" % self.current[i]))
            self.row_states[i].set(axis.get("fault") or ("运动中" if axis["moving"] else "空闲"))
            if not self.synced and ready and idle:
                self.targets[i].set(round(min(10, max(0, self.current[i])), 2))
            self.draw_axis(i)
        if ready and idle:
            self.synced = True
        elif not ready:
            self.synced = False
            self.cancel_debounce()
        self.updating = False
        self.preview_url()

    def receive_events(self):
        while True:
            try:
                session, kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if session != self.session:
                continue
            if kind == "status":
                self.update_status(value)
            elif kind == "disconnected":
                self.connected = self.synced = False
                self.axes = None
                self.cancel_debounce()
                self.enable_controls(False)
                self.stop_btn.configure(state="disabled")
                self.connect_btn.configure(state="normal", text="连接设备")
                self.host_entry.configure(state="disabled" if self.demo else "normal")
                self.state_text.set("未连接 · 位置反馈已过期")
                self.message.set(value)
                for label in self.row_states:
                    label.set("反馈过期")
            elif kind == "stopped":
                self.synced = False
                self.message.set(value)
            else:
                self.message.set(value)
        self.root.after(100, self.receive_events)

    def close(self):
        self.cancel_debounce()
        self.worker.closed.set()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://truss-ctrl.local")
    parser.add_argument("--demo", action="store_true", help="offline preview; no network")
    args = parser.parse_args()
    root = tk.Tk()
    SliderApp(root, args.base, args.demo)
    root.mainloop()


if __name__ == "__main__":
    main()
