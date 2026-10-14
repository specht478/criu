# Unix Socket Diagnostic Reproduction

Proof of concept for reproducing missing Unix socket diagnostics during CRIU
checkpointing. AI-generated with VS Code GitHub Copilot (GPT 6.1 Sol).
Provided as-is, without warranty or guarantees of safety or reproduction.

## Risks

- Requires root and shares the host network namespace. Use a disposable VM.
- Replaces any existing container named `socket-criu-test`.
- Creates high CPU/socket load and loops until failure or interruption.
- Leaves container/checkpoint artifacts; these and logs may contain sensitive data.

## How to Run

Requires GCC, pthread headers, Podman with checkpoint/restore support, and CRIU.
Podman uses its configured CRIU installation, not necessarily this checkout.
From the repository root:

```sh
cd reproduction
sudo sh ./reproduce.sh
```

Press Ctrl+C to stop. The script attempts to stop the workloads.

License: GPL-2.0-only; see [COPYING](../COPYING).
