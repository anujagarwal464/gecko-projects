# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import sys
import os
import subprocess
from collections import defaultdict
import copy
import itertools

import structuredlog
import wpttest
import expected
from vcs import git
manifest = None # Module that will be imported relative to test_root

logger = structuredlog.getOutputLogger("WPT")

# Inputs
#   - Old SHA1, New SHA1, old expected results, run log (from build that was previously green)

# Outputs:
#   - New expected results

#Possibilities:
# * New test
#   - Use results as expected results

# Existing test
#  * Result is expected
#    - Use existing expected
#  * Test is disabled
#    * File unchanged
#      - Test wasn't run so no change
#    * File changed
#      - Notify so test can be reexamined
#      - Improves if we can also detect changes in helper files
#  * Result is changed
#    * File changed
#      - use new result
#    * File unchanged
#      - manual review

# Test removed
#  - Delete expected result

# Open issues
#
# - Results from multiple platforms/configurations

def do_test_relative_imports(test_root):
    global manifest

    sys.path.insert(0, os.path.join(test_root))
    sys.path.insert(0, os.path.join(test_root, "tools", "scripts"))
    import manifest


def rev_range(rev_old, rev_new, symmetric=False):
    joiner = ".." if not symmetric else ".."
    return "".join([rev_old, joiner, rev_new])


def paths_changed(rev_old, rev_new, repo):
    data = git("diff", "--name-status", rev_range(rev_old, rev_new), repo=repo)
    lines = [tuple(item.strip() for item in line.strip().split("\t", 1))
             for line in data.split("\n") if line.strip()]
    output = set(lines)
    return output


def load_change_data(rev_old, rev_new, repo):
    changes = paths_changed(rev_old, rev_new, repo)
    rv = {}
    status_keys = {"M": "modified",
                   "A": "new",
                   "D": "deleted"}
    #TODO: deal with renames
    for item in changes:
        rv[item[1]] = status_keys[item[0]]
    return rv


def test_id(item):
    if isinstance(item, list):
        test_id = tuple(item)
    else:
        test_id = item
    return test_id


class TestsMetadata(object):
    def __init__(self, test_root, metadata_root):
        self.test_root = test_root
        self.metadata_root = metadata_root
        self.manifest_path = os.path.abspath(os.path.join(self.metadata_root, "MANIFEST.json"))
        self.manifest = self._load_manifest(test_root)

        self.rev = self.manifest.rev

    def _load_manifest(self, test_root):
        if manifest is None:
            do_test_relative_imports(test_root)

        logger.debug("Using manifest at %s" % self.manifest_path)
        test_manifest = manifest.load(self.manifest_path)
        return test_manifest

    def update_manifest(self):
        manifest.setup_git(self.test_root)
        manifest.update(self.manifest)
        manifest.write(self.manifest, self.manifest_path)

    def get_expected(self, test):
        expected_path = os.path.join(self.metadata_root, test.path + ".ini")
        if not os.path.exists(expected_path):
            expected_data = expected.ExpectedData(expected_path)
        else:
            with open(expected_path) as f:
                expected_data = expected.load(f, expected_path)
        return expected_data


class ResultCollection(object):
    def __init__(self):
        self.harness = None
        self.subtests = {}


def load_results(tests, log_data):
    rv = {}
    for item in structuredlog.action_filter(log_data,
                                            set(["test_start", "test_end", "test_status"])):
        if item["action"] == "test_start":
            id = test_id(item["test"])
            rv[id] = ResultCollection()
        elif item["action"] == "test_end":
            try:
                id = test_id(item["test"])
                test = tests[id]
                rv[id].harness = test.result_cls(item["status"],
                                                 item.get("message", None),
                                                 item.get("expected", None))
            except KeyError:
                print rv
        elif item["action"] == "test_status":
            id = test_id(item["test"])
            test = tests[id]
            rv[id].subtests[item["subtest"]] = test.subtest_result_cls(item["subtest"],
                                                                       item["status"],
                                                                       item.get("message", None),
                                                                       item.get("expected", None))
    return rv

def update_expected(run_info, change_data, tests, results):
    tests_needing_review = set()

    for test_id, test in tests.iteritems():
        result = results.get(test_id, None)

        new_expected, needs_review = new_expected_test(test,
                                                       run_info,
                                                       result,
                                                       change_data.get(test.path,
                                                                       "unchanged"))

        expected_path = test.expected.path
        if os.path.exists(expected_path):
            os.unlink(expected_path)

        if new_expected is not None and not new_expected.empty():
            expected_dir = os.path.split(expected_path)[0]
            if not os.path.exists(expected_dir):
                os.makedirs(expected_dir)

            with open(expected_path, "w") as f:
                expected.dump(new_expected, f)

        if needs_review:
            tests_needing_review.add(test)

    return tests_needing_review

def new_expected_test(test, run_info, result, change_status):
    review_needed = False
    if result:
        assert not test.disabled(run_info)
        new_expected, review_needed  = get_new_expected(test,
                                                        run_info,
                                                        result,
                                                        change_status)
    #Need some run_info to pass in here
    elif test.disabled(run_info):
        new_expected = test.expected.copy()
    else:
        logger.error("Missing result for test %s" % (test.id,))
        new_expected = None

    return new_expected, review_needed


def get_new_expected(test, run_info, result, change_status):
    if change_status not in ["new", "modified", "unchanged"]:
        raise ValueError, "Unexpected change status " + change_status

    if change_status == "new":
        assert test.expected.empty()

    new_expected = test.expected.copy()

    review_needed = False

    for subtest_name, subtest_result in itertools.chain([(None, result.harness)],
                                                        result.subtests.iteritems()):
        if subtest_result is None:
            print test.id, subtest_name

        updated_unchanged = set_expected_status(new_expected,
                                                subtest_name,
                                                    change_status,
                                                subtest_result.status,
                                                subtest_result.expected,
                                                subtest_result.default_expected)
        review_needed = review_needed or updated_unchanged


    #Remove tests that weren't run
    missing_tests = set()
    for subtest in new_expected.iter_subtests():
        if subtest not in result.subtests and not test.disabled(run_info, subtest):
            missing_tests.add(subtest)
            if change_status == "unchanged":
                review_needed = True

    for subtest in missing_tests:
        new_expected.remove_subtest(subtest)

    return new_expected, review_needed


def set_expected_status(new_expected, subtest_name, change_status, status,
                        expected, default_expected):
    if expected is not None:
        #Result was unexpected
        if status != default_expected:
            if not new_expected.has_subtest(subtest_name):
                new_expected.add_subtest(subtest_name)
            new_expected.set(subtest_name, "status", status)

        elif new_expected.has_subtest(subtest_name):
            new_expected.clear(subtest_name, "status")

        return change_status == "unchanged"

    return False

def load_tests(tests_metadata):
    rv = {}

    for path, items in tests_metadata.manifest:
        for manifest_item in items:
            if manifest_item.item_type in ("manual", "helper"):
                continue

            test = wpttest.from_manifest(manifest_item, tests_metadata)

            rv[test.id] = test
    return rv

def update(test_root, metadata_root, log, rev_old=None, rev_new="HEAD"):
    """Update the metadata files for web-platform-tests based on
    the results obtained in a previous run"""

    warnings = {}
    statuses = {}

    tests_metadata = TestsMetadata(test_root, metadata_root)

    if rev_old is None:
        rev_old = tests_metadata.rev

    if rev_old is not None:
        rev_old = git("rev-parse", rev_old, repo=test_root).strip()
    rev_new = git("rev-parse", rev_new, repo=test_root).strip()

    if rev_old is not None:
        change_data = load_change_data(rev_old, rev_new, repo=test_root)
    else:
        change_data = {}

    run_info = wpttest.RunInfo(False)

    tests = load_tests(tests_metadata)

    test_results = load_results(tests, structuredlog.read_logs(log))

    tests_needing_review = update_expected(run_info, change_data, tests, test_results)
    return tests_needing_review


if __name__ == "__main__":
    args = sys.argv[1:]
    with open(args[2]) as log:
        args[2] = log
        print update(*args)
