"""Protocol v1 frame codec -- the Python mirror of src/alink_frame.c.

    +------+------+-----------+--------+
    | CTRL | LEN  | PAYLOAD   | CRC-16 |
    | 1 B  | 1 B  | 0..64 B   | 2 B    |
    +------+------+-----------+--------+

CTRL bits: [TYPE:7-5] [reserved:4-2] [SEQ:1] [ACK:0]

Any change here must be mirrored in src/alink_frame.c and vice versa.
test/test_alink_xcheck.py is what actually enforces that.
"""

HELLO = 0
WELCOME = 1
LINK = 2
BYE = 3

MAX_PAYLOAD = 64
FRAME_MAX = 4 + MAX_PAYLOAD
HELLO_PAYLOAD = 3

VERSION = 1
CAPS = 0x00

CTRL_ACK = 0x01
CTRL_SEQ = 0x02
CTRL_TYPE_SHIFT = 5


class Frame:
    __slots__ = ("type", "seq", "ack", "payload")

    def __init__(self, type_, seq, ack, payload):
        self.type = type_
        self.seq = seq
        self.ack = ack
        self.payload = payload

    @property
    def len(self):
        return len(self.payload)

    def __repr__(self):
        names = {HELLO: "HELLO", WELCOME: "WELCOME", LINK: "LINK", BYE: "BYE"}
        return (f"Frame({names.get(self.type, self.type)}, seq={self.seq}, "
                f"ack={self.ack}, len={self.len})")


def crc16(data):
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode(type_, seq, ack, payload=b""):
    """Serialise one frame. Raises ValueError if the payload exceeds the cap."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} exceeds {MAX_PAYLOAD}")
    ctrl = (type_ & 0x07) << CTRL_TYPE_SHIFT
    if seq:
        ctrl |= CTRL_SEQ
    if ack:
        ctrl |= CTRL_ACK
    body = bytes([ctrl, len(payload)]) + payload
    return body + crc16(body).to_bytes(2, "big")


def decode(block, max_payload=MAX_PAYLOAD):
    """Validate and parse one received block.

    Returns a Frame, or None if the block fails any validation rule -- in which
    case the caller must behave exactly as if nothing had arrived.
    """
    if len(block) < 4:
        return None

    length = block[1]
    if length > MAX_PAYLOAD:                       # rule 3, absolute cap
        return None
    if len(block) != length + 4:                   # rule 1
        return None

    body = bytes(block[:2 + length])               # rule 2
    want = int.from_bytes(block[2 + length:4 + length], "big")
    if crc16(body) != want:
        return None

    type_ = (block[0] >> CTRL_TYPE_SHIFT) & 0x07
    if type_ > BYE:                                # 4-7 reserved: ignore
        return None
    if type_ == LINK and length > max_payload:     # rule 3, LINK only
        return None
    if type_ in (HELLO, WELCOME) and length < HELLO_PAYLOAD:   # rule 4
        return None

    return Frame(type_,
                 1 if block[0] & CTRL_SEQ else 0,
                 1 if block[0] & CTRL_ACK else 0,
                 bytes(block[2:2 + length]))
