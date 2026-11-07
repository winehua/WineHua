#!/usr/bin/env python3
"""Stress the actual signal handler while the process-registry lock is held."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = os.environ.get('CXX', 'c++')


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which(COMPILER), 'requires Linux C++')
class SigchldTests(unittest.TestCase):
    def test_signal_never_takes_registry_lock_and_worker_preserves_exits(self):
        source = (ROOT / 'entry/src/main/cpp/proc/wine_process.cpp').read_text(encoding='utf-8')
        handler = function(source, 'void sigchld_handler(')
        reaper = function(source, 'static void ReapExitedChildren(')
        code = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <map>
#include <mutex>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
static std::atomic<bool> gChildExitPending{false};
static std::mutex registry;
static std::map<pid_t,int> exits;
void LogProcessExit(const char*,pid_t,int) {}
void HandleProcessDeath(pid_t pid,int code,const char*) {
 std::lock_guard<std::mutex> lock(registry);exits[pid]=code;
}
HANDLER
REAPER
int main(){
 struct sigaction action{};action.sa_handler=sigchld_handler;sigemptyset(&action.sa_mask);
 action.sa_flags=SA_RESTART;assert(sigaction(SIGCHLD,&action,nullptr)==0);
 std::map<pid_t,int> expected;
 {
  std::lock_guard<std::mutex> lock(registry);
  for(int i=0;i<32;i++){
   pid_t pid=fork();assert(pid>=0);if(!pid)_exit(i+1);expected[pid]=i+1;
  }
  usleep(20000);
  for(int i=0;i<100;i++)raise(SIGCHLD);
  assert(gChildExitPending.load());
 }
 for(int i=0;i<200 && exits.size()!=expected.size();i++){ReapExitedChildren();usleep(1000);}
 assert(exits==expected);
 ReapExitedChildren();assert(exits==expected);
 assert(waitpid(-1,nullptr,WNOHANG)==-1 && errno==ECHILD);
}
'''.replace('HANDLER', handler).replace('REAPER', reaper)
        with tempfile.TemporaryDirectory(prefix='winehua-sigchld-') as directory:
            cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
            cpp.write_text(code)
            subprocess.run([COMPILER, '-std=c++17', '-pthread', '-Wall', '-Wextra', str(cpp), '-o', str(exe)], check=True)
            for _ in range(3):
                subprocess.run([str(exe)], check=True, timeout=8)


if __name__ == '__main__':
    unittest.main()
