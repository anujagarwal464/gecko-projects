# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import unicode_literals

import sys
import time

import logging
from mach import logging as mach_logging

import structuredlog

format_seconds = mach_logging.format_seconds

def get_test_id(data):
    test_id = data["test"]
    if isinstance(test_id, list):
        test_id = tuple(test_id)
    return test_id

class LogEntryFormatter(object):
    def generic_formatter(self, item):
        return "%s: %s" % (item["action"].upper(), item["thread"])

    def suite_start(self, item):
        data = "%i" % len(item["tests"])
        return "%s %s" % (self.generic_formatter(item),
                          data)

    def suite_end(self, item):
        return self.generic_formatter(item)

    def test_start(self, item):
        data = "%s" % (get_test_id(item),)
        return "%s %s" % (self.generic_formatter(item),
                          data)

    def test_end(self, item):
        if "expected" in item:
            expected_str = ", expected %s" % item["expected"]
        else:
            expected_str = ""

        data = "Harness status %s%s. Subtests passed %i/%i. Unexpected %i" % (
            item["status"], expected_str, item["_subtests"]["pass"],
            item["_subtests"]["count"], item["_subtests"]["unexpected"])

        return "%s %s" % (self.generic_formatter(item),
                          data)

    def process_output(self, item):
        data = '"%s" (pid:%s command:%s)' % (item["data"],
                                           item["process"],
                                           item["command"])
        return "%s %s" % (self.generic_formatter(item),
                          data)

    def log(self, item):
        data = "%s %s" % (item["level"], item["message"])
        return "%s %s" % (self.generic_formatter(item),
                          data)

    def __call__(self, item):
        action = item["action"].lower()
        if hasattr(self, action):
            return getattr(self, action)(item)

class SubtestBufferedStreamHandler(structuredlog.StreamHandler):
    def __init__(self,  stream=sys.stderr, formatter=None):
        self.buffer = {}
        structuredlog.StreamHandler.__init__(self, stream, formatter)

    def __call__(self, data):
        if data["action"] == "test_status":
            test = get_test_id(data)
            if test not in self.buffer:
                self.buffer[test] = {"count":0, "unexpected":0, "pass":0}
            self.buffer[test]["count"] += 1
            if "expected" in data:
                self.buffer[test]["unexpected"] += 1
            if data["status"] == "PASS":
                self.buffer[test]["pass"] += 1

        else:
            if data["action"] == "test_end":
                test = get_test_id(data)
                data = data.copy()
                data["_subtests"] = self.buffer.get(test, {"count":0, "unexpected":0, "pass":0})

            structuredlog.StreamHandler.__call__(self, data)

class StructuredHumanFormatter(object):
    def __init__(self, start_time, write_interval, write_times):
        self.start_time = start_time
        self.write_interval = write_interval
        self.write_times = write_times
        self.last_time = None
        self.entry_formatter = LogEntryFormatter()

    def __call__(self, entry, write_times=None):
        if write_times is None:
            write_times = self.write_times

        s = self.entry_formatter(entry)
        if s:
            if write_times:
                return "%s %s" % (format_seconds(self._time(entry)), s)
            else:
                return s

    def _time(self, entry):
        entry_time = (entry["time"] / 1000)
        t = entry_time - self.start_time

        if self.write_interval and self.last_time is not None:
            t = entry_time - self.last_time

        self.last_time = entry_time

        return t

class StructuredTerminalFormatter(StructuredHumanFormatter):
    """Log formatter for structured messages writing to a terminal."""
    def set_terminal(self, terminal):
        self.terminal = terminal

    def __call__(self, entry):
        s = StructuredHumanFormatter.__call__(self, entry, False)

        if not s:
            return

        t = self.terminal.blue(format_seconds(self._time(entry)))

        return '%s %s' % (t, self._colorize(entry, s))

    def _colorize(self, entry, s):
        if not self.terminal:
            return s

        result = s

        len_action = len(entry["action"])
        color = None

        if entry["action"] == "test_end":
            if "expected" not in entry and entry["_subtests"]["unexpected"] == 0:
                color = self.terminal.green
            else:
                color = self.terminal.red
        elif entry["action"] in ("suite_start", "suite_end", "test_start"):
            color = self.terminal.yellow

        if color is not None:
            result = color(result[:len_action]) + result[len_action:]

        return result

class StructuredLoggingManager(mach_logging.LoggingManager):
    def __init__(self):
        self.start_time = time.time()

        self.logging_wrapper = structuredlog.get_adapter_cls()

        root_logger = logging.getLogger()
        if not hasattr(root_logger, "add_handler"):
            root_logger = self.logging_wrapper(root_logger)

        self.structured_loggers = [root_logger]

        self._terminal = None

    def add_json_handler(self, fh):
        handler = structuredlog.StreamHandler(stream=fh)

        for logger in self.structured_loggers:
            logger.add_handler(handler)

    def add_terminal_logging(self, fh=sys.stdout, level=logging.INFO,
                             write_interval=False, write_times=True):

        formatter = StructuredHumanFormatter(self.start_time,
                                             write_interval=write_interval,
                                             write_times=write_times)

        if self.terminal:
            formatter = StructuredTerminalFormatter(self.start_time,
                                                    write_interval=write_interval,
                                                    write_times=write_times)
            formatter.set_terminal(self.terminal)

        handler = SubtestBufferedStreamHandler(stream=fh)
        handler.formatter = formatter
        handler = structuredlog.LogLevelFilter(handler, logging.getLevelName(level))

        for logger in self.structured_loggers:
            logger.add_handler(handler)

        self.terminal_handler = handler
        self.terminal_formatter = formatter

    def register_structured_logger(self, logger):
        """Register a structured logger.

        This needs to be called for all structured loggers that don't chain up
        to the mach logger in order for their output to be captured.
        """
        if not hasattr(logger, "add_handler"):
            logger = self.logging_wrapper(logger)
        self.structured_loggers.append(logger)
