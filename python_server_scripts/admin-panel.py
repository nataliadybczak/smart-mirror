import paho.mqtt.client as mqtt
import json
import sys

# --- KONFIGURACJA ---
BROKER = "srv38.mikr.us"
PORT = 40133
USER = "guest"
PASS = "guest"

# Tutaj wpisz ID swojego urządzenia z logów (np. 010F34)
DEVICE_ID = "010F34"
TOPIC_ROOT = f"smartmirror/user_01/{DEVICE_ID}"

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(USER, PASS)

print(f"Łączenie z brokerem dla urządzenia: {DEVICE_ID}...")
try:
    client.connect(BROKER, PORT, 60)
    client.loop_start()  # Uruchamia wątek w tle
except Exception as e:
    print(f"Błąd połączenia: {e}")
    sys.exit(1)


def send_text():
    msg = input("Podaj tekst motywacyjny: ")
    # Budujemy JSON
    payload = json.dumps({"msg": msg, "font": "Arial", "size": 20})
    topic = f"{TOPIC_ROOT}/display/text"

    client.publish(topic, payload)
    print(f"--> Wysłano do {topic}")


def send_calendar():
    time = input("Godzina (np. 14:00): ")
    title = input("Wydarzenie: ")
    payload = json.dumps({"time": time, "title": title})
    topic = f"{TOPIC_ROOT}/display/calendar"

    client.publish(topic, payload)
    print(f"--> Wysłano do {topic}")


def switch_screen():
    cmd = input("Ekran ON czy OFF? ").upper()
    if cmd in ["ON", "OFF"]:
        topic = f"{TOPIC_ROOT}/cmd/screen"
        client.publish(topic, cmd)
        print(f"--> Wysłano komendę {cmd}")


while True:
    print("\n--- MENU ADMINISTRATORA SMART MIRROR ---")
    print("1. Wyślij tekst motywacyjny")
    print("2. Dodaj wpis do kalendarza")
    print("3. Włącz/Wyłącz ekran")
    print("4. Wyjście")

    choice = input("Wybierz opcję: ")

    if choice == '1':
        send_text()
    elif choice == '2':
        send_calendar()
    elif choice == '3':
        switch_screen()
    elif choice == '4':
        break

client.loop_stop()
client.disconnect()