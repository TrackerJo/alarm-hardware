import asyncio
import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

spec = importlib.util.spec_from_file_location('dawn_clock', Path(__file__).resolve().parents[1] / 'tools/dawn_clock.py')
clock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clock)


class ProtocolTests(unittest.TestCase):
    def test_frames_roundtrip_at_both_att_sizes(self):
        for size in [20, 244]:
            assembler = clock.Assembler()
            result = None
            for frame in clock.fragments('example' * 200, 42, size):
                result = assembler.accept(frame)
            self.assertEqual(result, (42, 'example' * 200))

    def test_crc_and_order_fail_closed(self):
        frames = list(clock.fragments('x' * 60, 2, 20))
        with self.assertRaises(ValueError):
            clock.Assembler().accept(frames[1])
        frame = bytearray(next(clock.fragments('hello', 1, 20)))
        frame[-1] ^= 1
        with self.assertRaises(ValueError):
            clock.Assembler().accept(frame)

    def test_authenticated_responses_reject_replay_modification_and_old_connections(self):
        key = bytes(range(32))
        client = clock.Client(None, key, 4)
        client.nonce = 'test-nonce'
        payload = 'event\t9\tsession.stopped\t3\t1793100000\t1\na\talice\t20261027\t1793099990\t0\t1\t0'
        mac = clock.signature(key, f'R|test-nonce|4|1|{payload}')
        response = f'R\t1\t{mac}\t{payload}'
        self.assertEqual(client.verify_reply(response), payload)
        with self.assertRaises(ValueError):
            client.verify_reply(response)
        fresh = clock.Client(None, key, 4); fresh.nonce = 'test-nonce'
        with self.assertRaises(ValueError):
            fresh.verify_reply(response.replace('alice', 'mallory'))
        fresh.nonce = 'next-connection'
        with self.assertRaises(ValueError):
            fresh.verify_reply(response)
        fresh.nonce = 'test-nonce'; fresh.generation = 5
        with self.assertRaises(ValueError):
            fresh.verify_reply(response)
        with self.assertRaises(ValueError):
            fresh.verify_reply(payload)

    def test_canonical_schedule_vector_and_per_day_overrides(self):
        example = [{'id': 'a', 'ownerID': 'alice', 'weekdays': [1, 2], 'hour': 7, 'minute': 0,
                    'dayTimes': {'2': {'hour': 8, 'minute': 15}}, 'tone': 'birdsong'}]
        records = clock.encode_records(example, 1)
        self.assertEqual(records, ['a\talice\t6\t1\t1\t1\t420\t420\t495\t420\t420\t420\t420'])
        # Shared golden vector, also asserted by the C++ test suite.
        self.assertEqual(clock.schedule_digest(records), '012639e7')
        with self.assertRaises(ValueError):
            clock.encode_records(example + example, 1)
        example[0]['dayTimes']['2']['hour'] = 24
        with self.assertRaises(ValueError):
            clock.encode_records(example, 1)

    def test_large_response_is_bounded_and_reassembled(self):
        data = ('a\talice\t20261027\t1793099990\t0\t1\t0\n' * 500).encode()
        assembler = clock.Assembler(); result = None
        for offset in range(0, len(data), 230):
            result = assembler.accept(struct.pack('<BBIHHI', 0xd2, 0, 7, len(data), offset, zlib.crc32(data)) + data[offset:offset+230])
        self.assertEqual(result, (7, data.decode()))


class FakeBLE:
    mtu_size = 23

    def __init__(self):
        self.client = None
        self.assembler = clock.Assembler()
        self.response_sequence = 0
        self.commands = []

    async def write_gatt_char(self, characteristic, frame, response):
        assert characteristic == clock.RX and response
        received = self.assembler.accept(frame)
        if not received:
            return
        ident, message = received
        seq, mac, payload = message.split('\t', 2)
        expected = clock.signature(self.client.key, f'C|{self.client.nonce}|2|{seq}|{payload}')
        assert mac == expected
        self.commands.append(payload)
        self.response_sequence += 1
        reply = 'schedule.applied\t2\t12345678'
        signed = f'R\t{self.response_sequence}\t' + clock.signature(
            self.client.key, f'R|{self.client.nonce}|2|{self.response_sequence}|{reply}') + '\t' + reply
        for response_frame in clock.fragments(signed, ident, 20):
            self.client.notified(None, response_frame)


class AsyncClientTests(unittest.IsolatedAsyncioTestCase):
    async def test_serialized_signed_exchange_and_application_ack(self):
        ble = FakeBLE(); client = clock.Client(ble, bytes(32), 2); ble.client = client; client.nonce = 'test-nonce'
        answer = await client.exchange('schedule.commit\ttransaction')
        self.assertEqual(answer, 'schedule.applied\t2\t12345678')
        self.assertEqual(ble.commands, ['schedule.commit\ttransaction'])
        self.assertEqual(client.pending, {})


if __name__ == '__main__':
    unittest.main()
