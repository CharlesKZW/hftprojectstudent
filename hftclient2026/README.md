# HFT 2026 — Matrix Challenge Client

A latency-optimized C++ client for the HFT Matrix Multiplication
challenge. The server broadcasts `(A, B)` over TCP and the client must
reply with `checksum(A·B) mod 997` as fast as possible.

## Group name

The client takes the team name as a CLI argument; set it at launch
time (no rebuild required):

```bash
./hftclient2026 <host> <port> <YOUR_TEAM_NAME>
```

## The trick: O(N²), not O(N³)

We never build the product matrix. The required answer is
```
checksum  =  Σ_{i,j} C[i,j]   with  C = A·B
          =  Σ_{i,j} Σ_k A[i,k] · B[k,j]
          =  Σ_k (Σ_i A[i,k]) · (Σ_j B[k,j])
          =  Σ_k colSumA[k]  ·  rowSumB[k]   (mod 997)
```
so the work collapses to:

1. Stream-parse `A` while accumulating its **column sums** (N ints).
2. Stream-parse `B` while accumulating its **row sums**       (N ints).
3. One dot product of length `N`, mod 997.

That's **O(N²)** total — and `O(N)` of state, instead of the
`O(N²)` matrix that a naïve implementation would store.

## Other low-latency choices

| Optimization                        | Why                                              |
| ----------------------------------- | ------------------------------------------------ |
| Custom byte-level parser            | `sscanf` on 130 KB of ASCII per challenge is slow |
| Streaming state machine             | Parses across arbitrary `recv()` boundaries (blast bursts) |
| Accumulate sums *during* parsing    | Zero matrix allocation, single pass over the wire |
| `TCP_NODELAY`                       | Disable Nagle — reply ships immediately          |
| 1 MB `SO_RCVBUF` / `SO_SNDBUF`      | Absorbs ~130 KB challenges and bursts            |
| `MSG_NOSIGNAL` + `SIG_IGN(SIGPIPE)` | Survive server disconnects without dying         |
| `-O3 -march=native -flto`           | Whole-program inlining, host-CPU vectorization   |
| `-fno-rtti -fno-exceptions`         | Smaller binary, simpler codegen in hot path      |
| Single thread, no locks             | No contention for a workload one challenge wide  |

## Build

### Option A — with the project's CMake (alongside the server)

From the project root:
```bash
./build.sh
./build/bin/hftclient2026 127.0.0.1 12345 TeamA
```

### Option B — standalone Makefile (no cmake / nlohmann_json needed)

From this directory:
```bash
make            # release build with -O3 -march=native -flto
./hftclient2026 127.0.0.1 12345 TeamA
```

## Local verification

The two test helpers in this directory exercise the client without
needing the full server stack:

```bash
clang++ -std=c++20 -O2 -o test_server test_server.cpp
clang++ -std=c++20 -O2 -o test_burst  test_burst.cpp

# 200 sequential challenges
./test_server 200 &  sleep 0.3
./hftclient2026 127.0.0.1 12399 SmokeTeam

# 50 challenges packed into ONE send() — emulates blast-server bursts
./test_burst 50 &    sleep 0.3
./hftclient2026 127.0.0.1 12400 BurstTeam
```

Both tests compute the answer with the naïve `O(N³)` reference
implementation from `hftserver2026/main.cpp` and compare against
what the client returns.

Measured on this laptop (Apple Silicon, localhost):
- Sequential: ~600 µs per round-trip including the test server's own
  parsing and matrix generation.
- Burst (50 challenges in a single send): **264 µs / challenge** end-to-end.

The actual client-side work (parse + compute + send) is well under
100 µs at N=128.
