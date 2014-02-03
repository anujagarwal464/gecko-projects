# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os

import argparse
from multiprocessing import cpu_count


def abs_path(path):
    return os.path.abspath(path)


def create_parser(allow_mandatory=True):
    if not allow_mandatory:
        prefix = "--"
    else:
        prefix = ""
    parser = argparse.ArgumentParser("web-platform-tests",
                                     description="Runner for web-platform-tests tests.")
    parser.add_argument(prefix + "binary", action="store",
                        type=abs_path,
                        help="Binary to run tests against")
    parser.add_argument(prefix + "tests_root", action="store", type=abs_path,
                        help="Path to web-platform-tests"),
    parser.add_argument("--metadata-root", dest="metadata_root",
                        action="store", type=abs_path,
                        help="Path to the folder containing test metadata",
                        default=abs_path(
                            os.path.join(os.path.split(__file__)[0], "..", "metadata"))),
    parser.add_argument("--test-types", action="store",
                        nargs="*", default=["testharness", "reftest"],
                    choices=["testharness", "reftest"],
                        help="Test types to run")
    parser.add_argument("--processes", action="store", type=int, default=int(1.5 * cpu_count()),
                        help="Number of simultaneous processes to use")
    parser.add_argument("--xvfb", action="store_true",
                        help="Run processes that require the display under xvfb")
    parser.add_argument("--include", action="append", help="URL prefix to include")
    parser.add_argument("--log-stdout", action="store_true", help="Enable logging to stdout")
    parser.add_argument("-o", dest="output_file", action="store", help="File to write log to")
    if allow_mandatory:
        parser.add_argument("--product", action="store", choices=["firefox", "servo"],
                            default="firefox")
    return parser


def create_parser_update():
    parser = argparse.ArgumentParser("web-platform-tests-update",
                                     description="Update script for web-platform-tests tests.")

    parser.add_argument("--run", action="store", choices=["try", "local", "logfile"],
                        help="Place to run tests and update expected data")
    #Should make this required iff run=local
    parser.add_argument("--binary", action="store", type=abs_path,
                        help="Binary to run tests against")
    #Should make this required iff run=logfile
    parser.add_argument("--run-log", action="store", type=abs_path,
                        help="Log file from run of tests")
    return parser

def parse_args():
    parser = create_parser()
    rv = parser.parse_args()
    return rv
