web-platform-tests Harness
==========================

This harness is designed for running the W3C web-platform-tests
testsuite, which is stored on `github`_.

Running the Tests
-----------------

Tests are typically run using `mach`::

  mach web-platform-tests

It is possible to control exactly which tests will be run, where the
output will go and other settings of the test runner. See

::

  mach help web-platform-tests

for more details.

Alternatively the tests may be run using the `runtests.py` file.

Implementation Details
----------------------

Directory Structure
~~~~~~~~~~~~~~~~~~~

The harness and associated data is divided into three parts:

`wpttests/`
  This is the harness directory. It contains the python code for
  actually running the tests.

`tests/`
  These are the imported test files. Everthing in this
  directory will be wiped on update and so it should should be
  considered read-only (for now at least, a streamlined procedure for
  updating tests without submitting to the web-platform-tests
  repository directly is a future goal).

`metadata/`
  This contains the metadata required to run the tests and to
  determine whether the results match expectations or not. The
  `MANIFEST.json` file in this directory is generated when the tests
  are imported and should not be hand-edited. Expected results are
  stored in ini files, described below, and must be updated when
  patches affect the expected test results.

Expectation File Format
~~~~~~~~~~~~~~~~~~~~~~~

Test expectations are stored in ini files that use a
manifestdestiny-like syntax. The default expectation is that each test
runs without errors and all subtests pass. In this case no expectation
file is needed. In the case that the expected result is not a simple
pass, a file with the same relative path as the test, but in the
metadata directory rather than the tests directory, and with `.ini`
appended to the filename must be created. For example a test imported
into `tests/example/sometests/test.html` would have its expected data
in `metadata/example/sometests/test.html.ini`.

The format of an expecation file is::

  [FILE]
  status = ERROR

  [some subtest name]
  status = FAIL

For test types without subtests, the overall status goes in the
`[FILE]` section.

Disabling Tests
~~~~~~~~~~~~~~~

The expectation file may also be used to disable tests or ignore the
results from certain subtests. For example an expectation file like::

  [FILE]
  disabled = bug123456

would cause the entire test to be skipped, whereas

::

  [some subtest name]
  disabled = bug678901

would cause the result from that specific subtest to be ignored.

Updating the Tests
------------------

[Work In Progress]

The tests are updated with the update.py file. The plan is to make
this fully automated, so that this file will pull in the git repo,
update m-c, schedule a try run, and update the expected results based
on the results from try, only requiring human intervention if there is
an unexpected change. So far the update works, but scheduling the try
run is not implemented. To enable manual updates, there should probably
be a mach command like `mach web-platform-tests update`.

_github: https://github.com/w3c/web-platform-tests
