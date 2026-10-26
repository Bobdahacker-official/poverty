#!/usr/bin/env python3
"""Convert a PEM CA bundle into BearSSL trust anchors (build-time only)."""
import sys
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import rsa, ec
pem = open(sys.argv[1], 'rb').read()
anchors = []
print('/* CA public keys converted from the supplied PEM trust store. */')
def array(name, data):
    print('static unsigned char '+name+'[] = {'+','.join('0x%02x'%n for n in data)+'};')
def number(n):
    return n.to_bytes((n.bit_length()+7)//8, 'big')
for part in pem.split(b'-----END CERTIFICATE-----'):
    if b'-----BEGIN CERTIFICATE-----' not in part: continue
    cert = x509.load_pem_x509_certificate(part+b'-----END CERTIFICATE-----')
    key = cert.public_key()
    # Only explicitly CA certificates become trust anchors.
    try:
        if not cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca: continue
    except x509.ExtensionNotFound: continue
    i = len(anchors)
    dn = cert.subject.public_bytes()
    if isinstance(key, rsa.RSAPublicKey):
        pk = key.public_numbers()
        array('ta_n%d'%i, number(pk.n)); array('ta_e%d'%i, number(pk.e))
        value = '{BR_KEYTYPE_RSA, {.rsa = {ta_n%d,sizeof(ta_n%d),ta_e%d,sizeof(ta_e%d)}}}'%(i,i,i,i)
    elif isinstance(key, ec.EllipticCurvePublicKey):
        curve = {'secp256r1':23,'secp384r1':24,'secp521r1':25}.get(key.curve.name)
        if not curve: continue
        from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
        array('ta_q%d'%i, key.public_bytes(Encoding.X962, PublicFormat.UncompressedPoint))
        value = '{BR_KEYTYPE_EC, {.ec = {%d,ta_q%d,sizeof(ta_q%d)}}}'%(curve,i,i)
    else: continue
    array('ta_dn%d'%i, dn)
    anchors.append('{{ta_dn%d,sizeof(ta_dn%d)},BR_X509_TA_CA,%s}'%(i,i,value))
print('static const br_x509_trust_anchor trust_anchors[] = {\n'+',\n'.join(anchors)+'\n};')
