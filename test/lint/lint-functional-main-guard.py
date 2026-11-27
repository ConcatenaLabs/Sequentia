#!/usr/bin/env python3
#
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Every script test_runner.py lists ends by running its test.

A listed script without the `if __name__ == '__main__':` guard that calls its
test's main() only defines a class and exits 0, so the runner counts it as
passed while it tests nothing.
"""

import importlib.util
import os
import re
import sys

GUARD = re.compile(r"^if __name__ == ['\"]__main__['\"]:\s*\n\s+\S+\s*\(\s*\)\s*\.\s*main\s*\(", re.M)


def main():
    functional = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "functional")
    spec = importlib.util.spec_from_file_location("test_runner", os.path.join(functional, "test_runner.py"))
    runner = importlib.util.module_from_spec(spec)
    sys.path.insert(0, functional)
    spec.loader.exec_module(runner)
    missing = []
    for name in sorted({s.split()[0] for s in runner.ALL_SCRIPTS}):
        with open(os.path.join(functional, name), encoding="utf8") as f:
            if not GUARD.search(f.read()):
                missing.append(name)
    for name in missing:
        print("test/functional/%s is listed in test_runner.py but does not end with "
              "`if __name__ == '__main__':` calling its test's main(): it runs nothing and passes." % name)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
