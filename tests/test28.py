#!/bin/python3

# bucse-mount --version prints the bucse version and the version of the FUSE
# library it uses (it used to print the bucse version as the FUSE library
# version).

import bucseTests
import re
import subprocess


bucseTests.parseArgs()

for flag in ["--version", "-V"]:
    p = subprocess.run(["../bucse-mount", flag], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, "%s returned %d: %s %s" % (flag, p.returncode, p.stdout, p.stderr)

    bucseVersion = re.search(r"^bucse version (\S+)$", p.stdout, re.MULTILINE)
    fuseVersion = re.search(r"^FUSE library version (\S+)$", p.stdout, re.MULTILINE)
    assert bucseVersion and fuseVersion, p.stdout
    assert fuseVersion.group(1) != bucseVersion.group(1), p.stdout
    # FUSE versions look like 3.17.2
    assert re.fullmatch(r"3\.\d+(\.\d+)?", fuseVersion.group(1)), p.stdout
