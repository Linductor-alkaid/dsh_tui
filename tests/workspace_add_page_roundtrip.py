#!/usr/bin/env python3
"""Interactive round-trip test for the add-workspace page.

This test drives `dsh_tui --child` through a pty and a fake fd3/fd4 bridge. It
verifies the whole workspace feature path:

1. the sidebar renders the add-workspace button;
2. Enter opens the add-workspace page;
3. typing a path auto-fills the title;
4. Enter sends an `add-workspace` command over fd4;
5. a `workspace-added` reply closes the page and selects the new workspace;
6. Ctrl+N then creates a session in that selected workspace.
"""
import fcntl
import json
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time


def set_inheritable(fd):
    os.set_inheritable(fd, True)


def set_nonblocking(fd):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def spawn_child(binary):
    event_r, event_w = os.pipe()
    command_r, command_w = os.pipe()
    for fd in (event_r, event_w, command_r, command_w):
        set_inheritable(fd)

    master, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[0] &= ~termios.IXON  # let Ctrl+N/Ctrl+Q reach FTXUI
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

    actions = [
        (os.POSIX_SPAWN_DUP2, slave, 0),
        (os.POSIX_SPAWN_DUP2, slave, 1),
        (os.POSIX_SPAWN_DUP2, slave, 2),
        (os.POSIX_SPAWN_DUP2, event_r, 3),
        (os.POSIX_SPAWN_DUP2, command_w, 4),
    ]
    for fd in (event_r, event_w, command_r, command_w, slave):
        if fd not in (3, 4):
            actions.append((os.POSIX_SPAWN_CLOSE, fd))

    pid = os.posix_spawn(binary, [binary, "--child"], os.environ, file_actions=actions)
    os.close(slave)
    os.close(event_r)
    os.close(command_w)
    set_nonblocking(master)
    set_nonblocking(command_r)
    return pid, master, event_w, command_r


def drain(fd, delay=0.08):
    data = b""
    while True:
        ready, _, _ = select.select([fd], [], [], delay)
        if not ready:
            break
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            break
        if not chunk:
            break
        data += chunk
    return data.decode("utf-8", errors="replace")


def send_event(fd, payload):
    # Match JS JSON.stringify: compact, no spaces.
    os.write(fd, (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))


def read_command_line(fd, deadline):
    buffer = b""
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            chunk = b""
        if chunk:
            buffer += chunk
        newline = buffer.find(b"\n")
        if newline >= 0:
            line = buffer[:newline].decode("utf-8", errors="replace")
            return json.loads(line)
        time.sleep(0.02)
    return None


def wait_for_command(fd, predicate, timeout=4.0):
    deadline = time.time() + timeout
    seen = []
    while time.time() < deadline:
        command = read_command_line(fd, time.time() + 0.1)
        if command is None:
            continue
        seen.append(command)
        if predicate(command):
            return True, seen
    return False, seen


def cleanup(pid, master, event_w):
    try:
        os.write(event_w, b'{"type":"bye","reason":"ok"}\n')
        os.close(event_w)
    except OSError:
        pass
    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            waited, _ = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            return
        if waited == pid:
            return
        time.sleep(0.05)
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: workspace_add_page_roundtrip.py /path/to/dsh_tui")
    binary = os.path.abspath(sys.argv[1])
    if not os.path.exists(binary):
        raise SystemExit(f"binary not found: {binary}")

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "new-workspace")
        title = "new-workspace"
        pid, master, event_w, command_r = spawn_child(binary)
        try:
            for event in (
                {"type": "hello", "sessionId": "s", "model": "m", "provider": "p", "cwd": tmp},
                {"type": "workspaces", "workspaces": [{"id": "w1", "path": tmp, "title": "one", "sessionIds": []}]},
                {"type": "history", "messages": []},
                {"type": "status", "status": "idle"},
            ):
                send_event(event_w, event)

            output = ""
            deadline = time.time() + 12
            while time.time() < deadline and "＋ 添加工作区" not in output:
                time.sleep(0.05)
                output += drain(master)

            opened = False
            for _ in range(25):
                os.write(master, b"\t")
                time.sleep(0.08)
                output += drain(master)
                if "[＋ 添加工作区]" in output:
                    os.write(master, b"\r")
                    time.sleep(0.25)
                    output += drain(master)
                    opened = True
                    break
            if not opened or "新增工作区" not in output:
                raise SystemExit(f"add-workspace page did not open\n{output[-2000:]}")

            os.write(master, path.encode())
            time.sleep(0.2)
            output += drain(master)
            if path not in output or title not in output:
                raise SystemExit(f"path or auto title was not rendered\n{output[-2000:]}")

            os.write(master, b"\r")
            ok, commands = wait_for_command(
                command_r,
                lambda c: c.get("type") == "add-workspace",
            )
            if not ok:
                raise SystemExit(f"add-workspace command was not emitted: {commands}")

            time.sleep(0.2)
            output += drain(master)
            send_event(
                event_w,
                {"type": "workspaces", "workspaces": [{"id": "w2", "path": path, "title": title, "sessionIds": []}]},
            )
            send_event(
                event_w,
                {"type": "workspace-added", "id": "w2", "title": title, "path": path},
            )

            time.sleep(0.6)
            output += drain(master)

            # Once workspace-added is processed, the modal closes and the new
            # workspace is selected. Ctrl+N must then create a session in w2.
            time.sleep(0.4)
            os.write(master, b"\x0e")
            ok, commands = wait_for_command(
                command_r,
                lambda c: c.get("type") == "new-session" and c.get("text") == "w2",
            )
            if not ok:
                raise SystemExit(f"new workspace was not selected: {commands}")

            time.sleep(0.4)
            os.write(master, b"\x11")  # Ctrl+Q
            deadline = time.time() + 5
            exited = False
            while time.time() < deadline:
                waited, status = os.waitpid(pid, os.WNOHANG)
                if waited == pid:
                    exited = True
                    if os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0:
                        break
                    raise SystemExit(f"dsh_tui exited with status {status}")
                time.sleep(0.05)
            if not exited:
                raise SystemExit("dsh_tui did not exit after Ctrl+Q")
        finally:
            cleanup(pid, master, event_w)
            os.close(command_r)

    print("workspace add page roundtrip ok")


if __name__ == "__main__":
    main()
