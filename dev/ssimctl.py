"""Developer client for the SSIM engine's control pipe (\\\\.\\pipe\\SSIM).

    python ssimctl.py status
    python ssimctl.py set <serial> outputMode=xinput slot=2 rumble=0.5 name="My Pro"
    python ssimctl.py identify <serial>
    python ssimctl.py global dsu=false rumbleSafetySeconds=10
    python ssimctl.py watch            # print every status update
"""

import json
import sys

PIPE = r"\\.\pipe\SSIM"


def value(v):
    if v in ("true", "false"):
        return v == "true"
    for cast in (int, float):
        try:
            return cast(v)
        except ValueError:
            pass
    return v


def fields(args):
    return {k: value(v) for k, v in (a.split("=", 1) for a in args)}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd, rest = sys.argv[1], sys.argv[2:]
    if cmd == "status":
        req = {"cmd": "status"}
    elif cmd == "watch":
        req = {"cmd": "subscribe"}
    elif cmd == "set":
        req = {"cmd": "setController", "serial": rest[0], **fields(rest[1:])}
    elif cmd == "identify":
        req = {"cmd": "identify", "serial": rest[0]}
    elif cmd == "global":
        req = {"cmd": "setGlobal", **fields(rest)}
    else:
        print(__doc__)
        return

    with open(PIPE, "r+b", buffering=0) as pipe:
        pipe.write((json.dumps(req) + "\n").encode())
        buf = b""
        while True:
            chunk = pipe.read(65536)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(json.dumps(json.loads(line), indent=2))
                if cmd != "watch":
                    return


if __name__ == "__main__":
    main()
