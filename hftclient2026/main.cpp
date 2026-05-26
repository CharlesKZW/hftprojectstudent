// =============================================================
//  HFT 2026 — High-performance Matrix Challenge client
//
//  Protocol (text, newline / space separated):
//      <cid>\n
//      <N>\n
//      a00 a01 ... a(N-1)(N-1) \n
//      b00 b01 ... b(N-1)(N-1) \n
//
//  Task: reply "<cid> <checksum>\n" where
//      checksum = (sum of all entries of C) mod 997,  C = A·B
//
//  Math shortcut (THE key optimization):
//      checksum = Σ_{i,j} Σ_k A[i,k]·B[k,j]
//               = Σ_k (Σ_i A[i,k]) · (Σ_j B[k,j])
//               = Σ_k colSumA[k] · rowSumB[k]    (mod 997)
//  → O(N²) work instead of O(N³); no matrix storage needed.
//  → We accumulate the two sum vectors *during* parsing — single
//    pass over the wire bytes, no temporaries.
// =============================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr int  MODULO        = 997;
constexpr int  MAX_N         = 2048;          // generous upper bound
constexpr size_t RECV_BUF_SZ = 1 << 18;        // 256 KiB scratch

// ---- parser state machine -----------------------------------
enum class S : uint8_t { CID, N, A, B };

struct ParserState {
    S       state    = S::CID;
    int     cid      = 0;
    int     N        = 0;
    int     curInt   = 0;
    bool    inInt    = false;
    int     parsedA  = 0;        // count of values parsed in A
    int     parsedB  = 0;
    int     curColA  = 0;        // running column index in A
    int     curRowB  = 0;        // running row index in B
    int     curColB  = 0;
    int     colSumA[MAX_N];      // Σ_i A[i,k]
    int     rowSumB[MAX_N];      // Σ_j B[k,j]
};

// ---- low-level helpers --------------------------------------
[[gnu::always_inline]] inline bool send_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= w;
    }
    return true;
}

// itoa for non-negative int, returns length written
[[gnu::always_inline]] inline int u32_to_dec(unsigned v, char* out) {
    char tmp[12];
    int  k = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[k++] = char('0' + v % 10); v /= 10; }
    for (int i = 0; i < k; ++i) out[i] = tmp[k - 1 - i];
    return k;
}

// Emit "<cid> <checksum>\n" using fixed-width formatting (no snprintf cost)
[[gnu::always_inline]] inline int format_reply(char* out, int cid, int answer) {
    int p = 0;
    p += u32_to_dec(unsigned(cid), out + p);
    out[p++] = ' ';
    p += u32_to_dec(unsigned(answer), out + p);
    out[p++] = '\n';
    return p;
}

// ---- challenge completion handler ---------------------------
// Called once both matrices are fully parsed. Performs the
// O(N) dot-product reduction, sends reply, resets state.
[[gnu::always_inline]] inline void finish_and_reply(int sock, ParserState& ps) {
    long long acc = 0;
    const int N   = ps.N;
    for (int k = 0; k < N; ++k) {
        acc += (long long)ps.colSumA[k] * ps.rowSumB[k];
    }
    int answer = int(acc % MODULO);

    char reply[40];
    int  rlen = format_reply(reply, ps.cid, answer);
    send_all(sock, reply, size_t(rlen));

    // reset for next challenge
    ps.state   = S::CID;
    ps.cid     = 0;
    ps.N       = 0;
    ps.curInt  = 0;
    ps.inInt   = false;
    ps.parsedA = ps.parsedB = 0;
    ps.curColA = ps.curRowB = ps.curColB = 0;
}

// ---- the hot loop -------------------------------------------
// Streaming, branch-friendly byte parser. Handles arbitrary
// chunking from recv() — server may pack many challenges into
// one packet or split one across many.
void parse_chunk(const char* buf, size_t n, ParserState& ps, int sock) {
    for (size_t i = 0; i < n; ++i) {
        char c = buf[i];
        unsigned d = unsigned(c - '0');
        bool isDigit = (d < 10u);

        switch (ps.state) {

        case S::CID:
            if (isDigit) {
                ps.cid = ps.cid * 10 + int(d);
                ps.inInt = true;
            } else if (ps.inInt) {
                ps.inInt = false;
                ps.state = S::N;
            }
            break;

        case S::N:
            if (isDigit) {
                ps.N = ps.N * 10 + int(d);
                ps.inInt = true;
            } else if (ps.inInt) {
                ps.inInt = false;
                // Initialize accumulators for this challenge
                const int N = ps.N;
                std::memset(ps.colSumA, 0, sizeof(int) * N);
                std::memset(ps.rowSumB, 0, sizeof(int) * N);
                ps.curColA = ps.curRowB = ps.curColB = 0;
                ps.parsedA = ps.parsedB = 0;
                ps.state = S::A;
            }
            break;

        case S::A:
            if (isDigit) {
                ps.curInt = ps.curInt * 10 + int(d);
                ps.inInt = true;
            } else if (ps.inInt) {
                // value belongs to A[?, curColA]
                ps.colSumA[ps.curColA] += ps.curInt;
                ps.curInt = 0;
                ps.inInt = false;
                if (++ps.curColA == ps.N) ps.curColA = 0;
                if (++ps.parsedA == ps.N * ps.N) {
                    ps.state = S::B;
                }
            }
            break;

        case S::B:
            if (isDigit) {
                ps.curInt = ps.curInt * 10 + int(d);
                ps.inInt = true;
            } else if (ps.inInt) {
                // value belongs to B[curRowB, curColB]
                ps.rowSumB[ps.curRowB] += ps.curInt;
                ps.curInt = 0;
                ps.inInt = false;
                if (++ps.curColB == ps.N) {
                    ps.curColB = 0;
                    ++ps.curRowB;
                }
                if (++ps.parsedB == ps.N * ps.N) {
                    finish_and_reply(sock, ps);
                }
            }
            break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <host> <port> <team_name>\n", argv[0]);
        return 1;
    }
    const char* host = argv[1];
    int         port = std::atoi(argv[2]);
    const char* team = argv[3];

    // SIGPIPE would otherwise kill us if the server drops mid-send.
    signal(SIGPIPE, SIG_IGN);

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // Disable Nagle — we want our reply on the wire NOW.
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Larger socket buffers — a single challenge is ~130 KB at N=128.
    int bufsz = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "Bad host: %s\n", host);
        return 1;
    }

    if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    // Announce team name (single send, server's first recv reads it whole).
    {
        std::string intro = std::string(team) + "\n";
        if (!send_all(sock, intro.data(), intro.size())) {
            perror("send name");
            return 1;
        }
    }

    std::fprintf(stderr, "[%s] connected to %s:%d\n", team, host, port);

    static char  recvBuf[RECV_BUF_SZ];
    ParserState  ps;

    while (true) {
        ssize_t n = ::recv(sock, recvBuf, RECV_BUF_SZ, 0);
        if (n > 0) {
            parse_chunk(recvBuf, size_t(n), ps, sock);
        } else if (n == 0) {
            std::fprintf(stderr, "[%s] server closed connection\n", team);
            break;
        } else {
            if (errno == EINTR) continue;
            perror("recv");
            break;
        }
    }

    ::close(sock);
    return 0;
}
