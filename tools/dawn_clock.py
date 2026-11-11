#!/usr/bin/env python3
"""Dawn v2 provisioning and bench client. No cloud dependency. See README.md."""
import argparse
import asyncio
import getpass
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import struct
import time
import uuid
import zlib

SERVICE = '6c120001-3c16-4d96-9abc-7b0947bda001'
RX = '6c120002-3c16-4d96-9abc-7b0947bda001'
TX = '6c120003-3c16-4d96-9abc-7b0947bda001'
INFO = '6c120004-3c16-4d96-9abc-7b0947bda001'
TONES = ['daybreak', 'birdsong', 'softSignal', 'firstLight']
HEADER = struct.Struct('<BBIHHI')


def signature(key, text):
    return hmac.new(key, text.encode(), hashlib.sha256).hexdigest()


def fragments(text, message_id, att=20):
    payload = text.encode()
    if not 0 < len(payload) <= 2048 or not 15 <= att <= 244:
        raise ValueError('Request or ATT size out of bounds')
    checksum = zlib.crc32(payload)
    for offset in range(0, len(payload), att - HEADER.size):
        yield HEADER.pack(0xD2, 0, message_id, len(payload), offset, checksum) + payload[offset:offset + att - HEADER.size]


class Assembler:
    def __init__(self):
        self.reset()

    def reset(self):
        self.message_id = self.total = self.crc = 0
        self.data = bytearray()

    def accept(self, frame):
        if not 15 <= len(frame) <= 244:
            raise ValueError('Invalid response frame size')
        magic, flags, ident, total, offset, crc = HEADER.unpack_from(frame)
        if magic != 0xD2 or flags or not ident or not 0 < total <= 40960:
            raise ValueError('Invalid response header')
        if offset == 0:
            self.reset()
            self.message_id, self.total, self.crc = ident, total, crc
        if (ident, total, crc) != (self.message_id, self.total, self.crc) or offset != len(self.data):
            raise ValueError('Out-of-order response')
        self.data.extend(frame[HEADER.size:])
        if len(self.data) > total:
            raise ValueError('Response overflow')
        if len(self.data) == total:
            if zlib.crc32(self.data) != crc:
                raise ValueError('Response CRC mismatch')
            result = (ident, self.data.decode('utf-8'))
            self.reset()
            return result
        return None


def identifier(text, limit=128):
    return isinstance(text, str) and 0 < len(text) <= limit and re.fullmatch(r'[A-Za-z0-9_-]+', text) is not None


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'Expected integer in {low}..{high}')
    return value


def encode_records(document, revision):
    """Accepts Dawn's alarm fields, including dayTimes. Sunday = 0."""
    records, seen = [], set()
    if not isinstance(document, list) or len(document) > 64:
        raise ValueError('Expected an array of at most 64 alarms')
    for alarm in document:
        ident, owner = alarm['id'], alarm['ownerID']
        if not identifier(ident, 64) or not identifier(owner) or ident in seen:
            raise ValueError('Invalid/duplicate alarm or owner identity')
        seen.add(ident)
        days = alarm['weekdays']
        if not isinstance(days, list) or len(set(days)) != len(days):
            raise ValueError('weekdays must be a unique array')
        mask = sum(1 << integer(day, 0, 6) for day in days)
        enabled = alarm.get('enabled', True)
        if type(enabled) is not bool:
            raise ValueError('enabled must be Boolean')
        tone = TONES.index(alarm.get('tone', 'daybreak'))
        hour, minute = integer(alarm['hour'], 0, 23), integer(alarm['minute'], 0, 59)
        overrides = alarm.get('dayTimes') or {}
        if not isinstance(overrides, dict) or any(day not in [str(i) for i in range(7)] for day in overrides):
            raise ValueError('dayTimes keys must be 0..6')
        minutes = []
        for day in range(7):
            override = overrides.get(str(day), {'hour': hour, 'minute': minute})
            minutes.append(integer(override['hour'], 0, 23) * 60 + integer(override['minute'], 0, 59))
        fields = [ident, owner, mask, int(enabled), tone, revision, *minutes]
        records.append('\t'.join(map(str, fields)))
    return sorted(records, key=lambda record: record.split('\t')[0])


def schedule_digest(records):
    return f'{zlib.crc32("".join(record + chr(10) for record in records).encode()):08x}'


class Client:
    def __init__(self, ble, key, generation):
        self.ble, self.key, self.generation = ble, key, generation
        self.nonce = ''
        self.sequence = self.response_sequence = self.message_id = 0
        self.assembler = Assembler()
        self.pending = {}

    def notified(self, _characteristic, data):
        try:
            result = self.assembler.accept(data)
            if result:
                ident, text = result
                future = self.pending.get(ident)
                if future and not future.done():
                    future.set_result(text)
        except Exception as error:
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(error)
            self.assembler.reset()

    def verify_reply(self, response):
        parts = response.split('\t', 3)
        if len(parts) != 4 or parts[0] != 'R' or not parts[1].isascii() or not parts[1].isdigit():
            raise ValueError('Unsigned response; reconnect and authenticate again')
        _, seq, supplied, payload = parts
        if int(seq) <= self.response_sequence or not hmac.compare_digest(
            signature(self.key, f'R|{self.nonce}|{self.generation}|{seq}|{payload}'), supplied
        ):
            raise ValueError('Response signature or sequence mismatch')
        self.response_sequence = int(seq)
        return payload

    async def exchange(self, payload, authenticate=True):
        if authenticate:
            self.sequence += 1
            supplied = signature(self.key, f'C|{self.nonce}|{self.generation}|{self.sequence}|{payload}')
            payload = f'{self.sequence}\t{supplied}\t{payload}'
        self.message_id += 1
        ident = self.message_id
        future = asyncio.get_running_loop().create_future()
        self.pending[ident] = future
        try:
            # Respect the actual ATT limit. Write-with-response provides fragment
            # flow control; application acknowledgements confirm durable changes.
            att = min(244, max(20, self.ble.mtu_size - 3))
            for frame in fragments(payload, ident, att):
                await self.ble.write_gatt_char(RX, frame, response=True)
            response = await asyncio.wait_for(future, 240)
            answer = self.verify_reply(response)
            if answer.startswith('error\t'):
                raise RuntimeError(answer)
            return answer
        finally:
            self.pending.pop(ident, None)

    async def authenticate(self, house, gateway, claim):
        greeting = (await self.ble.read_gatt_char(INFO)).decode().split('\t')
        if len(greeting) != 14 or greeting[:2] != ['hello', '2'] or not greeting[6]:
            raise ValueError('Missing Dawn v2 challenge; reconnect')
        clock_id, self.nonce = greeting[2], greeting[6]
        action = 'claim' if claim else 'auth'
        supplied = signature(self.key, f'{action}|{clock_id}|{self.nonce}|{house}|{gateway}|{self.generation}')
        # Authentication succeeds only with a signed server response. A rejected
        # raw error cannot masquerade as an authenticated hardware confirmation.
        return await self.exchange(f'{action}\t{house}\t{gateway}\t{self.generation}\t{supplied}', authenticate=False)


async def main(args):
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise SystemExit('Install the bench-tool dependencies: python3 -m pip install -r tools/requirements.txt') from error
    if args.command == 'scan':
        devices = await BleakScanner.discover(timeout=8, service_uuids=[SERVICE])
        for device in devices:
            print(device.address, device.name or 'Dawn')
        return
    if not args.address or not identifier(args.house) or not identifier(args.gateway) or args.generation < 1:
        raise ValueError('--address, --house, --gateway and a positive --generation are required')
    key_text = os.environ.get('DAWN_PAIRING_KEY') or getpass.getpass('Clock pairing key (64 hex characters): ')
    if not re.fullmatch(r'[0-9a-fA-F]{64}', key_text):
        raise ValueError('The pairing key must be 32 bytes in hexadecimal')
    key = bytes.fromhex(key_text)
    async with BleakClient(args.address, timeout=60, pair=True) as ble:
        client = Client(ble, key, args.generation)
        # Force link encryption before subscribing (especially on NimBLE).
        await ble.read_gatt_char(TX)
        await ble.start_notify(TX, client.notified)
        await client.authenticate(args.house, args.gateway, args.claim)
        if args.command == 'status':
            print(await client.exchange('schedule.read'))
            print(await client.exchange('session.get'))
        elif args.command == 'sync':
            print(await client.exchange(f'time.set\t{int(time.time())}'))
        elif args.command == 'upload':
            document = json.loads(Path(args.file).read_text())
            snapshot = (await client.exchange('schedule.read')).split('\n')[0].split('\t')
            expected = int(snapshot[1])
            revision = expected + 1
            records = encode_records(document['alarms'], revision)
            zone, rule = document['timeZone'], document['posixTZ']
            if any(c in zone + rule for c in '\t\r\n\x00'):
                raise ValueError('Invalid timezone encoding')
            digest = schedule_digest(records)
            transaction = uuid.uuid4().hex
            print(await client.exchange(f'time.set\t{int(time.time())}'))
            await client.exchange(f'schedule.begin\t{transaction}\t{expected}\t{revision}\t{len(records)}\t{digest}\t{zone}\t{rule}\t{args.house}')
            for record in records:
                await client.exchange(f'schedule.record\t{transaction}\t{record}')
            print(await client.exchange(f'schedule.commit\t{transaction}'))
            readback = await client.exchange('schedule.read')
            header, *read_records = readback.split('\n')
            if header.split('\t')[1:3] != [str(revision), digest] or read_records != records:
                raise ValueError('Committed schedule readback differs from the upload')
            print(f'Verified {len(records)} recurring alarms at revision {revision}.')
        elif args.command == 'stop':
            print(await client.exchange(f'session.stop\t{uuid.uuid4().hex}\t{args.session}'))
        elif args.command == 'events':
            print('Polling signed durable events. Ctrl-C to disconnect.')
            while True:
                event = await client.exchange('event.next')
                print(event, flush=True)
                if args.ack and event.startswith('event\t'):
                    await client.exchange('event.ack\t' + event.split('\t')[1])
                await asyncio.sleep(1)
        elif args.command == 'preview':
            print(await client.exchange(f'sound.preview\t{TONES.index(args.tone)}'))
            await asyncio.sleep(3)
        elif args.command == 'identify':
            print(await client.exchange('identify'))


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', help='BLE address (macOS peripheral UUID), from scan')
    parser.add_argument('--house')
    parser.add_argument('--gateway', default='bench')
    parser.add_argument('--generation', type=int, default=1)
    parser.add_argument('--claim', action='store_true', help='First binding only; requires a physical pairing window')
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ['scan', 'status', 'sync', 'identify']:
        commands.add_parser(name)
    upload = commands.add_parser('upload'); upload.add_argument('file')
    stop = commands.add_parser('stop'); stop.add_argument('session', type=int)
    events = commands.add_parser('events'); events.add_argument('--ack', action='store_true')
    preview = commands.add_parser('preview'); preview.add_argument('tone', choices=TONES)
    return parser.parse_args()


if __name__ == '__main__':
    try:
        asyncio.run(main(arguments()))
    except KeyboardInterrupt:
        pass
    except (ValueError, RuntimeError, OSError, asyncio.TimeoutError) as error:
        raise SystemExit(str(error))
