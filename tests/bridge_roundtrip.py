#!/usr/bin/env python3
"""Round-trip tests for the dsh_tui fd3/fd4 bridge transport.

These tests intentionally do not need npm or a real DeepSeek account:

1. child mode: this test acts like the Node bridge and drives `dsh_tui --child`
   over fd3 (events) and fd4 (commands), expecting a clean exit after `bye`.
2. standalone mode: a fake dsh executable is placed in DSH_BIN; dsh_tui must
   create the profile, spawn it with fd3/fd4 attached, consume events and exit.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time


def spawn_stdio_devnull(argv, env=None):
    dn = os.open("/dev/null", os.O_RDWR)
    try:
        os.set_inheritable(dn, True)
        actions = [
            (os.POSIX_SPAWN_DUP2, dn, 0),
            (os.POSIX_SPAWN_DUP2, dn, 1),
            (os.POSIX_SPAWN_DUP2, dn, 2),
            (os.POSIX_SPAWN_CLOSE, dn),
        ]
        return os.posix_spawn(argv[0], argv, env if env is not None else os.environ, file_actions=actions)
    finally:
        os.close(dn)


def wait_exit(pid, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            waited, status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            return None
        if waited == pid:
            return os.waitstatus_to_exitcode(status)
        time.sleep(0.05)
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    return None


def test_child_mode(binary):
    event_r, event_w = os.pipe()
    command_r, command_w = os.pipe()
    for fd in (event_r, event_w, command_r, command_w):
        os.set_inheritable(fd, True)

    actions = [
        (os.POSIX_SPAWN_DUP2, event_r, 3),
        (os.POSIX_SPAWN_DUP2, command_w, 4),
    ]
    # Close the original pipe descriptors, but never the just-created fd 3/4.
    for fd in (event_r, event_w, command_r, command_w):
        if fd not in (3, 4):
            actions.append((os.POSIX_SPAWN_CLOSE, fd))

    dn = os.open("/dev/null", os.O_RDWR)
    os.set_inheritable(dn, True)
    actions += [
        (os.POSIX_SPAWN_DUP2, dn, 0),
        (os.POSIX_SPAWN_DUP2, dn, 1),
        (os.POSIX_SPAWN_DUP2, dn, 2),
        (os.POSIX_SPAWN_CLOSE, dn),
    ]
    pid = os.posix_spawn(binary, [binary, "--child"], os.environ, file_actions=actions)
    os.close(dn)
    os.close(event_r)
    os.close(command_w)

    events = [
        {"type": "hello", "sessionId": "test", "model": "m", "provider": "p", "cwd": "/tmp"},
        {"type": "history", "messages": [{"role": "user", "text": "hi"}]},
        {"type": "status", "status": "idle"},
        {"type": "bye", "reason": "ok"},
    ]
    for event in events:
        os.write(event_w, (json.dumps(event) + "\n").encode("utf-8"))
    os.close(event_w)

    code = wait_exit(pid, 8)
    os.close(command_r)
    if code != 0:
        raise SystemExit(f"child bridge roundtrip failed: exit={code}")


def test_standalone_mode(binary):
    with tempfile.TemporaryDirectory() as tmp:
        home = os.path.join(tmp, "home")
        fake_dsh = os.path.join(tmp, "fake-dsh")
        with open(fake_dsh, "w", encoding="utf-8") as handle:
            handle.write("#!/bin/sh\n")
            handle.write("printf '%s\\n' '{\"type\":\"hello\",\"sessionId\":\"standalone\",\"model\":\"m\",\"provider\":\"p\",\"cwd\":\"/tmp\"}' >&3\n")
            handle.write("printf '%s\\n' '{\"type\":\"history\",\"messages\":[]}' >&3\n")
            handle.write("printf '%s\\n' '{\"type\":\"bye\",\"reason\":\"ok\"}' >&3\n")
            handle.write("exit 0\n")
        os.chmod(fake_dsh, 0o755)

        env = os.environ.copy()
        env["DSH_HOME"] = home
        env["DSH_BIN"] = fake_dsh
        pid = spawn_stdio_devnull([binary], env)
        code = wait_exit(pid, 10)
        if code != 0:
            raise SystemExit(f"standalone bridge roundtrip failed: exit={code}")
        profile = os.path.join(home, "profiles", "tui", "node_modules", "dsh-tui")
        if not os.path.islink(profile) and not os.path.exists(profile):
            raise SystemExit("standalone mode did not initialize the TUI profile")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: bridge_roundtrip.py /path/to/dsh_tui")
    binary = os.path.abspath(sys.argv[1])
    if not os.path.exists(binary):
        raise SystemExit(f"binary not found: {binary}")
    test_child_mode(binary)
    test_standalone_mode(binary)
    print("bridge roundtrip ok")


if __name__ == "__main__":
    main()
