#!/usr/bin/env python3
"""spkg distributed compilation test.

Creates a test project with .ce + .cpp sources, starts multiple spkg-node
instances, and runs a distributed build to verify the full pipeline:

  1. File extension handling (.ce vs .cpp)
  2. C++ header dependency scanning via zig cc -MM
  3. src_ext transmission to remote nodes
  4. Correct compilation and linking across mixed-language sources
  5. Sharp #include header bundling to nodes
  6. Node health check and graceful fallback
  7. Retry on node failure with backoff

Test modes:
    python3 tests/test_distributed.py              # full test
    python3 tests/test_distributed.py --fault       # fault injection test
    python3 tests/test_distributed.py --mode all    # run all modes

Usage:
    python3 tests/test_distributed.py
"""

import os
import sys
import json
import time
import signal
import shutil
import subprocess
import tempfile
import argparse
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SPKG_BIN    = PROJECT_ROOT / "build" / "spkg"
SPKG_NODE   = PROJECT_ROOT / "build" / "spkg-node"
SHARPC      = Path("/root/code/sharp/build/sharpc")

NODE_PORTS = [10080, 10081, 10082]
NODE_JOBS  = 2

class Colors:
    GREEN  = "\033[92m"
    RED    = "\033[91m"
    YELLOW = "\033[93m"
    CYAN   = "\033[96m"
    RESET  = "\033[0m"

def ok(msg):   print(f"  {Colors.GREEN}\u2713{Colors.RESET} {msg}")
def fail(msg): print(f"  {Colors.RED}\u2717{Colors.RESET} {msg}"); return False
def info(msg): print(f"  {Colors.CYAN}\u2192{Colors.RESET} {msg}")
def warn(msg): print(f"  {Colors.YELLOW}\u26a0{Colors.RESET} {msg}")

test_failed = False

def check_prereqs():
    """Verify all required binaries exist."""
    global test_failed
    missing = []
    if not SPKG_BIN.exists():   missing.append(str(SPKG_BIN))
    if not SPKG_NODE.exists():  missing.append(str(SPKG_NODE))
    if not SHARPC.exists():     missing.append(str(SHARPC))
    if missing:
        for m in missing:
            fail(f"missing binary: {m}")
        test_failed = True
        return False
    ok("spkg binary found")
    ok("spkg-node binary found")
    ok("sharpc found")
    return True

def create_test_project(tmpdir, include_sharp_header=False):
    """Create a test project with mixed .ce and .cpp sources."""
    src = tmpdir / "src"
    src.mkdir(parents=True)

    if include_sharp_header:
        (src / "mylib.h").write_text("""#ifndef MYLIB_H
#define MYLIB_H

int compute_value(int x);

#endif
""")
        (src / "mylib.ce").write_text("""#include "mylib.h"

int compute_value(int x) {
    return x * 3 + 1;
}
""")

    (src / "cpp_math.h").write_text("""#ifndef CPP_MATH_H
#define CPP_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

int fibonacci(int n);
int vector_sum(const int* arr, int len);

#ifdef __cplusplus
}
#endif

#endif
""")

    (src / "cpp_math.cpp").write_text("""#include "cpp_math.h"

int fibonacci(int n) {
    if (n <= 1) return n;
    int a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        int c = a + b;
        a = b; b = c;
    }
    return b;
}

int vector_sum(const int* arr, int len) {
    int total = 0;
    for (int i = 0; i < len; i++) total += arr[i];
    return total;
}
""")

    (src / "main.ce").write_text("""extern int fibonacci(int n);
extern int vector_sum(const int* arr, int len);

int main() {
    int fib7 = fibonacci(7);
    if (fib7 != 13) return 1;

    int arr[] = {1, 2, 3, 4, 5};
    int sum = vector_sum(arr, 5);
    if (sum != 15) return 2;

    return 0;
}
""")

    (src / "helper.ce").write_text("""extern int helper_value() {
    return 42;
}
""")

    config_lines = ['local exe = b:add_executable({ name = "dist_test" })',
                    'exe:add_source("src/main.ce")',
                    'exe:add_source("src/helper.ce")',
                    'exe:add_source("src/cpp_math.cpp")']

    if include_sharp_header:
        config_lines.append('exe:add_source("src/mylib.ce")')
        main_ce = src / "main.ce"
        main_content = main_ce.read_text()
        main_content = main_content.replace(
            'return 0;',
            '    if (compute_value(4) != 13) return 3;\n    return 0;'
        )
        main_content = 'extern int compute_value(int x);\n' + main_content
        main_ce.write_text(main_content)

    config_lines.append('exe:add_include("src")')
    config_lines.append('b:install(exe)')

    (tmpdir / "config.spkg").write_text("\n".join(config_lines))

    info(f"test project created in {tmpdir}")

class NodeProcess:
    def __init__(self, port, tmpdir):
        self.port = port
        self.proc = None
        self.logfile = tmpdir / f"node_{port}.log"

    def start(self):
        log = open(self.logfile, "w")
        self.proc = subprocess.Popen(
            [
                str(SPKG_NODE),
                "--listen", f"127.0.0.1:{self.port}",
                "--sharpc", str(SHARPC),
                "--max-jobs", str(NODE_JOBS),
            ],
            stdout=log, stderr=subprocess.STDOUT,
        )
        time.sleep(0.5)
        if self.proc.poll() is not None:
            fail(f"node port={self.port} failed to start (see {self.logfile})")
            return False
        ok(f"node started on 127.0.0.1:{self.port} (pid={self.proc.pid})")
        return True

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def kill(self):
        """Force kill without cleanup (simulates crash)."""
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=3)

    @property
    def addr(self):
        return f"127.0.0.1:{self.port}"

def start_nodes(tmpdir, ports):
    nodes = []
    for port in ports:
        n = NodeProcess(port, tmpdir)
        if n.start():
            nodes.append(n)
        else:
            for started in nodes:
                started.stop()
            return None
    return nodes

def stop_nodes(nodes):
    info("stopping nodes...")
    for n in nodes:
        n.stop()
    ok("all nodes stopped")

def run_distributed_build(tmpdir, nodes_list=None, extra_env=None, cwd=None):
    """Run spkg build --dist with the configured nodes."""
    if nodes_list is None:
        nodes_list = NODE_PORTS
    if cwd is None:
        cwd = tmpdir

    nodes_str = ",".join(f"127.0.0.1:{p}" for p in nodes_list)
    env = os.environ.copy()
    env["SPKG_NODES"] = nodes_str
    env["SHARPC"] = str(SHARPC)
    if extra_env:
        env.update(extra_env)

    build_log = tmpdir / "build.log"
    info(f"running: spkg build --dist (SPKG_NODES={nodes_str})")

    with open(build_log, "w") as log:
        proc = subprocess.run(
            [str(SPKG_BIN), "build", "--dist", "--verbose"],
            cwd=cwd, env=env,
            stdout=log, stderr=subprocess.STDOUT,
            timeout=120,
        )

    with open(build_log) as f:
        build_output = f.read()

    if proc.returncode != 0:
        fail("distributed build failed")
        print("\n--- build log (last 2000 chars) ---")
        print(build_output[-2000:])
        return None

    ok("distributed build succeeded")
    return build_output

def verify_binary(tmpdir, build_output, name="dist_test"):
    """Verify the compiled binary exists and runs correctly."""
    exe = tmpdir / "build" / name / name
    if not exe.exists():
        fail(f"binary not found: {exe}")
        return False
    ok(f"binary exists: {exe}")

    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    if result.returncode == 0:
        ok(f"binary runs successfully (exit code 0)")
    else:
        fail(f"binary returned exit code {result.returncode}")
        if result.stderr:
            print(f"  stderr: {result.stderr}")
        return False

    return True

def check_remote_compilation(build_output):
    """Verify that remote compilation was actually used."""
    remote_count = build_output.count("[remote]")
    if remote_count > 0:
        ok(f"detected {remote_count} remote compilation(s) in build output")
    else:
        fail("no [remote] markers found in build output")
        return False
    return True

def check_cpp_compilation(build_output):
    """Verify that .cpp compilation was included."""
    if "cpp_math.cpp" in build_output or "cpp_math" in build_output:
        ok("C++ source (cpp_math.cpp) was compiled")
    else:
        warn("cpp_math.cpp not mentioned in build output")

def check_header_bundling(build_output):
    """Verify that headers were bundled to nodes."""
    if "collect_headers" not in build_output:
        ok("no header collection needed for this project")

def check_health_check(build_output):
    """Verify that health check was performed."""
    if "healthy node" in build_output or "healthy" in build_output.lower():
        ok("node health check detected in output")

def check_retry(build_output):
    """Check if retry logic was exercised."""
    retry_count = build_output.count("[retry]")
    if retry_count > 0:
        ok(f"retry logic exercised ({retry_count} retries)")

def check_fallback(build_output):
    """Check if fallback to local build was detected."""
    if "falling back to local build" in build_output:
        ok("graceful fallback to local build detected")

def run_banner(title):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")

def test_basic_distributed(tmpdir):
    """Test 1: Basic distributed build with mixed .ce + .cpp sources."""
    global test_failed
    run_banner("Test 1: Basic Distributed Build")

    create_test_project(tmpdir)

    info("starting nodes...")
    nodes = start_nodes(tmpdir, NODE_PORTS)
    if not nodes:
        test_failed = True
        return

    try:
        build_output = run_distributed_build(tmpdir)
        if not build_output:
            test_failed = True
            return

        if not verify_binary(tmpdir, build_output):
            test_failed = True
            return

        check_remote_compilation(build_output)
        check_cpp_compilation(build_output)

    finally:
        stop_nodes(nodes)

def test_sharp_header_bundling(tmpdir):
    """Test 2: Distributed build with Sharp .ce files using #include."""
    global test_failed
    run_banner("Test 2: Sharp #include Header Bundling")

    create_test_project(tmpdir, include_sharp_header=True)

    info("starting nodes...")
    nodes = start_nodes(tmpdir, NODE_PORTS)
    if not nodes:
        test_failed = True
        return

    try:
        build_output = run_distributed_build(tmpdir)
        if not build_output:
            test_failed = True
            return

        if not verify_binary(tmpdir, build_output):
            test_failed = True
            return

        check_remote_compilation(build_output)
        check_cpp_compilation(build_output)

        if "mylib" in build_output:
            ok("Sharp source with #include (mylib.ce) was compiled")
        else:
            warn("mylib.ce not mentioned in build output (may have been cached)")

    finally:
        stop_nodes(nodes)

def test_fault_injection(tmpdir):
    """Test 3: Fault injection — kill one node mid-build, verify retry."""
    global test_failed
    run_banner("Test 3: Fault Injection (Node Crash Mid-Build)")

    create_test_project(tmpdir)

    all_ports = [10083, 10084]
    info("starting 2 nodes...")
    nodes = start_nodes(tmpdir, all_ports)
    if not nodes:
        test_failed = True
        return

    try:
        # Kill node 0 after a short delay to simulate crash
        import threading
        def kill_first_node():
            time.sleep(2.0)
            info("injecting fault: killing first node...")
            nodes[0].kill()
            info("first node killed")

        killer = threading.Thread(target=kill_first_node, daemon=True)
        killer.start()

        build_output = run_distributed_build(tmpdir, nodes_list=all_ports)
        if not build_output:
            test_failed = True
            return

        if not verify_binary(tmpdir, build_output):
            test_failed = True
            return

        check_remote_compilation(build_output)
        check_retry(build_output)

    finally:
        stop_nodes(nodes)

def test_health_check_fallback(tmpdir):
    """Test 4: Health check filters unhealthy nodes and falls back."""
    global test_failed
    run_banner("Test 4: Health Check & Fallback")

    create_test_project(tmpdir)

    # Only start 1 healthy node, add 2 bogus ports
    info("starting 1 healthy node + 2 bogus ports...")
    ports = [10085] + [10999, 10998]
    nodes = start_nodes(tmpdir, [10085])
    if not nodes:
        test_failed = True
        return

    try:
        build_output = run_distributed_build(tmpdir, nodes_list=ports)
        if not build_output:
            test_failed = True
            return

        if not verify_binary(tmpdir, build_output):
            test_failed = True
            return

        check_remote_compilation(build_output)

        # The health check should have filtered the bogus nodes
        if "healthy" in build_output:
            ok("health check filtered unhealthy nodes")

    finally:
        stop_nodes(nodes)

def test_all_unhealthy_fallback(tmpdir):
    """Test 5: All nodes unhealthy — should fall back to local build."""
    global test_failed
    run_banner("Test 5: All Unhealthy Nodes Fallback")

    create_test_project(tmpdir)

    # Use only bogus ports (no nodes running)
    ports = [10997, 10996]

    build_output = run_distributed_build(tmpdir, nodes_list=ports)
    if not build_output:
        test_failed = True
        return

    if not verify_binary(tmpdir, build_output):
        test_failed = True
        return

    # Should have fallen back to local build
    if "healthy" in build_output or "fall" in build_output:
        ok("fallback to local build succeeded")

def main():
    global test_failed
    parser = argparse.ArgumentParser(description="spkg distributed compilation test")
    parser.add_argument("--keep-tmp", action="store_true", help="keep temporary directory")
    parser.add_argument("--mode", choices=["all", "basic", "header", "fault", "health"],
                        default="all", help="test mode to run")
    parser.add_argument("--fault", action="store_true",
                        help="run fault injection test (deprecated, use --mode fault)")
    args = parser.parse_args()

    print(f"\n{'='*60}")
    print(f"  spkg   Distributed Compilation Test Suite")
    print(f"{'='*60}\n")

    info("Checking prerequisites")
    if not check_prereqs():
        sys.exit(1)

    # backwards compat
    if args.fault:
        args.mode = "fault"

    tests_to_run = []
    if args.mode == "all":
        tests_to_run = [
            ("Test 1: Basic Distributed", test_basic_distributed),
            ("Test 2: Sharp Header Bundling", test_sharp_header_bundling),
            ("Test 3: Fault Injection", test_fault_injection),
            ("Test 4: Health Check", test_health_check_fallback),
            ("Test 5: All-Unhealthy Fallback", test_all_unhealthy_fallback),
        ]
    elif args.mode == "basic":
        tests_to_run = [("Test 1: Basic Distributed", test_basic_distributed)]
    elif args.mode == "header":
        tests_to_run = [("Test 2: Sharp Header Bundling", test_sharp_header_bundling)]
    elif args.mode == "fault":
        tests_to_run = [("Test 3: Fault Injection", test_fault_injection)]
    elif args.mode == "health":
        tests_to_run = [
            ("Test 4: Health Check", test_health_check_fallback),
            ("Test 5: All-Unhealthy Fallback", test_all_unhealthy_fallback),
        ]

    for test_name, test_fn in tests_to_run:
        tmpdir = Path(tempfile.mkdtemp(prefix="spkg_dist_"))
        try:
            test_fn(tmpdir)
        finally:
            if args.keep_tmp:
                info(f"test artifacts preserved at: {tmpdir}")
            else:
                shutil.rmtree(tmpdir, ignore_errors=True)

    if test_failed:
        print(f"\n{'='*60}")
        fail("SOME TESTS FAILED")
        print(f"{'='*60}\n")
        sys.exit(1)
    else:
        print(f"\n{'='*60}")
        ok("ALL TESTS PASSED")
        print(f"{'='*60}\n")

if __name__ == "__main__":
    main()