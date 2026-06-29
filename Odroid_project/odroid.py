#!/usr/bin/env python3
# Odroid bridge: Arduino <-> (Blynk + MQTT) and NodeMCU display control

import requests, serial, json, time, ssl, socket, threading, re, glob, os
import paho.mqtt.client as mqtt
from serial.serialutil import SerialException

# ======== CONFIG ========
BLYNK_TOKEN = "ใส่ token จริง"
API = "https://blynk.cloud/external/api"
PIN_MAP = {"item1": 0, "item2": 1, "item3": 2}

# Serial ports
SER_ARDUINO = "/dev/ttyACM0"    
SER_NODEMCU = "/dev/ttyUSB0"    
BAUD_ARD = 9600
BAUD_NMC = 115200

# MQTT
HOST = "..."
PORT = 8883
USERNAME = "..." # ใส่ broker mqtt ของตัวเอง
PASSWORD = "..."
SUB_TOPICS = [("/data", 1), ("sys/health", 0)]

sess = requests.Session()
serA = serial.Serial(SER_ARDUINO, BAUD_ARD, timeout=0.2)

# ---- NodeMCU port (lazy open + auto-reopen) ----
serN = None
_last_nodemcu_show_amt = None
_last_nodemcu_show_ts = 0.0

def _open_nodemcu(port_hint=SER_NODEMCU):
    """Open NodeMCU serial safely"""
    ports = [port_hint] if os.path.exists(port_hint) else sorted(glob.glob("/dev/ttyUSB*"))
    for p in ports:
        try:
            s = serial.Serial(p, BAUD_NMC, timeout=0.2)
            s.setDTR(False)
            s.setRTS(False)
            # เคลียร์ buffer ที่ค้าง
            s.reset_input_buffer()
            s.reset_output_buffer()
            print(f"[NODEMCU] opened {p}")
            return s
        except Exception as e:
            print(f"[NODEMCU] open fail {p}: {e}")
    return None

def ensure_serN():
    global serN
    if serN is None or not serN.is_open:
        serN = _open_nodemcu()
    return serN

def node_send(raw_bytes, retries=2):
    """Write to NodeMCU with retries + auto reopen. Return True if ok."""
    global serN
    for attempt in range(retries + 1):
        s = ensure_serN()
        if s is None:
            time.sleep(min(1.0 * (attempt + 1), 3.0))
            continue
        try:
            s.write(raw_bytes)
            s.flush()
            return True
        except (OSError, SerialException) as e:
            print(f"[NODEMCU] write error: {e} (attempt {attempt+1}/{retries})")
            try:
                s.close()
            except Exception:
                pass
            serN = None
            time.sleep(min(1.0 * (attempt + 1), 3.0))
    return False

def node_show(amount):
    """SHOW <amount> with small guard-cooldown tracking."""
    global _last_nodemcu_show_amt, _last_nodemcu_show_ts
    if amount not in (10, 15, 20):
        print("[NODEMCU] invalid amount", amount); return
    msg = f"SHOW {amount}\n".encode()
    if node_send(msg):
        print("[NODEMCU] ->", msg.decode().strip())
        _last_nodemcu_show_amt = amount
        _last_nodemcu_show_ts = time.time()

def node_hide(force=False):
    """HIDE with cooldown: if SHOW happened <0.5s ago and not forced, delay a bit."""
    now = time.time()
    if not force and (now - _last_nodemcu_show_ts) < 0.5:
        # ป้องกัน HIDE ทับทันทีหลัง SHOW โดยไม่ตั้งใจ
        time.sleep(0.5 - (now - _last_nodemcu_show_ts))
    if node_send(b"HIDE\n"):
        print("[NODEMCU] -> HIDE")

# shared state
paid_queue = []        # list of amounts that just paid

# ======== Blynk helpers ========
def clamp(n, lo=0, hi=9999):
    n = int(n)
    return max(lo, min(hi, n))

def push_one(item, value):
    vpin = PIN_MAP[item]
    params = {"token": BLYNK_TOKEN, f"V{vpin}": clamp(value)}
    r = sess.get(f"{API}/update", params=params, timeout=8)
    print(f"[BLYNK] push V{vpin}={value} status={r.status_code}")
    return r.status_code == 200

def pull_one(item):
    vpin = PIN_MAP[item]
    url = f"{API}/get?token={BLYNK_TOKEN}&V{vpin}"
    r = sess.get(url, timeout=8)
    if r.status_code != 200:
        print(f"[BLYNK] pull V{vpin} failed {r.status_code}")
        return None
    txt = r.text.strip().strip('[]"')
    try:
        val = int(txt)
        print(f"[BLYNK] pull V{vpin} -> {val}")
        return val
    except Exception:
        print(f"[BLYNK] pull parse error raw={r.text}")
        return None

# ======== MQTT ========
def build_mqtt():
    client = mqtt.Client(client_id="odroid-bridge", protocol=mqtt.MQTTv311, clean_session=True)
    client.username_pw_set(USERNAME, PASSWORD)
    client.will_set("sys/odroid/status", payload='{"status":"offline"}', qos=1, retain=True)
    ctx = ssl.create_default_context()
    client.tls_set_context(ctx)
    client.tls_insecure_set(False)

    def on_connect(c, u, f, rc, p=None):
        print("MQTT connected rc=", rc)
        if rc == 0:
            c.subscribe(SUB_TOPICS)
            print("MQTT subscribed:", SUB_TOPICS)

    def on_message(c, u, msg):
        # non-JSON payload เช่น b'15'
        try:
            payload_txt = msg.payload.decode().strip()
        except Exception:
            print(f"[MQTT:{msg.topic}] decode error: {msg.payload!r}")
            return
        print(f"[MQTT:{msg.topic}] '{payload_txt}'")
        if payload_txt.isdigit():
            amt = int(payload_txt)
            if amt in (10, 15, 20):
                paid_queue.append(amt)
                print(f"[MQTT] payment OK amount={amt}")
            else:
                print(f"[MQTT] ignore amount={amt}")
        else:
            print("[MQTT] non-numeric payload ignored")

    client.on_connect = on_connect
    client.on_message = on_message
    return client

def mqtt_thread():
    client = build_mqtt()
    backoff = 1
    while True:
        try:
            client.connect(HOST, PORT, keepalive=60)
            client.loop_forever(retry_first_connection=True)
        except (socket.error, ssl.SSLError) as e:
            print("MQTT conn error:", e, "retry", backoff, "s")
            time.sleep(backoff); backoff = min(60, backoff*2)

# ======== Robust parser helpers (Arduino serial) ========
def _maybe_amount_in_line(s):
    m = re.search(r'(\d+)', s)
    if not m: return None
    try:
        return int(m.group(1))
    except Exception:
        return None

def handle_arduino_line(line):
    raw = line.strip()
    if not raw:
        return
    up = raw.upper()

    # -------- PULL เผื่อกรณี arduino ส่งข้อมูลขาด --------
    if up.startswith("PULL") or up.startswith("ULL"):
        parts = raw.split()
        if len(parts) >= 2:
            item = parts[1].strip().lower()
            if item in PIN_MAP:
                val = pull_one(item)
                if val is not None:
                    serA.write(f"{item}:{val}\n".encode()); serA.flush()
                    print("[SER->ARD]", f"{item}:{val}")
                return
            elif item == "all":
                data = {}
                for k in PIN_MAP:
                    v = pull_one(k)
                    if v is not None: data[k] = v
                if data:
                    serA.write((json.dumps(data)+"\n").encode()); serA.flush()
                    print("[SER->ARD]", data)
                return
        # salvage pattern ITEMx
        m = re.search(r'(ITEM[123])', up)
        if m:
            item = m.group(1).lower()
            val = pull_one(item)
            if val is not None:
                serA.write(f"{item}:{val}\n".encode()); serA.flush()
                print("[SER->ARD]", f"{item}:{val}")
            return

    # -------- REQ_QR  เผื่อกรณี arduino ส่งข้อมูลขาด --------
    if up.startswith("REQ_QR") or up.startswith("EQ_QR") or ("REQ_QR" in up) or ("EQ_QR" in up):
        amt = _maybe_amount_in_line(up)
        if amt is not None:
            node_show(amt); return
        else:
            print("[ARD] REQ_QR without amount:", raw); return

    # -------- CANCEL  เผื่อกรณี arduino ส่งข้อมูลขาด --------
    if up == "CANCEL" or up == "CANCELLED" or up.endswith("ANCEL") or up.endswith("ANCELLED"):
        node_hide(force=True)
        serA.write(b"CANCELLED\n"); serA.flush()
        return

    # -------- QR_DONE  เผื่อกรณี arduino ส่งข้อมูลขาด --------
    if up == "QR_DONE" or up.endswith("R_DONE"):
        node_hide()
        return

    # -------- JSON stock update --------
    if raw.startswith("{") or raw.startswith('"item'):
        try:
            if raw.startswith('"item'): raw = "{" + raw
            data = json.loads(raw)
            for k,v in data.items():
                if k in PIN_MAP and isinstance(v, (int,float)):
                    push_one(k, int(v))
            serA.write(b"ACK\n"); serA.flush()
            print("[SER] ACK for", data)
        except Exception as e:
            print("[SER] JSON parse error:", e, "raw:", raw)
        return

    print("[SER-ARD] unknown:", line.strip())

def main():
    print("Odroid bridge started.")
    # เปิด NodeMCU ครั้งแรก (ถ้าไม่สำเร็จ จะเปิดเองเมื่อส่งคำสั่งครั้งแรก)
    ensure_serN()

    # start MQTT thread
    th = threading.Thread(target=mqtt_thread, daemon=True)
    th.start()

    while True:
        # 1) forward paid events from MQTT to Arduino (and hide QR)
        if paid_queue:
            _ = paid_queue.pop(0)
            serA.write(b"PAID OK\n"); serA.flush()
            print("[SER->ARD] PAID OK")
            node_hide()

        # 2) read Arduino serial
        try:
            line = serA.readline().decode(errors="ignore")
        except Exception:
            line = ""
        if line:
            handle_arduino_line(line)

        time.sleep(0.05)

if __name__ == "__main__":
    main()