import paho.mqtt.client as mqtt
import json
import sys

# --- KONFIGURACJA ---
BROKER_HOST = "srv38.mikr.us"
BROKER_PORT = 40133
MQTT_USER = "guest"
MQTT_PASS = "guest"

# Słuchamy wszystkiego co dotyczy projektu smartmirror
# Znak '#' oznacza: "dowolny użytkownik, dowolne urządzenie, dowolny sensor"
TOPIC_SUB = "smartmirror/#"


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"✅ Połączono z systemem SmartMirror!")
        print(f"📡 Nasłuchiwanie na: {TOPIC_SUB}")
        print("-" * 40)
        client.subscribe(TOPIC_SUB)
    else:
        print(f"❌ Błąd połączenia: {rc}")


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload_str = msg.payload.decode()

        # Oczekiwany temat: smartmirror / user / device_id / sensor / type
        parts = topic.split('/')

        # Zabezpieczenie: jeśli temat jest za krótki, ignorujemy go
        if len(parts) < 5:
            return

        device_id = parts[2]  # np. 010F34
        sensor_type = parts[-1]  # np. temp, humidity

        # Parsowanie JSON
        data = json.loads(payload_str)
        val = data.get('val')
        unit = data.get('unit', '')

        # Ikony dla lepszej czytelności
        icon = "❓"
        if sensor_type == "temp":
            icon = "🌡️"
        elif sensor_type == "humidity":
            icon = "💧"
        elif sensor_type == "light":
            icon = "💡"
        elif sensor_type == "motion":
            icon = "🏃"

        # Wyświetlanie sformatowanych danych
        print(f"{icon} [{device_id}] {sensor_type.upper()}: {val} {unit}")

    except Exception as e:
        # Jeśli przyjdzie coś dziwnego (nie JSON), po prostu to wypisz
        # print(f"RAW [{topic}]: {msg.payload}")
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
except Exception as e:
    print(f"Błąd: {e}")