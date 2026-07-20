import sys
import unittest

from test.integration.process_harness import ManagedProcess


class ChatStressTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = ManagedProcess(sys.argv[1])
        self.clients: list[ManagedProcess] = []
        listening = self.server.next_line(timeout=15)
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
        joined = client.expect_prefix("JOINED ", timeout=20)
        client.user_id = int(joined.split()[1])
        client.user_name = name
        client.expect(f"USER_JOINED {client.user_id} {name}", timeout=20)
        return client

    def read_users(self, client: ManagedProcess) -> set[str]:
        client.send("/users")
        client.expect("USERS_BEGIN", timeout=20)
        users: set[str] = set()
        while True:
            line = client.next_line(timeout=20)
            if line == "USERS_END":
                return users
            prefix, _, name = line.split(" ", 2)
            self.assertEqual(prefix, "USER")
            users.add(name)

    def test_sixty_four_clients_fanout_presence_direct_and_churn(self) -> None:
        clients = [self.connect(f"Load{index:02}") for index in range(64)]
        expected = {f"Load{index:02}" for index in range(64)}
        self.assertEqual(self.read_users(clients[0]), expected)

        for index in (0, 17, 34, 63):
            clients[index].send(f"fanout-{index}")
        for client in clients:
            observed = {
                client.expect_prefix("MESSAGE ", timeout=30)
                for _ in range(4)
            }
            self.assertEqual(
                observed,
                {
                    f"MESSAGE Load{index:02}: fanout-{index}"
                    for index in (0, 17, 34, 63)
                },
            )

        clients[0].send("/msg Load63 stress-secret")
        direct = f"DIRECT {clients[0].user_id} Load00: stress-secret"
        clients[0].expect(direct, timeout=20)
        clients[63].expect(direct, timeout=20)

        for client in clients[32:]:
            client.send("/quit")
        for client in clients[32:]:
            client.wait(timeout=30)
        replacements = [self.connect(f"Wave{index:02}") for index in range(32)]
        expected = {f"Load{index:02}" for index in range(32)} | {
            f"Wave{index:02}" for index in range(32)
        }
        self.assertEqual(self.read_users(clients[0]), expected)
        replacements[-1].send("wave-complete")
        for client in clients[:32] + replacements:
            client.expect("MESSAGE Wave31: wave-complete", timeout=30)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
