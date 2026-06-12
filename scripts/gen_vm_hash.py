#!/usr/bin/env python3
import sys, hashlib
if len(sys.argv)<2: print("Usage: gen_vm_hash.py <image>"); sys.exit(1)
data=open(sys.argv[1],'rb').read()
h=hashlib.sha256(data).hexdigest()
print(f"Image  : {sys.argv[1]}")
print(f"SHA-256: {h}")
print(f"YAML   : hash: \"{h}\"")
