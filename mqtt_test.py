"""
mqtt_test.py — publish relay3 ON/OFF to pump/03/cmd for LoRa test
Usage:
  python mqtt_test.py on
  python mqtt_test.py off
"""
import sys
import ssl
import paho.mqtt.client as mqtt

BROKER   = 'broker.emqx.io'
PORT     = 8883
TOPIC    = 'pump/03/cmd'

if len(sys.argv) < 2 or sys.argv[1].lower() not in ('on', 'off'):
    print('Usage: python mqtt_test.py on|off')
    sys.exit(1)

val     = 1 if sys.argv[1].lower() == 'on' else 0
payload = f'{{"relay3":{val}}}'

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f'[MQTT] Connected — publishing {TOPIC}: {payload}')
        client.publish(TOPIC, payload, qos=1)
    else:
        print(f'[MQTT] Connect failed rc={rc}')

def on_publish(client, userdata, mid):
    print('[MQTT] Published OK')
    client.disconnect()

client = mqtt.Client(client_id='test_relay3_' + str(val))
client.on_connect = on_connect
client.on_publish = on_publish
client.tls_set(cert_reqs=ssl.CERT_NONE)
client.tls_insecure_set(True)
client.connect(BROKER, PORT, keepalive=10)
client.loop_forever()
