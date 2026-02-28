# TQVault POSIX Replacement Project

## Mission Statement
The goal of this project is to develop a native Linux vault manager for Titan Quest that surpasses [TQVaultAE](https://github.com/EtienneLamoureux/TQVaultAE) in both performance and correctness. TQVaultAE is a valuable reference for understanding the game's data formats and mechanics, but it is an aging C# codebase with known bugs. We use it as a starting point for reverse-engineering, not as a specification to replicate blindly. Where TQVaultAE has deficiencies — incorrect behavior, poor UX patterns, or missing edge cases — TQVaultC should improve upon them rather than inherit them.

## Technical Core
- **Language:** POSIX C
- **Toolkit:** GTK4
- **Platform:** Wayland on Linux.
- **Data Access:** Pre-indexed static asset database for near-instant resource lookups.

## Objectives
1. **Extreme Performance:** Shift asset discovery from runtime to compile-time. Use O(log N) lookups on pre-hashed asset paths.
2. **POSIX Compliance:** Adhere to POSIX standards to ensure robustness and efficiency on Linux.
3. **Native Linux Integration:** Use the GTK4 toolkit and native Linux APIs to provide a seamless user experience.
4. **Freedesktop.org Compliance:** Adhere to Freedesktop.org specifications for configuration paths.
5. **Feature Parity and Beyond:** Cover the core features of TQVaultAE — inventory management and item modification within game rules — while fixing bugs and improving on UX where possible.
6. **No Cross-Platform Overhead:** Eschew abstraction layers in favor of direct, native implementations (e.g., `mmap`, `pread`).

## Current Components
- `tqvaultc`: The GTK4-based graphical interface for the vault manager.
- `tq-stats`: A command-line utility for parsing and reporting Titan Quest character data.
- various other tools to aid in troubleshooting and reverse engineering the structure of titan quest game files.
