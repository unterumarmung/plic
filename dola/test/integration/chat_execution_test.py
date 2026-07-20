import sys
import unittest

from test.integration.process_harness import ManagedProcess


class ChatExecutionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = ManagedProcess(sys.argv[1])
        self.clients: list[ManagedProcess] = []
        listening = self.server.next_line()
        prefix = "CHAT_LISTENING "
        self.assertTrue(listening.startswith(prefix), listening)
        self.address = listening[len(prefix) :]

    def tearDown(self) -> None:
        for client in self.clients:
            client.terminate()
        self.server.terminate()

    def connect(self, name: str) -> ManagedProcess:
        client = ManagedProcess(sys.argv[2])
        self.clients.append(client)
        client.send(self.address, name)
        joined = client.expect_prefix("JOINED ")
        user_id = int(joined.split()[1])
        client.expect(f"USER_JOINED {user_id} {name}")
        client.user_id = user_id
        client.user_name = name
        return client

    def users(self, client: ManagedProcess) -> set[tuple[int, str]]:
        client.send("/users")
        client.expect("USERS_BEGIN")
        users: set[tuple[int, str]] = set()
        while True:
            line = client.next_line()
            if line == "USERS_END":
                return users
            parts = line.split(" ", 2)
            self.assertEqual(parts[0], "USER", line)
            users.add((int(parts[1]), parts[2]))

    def test_multi_client_broadcast_history_and_leave(self) -> None:
        alice = self.connect("Alice")
        bob = self.connect("Bob")
        alice.expect(f"USER_JOINED {bob.user_id} Bob")

        alice.send("hello")
        alice.expect("MESSAGE Alice: hello")
        bob.expect("MESSAGE Alice: hello")

        bob.send("second", "third")
        for text in ("second", "third"):
            alice.expect(f"MESSAGE Bob: {text}")
            bob.expect(f"MESSAGE Bob: {text}")

        alice.send("/history 2")
        for line in (
            "HISTORY_BEGIN",
            "HISTORY Bob: second",
            "HISTORY Bob: third",
            "HISTORY_END",
        ):
            alice.expect(line)

        alice.send("/history 0")
        alice.expect("HISTORY_BEGIN")
        alice.expect("HISTORY_END")

        alice.send("/history 100")
        for line in (
            "HISTORY_BEGIN",
            "HISTORY Alice: hello",
            "HISTORY Bob: second",
            "HISTORY Bob: third",
            "HISTORY_END",
        ):
            alice.expect(line)

        alice.send("/history -1")
        alice.expect("REJECTED history count must not be negative")

        unnamed = ManagedProcess(sys.argv[2])
        self.clients.append(unnamed)
        unnamed.send(self.address, "")
        unnamed.expect("REJECTED name must be 1-32 bytes without spaces")
        unnamed.send("/quit")
        unnamed.wait()

        bob.send("/quit")
        bob.wait()
        alice.expect(f"USER_LEFT {bob.user_id}")

        charlie = self.connect("Charlie")
        alice.expect(f"USER_JOINED {charlie.user_id} Charlie")
        charlie.terminate()
        alice.expect(f"USER_LEFT {charlie.user_id}")

        alice.send("/quit")
        alice.wait()

    def test_users_direct_messages_and_privacy(self) -> None:
        alice = self.connect("Alice")
        bob = self.connect("Bob")
        charlie = self.connect("Charlie")
        self.assertEqual(
            self.users(alice),
            {
                (alice.user_id, "Alice"),
                (bob.user_id, "Bob"),
                (charlie.user_id, "Charlie"),
            },
        )

        alice.send("/msg Bob secret")
        direct = f"DIRECT {alice.user_id} Alice: secret"
        alice.expect(direct)
        bob.expect(direct)
        charlie.expect_absent(direct)

        alice.send("/msg Alice private")
        self_direct = f"DIRECT {alice.user_id} Alice: private"
        alice.expect(self_direct)
        alice.expect_absent(self_direct)
        alice.send("/msg Missing nowhere")
        alice.expect("REJECTED direct-message recipient not found")
        alice.send("/msg Bob")
        alice.expect("REJECTED malformed /msg command")

    def test_name_and_message_validation_and_duplicate_claim(self) -> None:
        for invalid in ("", "has space", "x" * 33, "λ" * 17):
            client = ManagedProcess(sys.argv[2])
            self.clients.append(client)
            client.send(self.address, invalid)
            client.expect("REJECTED name must be 1-32 bytes without spaces")
            client.send("/quit")
            client.wait()

        first = ManagedProcess(sys.argv[2])
        second = ManagedProcess(sys.argv[2])
        self.clients.extend((first, second))
        first.send(self.address, "Contested")
        second.send(self.address, "Contested")
        outcomes = [
            first.expect_matching(
                lambda line: line.startswith("JOINED ")
                or line == "REJECTED name is already in use",
                "join acceptance or duplicate rejection",
                8.0,
            ),
            second.expect_matching(
                lambda line: line.startswith("JOINED ")
                or line == "REJECTED name is already in use",
                "join acceptance or duplicate rejection",
                8.0,
            ),
        ]
        self.assertEqual(sum(line.startswith("JOINED ") for line in outcomes), 1)
        self.assertEqual(outcomes.count("REJECTED name is already in use"), 1)

        winner_index = 0 if outcomes[0].startswith("JOINED ") else 1
        winner = first if winner_index == 0 else second
        winner_id = int(outcomes[winner_index].split()[1])
        maximum_message = "x" * (1024 * 1024)
        winner.send(maximum_message)
        winner.expect(f"MESSAGE Contested: {maximum_message}", timeout=30)
        winner.send("/msg Contested " + maximum_message)
        winner.expect(
            f"DIRECT {winner_id} Contested: {maximum_message}", timeout=30
        )

        oversized_message = maximum_message + "x"
        winner.send(oversized_message)
        winner.expect("REJECTED message exceeds 1 MiB", timeout=30)
        winner.send("/msg Contested " + oversized_message)
        winner.expect("REJECTED message exceeds 1 MiB", timeout=30)

    def test_history_is_bounded_and_excludes_direct_messages(self) -> None:
        client = self.connect("Historian")
        client.send("/msg Historian private")
        client.expect(f"DIRECT {client.user_id} Historian: private")
        for index in range(101):
            client.send(f"public-{index:03}")
        client.send("/history 200")
        client.expect("HISTORY_BEGIN", timeout=20)
        history: list[str] = []
        while True:
            line = client.next_line(timeout=20)
            if line == "HISTORY_END":
                break
            history.append(line)
        self.assertEqual(len(history), 100)
        self.assertEqual(history[0], "HISTORY Historian: public-001")
        self.assertEqual(history[-1], "HISTORY Historian: public-100")
        self.assertNotIn("private", "\n".join(history))

    def test_broadcast_reaches_multiple_concurrent_users(self) -> None:
        clients = [self.connect(f"User{index:02}") for index in range(8)]
        sender = clients[-1]
        sender.send("fanout")
        for client in clients:
            client.expect("MESSAGE User07: fanout", timeout=15)

        observer = clients[0]
        for client in clients[1:]:
            client.send("/quit")
        for client in clients[1:]:
            client.wait(timeout=15)
        departed = {
            int(observer.expect_prefix("USER_LEFT ", timeout=15).split()[1])
            for _ in clients[1:]
        }
        self.assertEqual(departed, {client.user_id for client in clients[1:]})
        observer.send("/quit")
        observer.wait(timeout=15)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
