#!/usr/bin/env python3
"""vine_modem against the AT commands the guest's RIL (cuttlefish
reference-ril) sends: test_modem.py <vine_modem binary>"""
import os
import socket
import subprocess
import sys
import tempfile
import time

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


class Modem:
    def __init__(self, binary, *args):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "modem")
        self.proc = subprocess.Popen([binary, "--socket", self.path, *args], stderr=subprocess.PIPE)
        for _ in range(100):
            if os.path.exists(self.path):
                break
            time.sleep(0.05)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.path)
        self.sock.settimeout(2)
        self.buf = b""

    def at(self, cmd):
        """The lines up to and including the final result"""
        self.sock.sendall(cmd.encode() + b"\r")
        lines = []
        while True:
            while b"\r\n" not in self.buf:
                self.buf += self.sock.recv(4096)
            raw, self.buf = self.buf.split(b"\r\n", 1)
            if not raw:
                continue
            s = raw.decode()
            lines.append(s)
            if s in ("OK", "ERROR") or s.startswith("+CME ERROR:"):
                return lines

    def unsolicited(self):
        """Lines already sent after the last final result"""
        time.sleep(0.1)
        self.sock.setblocking(False)
        try:
            self.buf += self.sock.recv(4096)
        except BlockingIOError:
            pass
        self.sock.settimeout(2)
        out, self.buf = [l.decode() for l in self.buf.split(b"\r\n") if l], b""
        return out

    def close(self):
        self.sock.close()
        self.proc.kill()
        return self.proc.communicate()[1].decode()


def luhn_ok(digits):
    total = 0
    for i, c in enumerate(reversed(digits)):
        d = int(c)
        if i % 2 == 1:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return total % 10 == 0


def ucs2_spn(hexdata):
    b = bytes.fromhex(hexdata)
    assert b[1] == 0x80
    body = b[2:]
    chars = []
    for i in range(0, len(body) - 1, 2):
        if body[i:i + 2] == b"\xff\xff":
            break
        chars.append(body[i:i + 2].decode("utf-16-be"))
    return "".join(chars)


def main():
    binary = sys.argv[1]

    m = Modem(binary, "--seed", "instance-1", "--sim-operator", "46000", "--sim-name", "中国移动",
              "--net-operator", "46000", "--net-name", "CHINA MOBILE")
    # initializeCallback
    for c in ["ATE0Q0V1", "ATS0=0", "AT+CMEE=1", "AT+CREG=2", "AT+CGREG=1", "AT+CSCS=\"HEX\"", "AT+CMGF=0"]:
        check(m.at(c) == ["OK"], c)
    check(m.at("AT+CTEC?") == ["+CTEC: 5,23", "OK"], "multimode modem, LTE")
    check(m.at("AT+CTEC=?") == ["+CTEC: 0,1,5", "OK"], "supported techs")
    check(m.at("AT+CFUN?") == ["+CFUN: 0", "OK"], "radio starts off")
    check(m.at("AT+CREG?") == ["+CREG: 2,0", "OK"], "not registered while off")
    check(m.at("AT+CFUN=1") == ["OK"], "radio on")
    check(m.unsolicited() == ["+CREG: 1"], "registration change reported")
    check(m.at("AT+CFUN?") == ["+CFUN: 1", "OK"], "radio is on")
    check(m.at("AT+CPIN?") == ["+CPIN: READY", "OK"], "SIM ready")

    imsi = m.at("AT+CIMI")
    check(len(imsi) == 2 and imsi[0].startswith("46000") and len(imsi[0]) == 15 and imsi[0].isdigit(), "IMSI " + imsi[0])
    iccid = m.at("AT+CICCID")[0]
    check(len(iccid) == 20 and iccid.startswith("8946000") and luhn_ok(iccid), "ICCID " + iccid)
    imei = m.at("AT+CGSN")[0]
    check(len(imei) == 15 and luhn_ok(imei), "IMEI " + imei)

    check(m.at("AT+CREG?") == ["+CREG: 2,1,\"2142\",\"0000B804\",7", "OK"], "registered on LTE")
    check(m.at("AT+CEREG?") == ["+CEREG: 2,1,\"2142\",\"0000B804\",7", "OK"], "EPS registered")
    check(m.at("AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?") ==
          ["+COPS: 0,0,\"CHINA MOBILE\"", "+COPS: 0,1,\"CHINA MOBILE\"", "+COPS: 0,2,\"46000\"", "OK"], "operator")
    csq = m.at("AT+CSQ")
    check(len(csq) == 2 and len(csq[0].split(":")[1].split(",")) == 22, "signal strength has 22 values")

    # SIM files: GET RESPONSE gives a USIM FCP, reads the contents
    check(m.at("AT+CRSM=192,28589,0,0,15") == ["+CRSM: 144,0,62178202412183026FAD8A01058B036F060180020004880118", "OK"],
          "EF_AD header")
    check(m.at("AT+CRSM=176,28589,0,0,4") == ["+CRSM: 144,0,00000002", "OK"], "EF_AD: 2-digit MNC")
    spn = m.at("AT+CRSM=176,28486,0,0,17")[0].split(",")[2]
    check(ucs2_spn(spn) == "中国移动", "EF_SPN in UCS2: " + spn)
    iccid_ef = m.at("AT+CRSM=176,12258,0,0,10")[0].split(",")[2]
    swapped = "".join(iccid_ef[i + 1] + iccid_ef[i] for i in range(0, 20, 2))
    check(swapped == iccid, "EF_ICCID is the ICCID in BCD")
    check(m.at("AT+CRSM=178,28480,1,4,28") == ["+CRSM: 144,0," + "F" * 56, "OK"], "MSISDN empty")
    check(m.at("AT+CRSM=176,28472,0,0,10") == ["+CRSM: 106,130", "OK"], "other files not found")

    check(m.at("AT+CLCK=\"SC\",2,\"\",7") == ["+CLCK: 0", "OK"], "no SIM lock")
    check(m.at("ATD*99***1#") == ["ERROR"], "no data calls")
    check(m.at("AT+CGACT=1,0") == ["ERROR"], "no PDP activation")
    check(m.at("AT+CGDCONT?") == ["OK"], "no contexts")
    check(m.at("AT+CCHO=\"A00000015141434C00\"") == ["ERROR"], "no logical channels")
    check(m.at("AT+WRMP?") == ["ERROR"], "unknown query")
    check(m.at("AT%CPHS=1") == ["OK"], "unknown setting")
    m.close()

    # Same seed, same identity
    m = Modem(binary, "--seed", "instance-1", "--sim-operator", "46000", "--sim-name", "x")
    check(m.at("AT+CICCID")[0] == iccid and m.at("AT+CGSN")[0] == imei, "identity stable for a seed")
    m.close()
    m = Modem(binary, "--seed", "instance-2", "--sim-operator", "310260", "--sim-name", "T-Mobile")
    check(m.at("AT+CICCID")[0] != iccid, "another seed, another ICCID")
    check(m.at("AT+CRSM=176,28589,0,0,4") == ["+CRSM: 144,0,00000003", "OK"], "3-digit MNC")
    spn = bytes.fromhex(m.at("AT+CRSM=176,28486,0,0,17")[0].split(",")[2])
    check(spn[1] == 0x80, "a '-' needs UCS2")
    m.at("AT+CFUN=1")
    m.unsolicited()
    check(m.at("AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?")[2] == "+COPS: 0,2,\"310260\"",
          "network defaults to the SIM's")
    m.close()

    m = Modem(binary, "--seed", "instance-3")
    check(m.at("AT+CPIN?") == ["+CME ERROR: 10"], "no SIM: not inserted")
    check(m.at("AT+CREG?") == ["+CREG: 2,0", "OK"], "no SIM, radio off: no service")
    m.at("AT+CFUN=1")
    check(m.unsolicited() == ["+CREG: 12"], "no SIM: registration change reported")
    check(m.at("AT+CREG?") == ["+CREG: 2,12,\"2142\",\"0000B804\",7", "OK"], "no SIM: emergency calls only")
    check(m.at("AT+CEREG?") == ["+CEREG: 2,0", "OK"], "no SIM: no packet service")
    check(m.at("AT+COPS?") == ["+COPS: 0", "OK"], "no SIM: no operator")
    check(m.at("AT+CSQ")[0].startswith("+CSQ: 99,99,-1,-1,-1,-1,-1,20,90,"), "no SIM: the cell is still heard")
    check(m.at("AT+CRSM=176,28589,0,0,4") == ["+CME ERROR: 10"], "no SIM: no files")
    check(m.at("AT+CGSN")[0] != imei and luhn_ok(m.at("AT+CGSN")[0]), "no SIM: still an IMEI")
    log = m.close()
    check("no SIM" in log, "logs no SIM")

    # A restarted RIL reconnects
    m = Modem(binary, "--seed", "s", "--sim-operator", "46001", "--sim-name", "CU")
    m.at("AT+CFUN=1")
    m.sock.close()
    m.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    m.sock.connect(m.path)
    m.sock.settimeout(2)
    m.buf = b""
    check(m.at("AT+CFUN?") == ["+CFUN: 0", "OK"], "a new RIL starts with the radio off")
    m.close()

    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
