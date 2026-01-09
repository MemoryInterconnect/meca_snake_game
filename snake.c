/*
 * Terminal Snake Game - 2 Instance Version
 * Features: ANSI colors, ASCII characters, mmap-based shared state
 * Two instances share game state - one plays, one watches
 * Compile: gcc -o snake snake.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <time.h>
#include <termios.h>
#include <signal.h>

/* Maximum snake length */
#define MAX_SNAKE_LEN 500

/* Memory map layout (./mem file) */
#define MMAP_SIZE 8192

/* Snake segment stored in mmap */
typedef struct {
    int16_t x, y;
} MmapPoint;

/* Shared game state in mmap */
typedef struct {
    /* Instance control */
    uint32_t active_player;       /* 0=none, 1=instance1, 2=instance2 */
    uint64_t heartbeat;           /* Timestamp of last update (ms) */

    /* Game state */
    uint32_t score;
    uint32_t high_score;
    uint32_t snake_length;
    uint32_t game_state;          /* 0=running, 1=paused, 2=game_over */
    uint32_t direction;           /* 0=UP, 1=DOWN, 2=LEFT, 3=RIGHT */
    int32_t food_x;
    int32_t food_y;
    uint32_t games_played;
    uint32_t total_food;
    uint32_t max_length;

    /* Full snake body for watcher to display */
    MmapPoint snake[MAX_SNAKE_LEN];
} GameMmap;

/* Direction enum */
enum Direction { DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3 };

/* Game state enum */
enum GameState { STATE_RUNNING = 0, STATE_PAUSED = 1, STATE_GAME_OVER = 2 };

/* Instance mode */
enum InstanceMode { MODE_PLAYER = 0, MODE_WATCHER = 1 };

/* ANSI color codes */
#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_RED        "\033[31m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_BLUE       "\033[34m"

/* ASCII characters */
#define CHAR_WALL_H    '-'
#define CHAR_WALL_V    '|'
#define CHAR_CORNER    '+'
#define CHAR_FOOD      '*'
#define CHAR_BODY      'o'
#define CHAR_TAIL      '.'
#define CHAR_HEAD_UP   '^'
#define CHAR_HEAD_DOWN 'v'
#define CHAR_HEAD_LEFT '<'
#define CHAR_HEAD_RIGHT '>'

/* Local point for game logic */
typedef struct {
    int x, y;
} Point;

/* Game structure */
typedef struct {
    Point snake[MAX_SNAKE_LEN];
    int snake_len;
    int direction;
    int next_direction;
    Point food;
    int score;
    int game_over;
    int paused;

    int game_width;
    int game_height;
    int game_left;
    int game_top;
    int screen_width;
    int screen_height;

    GameMmap *mmap_state;
    int mmap_fd;

    /* Instance control */
    uint32_t instance_id;         /* 1 or 2 */
    int mode;                     /* MODE_PLAYER or MODE_WATCHER */
} Game;

/* Terminal state for restoration */
static struct termios orig_termios;
static int terminal_configured = 0;

/* Heartbeat timeout in milliseconds */
#define HEARTBEAT_TIMEOUT_MS 500

/* Get current time in milliseconds */
uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Physical memory offset */
//#define DEVMEM "/dev/mem"
//#define MEM_OFFSET 0x200000000ULL
#define DEVMEM "./mem"
#define MEM_OFFSET 0x0ULL

/* Initialize mmap to /dev/mem */
GameMmap* init_mmap(int *fd) {
    *fd = open(DEVMEM, O_RDWR | O_SYNC);
    if (*fd < 0) {
        perror("open /dev/mem");
        return NULL;
    }

    GameMmap *mm = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, MEM_OFFSET);
    if (mm == MAP_FAILED) {
        perror("mmap");
        close(*fd);
        return NULL;
    }

    return mm;
}

/* Terminal control functions using ANSI escape codes */
void term_clear(void) {
    printf("\033[2J");
}

void term_move(int row, int col) {
    printf("\033[%d;%dH", row + 1, col + 1);
}

void term_hide_cursor(void) {
    printf("\033[?25l");
}

void term_show_cursor(void) {
    printf("\033[?25h");
}

void term_flush(void) {
    fflush(stdout);
}

/* Configure terminal for raw input */
void term_raw_mode(void) {
    if (terminal_configured) return;

    tcgetattr(STDIN_FILENO, &orig_termios);
    terminal_configured = 1;

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* Restore terminal settings */
void term_restore(void) {
    if (terminal_configured) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        terminal_configured = 0;
    }
    term_show_cursor();
    printf(ANSI_RESET);
    term_flush();
}

/* Non-blocking character read */
int term_getch(void) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 50000;  /* 50ms timeout */

    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            /* Handle escape sequences for arrow keys */
            if (c == 27) {  /* ESC */
                unsigned char seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                    if (read(STDIN_FILENO, &seq[1], 1) == 1) {
                        switch (seq[1]) {
                            case 'A': return 1001;  /* Up */
                            case 'B': return 1002;  /* Down */
                            case 'C': return 1003;  /* Right */
                            case 'D': return 1004;  /* Left */
                        }
                    }
                }
                return 27;
            }
            return c;
        }
    }
    return -1;  /* No input */
}

/* Spawn food at random location */
void spawn_food(Game *g) {
    int valid = 0;
    while (!valid) {
        g->food.x = rand() % g->game_width;
        g->food.y = rand() % g->game_height;
        valid = 1;
        for (int i = 0; i < g->snake_len; i++) {
            if (g->snake[i].x == g->food.x && g->snake[i].y == g->food.y) {
                valid = 0;
                break;
            }
        }
    }
}

/* Update mmap state (called by player) */
void update_mmap(Game *g) {
    GameMmap *mm = g->mmap_state;

    mm->active_player = g->instance_id;
    mm->heartbeat = get_time_ms();

    mm->score = g->score;
    if ((uint32_t)g->score > mm->high_score) {
        mm->high_score = g->score;
    }
    mm->snake_length = g->snake_len;
    if ((uint32_t)g->snake_len > mm->max_length) {
        mm->max_length = g->snake_len;
    }
    mm->direction = g->direction;
    mm->food_x = g->food.x;
    mm->food_y = g->food.y;

    if (g->game_over) {
        mm->game_state = STATE_GAME_OVER;
    } else if (g->paused) {
        mm->game_state = STATE_PAUSED;
    } else {
        mm->game_state = STATE_RUNNING;
    }

    /* Copy snake body to mmap */
    for (int i = 0; i < g->snake_len && i < MAX_SNAKE_LEN; i++) {
        mm->snake[i].x = g->snake[i].x;
        mm->snake[i].y = g->snake[i].y;
    }

    msync(mm, sizeof(GameMmap), MS_ASYNC);
}

/* Load state from mmap (for takeover) */
void load_from_mmap(Game *g) {
    GameMmap *mm = g->mmap_state;

    g->score = mm->score;
    g->snake_len = mm->snake_length;
    if (g->snake_len > MAX_SNAKE_LEN) g->snake_len = MAX_SNAKE_LEN;
    if (g->snake_len < 1) g->snake_len = 0;
    g->direction = mm->direction;
    g->next_direction = mm->direction;
    g->food.x = mm->food_x;
    g->food.y = mm->food_y;
    g->game_over = (mm->game_state == STATE_GAME_OVER);
    g->paused = (mm->game_state == STATE_PAUSED);

    /* Copy snake body from mmap */
    for (int i = 0; i < g->snake_len; i++) {
        g->snake[i].x = mm->snake[i].x;
        g->snake[i].y = mm->snake[i].y;
    }
}

/* Initialize/reset game */
void reset_game(Game *g) {
    int start_x = g->game_width / 2;
    int start_y = g->game_height / 2;

    g->snake_len = 3;
    g->snake[0].x = start_x;
    g->snake[0].y = start_y;
    g->snake[1].x = start_x - 1;
    g->snake[1].y = start_y;
    g->snake[2].x = start_x - 2;
    g->snake[2].y = start_y;

    g->direction = DIR_RIGHT;
    g->next_direction = DIR_RIGHT;
    g->score = 0;
    g->game_over = 0;
    g->paused = 0;

    spawn_food(g);

    g->mmap_state->games_played++;
    update_mmap(g);
}

/* Check if another instance is actively playing */
int is_other_playing(Game *g) {
    GameMmap *mm = g->mmap_state;
    uint64_t now = get_time_ms();

    /* Check if there's an active player that isn't us */
    if (mm->active_player != 0 && mm->active_player != g->instance_id) {
        /* Check if their heartbeat is recent */
        if (now - mm->heartbeat < HEARTBEAT_TIMEOUT_MS) {
            return 1;  /* Other instance is playing */
        }
    }
    return 0;  /* No one else is playing */
}

/* Try to become the player */
int try_become_player(Game *g) {
    GameMmap *mm = g->mmap_state;
    uint64_t now = get_time_ms();

    /* If no active player or heartbeat timeout */
    if (mm->active_player == 0 ||
        mm->active_player == g->instance_id ||
        (now - mm->heartbeat >= HEARTBEAT_TIMEOUT_MS)) {

        g->mode = MODE_PLAYER;
        mm->active_player = g->instance_id;
        mm->heartbeat = now;
        return 1;
    }
    return 0;
}

/* Initialize game */
Game* init_game(void) {
    Game *g = calloc(1, sizeof(Game));
    if (!g) return NULL;

    g->mmap_state = init_mmap(&g->mmap_fd);
    if (!g->mmap_state) {
        free(g);
        return NULL;
    }

    /* Assign instance ID based on what's in mmap */
    GameMmap *mm = g->mmap_state;
    uint64_t now = get_time_ms();

    if (mm->active_player == 0 || (now - mm->heartbeat >= HEARTBEAT_TIMEOUT_MS)) {
        /* No active player, we become player 1 */
        g->instance_id = 1;
        g->mode = MODE_PLAYER;
    } else if (mm->active_player == 1) {
        /* Player 1 is active, we become player 2 (watcher) */
        g->instance_id = 2;
        g->mode = MODE_WATCHER;
    } else {
        /* Player 2 is active, we become player 1 (watcher) */
        g->instance_id = 1;
        g->mode = MODE_WATCHER;
    }

    /* Initialize terminal */
    term_raw_mode();
    term_hide_cursor();
    term_clear();

    /* Fixed 80x24 screen size */
    g->screen_width = 80;
    g->screen_height = 24;

    g->game_top = 3;
    g->game_left = 1;
    g->game_height = g->screen_height - 5;  /* 19 rows */
    g->game_width = g->screen_width - 4;    /* 76 cols */

    srand(time(NULL) ^ getpid());  /* Different seed per instance */

    if (g->mode == MODE_PLAYER) {
        reset_game(g);
    }
    /* Watcher mode: don't load anything, just wait */

    return g;
}

/* Draw walls */
void draw_walls(Game *g) {
    printf(ANSI_CYAN);

    /* Top wall */
    term_move(g->game_top - 1, g->game_left);
    putchar(CHAR_CORNER);
    for (int x = 0; x < g->game_width; x++) {
        putchar(CHAR_WALL_H);
    }
    putchar(CHAR_CORNER);

    /* Bottom wall */
    term_move(g->game_top + g->game_height, g->game_left);
    putchar(CHAR_CORNER);
    for (int x = 0; x < g->game_width; x++) {
        putchar(CHAR_WALL_H);
    }
    putchar(CHAR_CORNER);

    /* Side walls */
    for (int y = 0; y < g->game_height; y++) {
        term_move(g->game_top + y, g->game_left);
        putchar(CHAR_WALL_V);
        term_move(g->game_top + y, g->game_left + g->game_width + 1);
        putchar(CHAR_WALL_V);
    }

    printf(ANSI_RESET);
}

/* Draw score bar */
void draw_score(Game *g) {
    /* Title */
    printf(ANSI_BOLD ANSI_CYAN);
    const char *title = "=== SNAKE GAME ===";
    term_move(0, (g->screen_width - strlen(title)) / 2);
    printf("%s", title);
    printf(ANSI_RESET);

    /* Mode indicator */
    term_move(0, 2);
    if (g->mode == MODE_PLAYER) {
        printf(ANSI_BOLD ANSI_YELLOW "[P%d:PLAYING]" ANSI_RESET, g->instance_id);
    } else {
        printf(ANSI_BOLD ANSI_YELLOW "[P%d:WATCHING]" ANSI_RESET, g->instance_id);
    }

    /* Score */
    printf(ANSI_BOLD ANSI_WHITE);
    term_move(1, 2);
    printf("Score: %d", g->score);
    printf(ANSI_RESET);

    /* High score */
    printf(ANSI_BOLD ANSI_RED);
    term_move(1, g->screen_width / 2 - 10);
    printf("High Score: %d", g->mmap_state->high_score);
    printf(ANSI_RESET);

    /* Games played */
    printf(ANSI_BLUE);
    term_move(1, g->screen_width - 20);
    printf("Games: %d", g->mmap_state->games_played);
    printf(ANSI_RESET);

    /* Controls */
    printf(ANSI_WHITE);
    term_move(g->screen_height - 1, 0);
    if (g->mode == MODE_PLAYER) {
        const char *controls = "[Arrows] Move  [P] Pause  [R] Restart  [Q] Quit";
        term_move(g->screen_height - 1, (g->screen_width - strlen(controls)) / 2);
        printf("%s", controls);
    } else {
        const char *controls = "[Q] Quit  --  Waiting for other player to stop...";
        term_move(g->screen_height - 1, (g->screen_width - strlen(controls)) / 2);
        printf("%s", controls);
    }
    printf(ANSI_RESET);
}

/* Draw snake */
void draw_snake(Game *g) {
    for (int i = 0; i < g->snake_len; i++) {
        int screen_x = g->game_left + 1 + g->snake[i].x;
        int screen_y = g->game_top + g->snake[i].y;

        if (screen_x >= 0 && screen_x < g->screen_width - 1 &&
            screen_y >= 0 && screen_y < g->screen_height - 1) {

            term_move(screen_y, screen_x);

            if (i == 0) {
                printf(ANSI_BOLD ANSI_YELLOW);
                char head_char;
                switch (g->direction) {
                    case DIR_UP:    head_char = CHAR_HEAD_UP; break;
                    case DIR_DOWN:  head_char = CHAR_HEAD_DOWN; break;
                    case DIR_LEFT:  head_char = CHAR_HEAD_LEFT; break;
                    default:        head_char = CHAR_HEAD_RIGHT; break;
                }
                putchar(head_char);
                printf(ANSI_RESET);
            } else if (i == g->snake_len - 1) {
                printf(ANSI_BLUE);
                putchar(CHAR_TAIL);
                printf(ANSI_RESET);
            } else {
                printf(ANSI_GREEN);
                putchar(CHAR_BODY);
                printf(ANSI_RESET);
            }
        }
    }
}

/* Draw food */
void draw_food(Game *g) {
    int screen_x = g->game_left + 1 + g->food.x;
    int screen_y = g->game_top + g->food.y;

    if (screen_x >= 0 && screen_x < g->screen_width - 1 &&
        screen_y >= 0 && screen_y < g->screen_height - 1) {
        term_move(screen_y, screen_x);
        printf(ANSI_BOLD ANSI_RED "%c" ANSI_RESET, CHAR_FOOD);
    }
}

/* Draw game over screen */
void draw_game_over(Game *g) {
    const char *lines[] = {
        "+------------------------+",
        "|      GAME OVER!        |",
        "|                        |",
        "|  [R] Restart  [Q] Quit |",
        "+------------------------+"
    };
    int num_lines = 5;
    int start_y = g->screen_height / 2 - num_lines / 2;

    printf(ANSI_BOLD ANSI_MAGENTA);
    for (int i = 0; i < num_lines; i++) {
        int x = (g->screen_width - strlen(lines[i])) / 2;
        term_move(start_y + i, x);
        printf("%s", lines[i]);
    }

    char score_line[30];
    snprintf(score_line, sizeof(score_line), "|   Final Score: %-5d   |", g->score);
    term_move(start_y + 2, (g->screen_width - strlen(score_line)) / 2);
    printf("%s", score_line);
    printf(ANSI_RESET);
}

/* Draw pause screen */
void draw_paused(Game *g) {
    const char *lines[] = {
        "+-----------------+",
        "|     PAUSED      |",
        "|  [P] to Resume  |",
        "+-----------------+"
    };
    int num_lines = 4;
    int start_y = g->screen_height / 2 - num_lines / 2;

    printf(ANSI_BOLD ANSI_WHITE);
    for (int i = 0; i < num_lines; i++) {
        int x = (g->screen_width - strlen(lines[i])) / 2;
        term_move(start_y + i, x);
        printf("%s", lines[i]);
    }
    printf(ANSI_RESET);
}

/* Draw watcher waiting message */
void draw_watcher_status(Game *g) {
    const char *lines[] = {
        "+-----------------------------+",
        "|     WAITING FOR PLAYER      |",
        "|                             |",
        "|  Another instance is playing |",
        "|  Will start when they quit  |",
        "+-----------------------------+"
    };
    int num_lines = 6;
    int start_y = g->screen_height / 2 - num_lines / 2;

    printf(ANSI_BOLD ANSI_YELLOW);
    for (int i = 0; i < num_lines; i++) {
        int x = (g->screen_width - strlen(lines[i])) / 2;
        term_move(start_y + i, x);
        printf("%s", lines[i]);
    }
    printf(ANSI_RESET);
}

/* Move snake */
void move_snake(Game *g) {
    if (g->game_over || g->paused) return;

    g->direction = g->next_direction;

    Point new_head = g->snake[0];

    switch (g->direction) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
    }

    if (new_head.x < 0 || new_head.x >= g->game_width ||
        new_head.y < 0 || new_head.y >= g->game_height) {
        g->game_over = 1;
        update_mmap(g);
        return;
    }

    for (int i = 0; i < g->snake_len; i++) {
        if (g->snake[i].x == new_head.x && g->snake[i].y == new_head.y) {
            g->game_over = 1;
            update_mmap(g);
            return;
        }
    }

    for (int i = g->snake_len - 1; i > 0; i--) {
        g->snake[i] = g->snake[i - 1];
    }
    g->snake[0] = new_head;

    if (new_head.x == g->food.x && new_head.y == g->food.y) {
        if (g->snake_len < MAX_SNAKE_LEN) {
            g->snake_len++;
        }
        g->score += 10;
        g->mmap_state->total_food++;
        spawn_food(g);
    }

    update_mmap(g);
}

/* Handle input for player mode */
int handle_player_input(Game *g) {
    int ch = term_getch();
    if (ch < 0) return 1;  /* No input */

    switch (ch) {
        case 'q':
        case 'Q':
            /* Clear active player on quit */
            g->mmap_state->active_player = 0;
            msync(g->mmap_state, sizeof(GameMmap), MS_SYNC);
            return 0;

        case 'r':
        case 'R':
            reset_game(g);
            break;

        case 'p':
        case 'P':
            g->paused = !g->paused;
            update_mmap(g);
            break;

        case 1001:  /* Up arrow */
            if (!g->game_over && !g->paused && g->direction != DIR_DOWN)
                g->next_direction = DIR_UP;
            break;

        case 1002:  /* Down arrow */
            if (!g->game_over && !g->paused && g->direction != DIR_UP)
                g->next_direction = DIR_DOWN;
            break;

        case 1004:  /* Left arrow */
            if (!g->game_over && !g->paused && g->direction != DIR_RIGHT)
                g->next_direction = DIR_LEFT;
            break;

        case 1003:  /* Right arrow */
            if (!g->game_over && !g->paused && g->direction != DIR_LEFT)
                g->next_direction = DIR_RIGHT;
            break;
    }

    return 1;
}

/* Handle input for watcher mode */
int handle_watcher_input(Game *g) {
    (void)g;  /* Unused in watcher mode */
    int ch = term_getch();

    if (ch == 'q' || ch == 'Q') {
        return 0;
    }

    return 1;
}

/* Draw everything */
void draw(Game *g) {
    term_clear();
    draw_walls(g);
    draw_score(g);

    if (g->mode == MODE_WATCHER) {
        /* Watcher only shows blank screen with waiting message */
        draw_watcher_status(g);
    } else {
        /* Player mode - show game */
        draw_food(g);
        draw_snake(g);

        if (g->game_over) {
            draw_game_over(g);
        } else if (g->paused) {
            draw_paused(g);
        }
    }

    term_flush();
}

/* Cleanup */
void cleanup(Game *g) {
    if (g) {
        /* If we're the player, clear active player on exit */
        if (g->mode == MODE_PLAYER && g->mmap_state) {
            g->mmap_state->active_player = 0;
            msync(g->mmap_state, sizeof(GameMmap), MS_SYNC);
        }
        if (g->mmap_state) {
            munmap(g->mmap_state, MMAP_SIZE);
        }
        if (g->mmap_fd >= 0) {
            close(g->mmap_fd);
        }
        free(g);
    }
    term_restore();
    term_clear();
    term_move(0, 0);
    term_flush();
}

/* Signal handler for clean exit */
static Game *global_game = NULL;

void signal_handler(int sig) {
    (void)sig;
    if (global_game) {
        cleanup(global_game);
    }
    exit(0);
}

/* Main */
int main(void) {
    Game *game = init_game();
    if (!game) {
        fprintf(stderr, "Failed to initialize game\n");
        return 1;
    }

    global_game = game;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    struct timespec last_move;
    clock_gettime(CLOCK_MONOTONIC, &last_move);

    int move_delay_ms = 120;

    while (1) {
        if (game->mode == MODE_PLAYER) {
            /* Player mode */
            if (!handle_player_input(game)) {
                break;
            }

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_move.tv_sec) * 1000 +
                             (now.tv_nsec - last_move.tv_nsec) / 1000000;

            if (elapsed_ms >= move_delay_ms) {
                move_snake(game);
                last_move = now;

                move_delay_ms = 120 - (game->score / 20);
                if (move_delay_ms < 50) move_delay_ms = 50;
            }
        } else {
            /* Watcher mode - just wait for takeover */
            if (!handle_watcher_input(game)) {
                break;
            }

            /* Check if we can take over */
            if (!is_other_playing(game)) {
                /* Take over as player */
                if (try_become_player(game)) {
                    /* Load last game state */
                    load_from_mmap(game);

                    /* If game was over or invalid, start fresh */
                    if (game->game_over || game->snake_len == 0) {
                        reset_game(game);
                    } else {
                        /* Continue the game, update mmap with our heartbeat */
                        update_mmap(game);
                    }
                    clock_gettime(CLOCK_MONOTONIC, &last_move);
                }
            }
        }

        draw(game);
    }

    cleanup(game);
    return 0;
}
