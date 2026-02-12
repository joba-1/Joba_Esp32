#!/usr/bin/env python3
"""
Subscribe to MQTT part topics and reassemble multipart messages published by the firmware.

Usage:
  python3 reassemble_mqtt_parts.py --broker <broker> --topic "<base>/modbus/resp/list_registers/#"

The script expects topics of the form:
  <base>/modbus/resp/list_registers/part/<id>/<index>/<parts>

When all parts for an id are received the reconstructed payload is printed to stdout.
"""
import argparse
import sys
import time
from collections import defaultdict
import paho.mqtt.client as mqtt


def parse_part_topic(topic):
    # split and expect last segments: part, id, index, parts
    parts = topic.split('/')
    if len(parts) < 4:
        return None
    # find 'part' segment
    try:
        pidx = parts.index('part')
    except ValueError:
        return None
    # ensure we have id/index/total after 'part'
    if len(parts) <= pidx + 3:
        return None
    pid = parts[pidx + 1]
    try:
        idx = int(parts[pidx + 2])
        total = int(parts[pidx + 3])
    except ValueError:
        return None
    return pid, idx, total


class Reassembler:
    def __init__(self):
        # id -> { total: int, parts: dict(index->str), last_seen: ts }
        self.store = {}

    def add(self, pid, idx, total, payload):
        e = self.store.get(pid)
        if not e:
            e = {'total': total, 'parts': {}, 'last_seen': time.time()}
            self.store[pid] = e
        e['parts'][idx] = payload
        e['last_seen'] = time.time()
        if len(e['parts']) == e['total']:
            # assemble
            out = ''.join(e['parts'][i] for i in range(1, e['total'] + 1))
            del self.store[pid]
            return out
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--broker', required=True)
    parser.add_argument('--port', type=int, default=1883)
    parser.add_argument('--topic', required=True)
    parser.add_argument('--out', help='File to write complete messages (appends).')
    args = parser.parse_args()

    r = Reassembler()

    def on_connect(client, userdata, flags, rc):
        print('Connected to broker, rc=', rc)
        client.subscribe(args.topic)

    def on_message(client, userdata, msg):
        meta = parse_part_topic(msg.topic)
        payload = msg.payload.decode('utf-8', errors='replace')
        if meta:
            pid, idx, total = meta
            res = r.add(pid, idx, total, payload)
            if res is not None:
                header = f"--- Reassembled id={pid} parts={total} ---"
                print(header)
                print(res)
                print('-' * len(header))
                if args.out:
                    with open(args.out, 'a') as f:
                        f.write(res)
                        f.write('\n')
        else:
            # Not a part topic: just print raw
            print(f"{msg.topic} {payload}")

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    try:
        client.connect(args.broker, args.port, 60)
    except Exception as e:
        print('Failed to connect to broker:', e)
        sys.exit(1)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print('Exiting')


if __name__ == '__main__':
    main()
