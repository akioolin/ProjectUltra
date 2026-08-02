#!/usr/bin/env python3
"""Focused regression tests for transfer-log ACK reconstruction."""

import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "analyze_transfer", ROOT / "tools" / "analyze_transfer.py")
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)


def ack(group, mask="0x0000", samples="19584"):
    return {
        "g": str(group),
        "ty": "ACK",
        "fm": mask,
        "s": samples,
    }


class AckReconstructionTests(unittest.TestCase):
    def test_detailed_ack_acceptance_telemetry_parses(self):
        match = ANALYZER.SENDER_PATTERNS["ack_seen_detail"].search(
            "[ 65.371][INFO ][MODEM] ToneBurstAck monitor: detected "
            "group_seq=12 type=ACK mask=0x0000 epoch=0 peak=311.7 "
            "normalized_peak=0.601 min_conf=6.251 hamming_corrected=0 "
            "symbol_ms=12 stream_offset=1444408")
        self.assertIsNotNone(match)
        self.assertEqual(match.groupdict(), {
            "g": "12",
            "peak": "0.601",
            "conf": "6.251",
            "hamming": "0",
            "symbol_ms": "12",
        })

    def test_semantic_rejection_telemetry_parses(self):
        match = ANALYZER.SENDER_PATTERNS["ack_semantic_reject"].search(
            "ToneBurstAckMonitor: CRC-valid candidate rejected before commit "
            "group_seq=10 type=ACK mask=0x0001 epoch=0 peak=33.1 "
            "normalized_peak=0.064 min_conf=5.200 hamming_corrected=0 "
            "symbol_ms=12 stream_offset=12345 rejected_in_search=2 "
            "— monitor remains armed")
        self.assertIsNotNone(match)
        self.assertEqual(match.groupdict(), {
            "g": "10",
            "peak": "0.064",
            "conf": "5.200",
            "hamming": "0",
            "symbol_ms": "12",
            "count": "2",
        })

    def test_audio_commit_marker_contract_parses(self):
        match = ANALYZER.RECEIVER_PATTERNS["ack_commit"].search(
            "[ 12.345][INFO ][MODEM] TX-AUDIO-COMMIT: tone-ack "
            "source=silent-repeat samples=40800")
        self.assertIsNotNone(match)
        self.assertEqual(match.groupdict(), {
            "src": "silent-repeat",
            "s": "40800",
        })

    def test_commit_mode_excludes_rendered_but_dropped_ack(self):
        events = [
            (1.000, "group", {}),
            (1.000, "ack_tx", ack(1, "0x0001")),  # rendered, then dropped
            (4.000, "sack_timer", {}),
            (4.000, "ack_tx", ack(2, "0x0002")),
            # The App commits postProcessTx output: raw 19,584 + 9,600
            # lead-in/tail samples.  The older analyzer compared these counts
            # for equality and consequently treated both renders as dropped.
            (6.500, "ack_commit", {"src": "primary", "s": "29184"}),
        ]

        tx = ANALYZER.reconstruct_ack_transmissions(events)
        self.assertEqual(len(tx), 1)
        self.assertEqual(tx[0][0], 6.500)
        self.assertEqual(tx[0][1]["fm"], "0x0002")
        self.assertEqual(tx[0][1]["_render_t"], 4.000)
        self.assertEqual(tx[0][1]["_keyed_samples"], 29184)
        self.assertAlmostEqual(
            ANALYZER.ack_keydown_seconds(tx[0][1]), 0.608, places=6)
        self.assertEqual(
            ANALYZER.classify_ack_origins(events, tx), ["timer_sack"])

    def test_commit_mode_counts_silent_repeat_commit_not_firing_proxy(self):
        events = [
            (2.000, "group", {}),
            (2.000, "ack_tx", ack(7)),
            (2.001, "ack_commit", {"src": "primary", "s": "29184"}),
            (5.000, "ack_repeat_tx", {}),
            (5.002, "ack_commit", {"src": "silent-repeat", "s": "29184"}),
        ]

        tx = ANALYZER.reconstruct_ack_transmissions(events)
        self.assertEqual(len(tx), 2)
        self.assertEqual([t for t, _ in tx], [2.001, 5.002])
        self.assertEqual(
            ANALYZER.classify_ack_origins(events, tx),
            ["group", "silent_repeat"],
        )

    def test_legacy_logs_keep_render_and_repeat_fallback(self):
        events = [
            (1.000, "ack_tx", ack(1)),
            (2.000, "ack_tx", ack(2, "0x0004")),
            (3.000, "ack_repeat_tx", {}),
        ]

        tx = ANALYZER.reconstruct_ack_transmissions(events)
        self.assertEqual(len(tx), 3)
        self.assertEqual(tx[-1][1]["fm"], "0x0004")
        self.assertEqual(tx[-1][1]["_origin"], "silent_repeat")
        self.assertTrue(all(d["_evidence"].startswith("legacy_") for _, d in tx))

    def test_unmatched_commit_remains_physical_evidence(self):
        events = [
            (9.000, "ack_commit", {"src": "primary", "s": "50400"}),
        ]

        tx = ANALYZER.reconstruct_ack_transmissions(events)
        self.assertEqual(tx, [(9.000, {
            "g": "?",
            "ty": "ACK",
            "fm": "unknown",
            "s": "50400",
            "_render_matched": False,
            "_evidence": "audio_commit",
            "_commit_source": "primary",
            "_keyed_samples": 50400,
        })])
        self.assertAlmostEqual(
            ANALYZER.ack_keydown_seconds(tx[0][1]), 1.05, places=6)


class ClockAlignmentTests(unittest.TestCase):
    @staticmethod
    def sender_burst(t, group_size, signal_airtime_s):
        modulated_samples = round(
            (signal_airtime_s - ANALYZER.LEAD_IN_S) * 48000.0)
        return [
            (t, "tx_burst", {
                "n": str(group_size),
                "cw": "8",
                "mod": "QPSK",
                "rate": "R3/4",
            }),
            (t + 0.010, "tx_samples", {
                "n": str(group_size),
                "samples": str(modulated_samples),
            }),
        ]

    @staticmethod
    def receiver_group(t, total):
        return (t, "group", {
            "ord": None,
            "g": "0",
            "ok": str(total),
            "tot": str(total),
            "all": "1",
            "it": "1",
            "q": "0.99",
        })

    def test_missing_first_rx_group_uses_multi_burst_consensus(self):
        # Sender clock +100s = receiver clock.  Burst #1 produces no callback;
        # first->first would therefore return +112s and shift the entire run by one
        # cycle.  The later measured end times and 5/7-frame geometry identify the
        # surviving callbacks as bursts #2 and #3.
        sender = []
        sender += self.sender_burst(10.0, 3, 6.0)
        sender += self.sender_burst(21.0, 5, 7.0)
        sender += self.sender_burst(35.0, 7, 5.0)
        receiver = [
            self.receiver_group(128.0, 5),
            self.receiver_group(140.0, 7),
        ]

        offset, provenance = ANALYZER.align(sender, receiver)

        self.assertAlmostEqual(offset, 100.0, places=6)
        self.assertIn("multi-burst consensus 2/3 TX, 2/2 groups", provenance)
        self.assertIn("first matched TX #2", provenance)

        aligned_txs = [
            (t + offset, d) for t, kind, d in sender if kind == "tx_burst"
        ]
        air = [6.0, 7.0, 5.0]
        groups = [(t, d) for t, kind, d in receiver if kind == "group"]
        matches, unmatched = ANALYZER.associate_groups_to_bursts(
            aligned_txs, air, groups)
        self.assertEqual(sorted(matches), [1, 2])
        self.assertEqual(unmatched, [])


class TransferAccountingTests(unittest.TestCase):
    def test_mixed_timer_origins_remain_distinct(self):
        clocks = ANALYZER.transfer_time_bases(
            t_done=22.0,
            app_duration_s=10.0,
            first_keydown_t=10.0,
            file_start_t=12.0)

        self.assertEqual(clocks['app_origin_t'], 12.0)
        self.assertEqual(clocks['app_origin_evidence'], 'FILE_START log marker')
        self.assertEqual(clocks['pre_app_keydown_s'], 2.0)
        self.assertEqual(clocks['physical_span_s'], 12.0)

        inferred = ANALYZER.transfer_time_bases(
            t_done=22.0,
            app_duration_s=10.0,
            first_keydown_t=10.0)
        self.assertEqual(inferred['app_origin_t'], 12.0)
        self.assertEqual(
            inferred['app_origin_evidence'],
            'inferred from completion duration')

        unavailable = ANALYZER.transfer_time_bases(
            t_done=None, app_duration_s=None, first_keydown_t=10.0)
        self.assertIsNone(unavailable['app_origin_t'])
        self.assertEqual(unavailable['app_origin_evidence'], 'unavailable')

    def test_one_burst_duty_clips_final_tail_after_completion(self):
        txs = [(10.0, {})]
        emitted = [2.0]
        occupied = ANALYZER.keydown_overlap_seconds(
            txs, emitted, start_t=10.0, end_t=11.9)

        self.assertAlmostEqual(occupied, 1.9)
        self.assertAlmostEqual(sum(emitted) - occupied, 0.1)

    def test_rung_state_announcements_are_not_all_transitions(self):
        segments, transitions = ANALYZER.rung_transfer_timeline([
            (4.0, 'QPSK R1/2', 15.0),       # state already active at start
            (8.0, 'QPSK R1/2', 14.0),       # duplicate announcement
            (15.0, 'QPSK R2/3', 12.0),      # one real transition
            (25.0, '8PSK R2/3', 14.0),      # after completion
        ], start_t=10.0, end_t=22.0)

        self.assertEqual(segments, [
            (10.0, 15.0, 'QPSK R1/2', 14.0, 8.0),
            (15.0, 22.0, 'QPSK R2/3', 12.0, 15.0),
        ])
        self.assertEqual(transitions, [(15.0, 'QPSK R2/3', 12.0)])

    def test_rung_unknown_prefix_is_covered_and_completion_event_excluded(self):
        segments, transitions = ANALYZER.rung_transfer_timeline([
            (12.0, 'QPSK R1/2', 15.0),
            (15.0, 'QPSK R2/3', 12.0),
            (22.0, '8PSK R2/3', 14.0),  # exactly at completion
        ], start_t=10.0, end_t=22.0)

        self.assertEqual(segments[0][:3], (10.0, 12.0, 'UNKNOWN'))
        self.assertEqual(segments[1:], [
            (12.0, 15.0, 'QPSK R1/2', 15.0, 12.0),
            (15.0, 22.0, 'QPSK R2/3', 12.0, 15.0),
        ])
        self.assertEqual(transitions, [(15.0, 'QPSK R2/3', 12.0)])
        self.assertAlmostEqual(sum(end - start for start, end, *_ in segments), 12.0)

    def test_report_names_each_clock_and_uses_physical_span_for_efficiency(self):
        sender_log = """\
[ 10.000][INFO ] TX Burst descriptor: group=1 cw/frame=8 QPSK R1/2
[ 10.010][INFO ] TX Burst: 1 frames -> 86400 samples
[ 20.000][INFO ] TX Burst descriptor: group=1 cw/frame=8 QPSK R2/3
[ 20.010][INFO ] TX Burst: 1 frames -> 86400 samples
"""
        receiver_log = """\
[  4.000][INFO ] [MODE] OFDM QPSK R1/2 (usable RX SNR=15)
[  5.000][INFO ] [CONNECT]
[ 11.900][INFO ] Burst logical frame 1/1: OK
[ 11.950][INFO ] Burst #1 (group_seq=0) delivered as unit: 1/1 logical OK (all_ok=1) max_iters=1 quality=0.99
[ 12.000][INFO ] [FILE] Receiving sample.bin (1000 bytes)...
[ 15.000][INFO ] [MODE] OFDM QPSK R2/3 (usable RX SNR=12)
[ 21.900][INFO ] Burst logical frame 1/1: OK
[ 21.950][INFO ] Burst #2 (group_seq=1) delivered as unit: 1/1 logical OK (all_ok=1) max_iters=1 quality=0.99
[ 22.000][INFO ] [FILE] Received /tmp/sample.bin (1000 bytes, CRC ok, 10.0s, 0.80 kbps)
"""

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            sender = temp / 'sender.log'
            receiver = temp / 'receiver.log'
            report_json = temp / 'report.json'
            sender.write_text(sender_log)
            receiver.write_text(receiver_log)
            old_argv = sys.argv
            stdout = io.StringIO()
            try:
                sys.argv = [
                    'analyze_transfer.py', str(sender), str(receiver),
                    '--json', str(report_json)]
                with contextlib.redirect_stdout(stdout):
                    ANALYZER.main()
            finally:
                sys.argv = old_argv

            report = stdout.getvalue()
            data = json.loads(report_json.read_text())

        self.assertIn('app-reported delivered : 0.80 kbps over 10.0s', report)
        self.assertIn('physical-span delivered: 0.67 kbps over 12.00s', report)
        self.assertIn('CONNECTED -> first data key-down: 5.00s', report)
        self.assertIn(
            'first data key-down -> app RX timer origin: 2.00s', report)
        self.assertIn(
            'rung state announcements through completion: 2', report)
        self.assertIn('physical-transfer rung transitions: 1', report)
        self.assertIn('keyed-to-physical-span efficiency: 33.3%', report)
        self.assertIn(
            'per-frame-position decode rate (descriptive; does not by itself '
            'rule stale CSI in or out)', report)
        self.assertNotIn('handshake->data gap', report)
        self.assertNotIn('flat => NOT stale-CSI', report)
        self.assertAlmostEqual(data['physical_transfer_span_s'], 12.0)
        self.assertAlmostEqual(data['physical_span_kbps'], 2.0 / 3.0)
        self.assertAlmostEqual(
            data['keyed_to_physical_span_efficiency_pct'], 100.0 / 3.0)
        self.assertEqual(data['rung_state_announcements'], 2)
        self.assertEqual(data['physical_transfer_rung_transitions'], 1)


if __name__ == "__main__":
    unittest.main()
