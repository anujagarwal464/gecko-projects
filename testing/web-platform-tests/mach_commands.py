# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Integrates the xpcshell test runner with mach.

from __future__ import unicode_literals, print_function

import mozpack.path
import os
import shutil
import sys

from StringIO import StringIO

from mozbuild.base import (
    MachCommandBase,
    MozbuildObject,
)

from mach.decorators import (
    CommandArgument,
    CommandProvider,
    Command,
)

from wpttests import wptcommandline

# This should probably be consolidated with similar classes in other test
# runners.
class InvalidTestPathError(Exception):
    """Exception raised when the test path is not valid."""

class WebPlatformTestsRunner(MozbuildObject):
    """Run web platform tests."""

    def __init__(self, topsrcdir, settings, log_manager, topobjdir=None):
        from wpttests import machlogging, structuredlog
        log_manager = machlogging.StructuredLoggingManager()
        self._logger = structuredlog.getOutputLogger("WPT.mach")
        MozbuildObject.__init__(self, log_manager, settings, log_manager, topobjdir)

    def run_tests(self, **kwargs):
        # TODO Bug 794506 remove once mach integrates with virtualenv.
        build_path = os.path.join(self.topobjdir, 'build')
        if build_path not in sys.path:
            sys.path.append(build_path)

        from wpttests import wptrunner

        if kwargs["binary"] is None:
            kwargs["binary"] = os.path.join(self.bindir, 'firefox')

        if kwargs["tests_root"] is None:
            kwargs["tests_root"] = os.path.join(self.topobjdir, '_tests', 'web-platform-tests', "tests")

        if kwargs["metadata_root"] is None:
            kwargs["metadata_root"] = os.path.join(self.topobjdir, '_tests', 'web-platform-tests', "metadata")

        output_file = kwargs.pop("output_file", None)
        if output_file is not None:
            kwargs["output_file"] = open(output_file, "w")


        kwargs["capture_stdio"] = False

        self.log_manager.register_structured_logger(wptrunner.logger)
        self.log_manager.add_terminal_logging()
        result = wptrunner.run_tests(**kwargs)

        return int(not result)

class WebPlatformTestsUpdater(MozbuildObject):
    """Update web platform tests."""
    def __init__(self, topsrcdir, settings, log_manager, topobjdir=None):
        from wpttests import machlogging, structuredlog
        log_manager = machlogging.StructuredLoggingManager()
        self._logger = structuredlog.getOutputLogger("WPT.update.mach")
        MozbuildObject.__init__(self, log_manager, settings, log_manager, topobjdir)

    def run_update(self, **kwargs):
        from wpttests import update

        if kwargs["binary"] is None:
            kwargs["binary"] = os.path.join(self.bindir, 'firefox')

        print(kwargs)

        update.main(**kwargs)

@CommandProvider
class MachCommands(MachCommandBase):
    @Command("web-platform-tests",
             category="testing",
             parser=wptcommandline.create_parser(False))
    def run_web_platform_tests(self, **params):
        self.setup()
        wpt_runner = self._spawn(WebPlatformTestsRunner)
        return wpt_runner.run_tests(**params)

    @Command("web-platform-tests-update",
             category="testing",
             parser=wptcommandline.create_parser_update())
    def update_web_platform_tests(self, **params):
        self.setup()
        self.virtualenv_manager.install_pip_package('html5lib==0.99')
        wpt_updater = self._spawn(WebPlatformTestsUpdater)
        return wpt_updater.run_update(**params)

    def setup(self):
        self._activate_virtualenv()
        self.virtualenv_manager.install_pip_package('py==1.4.14')
