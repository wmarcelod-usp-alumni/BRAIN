import os
import json
import time
import math
import queue
import random
import threading
from dataclasses import dataclass, asdict
from typing import Callable, Dict, Any, Optional, List, Tuple

from flask import Flask, Response, request, jsonify, render_template
import serial

# =========================
# Config do host
# =========================
SERIAL_PORT = os.getenv("SERIAL_PORT", "/dev/ttyACM0")
SERIAL_BAUD = int(os.getenv("SERIAL_BAUD", "115200"))

# OBD "protocols" (ELM style):
# 6 = CAN 11/500
# 7 = CAN 29/500
# 8 = CAN 11/250
# 9 = CAN 29/250
DEFAULT_PROTO = int(os.getenv("OBD_PROTO", "6"))

app = Flask(__name__)

# =========================
# Utilidades CAN/Serial
# =========================
serial_lock = threading.Lock()
ser: Optional[serial.Serial] = None

def serial_open() -> serial.Serial:
    return serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.2)

def serial_send_line(line: str) -> None:
    global ser
    if ser is None:
        raise RuntimeError("Serial não conectada")
    payload = (line.strip() + "\n").encode("utf-8")
    with serial_lock:
        ser.write(payload)
        ser.flush()

def parse_rx_tx_line(line: str) -> Optional[Dict[str, Any]]:
    # RX,<ID_HEX>,<EXT>,<DLC>,<DATAHEX>,<MS>
    # TX,<ID_HEX>,<EXT>,<DLC>,<DATAHEX>,<MS>
    parts = line.strip().split(",")
    if len(parts) < 1:
        return None
    tag = parts[0].strip().upper()

    if tag in ("RX", "TX") and len(parts) >= 6:
        try:
            return {
                "type": tag,
                "id_hex": parts[1].strip().upper(),
                "id": int(parts[1].strip(), 16),
                "ext": int(parts[2].strip()),
                "dlc": int(parts[3].strip()),
                "data_hex": parts[4].strip().upper(),
                "data": bytes.fromhex(parts[4].strip()),
                "ts": int(parts[5].strip()),
            }
        except Exception:
            return None

    if tag in ("INFO", "ERR"):
        return {"type": tag, "msg": ",".join(parts[1:]).strip()}

    return None

# =========================
# SSE broadcasting
# =========================
subs_lock = threading.Lock()
subscribers: set[queue.Queue[str]] = set()

def sse_broadcast(obj: Dict[str, Any]) -> None:
    payload = json.dumps(obj, ensure_ascii=False)
    with subs_lock:
        dead = []
        for q in list(subscribers):
            try:
                q.put_nowait(payload)
            except queue.Full:
                # cliente lento: descartamos
                pass
            except Exception:
                dead.append(q)
        for q in dead:
            subscribers.discard(q)

# =========================
# Modelo de sensores adjacentes
# =========================
@dataclass
class Sensor:
    key: str
    name: str
    unit: str

    # dinâmica
    base: float
    value: float
    vmin: float
    vmax: float
    noise_std: float      # ruído pequeno
    pull: float           # "puxa" para a base (0..1)
    period_ms: int
    mode: str             # normal | freeze | silence | spike
    source: str           # sim | can

    # CAN (broadcast)
    can_id: int
    can_ext: int

    # encoder/decoder
    encode: Callable[[float], bytes]
    decode: Callable[[bytes], Optional[float]]

    # runtime
    next_due: float = 0.0
    last_update_ts: float = 0.0

    def clamp(self, x: float) -> float:
        return max(self.vmin, min(self.vmax, x))

    def step(self, now: float) -> None:
        """Atualiza value (se sim) com pequenas variações."""
        if self.source != "sim":
            return

        if self.mode == "freeze":
            # não muda o valor
            return

        # Processo tipo OU simplificado: aproxima da base + ruído
        # variações pequenas e estáveis
        dv = (self.base - self.value) * self.pull + random.gauss(0.0, self.noise_std)

        # modo spike: raramente dá um salto (simula anomalia)
        if self.mode == "spike":
            if random.random() < 0.01:
                dv += (self.vmax - self.vmin) * 0.15 * random.choice([-1, 1])

        self.value = self.clamp(self.value + dv)
        self.last_update_ts = now

# =========================
# Encoders/decoders para o protocolo de bancada
# =========================
def u16_be(n: int) -> bytes:
    n = max(0, min(65535, int(n)))
    return bytes([(n >> 8) & 0xFF, n & 0xFF])

def i16_be(n: int) -> bytes:
    n = int(n)
    if n < -32768: n = -32768
    if n >  32767: n =  32767
    return int(n).to_bytes(2, "big", signed=True)

def decode_u16_be(b: bytes) -> Optional[int]:
    if len(b) < 2:
        return None
    return (b[0] << 8) | b[1]

def decode_i16_be(b: bytes) -> Optional[int]:
    if len(b) < 2:
        return None
    return int.from_bytes(b[:2], "big", signed=True)

# SPEED: uint16 = kmh*100
def enc_speed(v: float) -> bytes:
    return u16_be(int(round(v * 100.0)))

def dec_speed(b: bytes) -> Optional[float]:
    n = decode_u16_be(b)
    if n is None: return None
    return n / 100.0

# RPM: uint16 = rpm*4
def enc_rpm(v: float) -> bytes:
    return u16_be(int(round(v * 4.0)))

def dec_rpm(b: bytes) -> Optional[float]:
    n = decode_u16_be(b)
    if n is None: return None
    return n / 4.0

# VOLT: uint16 = V*1000
def enc_volt(v: float) -> bytes:
    return u16_be(int(round(v * 1000.0)))

def dec_volt(b: bytes) -> Optional[float]:
    n = decode_u16_be(b)
    if n is None: return None
    return n / 1000.0

# TEMP: int16 = C*10
def enc_temp(v: float) -> bytes:
    return i16_be(int(round(v * 10.0)))

def dec_temp(b: bytes) -> Optional[float]:
    n = decode_i16_be(b)
    if n is None: return None
    return n / 10.0

# MAF: uint16 = g/s*100
def enc_maf(v: float) -> bytes:
    return u16_be(int(round(v * 100.0)))

def dec_maf(b: bytes) -> Optional[float]:
    n = decode_u16_be(b)
    if n is None: return None
    return n / 100.0

# PERCENT: uint8 0..100
def enc_pct(v: float) -> bytes:
    n = int(round(v))
    n = max(0, min(100, n))
    return bytes([n])

def dec_pct(b: bytes) -> Optional[float]:
    if len(b) < 1: return None
    return float(b[0])

# =========================
# Estado global (sensores + ECU)
# =========================
state_lock = threading.Lock()

def make_default_sensors() -> Dict[str, Sensor]:
    now = time.time()
    sensors = {
        "speed": Sensor("speed", "Velocidade", "km/h",
                        base=60.0, value=60.0, vmin=0.0, vmax=240.0,
                        noise_std=0.25, pull=0.05, period_ms=100, mode="normal", source="sim",
                        can_id=0x300, can_ext=0, encode=enc_speed, decode=dec_speed, next_due=now),

        "rpm": Sensor("rpm", "RPM", "rpm",
                      base=1800.0, value=1800.0, vmin=600.0, vmax=6500.0,
                      noise_std=8.0, pull=0.06, period_ms=50, mode="normal", source="sim",
                      can_id=0x301, can_ext=0, encode=enc_rpm, decode=dec_rpm, next_due=now),

        "voltage": Sensor("voltage", "Tensão do módulo", "V",
                          base=13.6, value=13.6, vmin=11.5, vmax=14.8,
                          noise_std=0.01, pull=0.10, period_ms=500, mode="normal", source="sim",
                          can_id=0x302, can_ext=0, encode=enc_volt, decode=dec_volt, next_due=now),

        "intake": Sensor("intake", "Temp. ar admissão", "°C",
                         base=32.0, value=32.0, vmin=-10.0, vmax=90.0,
                         noise_std=0.08, pull=0.03, period_ms=300, mode="normal", source="sim",
                         can_id=0x303, can_ext=0, encode=enc_temp, decode=dec_temp, next_due=now),

        "coolant": Sensor("coolant", "Temp. coolant", "°C",
                          base=88.0, value=88.0, vmin=20.0, vmax=115.0,
                          noise_std=0.05, pull=0.02, period_ms=500, mode="normal", source="sim",
                          can_id=0x304, can_ext=0, encode=enc_temp, decode=dec_temp, next_due=now),

        "maf": Sensor("maf", "Vazão de ar (MAF)", "g/s",
                      base=9.5, value=9.5, vmin=1.0, vmax=90.0,
                      noise_std=0.08, pull=0.06, period_ms=100, mode="normal", source="sim",
                      can_id=0x305, can_ext=0, encode=enc_maf, decode=dec_maf, next_due=now),

        "fuel": Sensor("fuel", "Nível de combustível", "%",
                       base=63.0, value=63.0, vmin=0.0, vmax=100.0,
                       noise_std=0.02, pull=0.001, period_ms=1000, mode="normal", source="sim",
                       can_id=0x306, can_ext=0, encode=enc_pct, decode=dec_pct, next_due=now),

        "throttle": Sensor("throttle", "Abertura borboleta", "%",
                           base=18.0, value=18.0, vmin=0.0, vmax=100.0,
                           noise_std=0.6, pull=0.08, period_ms=100, mode="normal", source="sim",
                           can_id=0x307, can_ext=0, encode=enc_pct, decode=dec_pct, next_due=now),
    }
    return sensors

sensors: Dict[str, Sensor] = make_default_sensors()

ecu_cfg = {
    "proto": DEFAULT_PROTO,      # 6/7/8/9
    "obd_enabled": True,
}

def obd_ids_for_proto(proto: int) -> Tuple[List[int], int, int]:
    """
    Retorna (req_ids, resp_id, resp_ext)
    """
    if proto in (6, 8):  # 11-bit
        return [0x7DF, 0x7E0], 0x7E8, 0
    if proto in (7, 9):  # 29-bit
        return [0x18DB33F1, 0x18DA10F1], 0x18DAF110, 1
    # fallback
    return [0x7DF, 0x7E0], 0x7E8, 0

SUPPORTED_PIDS = {
    0x04,  # engine load (derivado)
    0x05,  # coolant
    0x0C,  # rpm
    0x0D,  # speed
    0x0F,  # intake temp
    0x10,  # MAF
    0x11,  # throttle
    0x2F,  # fuel level
    0x42,  # module voltage
}

def pid_mask_for_range(start_pid: int, supported: set[int], also_support_next: bool) -> int:
    """
    start_pid: 0x01, 0x21, 0x41
    retorno: 32-bit mask
    """
    mask = 0
    for pid in supported:
        if start_pid <= pid <= start_pid + 0x1F:
            bit = (start_pid + 0x1F) - pid
            mask |= (1 << bit)
    # sinaliza "há próxima faixa" setando o último PID da faixa (0x20/0x40/0x60)
    if also_support_next:
        last_pid = start_pid + 0x1F
        bit = 0  # PID last => bit 0
        mask |= (1 << bit)
    return mask

def build_obd01_response(pid: int) -> Optional[bytes]:
    """
    Resposta single-frame (8 bytes) para serviço 01.
    """
    with state_lock:
        spd = sensors["speed"].value
        rpm = sensors["rpm"].value
        coolant = sensors["coolant"].value
        intake = sensors["intake"].value
        maf = sensors["maf"].value
        fuel = sensors["fuel"].value
        volt = sensors["voltage"].value
        thr = sensors["throttle"].value

    # Derivado simples (não é “sensor adjacente”, mas é comum em apps)
    # load% ~ função de throttle e rpm (apenas heurística interna)
    load = max(0.0, min(100.0, (thr * 0.7) + (rpm / 6500.0) * 30.0))

    out = bytearray([0x00] * 8)

    # Range queries
    if pid == 0x00:
        mask = pid_mask_for_range(0x01, SUPPORTED_PIDS, also_support_next=True)
        out[0] = 0x06
        out[1] = 0x41
        out[2] = 0x00
        out[3] = (mask >> 24) & 0xFF
        out[4] = (mask >> 16) & 0xFF
        out[5] = (mask >> 8) & 0xFF
        out[6] = mask & 0xFF
        return bytes(out)

    if pid == 0x20:
        # inclui 0x2F, e sinaliza próxima faixa (0x40) porque suportamos 0x42
        mask = pid_mask_for_range(0x21, SUPPORTED_PIDS, also_support_next=True)
        out[0] = 0x06
        out[1] = 0x41
        out[2] = 0x20
        out[3] = (mask >> 24) & 0xFF
        out[4] = (mask >> 16) & 0xFF
        out[5] = (mask >> 8) & 0xFF
        out[6] = mask & 0xFF
        return bytes(out)

    if pid == 0x40:
        mask = pid_mask_for_range(0x41, SUPPORTED_PIDS, also_support_next=False)
        out[0] = 0x06
        out[1] = 0x41
        out[2] = 0x40
        out[3] = (mask >> 24) & 0xFF
        out[4] = (mask >> 16) & 0xFF
        out[5] = (mask >> 8) & 0xFF
        out[6] = mask & 0xFF
        return bytes(out)

    # PIDs
    if pid == 0x04:  # Engine load
        A = int(round(load * 255.0 / 100.0))
        A = max(0, min(255, A))
        out[0] = 0x03; out[1] = 0x41; out[2] = 0x04; out[3] = A
        return bytes(out)

    if pid == 0x05:  # Coolant: A-40
        A = int(round(coolant + 40.0))
        A = max(0, min(255, A))
        out[0] = 0x03; out[1] = 0x41; out[2] = 0x05; out[3] = A
        return bytes(out)

    if pid == 0x0C:  # RPM: ((A*256)+B)/4
        raw = int(round(rpm * 4.0))
        raw = max(0, min(65535, raw))
        A = (raw >> 8) & 0xFF
        B = raw & 0xFF
        out[0] = 0x04; out[1] = 0x41; out[2] = 0x0C; out[3] = A; out[4] = B
        return bytes(out)

    if pid == 0x0D:  # Speed: A
        A = int(round(spd))
        A = max(0, min(255, A))
        out[0] = 0x03; out[1] = 0x41; out[2] = 0x0D; out[3] = A
        return bytes(out)

    if pid == 0x0F:  # Intake: A-40
        A = int(round(intake + 40.0))
        A = max(0, min(255, A))
        out[0] = 0x03; out[1] = 0x41; out[2] = 0x0F; out[3] = A
        return bytes(out)

    if pid == 0x10:  # MAF: (A*256+B)/100
        raw = int(round(maf * 100.0))
        raw = max(0, min(65535, raw))
        A = (raw >> 8) & 0xFF
        B = raw & 0xFF
        out[0] = 0x04; out[1] = 0x41; out[2] = 0x10; out[3] = A; out[4] = B
        return bytes(out)

    if pid == 0x11:  # Throttle: 100/255 * A
        A = int(round(thr * 255.0 / 100.0))
        A = max(0, min(255, A))
        out[0] = 0x03; out[1] = 0x41; out[2] = 0x11; out[3] = A
        return bytes(out)

    if pid == 0x2F:  # Fuel Level: 100/255 * A
        A = int(round(fuel * 255.0 / 100.0))
        A = max(0, min(255, A))
        out[0] = 0x03; out[1] = 0x41; out[2] = 0x2F; out[3] = A
        return bytes(out)

    if pid == 0x42:  # Control module voltage: (A*256+B)/1000
        raw = int(round(volt * 1000.0))
        raw = max(0, min(65535, raw))
        A = (raw >> 8) & 0xFF
        B = raw & 0xFF
        out[0] = 0x04; out[1] = 0x41; out[2] = 0x42; out[3] = A; out[4] = B
        return bytes(out)

    return None

# =========================
# Processamento de frames CAN
# =========================
def send_can_frame(can_id: int, ext: int, data: bytes) -> None:
    # Arduino espera TX,<IDHEX>,<EXT>,<DATAHEX>
    serial_send_line(f"TX,{can_id:X},{ext},{data.hex().upper()}")
    sse_broadcast({
        "type": "HOST_TX",
        "id": f"{can_id:X}",
        "ext": int(ext),
        "dlc": len(data),
        "data": data.hex().upper(),
        "ts": int(time.time() * 1000),
    })

def handle_can_frame(frm: Dict[str, Any]) -> None:
    """
    frm: dict parseado de RX/TX do Arduino (onde RX é frame visto no barramento).
    """
    # 1) Atualiza sensores cujo source="can" (ou que você queira aceitar do barramento)
    with state_lock:
        for s in sensors.values():
            if s.source == "can":
                if frm["id"] == s.can_id and frm["ext"] == s.can_ext:
                    val = s.decode(frm["data"])
                    if val is not None:
                        s.value = s.clamp(val)
                        s.last_update_ts = time.time()
                        sse_broadcast({"type": "SENSOR", "key": s.key, "value": s.value, "ts": int(time.time()*1000)})

    # 2) ECU responde OBD (se habilitado)
    with state_lock:
        proto = ecu_cfg["proto"]
        obd_enabled = ecu_cfg["obd_enabled"]

    if not obd_enabled:
        return

    req_ids, resp_id, resp_ext = obd_ids_for_proto(proto)

    if frm["id"] in req_ids:
        # Esperamos single-frame request: [0x02, 0x01, PID, ...]
        data = frm["data"]
        if len(data) < 3:
            return
        pci = data[0]
        service = data[1]
        pid = data[2]

        # Single Frame: nibble alto 0x0
        if (pci & 0xF0) != 0x00:
            return

        if service == 0x01:
            resp = build_obd01_response(pid)
            if resp is None:
                return
            send_can_frame(resp_id, resp_ext, resp)

# =========================
# Threads: Serial reader + simulador de sensores
# =========================
def serial_thread() -> None:
    global ser
    while True:
        try:
            if ser is None:
                ser = serial_open()
                sse_broadcast({"type": "INFO", "msg": f"Serial conectada em {SERIAL_PORT}@{SERIAL_BAUD}"})
                # aplica proto inicial no Arduino
                with state_lock:
                    p = ecu_cfg["proto"]
                try:
                    serial_send_line(f"PROTO,{p}")
                except Exception:
                    pass

            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if not text:
                continue

            obj = parse_rx_tx_line(text)
            if not obj:
                continue

            if obj["type"] in ("RX", "TX"):
                # loga
                sse_broadcast({
                    "type": obj["type"],
                    "id": obj["id_hex"],
                    "ext": obj["ext"],
                    "dlc": obj["dlc"],
                    "data": obj["data_hex"],
                    "ts": obj["ts"],
                })
                # processa RX apenas (tráfego vindo do barramento)
                if obj["type"] == "RX":
                    handle_can_frame(obj)
            else:
                sse_broadcast(obj)

        except Exception as e:
            sse_broadcast({"type": "ERR", "msg": f"Serial erro: {repr(e)}"})
            try:
                if ser is not None:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(1.0)

def sensor_sim_thread() -> None:
    """
    Simula sensores adjacentes independentes e publica frames no CAN.
    """
    while True:
        now = time.time()
        to_send: List[Tuple[int,int,bytes,str,float]] = []

        with state_lock:
            for s in sensors.values():
                if s.source != "sim":
                    continue
                if now >= s.next_due:
                    # atualiza valor (pode congelar)
                    s.step(now)

                    # modo silence: não transmite
                    if s.mode != "silence":
                        payload = s.encode(s.value)
                        to_send.append((s.can_id, s.can_ext, payload, s.key, s.value))

                    # agenda próximo envio
                    s.next_due = now + (s.period_ms / 1000.0)

        # envia fora do lock
        for can_id, ext, payload, key, val in to_send:
            try:
                send_can_frame(can_id, ext, payload)
                sse_broadcast({"type": "SENSOR", "key": key, "value": val, "ts": int(time.time()*1000)})
            except Exception as e:
                sse_broadcast({"type": "ERR", "msg": f"Falha ao TX sensor {key}: {repr(e)}"})

        time.sleep(0.01)

# =========================
# Web: UI + APIs
# =========================
@app.route("/")
def index():
    return render_template("index.html")

@app.route("/stream")
def stream():
    q: queue.Queue[str] = queue.Queue(maxsize=500)
    with subs_lock:
        subscribers.add(q)

    def gen():
        try:
            while True:
                msg = q.get()
                yield f"data: {msg}\n\n"
        except GeneratorExit:
            pass
        finally:
            with subs_lock:
                subscribers.discard(q)

    return Response(gen(), mimetype="text/event-stream")

@app.route("/api/ecu", methods=["GET"])
def api_ecu_get():
    with state_lock:
        return jsonify(ecu_cfg)

@app.route("/api/ecu/proto", methods=["POST"])
def api_ecu_proto():
    data = request.get_json(force=True)
    proto = int(data.get("proto"))
    if proto not in (6, 7, 8, 9):
        return jsonify({"ok": False, "error": "proto inválido (use 6,7,8,9)"}), 400

    with state_lock:
        ecu_cfg["proto"] = proto

    # aplica no Arduino (bitrate)
    serial_send_line(f"PROTO,{proto}")
    return jsonify({"ok": True})

@app.route("/api/ecu/obd", methods=["POST"])
def api_ecu_obd():
    data = request.get_json(force=True)
    enabled = bool(data.get("enabled", True))
    with state_lock:
        ecu_cfg["obd_enabled"] = enabled
    return jsonify({"ok": True})

@app.route("/api/sensors", methods=["GET"])
def api_sensors_list():
    with state_lock:
        out = []
        for s in sensors.values():
            d = asdict(s)
            # remove funções não serializáveis
            d.pop("encode", None)
            d.pop("decode", None)
            out.append(d)
        return jsonify(out)

@app.route("/api/sensors/<key>", methods=["POST"])
def api_sensor_update(key: str):
    data = request.get_json(force=True)
    with state_lock:
        if key not in sensors:
            return jsonify({"ok": False, "error": "sensor não existe"}), 404
        s = sensors[key]

        # atualizações permitidas
        for field in ("base", "noise_std", "pull", "period_ms", "mode", "source", "can_id", "can_ext", "vmin", "vmax"):
            if field in data:
                val = data[field]
                if field in ("mode", "source"):
                    setattr(s, field, str(val))
                elif field in ("period_ms", "can_id", "can_ext"):
                    setattr(s, field, int(val))
                else:
                    setattr(s, field, float(val))

        # clamp e re-agenda
        s.value = s.clamp(s.value)
        s.next_due = time.time() + (s.period_ms / 1000.0)

        d = asdict(s)
        d.pop("encode", None)
        d.pop("decode", None)
        return jsonify({"ok": True, "sensor": d})

@app.route("/api/can/tx", methods=["POST"])
def api_can_tx():
    """
    Transmissão manual (útil para bancada).
    Observação: isso transmite no barramento real conectado ao Arduino.
    """
    data = request.get_json(force=True)
    can_id = int(str(data["id"]), 16)
    ext = int(data.get("ext", 0))
    payload = bytes.fromhex(str(data["data"]).replace(" ", ""))
    if len(payload) > 8:
        return jsonify({"ok": False, "error": "payload > 8 bytes"}), 400
    send_can_frame(can_id, ext, payload)
    return jsonify({"ok": True})

def main():
    # inicia threads
    threading.Thread(target=serial_thread, daemon=True).start()
    threading.Thread(target=sensor_sim_thread, daemon=True).start()

    app.run(host="0.0.0.0", port=5000, debug=False)

if __name__ == "__main__":
    main()
