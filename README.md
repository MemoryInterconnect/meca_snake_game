# Snake Game for RISC-V

Terminal-based snake game with shared memory support for dual-instance gameplay.

## Features

- Unicode graphics (box drawing characters)
- Color display using ncurses
- 80x24 fixed screen size
- Dual-instance mode via `/dev/mem` shared memory
- Game state persistence at physical address `0x200000000`

## Building

### Prerequisites

Install RISC-V cross-compiler:
```bash
apt install gcc-riscv64-linux-gnu
```

Build ncurses for RISC-V (static):
```bash
wget https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz
tar xzf ncurses-6.4.tar.gz
cd ncurses-6.4

./configure --host=riscv64-linux-gnu \
    --prefix=/usr/riscv64-linux-gnu \
    --enable-widec \
    --with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo \
    --without-shared \
    --without-debug

make
sudo make install
```

### Compile

```bash
make
```

Or specify custom library paths:
```bash
make LIBDIR=/path/to/libs INCDIR=/path/to/include
```

## Running

```bash
./snake
```

Requires root privileges for `/dev/mem` access.

## Controls

| Key | Action |
|-----|--------|
| Arrow Keys | Move snake |
| P | Pause/Resume |
| R | Restart |
| Q | Quit |

## Dual-Instance Mode

Two instances share game state through memory-mapped `/dev/mem` at offset `0x200000000`.

**Instance 1 (Player):**
- Starts first, controls the game
- Shows `[P1:PLAYING]` indicator

**Instance 2 (Watcher):**
- Starts second, displays blank screen
- Shows `[P2:WATCHING]` indicator
- Waits for player to quit

**Takeover:**
- When player quits, watcher takes over
- Continues from last game state
- If game was over, starts fresh

## Memory Layout

Shared state structure at `0x200000000`:

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 4 | active_player |
| 0x04 | 8 | heartbeat |
| 0x0C | 4 | score |
| 0x10 | 4 | high_score |
| 0x14 | 4 | snake_length |
| 0x18 | 4 | game_state |
| 0x1C | 4 | direction |
| 0x20 | 4 | food_x |
| 0x24 | 4 | food_y |
| 0x28 | 4 | games_played |
| 0x2C | 4 | total_food |
| 0x30 | 4 | max_length |
| 0x34 | ... | snake body array |

## License

MIT
