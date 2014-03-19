# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

config = {
    "suite_definitions": {
        "mochitest": {
            "run_filename": "runtestsremote.py",
            "options": ["--autorun", "--close-when-done", "--dm_trans=sut",
                "--console-level=INFO", "--app=%(app)s", "--remote-webserver=%(remote_webserver)s",
                "--xre-path=%(xre_path)s", "--utility-path=%(utility_path)s",
                "--deviceIP=%(device_ip)s", "--devicePort=%(device_port)s",
                "--http-port=%(http_port)s", "--ssl-port=%(ssl_port)s",
                "--certificate-path=%(certs_path)s", "--symbols-path=%(symbols_path)s"
            ],
        },
        "reftest": {
            "run_filename": "remotereftest.py",
            "options": [ "--app=%(app)s", "--ignore-window-size",
                "--enable-privilege",
                "--remote-webserver=%(remote_webserver)s", "--xre-path=%(xre_path)s",
                "--utility-path=%(utility_path)s", "--deviceIP=%(device_ip)s",
                "--devicePort=%(device_port)s", "--http-port=%(http_port)s",
                "--ssl-port=%(ssl_port)s", "--httpd-path", "reftest/components",
                "--symbols-path=%(symbols_path)s",
            ],
        },
        "xpcshell": {
            "run_filename": "remotexpcshelltests.py",
            "options": ["--deviceIP=%(device_ip)s", "--devicePort=%(device_port)s",
                "--xre-path=%(xre_path)s", "--testing-modules-dir=%(modules_dir)s",
                "--apk=%(installer_path)s", "--no-logfiles",
                "--symbols-path=%(symbols_path)s",
            ],
        },
    }, # end suite_definitions
    "test_suite_definitions": {
        "jsreftest": {
            "category": "reftest",
            "extra_args": ["../jsreftest/tests/jstests.list",
                "--extra-profile-file=jsreftest/tests/user.js"]
        },
        "mochitest-1": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "1", "--run-only-tests", "android23.json"],
        },
        "mochitest-2": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "2", "--run-only-tests", "android23.json"],
        },
        "mochitest-3": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "3", "--run-only-tests", "android23.json"],
        },
        "mochitest-4": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "4", "--run-only-tests", "android23.json"],
        },
        "mochitest-5": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "5", "--run-only-tests", "android23.json"],
        },
        "mochitest-6": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "6", "--run-only-tests", "android23.json"],
        },
        "mochitest-7": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "7", "--run-only-tests", "android23.json"],
        },
        "mochitest-8": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "8", "--run-only-tests", "android23.json"],
        },
        "mochitest-9": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "9", "--run-only-tests", "android23.json"],
        },
        "mochitest-10": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "10", "--this-chunk", "10", "--run-only-tests", "android23.json"],
        },
        "mochitest-gl": {
            "category": "mochitest",
            "extra_args": ["--test-path", "content/canvas/test/webgl"],
        },
        "reftest-1": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "3", "--this-chunk", "1",
                "tests/layout/reftests/reftest.list"]
        },
        "reftest-2": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "3", "--this-chunk", "2",
                "tests/layout/reftests/reftest.list"]
        },
        "reftest-3": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "3", "--this-chunk", "3",
                "tests/layout/reftests/reftest.list"]
        },
        "crashtest-1": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "1",
                "tests/testing/crashtest/crashtests.list"]
        },
        "crashtest-2": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "2",
                "tests/testing/crashtest/crashtests.list"]
        },
        "crashtest-3": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "3",
                "tests/testing/crashtest/crashtests.list"]
        },
        "crashtest-4": {
            "category": "reftest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "4",
                "tests/testing/crashtest/crashtests.list"]
        },
        "xpcshell-1": {
            "category": "xpcshell",
            "extra_args": ["--total-chunks", "3", "--this-chunk", "1",
                "--manifest", "tests/xpcshell_android.ini"]
        },
        "xpcshell-2": {
            "category": "xpcshell",
            "extra_args": ["--total-chunks", "3", "--this-chunk", "2",
                "--manifest", "tests/xpcshell_android.ini"]
        },
        "xpcshell-3": {
            "category": "xpcshell",
            "extra_args": ["--total-chunks", "3", "--this-chunk", "3",
                "--manifest", "tests/xpcshell_android.ini"]
        },
        "robocop-1": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "1", "--robocop-path=../..",
                "--robocop-ids=fennec_ids.txt", "--robocop=robocop.ini"],
        },
        "robocop-2": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "2", "--robocop-path=../..",
                "--robocop-ids=fennec_ids.txt", "--robocop=robocop.ini"],
        },
        "robocop-3": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "3", "--robocop-path=../..",
                "--robocop-ids=fennec_ids.txt", "--robocop=robocop.ini"],
        },
        "robocop-4": {
            "category": "mochitest",
            "extra_args": ["--total-chunks", "4", "--this-chunk", "4", "--robocop-path=../..",
                "--robocop-ids=fennec_ids.txt", "--robocop=robocop.ini"],
        },
    }, # end test_definitions
}
