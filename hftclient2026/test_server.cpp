// Minimal stand-alone test server that mirrors the protocol of
// hftserver2026/main.cpp but has no external dependencies. Used
// to validate the client end-to-end without needing cmake/json.
//
// It generates a few challenges, prints the correct answer it
// computed itself, then prints what the client sent back so we
// can eyeball a match.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr int MODULO = 997;
constexpr int PORT   = 12399;     // distinct from production 12345
constexpr int N      = 128;

int computeChecksumNaive(const std::vector<std::vector<int>>& A,
                         const std::vector<std::vector<int>>& B) {
    const int n = (int)A.size();
    int sum = 0;
    for (int i = 0; i < n; ++i) {
        std::vector<int> rowC(n, 0);
        for (int k = 0; k < n; ++k) {
            int a = A[i][k];
            for (int j = 0; j < n; ++j)
                rowC[j] = (rowC[j] + (int)((1LL * a * B[k][j]) % MODULO)) % MODULO;
        }
        for (int j = 0; j < n; ++j) sum = (sum + rowC[j]) % MODULO;
    }
    return sum;
}
}  // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    int numChallenges = (argc >= 2) ? std::atoi(argv[1]) : 3;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(srv, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(srv, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    listen(srv, 4);

    std::fprintf(stderr, "[test_server] listening on 127.0.0.1:%d\n", PORT);

    sockaddr_in ca{};
    socklen_t   cl  = sizeof(ca);
    int         cs  = accept(srv, (sockaddr*)&ca, &cl);
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Read team name (single recv as the real server does).
    char nb[256] = {0};
    int  nr      = recv(cs, nb, sizeof(nb) - 1, 0);
    if (nr <= 0) { std::fprintf(stderr, "no name\n"); return 1; }
    while (nr > 0 && (nb[nr - 1] == '\n' || nb[nr - 1] == '\r' || nb[nr - 1] == ' '))
        nb[--nr] = 0;
    std::fprintf(stderr, "[test_server] client name: '%s'\n", nb);

    std::mt19937 rng(42);
    int passed = 0, failed = 0;

    for (int cid = 1; cid <= numChallenges; ++cid) {
        std::vector<std::vector<int>> A(N, std::vector<int>(N));
        std::vector<std::vector<int>> B(N, std::vector<int>(N));
        for (auto& r : A) for (int& v : r) v = rng() % MODULO;
        for (auto& r : B) for (int& v : r) v = rng() % MODULO;

        int expected = computeChecksumNaive(A, B);

        // Serialize EXACTLY as hftserver2026 does.
        std::stringstream ss;
        ss << cid << "\n" << N << "\n";
        for (auto& r : A) for (int v : r) ss << v << " ";
        ss << "\n";
        for (auto& r : B) for (int v : r) ss << v << " ";
        ss << "\n";
        std::string payload = ss.str();

        auto t0 = std::chrono::steady_clock::now();
        send(cs, payload.data(), payload.size(), 0);

        // Read reply (one line).
        char rb[128] = {0};
        int  got     = 0;
        while (got < (int)sizeof(rb) - 1) {
            int n = recv(cs, rb + got, sizeof(rb) - 1 - got, 0);
            if (n <= 0) goto done;
            got += n;
            if (memchr(rb, '\n', got)) break;
        }
        {
            auto t1   = std::chrono::steady_clock::now();
            auto usec = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

            int gotCid = -1, gotAns = -1;
            sscanf(rb, "%d %d", &gotCid, &gotAns);
            bool ok = (gotCid == cid && gotAns == expected);
            (ok ? passed : failed)++;
            std::fprintf(stderr,
                         "[test_server] cid=%d expected=%d got=(%d,%d) rt=%lldus  %s\n",
                         cid, expected, gotCid, gotAns, (long long)usec,
                         ok ? "OK" : "FAIL");
        }
    }
done:
    std::fprintf(stderr, "[test_server] passed=%d failed=%d\n", passed, failed);
    close(cs);
    close(srv);
    return failed == 0 ? 0 : 1;
}
