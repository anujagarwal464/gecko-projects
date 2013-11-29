# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

DEFAULT_TIMEOUT = 10 #seconds
LONG_TIMEOUT = 60 #seconds

import structuredlog

import mozinfo

logger = structuredlog.getOutputLogger("WPT")

#These are quite similar to moztest, but slightly different
class Result(object):
    def __init__(self, status, message, expected=None):
        if status not in self.statuses:
            raise ValueError("Unrecognised status %s" % status)
        self.status = status
        self.message = message
        self.expected = expected


class SubtestResult(object):
    def __init__(self, name, status, message, expected=None):
        self.name = name
        if status not in self.statuses:
            raise ValueError("Unrecognised status %s" % status)
        self.status = status
        self.message = message
        self.expected = expected


class TestharnessResult(Result):
    default_expected = "OK"
    statuses = set(["OK", "ERROR", "TIMEOUT", "EXTERNAL-TIMEOUT", "CRASH"])


class ReftestResult(Result):
    default_expected = "PASS"
    statuses = set(["PASS", "FAIL", "TIMEOUT", "EXTERNAL-TIMEOUT", "CRASH"])


class TestharnessSubtestResult(SubtestResult):
    default_expected = "PASS"
    statuses = set(["PASS", "FAIL", "TIMEOUT", "NOTRUN"])

class RunInfo(object):
    def __init__(self, debug):
        self.platform = mozinfo.info
        self.debug = debug

class Test(object):
    result_cls = None
    subtest_result_cls = None

    def __init__(self, url, expected, timeout=None, path=None):
        self.url = url
        self.expected = expected
        self.timeout = timeout
        self.path = path

    @property
    def id(self):
        return self.url

    def disabled(self, run_info, subtest=None):
        if subtest is None:
            subtest = "FILE"

        return self.expected.get(subtest=subtest, key="disabled") is not None

    def expected_status(self, run_info, subtest=None):
        if subtest is None:
            default = self.result_cls.default_expected
        else:
            default = self.subtest_result_cls.default_expected
        return self.expected.get(subtest=subtest, key="status", default=default).upper()


class TestharnessTest(Test):
    result_cls = TestharnessResult
    subtest_result_cls = TestharnessSubtestResult

    @property
    def id(self):
        return self.url

class ManualTest(Test):
    @property
    def id(self):
        return self.url


class ReftestTest(Test):
    result_cls = ReftestResult

    def __init__(self, url, ref_url, ref_type, expected, timeout=None, path=None):
        self.url = url
        self.ref_url = ref_url
        if ref_type not in ("==", "!="):
            raise ValueError
        self.ref_type = ref_type
        self.expected = expected
        self.timeout = timeout
        self.path = path

    @property
    def id(self):
        return self.url, self.ref_type, self.ref_url

def from_manifest(manifest_test, test_metadata):
    test_cls = {"reftest":ReftestTest,
                "testharness":TestharnessTest,
                "manual":ManualTest}[manifest_test.item_type]

    timeout = LONG_TIMEOUT if manifest_test.timeout == "long" else DEFAULT_TIMEOUT

    if test_cls == ReftestTest:
        return test_cls(manifest_test.url,
                        manifest_test.ref_url,
                        manifest_test.ref_type,
                        test_metadata.get_expected(manifest_test),
                        timeout=timeout,
                        path=manifest_test.path)
    else:
        return test_cls(manifest_test.url,
                        test_metadata.get_expected(manifest_test),
                        timeout=timeout,
                        path=manifest_test.path)
