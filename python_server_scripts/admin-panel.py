# import paho.mqtt.client as mqtt
# import json
# import sys
#
# # --- KONFIGURACJA ---
# BROKER = "srv38.mikr.us"
# PORT = 40133
# USER = "guest"
# PASS = "guest"
#
# # Tutaj wpisz ID swojego urządzenia z logów (np. 010F34)
# DEVICE_ID = "010F34"
# TOPIC_ROOT = f"smartmirror/user_01/{DEVICE_ID}"
#
# client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
# client.username_pw_set(USER, PASS)
#
# print(f"Łączenie z brokerem dla urządzenia: {DEVICE_ID}...")
# try:
#     client.connect(BROKER, PORT, 60)
#     client.loop_start()  # Uruchamia wątek w tle
# except Exception as e:
#     print(f"Błąd połączenia: {e}")
#     sys.exit(1)
#
#
# def send_text():
#     msg = input("Podaj tekst motywacyjny: ")
#     # Budujemy JSON
#     payload = json.dumps({"msg": msg, "font": "Arial", "size": 20})
#     topic = f"{TOPIC_ROOT}/display/text"
#
#     client.publish(topic, payload)
#     print(f"--> Wysłano do {topic}")
#
#
# def send_calendar():
#     time = input("Godzina (np. 14:00): ")
#     title = input("Wydarzenie: ")
#     payload = json.dumps({"time": time, "title": title})
#     topic = f"{TOPIC_ROOT}/display/calendar"
#
#     client.publish(topic, payload)
#     print(f"--> Wysłano do {topic}")
#
#
# def switch_screen():
#     cmd = input("Ekran ON czy OFF? ").upper()
#     if cmd in ["ON", "OFF"]:
#         topic = f"{TOPIC_ROOT}/cmd/screen"
#         client.publish(topic, cmd)
#         print(f"--> Wysłano komendę {cmd}")
#
#
# while True:
#     print("\n--- MENU ADMINISTRATORA SMART MIRROR ---")
#     print("1. Wyślij tekst motywacyjny")
#     print("2. Dodaj wpis do kalendarza")
#     print("3. Włącz/Wyłącz ekran")
#     print("4. Wyjście")
#
#     choice = input("Wybierz opcję: ")
#
#     if choice == '1':
#         send_text()
#     elif choice == '2':
#         send_calendar()
#     elif choice == '3':
#         switch_screen()
#     elif choice == '4':
#         break
#
# client.loop_stop()
# client.disconnect()


import paho.mqtt.client as mqtt
import json

# Ustawienia
BROKER = "srv38.mikr.us"
PORT = 40133
USER = "guest"
PASS = "guest"
DEVICE_MAC = "78EE4C010F34" # Wpisz swój MAC z logów (bez dwukropków!)

TOPIC_CMD = f"smartmirror/user_01/{DEVICE_MAC}/cmd"

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(USER, PASS)
client.connect(BROKER, PORT, 60)

def send_update_text():
    msg = input("Podaj nowy tekst na lustro: ")
    # Komenda wysokopoziomowa
    command = {
        "action": "update_text",
        "msg": msg,
        "priority": "high" # Przykładowe dodatkowe pole
    }
    client.publish(TOPIC_CMD, json.dumps(command))
    print("Wysłano!")

def send_screen_control():
    state = input("Ekran ON/OFF? ").upper()
    command = {
        "action": "set_screen",
        "state": state
    }
    client.publish(TOPIC_CMD, json.dumps(command))
    print("Wysłano!")

while True:
    print("1. Zmień tekst")
    print("2. Steruj ekranem")
    opt = input("> ")
    if opt == "1": send_update_text()
    if opt == "2": send_screen_control()