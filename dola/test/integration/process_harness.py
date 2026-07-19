import queue
import subprocess
import threading
import time


class ManagedProcess:
    def __init__(self, executable: str) -> None:
        self.process = subprocess.Popen(
            [executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.lines: queue.Queue[str] = queue.Queue()
        self.stderr: list[str] = []
        self.closed = False
        self.readers = [
            threading.Thread(target=self._read_stdout, daemon=True),
            threading.Thread(target=self._read_stderr, daemon=True),
        ]
        for reader in self.readers:
            reader.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        self.stderr.extend(self.process.stderr)

    def send(self, *lines: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write("".join(f"{line}\n" for line in lines))
        self.process.stdin.flush()

    def next_line(self, timeout: float = 8.0) -> str:
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty as error:
            self.fail("process did not produce a line before timeout")
            raise error

    def expect(self, expected: str, timeout: float = 8.0) -> str:
        return self.expect_matching(lambda line: line == expected, expected, timeout)

    def expect_prefix(self, prefix: str, timeout: float = 8.0) -> str:
        return self.expect_matching(
            lambda line: line.startswith(prefix), f"prefix {prefix!r}", timeout
        )

    def expect_matching(self, predicate, description: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        observed: list[str] = []
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            observed.append(line)
            if predicate(line):
                return line
        self.fail(f"expected {description}; observed {observed!r}")
        raise AssertionError("unreachable")

    def expect_absent(self, unexpected: str, duration: float = 0.4) -> None:
        deadline = time.monotonic() + duration
        observed: list[str] = []
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                return
            observed.append(line)
            if line == unexpected:
                self.fail(f"unexpected line {unexpected!r}; observed {observed!r}")

    def wait(self, timeout: float = 8.0) -> None:
        try:
            status = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.terminate()
            raise AssertionError("process did not exit before timeout") from error
        self._close_streams()
        if status != 0:
            self.fail(f"process exited with {status}: {''.join(self.stderr)}")

    def terminate(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self._close_streams()

    def _close_streams(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.process.stdin is not None:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass
        for reader in self.readers:
            reader.join(timeout=1)
        for stream in (self.process.stdout, self.process.stderr):
            if stream is not None:
                stream.close()

    def fail(self, message: str) -> None:
        self.terminate()
        raise AssertionError(f"{message}; stderr={''.join(self.stderr)!r}")
