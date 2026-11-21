#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Which cases of a lost-block suite run, and proof that they did.

A suite names its cases `case_<name>`. The names to run come from `--cases=a,b`,
else from the LOST_CASES environment variable, else from the suite's default
list. A name the suite does not define stops the run before any node starts, so
a mistyped case cannot pass having run nothing. The log names each case as it
starts and ends, and a last line lists the cases that ran.
"""

import inspect
import os
import sys


def selected(default):
    """The case names asked for. Read at import, before the framework parses."""
    picked = None
    argv = sys.argv[1:]
    for i, arg in enumerate(argv):
        if arg.startswith("--cases="):
            picked = arg.split("=", 1)[1]
        elif arg == "--cases" and i + 1 < len(argv):
            picked = argv[i + 1]
    if picked is None:
        picked = os.environ.get("LOST_CASES", default)
    return [c for c in picked.split(",") if c]


def runnable(cls, variants=()):
    """The names this suite itself defines. A method that takes required
    arguments is a helper of another case; `variants` are the names that run it
    with its arguments filled in."""
    names = set(variants)
    for name, fn in vars(cls).items():
        if not name.startswith("case_") or not callable(fn):
            continue
        params = list(inspect.signature(fn).parameters.values())[1:]
        if all(p.default is not inspect.Parameter.empty for p in params):
            names.add(name[len("case_"):])
    return names


def check_cases(cls, cases, variants=(), switches=(), needs=None):
    """Stop before any node starts on a name the suite cannot run.

    `switches` are names that change what another case does and run nothing by
    themselves. `needs` maps a case to the environment variable without which it
    would pass having tested nothing.
    """
    known = runnable(cls, variants)
    unknown = [c for c in cases if c not in known and c not in switches]
    if unknown:
        sys.exit("unknown case(s) %s; this suite defines: %s" % (",".join(unknown), ",".join(sorted(known))))
    if not [c for c in cases if c not in switches]:
        sys.exit("no case to run; this suite defines: %s" % ",".join(sorted(known)))
    for case, var in (needs or {}).items():
        if case in cases and not os.environ.get(var):
            sys.exit("case %s needs %s in the environment" % (case, var))


def each_case(test, cases, switches=()):
    """Iterate over the cases to run, logging each one."""
    ran = []
    for case in cases:
        if case in switches:
            continue
        test.log.info("case %s: start", case)
        yield case
        test.log.info("case %s: done", case)
        ran.append(case)
    test.log.info("cases run: %s", ",".join(ran))
