# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import ConfigParser
import cStringIO
import os
import shutil
import subprocess
import sys
import tempfile
import traceback
import uuid
from multiprocessing import cpu_count

import vcs
from vcs import git, hg
manifest = None
import metadata
import wptrunner

base_path = os.path.abspath(
    os.path.join(os.path.split(__file__)[0], ".."))

def do_test_relative_imports(test_root):
    global manifest

    sys.path.insert(0, os.path.join(test_root))
    sys.path.insert(0, os.path.join(test_root, "tools", "scripts"))
    print sys.path
    import manifest

class RepositoryError(Exception):
    pass

class WebPlatformTests(object):
    def __init__(self, remote_url, repo_path, remote_branch="master"):
        self.remote_url = remote_url
        self.repo_path = repo_path
        self.remote_branch = remote_branch
        self.local_branch = uuid.uuid4().hex

    def update(self):
        if not os.path.exists(self.repo_path):
            os.makedirs(self.repo_path)
        if not vcs.is_git_root(self.repo_path):
            git("clone", self.remote_url, ".", repo=self.repo_path)
            git("checkout", "-b", self.local_branch, self.remote_branch, repo=self.repo_path)
            assert vcs.is_git_root(self.repo_path)
        else:
            if git("status", "--porcelain", repo=self.repo_path):
                raise RepositoryError("Repository in %s not clean" % self.repo_path)

            git("fetch",
                self.remote_url,
                "%s:%s" % (self.remote_branch,
                           self.local_branch),
                repo=self.repo_path)
            git("checkout", self.local_branch, repo=self.repo_path)
        git("submodule", "init", repo=self.repo_path)
        git("submodule", "update", "--recursive", repo=self.repo_path)

    @property
    def rev(self):
        if vcs.is_git_root(self.repo_path):
            return git("rev-parse", "HEAD", repo=self.repo_path).strip()
        else:
            return None

    def clean(self):
        git("checkout", self.rev, repo=self.repo_path)
        git("branch", "-D", self.local_branch, repo=self.repo_path)

    def _tree_paths(self):
        repo_paths = [self.repo_path] + [os.path.join(self.repo_path, path) for path in self._submodules()]
        ls_tree = ("ls-tree", "-r", "--name-only", "HEAD")

        rv = []

        for repo_path in repo_paths:
            paths = git("ls-tree", "-r", "--name-only", "HEAD", repo=repo_path).split("\n")
            rel_path = os.path.relpath(repo_path, self.repo_path)
            rv.extend([os.path.join(rel_path, item.strip()) for item in paths if item.strip()])

        return rv

    def _submodules(self):
        output = git("submodule", "status", "--recursive", repo=self.repo_path)
        rv = []
        for line in output.split("\n"):
            line = line.strip()
            if not line:
                continue
            parts = line.split(" ")
            rv.append(parts[1])
        return rv

    def copy_work_tree(self, dest):
        if os.path.exists(dest):
            assert os.path.isdir(dest)

        for sub_path in os.listdir(dest):
            path = os.path.join(dest, sub_path)
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)

        for tree_path in self._tree_paths():
            source_path = os.path.join(self.repo_path, tree_path)
            dest_path = os.path.join(dest, tree_path)

            dest_dir = os.path.split(dest_path)[0]
            if not os.path.isdir(source_path):
                if not os.path.exists(dest_dir):
                    os.makedirs(dest_dir)
                shutil.copy2(source_path, dest_path)

        for source, destination in [("gecko_runner.html", ""),
                                    ("testharnessreport.js", "resources/")]:
            source_path = os.path.join(base_path, source)
            dest_path = os.path.join(base_path, dest, destination, os.path.split(source)[1])
            shutil.copy2(source_path, dest_path)

class MozillaTree(object):
    def __init__(self, root=None):
        if root is None:
            root = hg("root").strip()
        self.root = root
        self.hg = vcs.bind_to_repo(hg, self.root)

    def is_clean(self):
        return self.hg("status").strip() == ""

    def add_new(self, prefix=None):
        if prefix is not None:
            args = ("-I", prefix)
        else:
            args = (",")
        self.hg("add", *args)

    def create_patch(self, patch_name, message):
        try:
            pass
            #self.hg("qinit")
        except subprocess.CalledProcessError:
            # There is already a patch queue in this repo
            # Should only happen during development
            pass
        self.hg("qnew", patch_name, "-X", self.root, "-m", message)

    def refresh_patch(self, include=None):
        if include is not None:
            args = []
            for item in include:
                args.extend(["-I", item])
        else:
            args = ()

        self.hg("qrefresh", *args)

    def commit_patch(self):
        self.hg("qfinish", repo=self.repo_root)

    def export_patch(self):
        return hg("qexport", "qtip", repo=self.repo_root)

class Try(object):
    def __init__(self, url):
        self.url = url

    def options_message(self):
        #Just hardcode what's actually needed here for now
        #XXX - probably want more platforms
        return "try: -b o -p linux -u web-platform-tests -t none"

    def push(self, mozilla_tree):
        sys.exit(1)
        patch_name = "try_" + str(uuid.uuid4())
        hg("qnew", "-m", self.options_message(), patch_name, repo=mozilla_tree.root)
        hg("push", "-f", "-r tip", try_server.url, repo=mozilla_tree.root)
        hg("qpop", repo=mozilla_tree.root)
        hg("qremove", patch_name, repo=mozilla_tree.root)

class TryRunner(object):
    def __init__(self, config, bug):
        self.bug = bug
        self.server = Try(config["try"]["url"])

    def run(self, mozilla_tree, log_file):
        self.bug.comment("Pushing hg revision %s to try" % mozilla_tree.revision)
        self.server.push(mozilla_tree)

        sys.exit(0)

        pulse = Pulse()

        pulse.wait_on_job()
        #self.bug.comment("Try job complete")

        logs = moz_try.get_logs()

        log_file.write(logs.read())


class LocalRunner(object):
    def __init__(self, config, bug):
        self.config = config
        self.bug = bug

    def do_run(self, mozilla_tree, log_file):
        wptrunner.run_tests(binary=self.config["command-args"].get_path("binary"),
                            tests_root=self.config["mozilla-central"].get_path("test_path"),
                            metadata_root=self.config["mozilla-central"].get_path("metadata_path"),
                            test_types=["testharness", "reftest"],
                            output_file=log_file,
                            processes=int(cpu_count() * 1.5),
                            log_stdout=True)
        log_file.seek(0)
        with open("wpt-update.log", "w") as f:
            f.write(log_file.read())

class ExistingLogRunner(object):
    def __init__(self, config, bug):
        self.config = config
        self.bug = bug

    def do_run(self, mozilla_tree, log_file):
        with open(self.config["command-args"]["run_log"]) as f:
            log_file.write(f.read())

class ConfigDict(dict):
    def get_path(self, key):
        return os.path.join(base_path, os.path.expanduser(self[key]))


def read_config(command_args):
    parser = ConfigParser.SafeConfigParser()
    config_path = os.path.join(base_path, "config.ini")
    success = parser.read(config_path)
    assert config_path in success, success
    rv = {}
    for section in parser.sections():
        rv[section] = ConfigDict(parser.items(section))
    rv["command-args"] = ConfigDict(command_args.items())
    return rv


def main(**kwargs):
    config = read_config(kwargs)

    print config

    sync_path = config["web-platform-tests"].get_path("sync_path")
    test_path = config["mozilla-central"].get_path("test_path")
    metadata_path = config["mozilla-central"].get_path("metadata_path")

    mozilla_tree = MozillaTree()
    if not mozilla_tree.is_clean():
        sys.stderr.write("Working tree is not clean\n")
        sys.exit(1)

    #bug = bz.create_bug("Doing update of web-platform tests")
    bug = None

    wpt = WebPlatformTests(config["web-platform-tests"]["remote_url"],
                           sync_path,
                           remote_branch=config["web-platform-tests"].get("branch", "master"))

    wpt.update()
    try:
        #bug.comment("Updating to %s" % wpt.rev)
        initial_metadata = metadata.TestsMetadata(sync_path, metadata_path)
        initial_rev = initial_metadata.rev
        wpt.copy_work_tree(test_path)
        initial_metadata.update_manifest()

        mozilla_tree.create_patch("web-platform-tests_update_%s"  % wpt.rev,
                                  "Bug %i - Update web-platform-tests to revision %s" % (
                                      bug.id if bug else 0, wpt.rev
                                  ))
        mozilla_tree.add_new(os.path.relpath(test_path, mozilla_tree.root))
        mozilla_tree.refresh_patch(include=[test_path, metadata_path])

        runner = {"try": TryRunner,
                  "local": LocalRunner,
                  "logfile": ExistingLogRunner}[config["command-args"]["run"]](config, bug)

        with tempfile.TemporaryFile() as log_file:
            print 8
            runner.do_run(mozilla_tree, log_file)
            log_file.seek(0)
            mozilla_tree.create_patch("web-platform-tests_update_%s_metadata"  % wpt.rev,
                                      "Bug %i - Update web-platform-tests expected data to revision %s" % (
                                          bug.id if bug else 0, wpt.rev
                                      ))
            needs_human = metadata.update(sync_path, metadata_path, log_file, rev_old=initial_rev)
            if not mozilla_tree.is_clean():
                mozilla_tree.add_new(os.path.relpath(metadata_path, mozilla_tree.root))
                mozilla_tree.refresh_patch(include=[metadata_path])

        sys.exit(1)

        #Need to be more careful about only operating on patches that we have actually added

        mozilla_tree.commit_patch_queue()

        patch = mozilla_tree.make_patches()
        bug.upload_patch(patch)

        if not needs_human:
            #bug.request_checkin() or mozilla_tree.push()
            pass
        else:
            #bug.comment("Not auto updating because of unexpected changes in the following files: ")
            pass
    except Exception as e:
        #bug.comment("Update failed with error:\n %s" % traceback.format_exc())
        print traceback.format_exc()
        raise
    finally:
        pass#wpt.clean()

if __name__ == "__main__":
    main(run="try")
