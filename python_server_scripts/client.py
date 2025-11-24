import paho.mqtt.client as mqtt
import json
import datetime

# --- KONFIGURACJA ---
BROKER_HOST = "srv38.mikr.us"
BROKER_PORT = 40133
MQTT_USER = "guest"
MQTT_PASS = "guest"

# Nasłuchujemy wszystkiego
TOPIC_SUB = "smartmirror/#"


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"✅ Połączono z systemem SmartMirror!")
        print(f"📡 Nasłuchiwanie na: {TOPIC_SUB}")
        print("-" * 50)
        client.subscribe(TOPIC_SUB)
    else:
        print(f"❌ Błąd połączenia: {rc}")


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload_str = msg.payload.decode()
        data = json.loads(payload_str)  # Parsujemy JSON

        # Rozbijamy temat: smartmirror / user / device_id / KATEGORIA
        parts = topic.split('/')
        if len(parts) < 4: return

        device_id = parts[2]  # ID urządzenia
        category = parts[-1]  # telemetry, status, event, cmd

        timestamp = datetime.datetime.now().strftime("%H:%M:%S")

        # --- 1. OBSŁUGA TELEMETRII (temp, lux) ---
        if category == "telemetry":
            temp = data.get("temp", "?")
            lux = data.get("lux", "?")
            print(f"[{timestamp}] 📊 [{device_id}] TELEMETRIA")
            print(f"   🌡️ Temp:  {temp} °C")
            print(f"   💡 Światło: {lux} lx")
            print("-" * 20)

        # --- 2. OBSŁUGA STATUSU TECHNICZNEGO (ip, uptime) ---
        elif category == "status":
            uptime = data.get("uptime", 0)
            ip = data.get("ip", "?.?.?.?")
            print(f"[{timestamp}] 🛠️ [{device_id}] STATUS")
            print(f"   🌐 IP: {ip}")
            print(f"   ⏱️ Uptime: {uptime} s")
            print("-" * 20)

        # --- 3. OBSŁUGA ZDARZEŃ (Ruch) ---
        elif category == "event":
            evt_type = data.get("type", "")
            val = data.get("val", "")

            icon = "🔔"
            if evt_type == "motion": icon = "🏃"

            print(f"[{timestamp}] {icon} [{device_id}] ALARM / ZDARZENIE")
            print(f"   Typ: {evt_type} -> {val}")
            print("-" * 20)

        # --- 4. OBSŁUGA KOMEND (To co wysłałaś z admin_panel) ---
        elif category == "cmd":
            action = data.get("action", "")
            msg = data.get("msg", "")
            print(f"[{timestamp}] 📱 [{device_id}] KOMENDA")
            print(f"   Akcja: {action}")
            if msg: print(f"   Treść: {msg}")
            print("-" * 20)

    except Exception as e:
        # Opcjonalnie: wypisz błędy parsowania
        # print(f"Błąd: {e}, Raw: {msg.payload}")
        pass


# Setup klienta
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(MQTT_USER, MQTT_PASS)
client.on_connect = on_connect
client.on_message = on_message

print(f"⏳ Łączenie z chmurą IoT ({BROKER_HOST})...")

try:
    client.connect(BROKER_HOST, BROKER_PORT, 60)
    client.loop_forever()
except KeyboardInterrupt:
    print("\nZakończono.")