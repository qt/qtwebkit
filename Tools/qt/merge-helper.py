#!/usr/bin/env python
# Copyright (C) 2020 Sergey Lapin <slapinid@gmail.com>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.

import subprocess
import os
import sys
from ConfigParser import ConfigParser
from exceptions import Exception

config = {}

conflict_message = """
Merge ended up having conflicts.

Use "git status" command to find-out which conflicts require resolution.

Resove them by editing files, adding and removing files, then commit the
result.
"""


def configure():
    global config
    conf = ConfigParser()
    conf_name = os.path.join(os.path.dirname(
        os.path.abspath(sys.argv[0])), "merge.conf")
    print "config: " + conf_name
    conf.read([conf_name])
    config["src_branch"] = conf.get("merge", "src_branch")
    config["dst_branch"] = conf.get("merge", "dst_branch")
    config["tmp_branch"] = conf.get("merge", "tmp_branch")
    config["pre_merge"] = conf.get("scripts", "pre_merge")
    config["post_merge"] = conf.get("scripts", "post_merge")
    config["keep_tmp"] = False
    config["reuse_tmp"] = False
    for p in sys.argv[1:]:
        if p == "--keep-tmp":
            config["keep_tmp"] = True
        elif p == "--reuse-tmp":
            config["reuse_tmp"] = True


def git_new_branch(branch, commit):
    result = subprocess.call(["git", "checkout", "-b", branch, commit])
    return result


def try_merge():
    result = subprocess.call(["git", "merge", config["src_branch"]])
    if result != 0:
        subprocess.call(["git", "merge", "--abort"])
    return result


def try_merge_with_fix():
    result = subprocess.call(["git", "merge", config["src_branch"]])
    if result != 0:
        print conflict_message
        unmerged = git_get_unmerged_files()
        while len(unmerged) > 0:
            print "Found " + str(len(unmerged)) + " unmerged files."
            for f in unmerged:
                print f
            print "Please fix conflicts and commit result."
            os.environ["PS1"] = "git conflicts(^D to exit)\n> "
            os.system("bash  --norc --noprofile -i")
            unmerged = git_get_unmerged_files()
    return result


def pre_merge():
    result = os.system(config["pre_merge"])
    return result


def post_merge():
    result = os.system(config["post_merge"])
    return result


def git_switch_branch(branch):
    result = subprocess.call(["git", "checkout", branch])
    return result


def git_checkout_branch_files(branch):
    result = subprocess.call(["git", "checkout", branch, "--", "*"])
    return result


def git_remove_branch(branch):
    result = subprocess.call(["git", "branch", "-D",  branch])
    return result


def git_write_tree():
    result = subprocess.check_output(["git", "write-tree"])
    return result.strip()


def git_get_unmerged_files():
    result = subprocess.check_output(["git", "ls-files", "-u"])
    data = result.strip()
    ret = []
    if data.find("\n") >= 0:
        for l in data.split("\n"):
            data = l.split()
            if len(data) == 4:
                if not data[3] in ret:
                    ret.append(data[3])

    return ret


def git_commit_tree(branch, tree):
    result = subprocess.check_output(["git",
                                      "commit-tree", "-p", "HEAD", "-p",
                                      branch, "-m", "Merge from %s " % (branch), tree])
    return result.strip()


def git_update_ref(commit):
    result = subprocess.call(["git", "update-ref",
                              "-m",  "commit: Merge from %s" % (config["src_branch"]), "HEAD", commit])
    return result


configure()
print config

if not config["reuse_tmp"]:
    result = git_new_branch(config["tmp_branch"], config["dst_branch"])
    if result != 0:
        raise Exception("Can't create branch " + config["tmp_branch"])
else:
    result = git_switch_branch(config["tmp_branch"])
    if result != 0:
        raise Exception("Can't switch to branch " + config["tmp_branch"])
if not config["reuse_tmp"] or try_merge() != 0:
    result = pre_merge()
    if result != 0:
        raise Exception("pre_merge section failed")

    result = try_merge_with_fix()

    result = post_merge()
    if result != 0:
        raise Exception("post_merge section failed")

print "Checking out destination branch..."
git_switch_branch(config["dst_branch"])
print "Checking out temporary branch files..."
git_checkout_branch_files(config["tmp_branch"])
print "Finishing merge..."
tree = git_write_tree()
commit = git_commit_tree(config["src_branch"], tree)
result = git_update_ref(commit)
if result != 0:
    raise Exception("Could not commit merge")
print "Merge done..."
if not config["keep_tmp"]:
    print "Removing temporary branch"
    git_remove_branch(config["tmp_branch"])
else:
    print "Temporary branch " + config["tmp_branch"] + " was kept"
