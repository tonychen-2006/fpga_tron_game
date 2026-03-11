# FPGA Tron Game

A Tron light-cycle game implemented in C for the Intel DE10-Lite (and DE1-SoC) FPGA board, running on the Nios V (RISC-V) soft processor with VGA output.

## Overview

Two light-cycles race across a 160×120 pixel VGA display, each leaving a trail behind them. A player loses when their cycle collides with a wall, an obstacle, or any trail. The first player to reach **9 wins** ends the game.

- **Player 1 (Human)** — Red cycle, controlled via pushbuttons
- **Player 2 (Robot)** — Blue cycle, controlled by a simple lookahead AI

## Hardware Requirements

| Component | Details |
|-----------|---------|
| Board | Intel DE10-Lite (default) or DE1-SoC |
| Processor | Nios V (RISC-V `rv32im_zicsr`) |
| Display | VGA — 160×120 (DE10-Lite) or 320×240 (DE1-SoC) |
| Inputs | Pushbuttons (KEY0, KEY1), Slide switches (SW3–SW0) |
| Outputs | VGA display, 7-segment displays (HEX0–HEX5), LEDs |

## Controls

| Input | Action |
|-------|--------|
| **KEY1** | Turn left (toggle pending turn) |
| **KEY0** | Turn right (toggle pending turn) |
| **SW3** | Fastest speed |
| **SW2** | Fast speed |
| **SW1** | Medium speed |
| **SW0** | Slow speed |
| *(no switch)* | Slowest speed |

Turns are interrupt-driven — press a key to queue a turn that takes effect on the next game tick.

## Scoring

Scores are shown on the 7-segment displays:
- **HEX0** — Player 1 (human) score
- **HEX1** — Player 2 (robot) score

The game flashes the full screen **red** if the human wins 9 rounds, or **blue** if the robot wins 9 rounds.

## LED Indicators

| LED | Meaning |
|-----|---------|
| LEDR[0] | Right turn pending |
| LEDR[1] | Left turn pending |
| LEDR[2] | Blinks every ~50 timer ticks (heartbeat) |

## Building

The project uses the Intel FPGA Academy toolchain. Set `QUARTUS_ROOTDIR` to your Quartus installation, then run:

```bat
make
```

To target the **DE1-SoC** instead of the DE10-Lite, change the following line in `address_map_niosv.h`:

```c
#define DE10LITE 0   // 0 = DE1-SoC, 1 = DE10-Lite
```

## File Structure

| File | Description |
|------|-------------|
| `vga.c` | Main game logic, VGA rendering, AI, and interrupt handlers |
| `address_map_niosv.h` | Memory-mapped I/O address definitions for Nios V |
| `Makefile` | Build rules for the RISC-V cross-compiler toolchain |

## Game Mechanics

- The arena is bordered by a white wall. Hitting any wall or trail causes a loss.
- **Obstacles** are scattered across the arena and act as additional walls.
- A head-on collision (both cycles occupy the same cell simultaneously) results in a **draw** — neither player scores.
- After each round, the arena resets and players return to their starting positions.
