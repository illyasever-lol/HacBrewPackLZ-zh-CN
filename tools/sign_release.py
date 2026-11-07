"""Signs a release, so the built-in updaters know it's the real thing whatever the network or the download host does.

    python tools\\sign_release.py <private key.pem> <version> <folder to write to> <file>...

Writes hacbrewpacklz-update.txt, which names the version and has the SHA-256 of each file, and hacbrewpacklz-update.sig, which
is an RSA-2048-PSS signature of it with SHA-256. Upload both with the release. The private key stays out of the repository, and the
matching public key is built in: switch\\core\\update_key.inc and src\\Updater.cs.

    python tools\\sign_release.py --new-key <private key.pem>

makes the key, and prints the public key's modulus for those two files.
"""
import hashlib
import os
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa


def new_key(path):
    if os.path.exists(path):
        sys.exit(f"{path} is already there, and replacing it would stop updates from reaching anyone who has this version")
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    with open(path, 'wb') as out:
        out.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    modulus = key.public_key().public_numbers().n.to_bytes(256, 'big')
    print(modulus.hex())


def sign(key_path, version, folder, files):
    with open(key_path, 'rb') as source:
        key = serialization.load_pem_private_key(source.read(), None)
    lines = [f"version {version}"]
    for path in files:
        with open(path, 'rb') as source:
            digest = hashlib.sha256(source.read()).hexdigest()
        lines.append(f"{digest}  {os.path.basename(path)}")
    text = ("\n".join(lines) + "\n").encode()
    signature = key.sign(text, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
    with open(os.path.join(folder, 'hacbrewpacklz-update.txt'), 'wb') as out:
        out.write(text)
    with open(os.path.join(folder, 'hacbrewpacklz-update.sig'), 'wb') as out:
        out.write(signature)
    print(text.decode())


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--new-key':
        new_key(sys.argv[2])
    elif len(sys.argv) >= 5:
        sign(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        sys.exit(__doc__)
