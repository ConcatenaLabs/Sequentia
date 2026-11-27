#!/usr/bin/env bash
#
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Every functional test script that test_runner.py lists runs its test.

export LC_ALL=C
python3 "$(dirname "${BASH_SOURCE[0]}")/lint-functional-main-guard.py"
