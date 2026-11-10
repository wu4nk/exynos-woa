#!/usr/bin/env python3
"""sign-pe.py (v0.1) - exynos-woa: Authenticode-sign a PE (a kernel driver) on Linux with a self-made test certificate.

    sign-pe.py <in.sys> <out.sys> <key-dir> [--verify-only]

<key-dir> holds testsign.key (RSA-2048, PEM) and testsign.cer (DER, self-signed, EKU code signing); both are made on
the first run and reused after. The certificate is a TEST certificate: Windows loads a driver signed with it only in
test mode (bcdedit /set testsigning on - the "Test Mode" watermark), as with the WDK's own test certificates. The
private key never leaves <key-dir> (keep it out of git).

What is written (MS "Windows Authenticode Portable Executable Signature Format", and what signtool / osslsigncode
produce): the file is padded to 8 bytes; its Authenticode SHA-256 hash skips the CheckSum field, the Certificate
Table directory entry and the certificate table; the hash goes into an SpcIndirectDataContent
(SPC_PE_IMAGE_DATAOBJ), signed in a PKCS#7 SignedData with authenticated attributes (contentType, messageDigest over
the SpcIndirectDataContent's contents without its own tag and length, SpcSpOpusInfo, SpcStatementType individual),
RSA PKCS#1 v1.5 + SHA-256; appended as a WIN_CERTIFICATE (revision 2.0, PKCS_SIGNED_DATA); the security directory
and the PE CheckSum are updated. The script then verifies what it wrote (hash and signature) before returning 0.
"""
import datetime
import hashlib
import os
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


# ---- minimal DER ----
def der(tag, content):
    n = len(content)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + content


def seq(*parts):
    return der(0x30, b''.join(parts))


def set_of(*parts):
    return der(0x31, b''.join(sorted(parts)))      # DER: SET OF sorted by encoding


def oid(dotted):
    a = [int(x) for x in dotted.split('.')]
    out = bytes([40 * a[0] + a[1]])
    for v in a[2:]:
        enc = [v & 0x7F]
        v >>= 7
        while v:
            enc.append(0x80 | (v & 0x7F))
            v >>= 7
        out += bytes(reversed(enc))
    return der(0x06, out)


def integer(v):
    b = v.to_bytes((v.bit_length() + 8) // 8 or 1, 'big')
    return der(0x02, b)


def octets(b):
    return der(0x04, b)


def ctx(n, content, constructed=True):
    return der((0xA0 if constructed else 0x80) | n, content)


NULL = b'\x05\x00'
OID_SHA256 = '2.16.840.1.101.3.4.2.1'
OID_RSA = '1.2.840.113549.1.1.1'
OID_SIGNED_DATA = '1.2.840.113549.1.7.2'
OID_CONTENT_TYPE = '1.2.840.113549.1.9.3'
OID_MESSAGE_DIGEST = '1.2.840.113549.1.9.4'
OID_SPC_INDIRECT = '1.3.6.1.4.1.311.2.1.4'
OID_SPC_STATEMENT = '1.3.6.1.4.1.311.2.1.11'
OID_SPC_OPUS = '1.3.6.1.4.1.311.2.1.12'
OID_SPC_PE_IMAGE = '1.3.6.1.4.1.311.2.1.15'
OID_SPC_INDIVIDUAL = '1.3.6.1.4.1.311.2.1.21'


# ---- PE ----
def pe_layout(data):
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise SystemExit('not a PE file')
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic != 0x20B:
        raise SystemExit('only PE32+ images')
    checksum_off = opt + 64
    dirs = opt + 112
    sec_dir_off = dirs + 4 * 8          # IMAGE_DIRECTORY_ENTRY_SECURITY
    return checksum_off, sec_dir_off


def authenticode_hash(data):
    checksum_off, sec_dir_off = pe_layout(data)
    cert_off, cert_len = struct.unpack_from('<II', data, sec_dir_off)
    end = cert_off if cert_len else len(data)
    h = hashlib.sha256()
    h.update(data[:checksum_off])
    h.update(data[checksum_off + 4:sec_dir_off])
    h.update(data[sec_dir_off + 8:end])
    return h.digest()


def pe_checksum(data, checksum_off):
    s = 0
    n = len(data)
    padded = data + (b'\0' if n % 2 else b'')
    for i in range(0, len(padded), 2):
        if i == checksum_off or i == checksum_off + 2:
            continue
        s += padded[i] | (padded[i + 1] << 8)
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    return (s + n) & 0xFFFFFFFF


# ---- certificate ----
def load_or_make_cert(key_dir):
    kp, cp = os.path.join(key_dir, 'testsign.key'), os.path.join(key_dir, 'testsign.cer')
    if os.path.exists(kp) and os.path.exists(cp):
        key = serialization.load_pem_private_key(open(kp, 'rb').read(), None)
        cert = x509.load_der_x509_certificate(open(cp, 'rb').read())
        return key, cert
    os.makedirs(key_dir, exist_ok=True)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'exynos-woa S10 test signing (not trusted)')])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]), False)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), False)
            .sign(key, hashes.SHA256()))
    fd = os.open(kp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    open(cp, 'wb').write(cert.public_bytes(serialization.Encoding.DER))
    return key, cert


# ---- signing ----
def build_signature(digest, key, cert):
    alg = seq(oid(OID_SHA256), NULL)
    pe_image_data = bytes.fromhex('3009030100a004a2028000')    # SpcPeImageData {flags '', file <<>>} as signtool
    spc = seq(seq(oid(OID_SPC_PE_IMAGE), pe_image_data), seq(alg, octets(digest)))
    spc_contents = spc[2:] if spc[1] < 0x80 else spc[2 + (spc[1] & 0x7F):]
    attrs = [
        seq(oid(OID_CONTENT_TYPE), set_of(oid(OID_SPC_INDIRECT))),
        seq(oid(OID_MESSAGE_DIGEST), set_of(octets(hashlib.sha256(spc_contents).digest()))),
        seq(oid(OID_SPC_OPUS), set_of(seq())),
        seq(oid(OID_SPC_STATEMENT), set_of(seq(oid(OID_SPC_INDIVIDUAL)))),
    ]
    signed_attrs = set_of(*attrs)                       # what is signed: the SET OF form (tag 0x31)
    sig = key.sign(signed_attrs, padding.PKCS1v15(), hashes.SHA256())
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    issuer = cert.issuer.public_bytes()
    signer = seq(integer(1), seq(issuer, integer(cert.serial_number)), alg,
                 b'\xa0' + signed_attrs[1:],            # [0] IMPLICIT SET OF
                 seq(oid(OID_RSA), NULL), octets(sig))
    signed_data = seq(integer(1), set_of(alg), seq(oid(OID_SPC_INDIRECT), ctx(0, spc)),
                      ctx(0, cert_der), set_of(signer))
    return seq(oid(OID_SIGNED_DATA), ctx(0, signed_data)), spc_contents, signed_attrs, sig


def sign(path_in, path_out, key_dir):
    data = bytearray(open(path_in, 'rb').read())
    checksum_off, sec_dir_off = pe_layout(data)
    if struct.unpack_from('<II', data, sec_dir_off)[1]:
        raise SystemExit('already signed')
    data += b'\0' * (-len(data) % 8)
    key, cert = load_or_make_cert(key_dir)
    digest = authenticode_hash(bytes(data))
    pkcs7, spc_contents, signed_attrs, sig = build_signature(digest, key, cert)
    blob = pkcs7 + b'\0' * (-(len(pkcs7) + 8) % 8)
    wc = struct.pack('<IHH', 8 + len(blob), 0x0200, 0x0002) + blob
    cert_off = len(data)
    struct.pack_into('<II', data, sec_dir_off, cert_off, len(wc))
    data += wc
    struct.pack_into('<I', data, checksum_off, 0)
    struct.pack_into('<I', data, checksum_off, pe_checksum(bytes(data), checksum_off))
    open(path_out, 'wb').write(data)
    return verify(path_out, key_dir)


def verify(path, key_dir):
    data = open(path, 'rb').read()
    checksum_off, sec_dir_off = pe_layout(data)
    cert_off, cert_len = struct.unpack_from('<II', data, sec_dir_off)
    assert cert_len and cert_off % 8 == 0 and cert_off + cert_len == len(data), 'certificate table placement'
    ln, rev, typ = struct.unpack_from('<IHH', data, cert_off)
    assert ln == cert_len and rev == 0x0200 and typ == 2, 'WIN_CERTIFICATE header'
    assert struct.unpack_from('<I', data, checksum_off)[0] == pe_checksum(data, checksum_off), 'PE checksum'
    key, cert = load_or_make_cert(key_dir)
    _, spc_contents, signed_attrs, sig = build_signature(authenticode_hash(data), key, cert)
    pkcs7 = data[cert_off + 8:]
    assert spc_contents in pkcs7, 'the image hash in the signature is not this file\'s'
    assert sig in pkcs7, 'signature'
    cert.public_key().verify(sig, signed_attrs, padding.PKCS1v15(), hashes.SHA256())
    from cryptography.hazmat.primitives.serialization import pkcs7 as p7
    certs = p7.load_der_pkcs7_certificates(pkcs7.rstrip(b'\0'))
    assert certs and certs[0] == cert, 'embedded certificate'
    print(f'sign-pe: {path}: signed, Authenticode SHA-256 {authenticode_hash(data).hex()[:16]}..., '
          f'cert {cert.fingerprint(hashes.SHA1()).hex()} (test), PE checksum ok')
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    if '--verify-only' in sys.argv:
        sys.exit(verify(sys.argv[1], sys.argv[3]))
    sys.exit(sign(sys.argv[1], sys.argv[2], sys.argv[3]))
