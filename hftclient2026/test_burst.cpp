// Burst test: pack many challenges into a SINGLE send() call to
// exercise the client's streaming parser across recv() boundaries
// (the blast server's ultra mode does this).

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
constexpr int PORT   = 12400;
constexpr int N      = 128;

int naiveChecksum(const std::vector<std::vector<int>>& A,
                  const std::vector<std::vector<int>>& B) {
    int n = (int)A.size(), sum = 0;
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

    int burst = (argc >= 2) ? std::atoi(argv[1]) : 20;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(srv, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    listen(srv, 4);

    std::fprintf(stderr, "[burst] listening on 127.0.0.1:%d, burst=%d\n", PORT, burst);

    sockaddr_in ca{};
    socklen_t   cl  = sizeof(ca);
    int         cs  = accept(srv, (sockaddr*)&ca, &cl);
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int big = 1 << 22;
    setsockopt(cs, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));

    char nb[256] = {0};
    int  nr      = recv(cs, nb, sizeof(nb) - 1, 0);
    if (nr <= 0) return 1;
    std::fprintf(stderr, "[burst] client: %.*s\n", nr, nb);

    std::mt19937 rng(123);
    std::vector<int> expected(burst);
    std::string mega;
    mega.reserve(size_t(burst) * 140000);

    for (int cid = 1; cid <= burst; ++cid) {
        std::vector<std::vector<int>> A(N, std::vector<int>(N));
        std::vector<std::vector<int>> B(N, std::vector<int>(N));
        for (auto& r : A) for (int& v : r) v = rng() % MODULO;
        for (auto& r : B) for (int& v : r) v = rng() % MODULO;
        expected[cid - 1] = naiveChecksum(A, B);

        std::stringstream ss;
        ss << cid << "\n" << N << "\n";
        for (auto& r : A) for (int v : r) ss << v << " ";
        ss << "\n";
        for (auto& r : B) for (int v : r) ss << v << " ";
        ss << "\n";
        mega += ss.str();
    }
    std::fprintf(stderr, "[burst] sending %zu bytes (%d challenges) in one shot\n",
                 mega.size(), burst);

    auto t0 = std::chrono::steady_clock::now();
    // Single send call — kernel may fragment but it's one logical write.
    ssize_t off = 0;
    while (off < (ssize_t)mega.size()) {
        ssize_t w = send(cs, mega.data() + off, mega.size() - off, 0);
        if (w < 0) { perror("send"); return 1; }
        off += w;
    }

    // Collect all replies — keep reading until we have `burst` newlines.
    std::string acc;
    int newlines = 0;
    while (newlines < burst) {
        char buf[4096];
        int  n = recv(cs, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) if (buf[i] == '\n') ++newlines;
        acc.append(buf, n);
    }
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // Parse replies line by line.
    int pass = 0, fail = 0;
    size_t p = 0;
    int idx = 0;
    while (p < acc.size() && idx < burst) {
        size_t nl = acc.find('\n', p);
        if (nl == std::string::npos) break;
        int gotCid = -1, gotAns = -1;
        sscanf(acc.c_str() + p, "%d %d", &gotCid, &gotAns);
        bool ok = (gotCid == idx + 1) && (gotAns == expected[idx]);
        (ok ? pass : fail)++;
        if (!ok) {
            std::fprintf(stderr, "  FAIL cid=%d expected=%d got=(%d,%d)\n",
                         idx + 1, expected[idx], gotCid, gotAns);
        }
        p = nl + 1;
        ++idx;
    }
    std::fprintf(stderr,
                 "[burst] pass=%d fail=%d  total=%lldus  per-challenge=%.1fus\n",
                 pass, fail, (long long)us, (double)us / burst);
    close(cs);
    close(srv);
    return fail == 0 ? 0 : 1;
}
