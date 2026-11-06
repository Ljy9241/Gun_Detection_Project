"""Versioned LAN discovery on the telemetry UDP socket."""
import re


def discovery_reply(data):
    """Echo a request nonce; the device uses the reply's source IP and port."""
    match = re.fullmatch(rb'GUN_MONITOR_DISCOVER_V1 ([0-9a-f]{16})', data)
    if match is None:
        return None
    return b'GUN_MONITOR_HERE_V1 ' + match[1]
