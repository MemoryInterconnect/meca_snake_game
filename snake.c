#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>

/* Game constants */
#define MAX_SNAKE_LEN 1000
#define BOARD_WIDTH 78
#define BOARD_HEIGHT 18
#define MEM_FILE "/dev/mem"
#define INITIAL_SNAKE_LEN 3
#define BASE_MOVE_INTERVAL_MS 200
#define MIN_MOVE_INTERVAL_MS 50

/* Direction constants */
#define DIR_UP    0
#define DIR_DOWN  1
#define DIR_LEFT  2
#define DIR_RIGHT 3

/* Game state constants */
#define STATE_RUNNING  0
#define STATE_PAUSED   1
#define STATE_GAMEOVER 2

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_BRIGHT_GREEN "\033[92m"
#define COLOR_BG_GREEN "\033[42m"
#define COLOR_BG_RED   "\033[41m"

#define MAGIC_NUMBER    0x12345678

/* Point structure */
typedef struct {
    int32_t x;
    int32_t y;
} Point;

/* Shared game state structure */
typedef struct {
    uint64_t heartbeat;
    uint32_t magic_number;
    uint32_t game_state;
    uint32_t score;
    uint32_t high_score;
    uint32_t snake_length;
    uint32_t direction;
    int32_t food_x;
    int32_t food_y;
    uint32_t takeover_request;    /* 1 = active wants to hand over control */
    Point snake[MAX_SNAKE_LEN];
} GameState;

/* Global variables */
static GameState *g_state = NULL;
static int g_mem_fd = -1;
static struct termios g_orig_termios;
static bool g_terminal_raw = false;
static volatile sig_atomic_t g_running = 1;
static bool g_is_active = false;
static bool g_initiated_takeover = false;  /* True if we pressed 't' */
static const char *g_mem_file = MEM_FILE;  /* mmap file path */
static off_t g_mem_offset = 0x200000000;   /* mmap offset */

/* Function prototypes */
static void cleanup(void);
static void signal_handler(int sig);
static void setup_signals(void);
static void enable_raw_mode(void);
static void disable_raw_mode(void);
static int setup_mmap(void);
static void init_game(void);
static void spawn_food(void);
static void move_snake(void);
static bool check_collision(void);
static void handle_input(void);
static void render(void);
static void render_waiting(void);
static int get_move_interval(void);
static uint64_t get_time_ms(void);
static void clear_screen(void);
static void hide_cursor(void);
static void show_cursor(void);
static void move_cursor(int row, int col);
static int kbhit(void);
static int getch(void);

/* Get current time in milliseconds */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Clear screen */
static void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

/* Hide cursor */
static void hide_cursor(void) {
    printf("\033[?25l");
    fflush(stdout);
}

/* Show cursor */
static void show_cursor(void) {
    printf("\033[?25h");
    fflush(stdout);
}

/* Move cursor to position */
static void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

/* Check if key is available */
static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* Get character without blocking */
static int getch(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        return c;
    }
    return -1;
}

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* Setup signal handlers */
static void setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* Enable raw terminal mode */
static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        perror("tcgetattr");
        exit(1);
    }
    g_terminal_raw = true;

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        exit(1);
    }
}

/* Disable raw terminal mode */
static void disable_raw_mode(void) {
    if (g_terminal_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_terminal_raw = false;
    }
}

/* Cleanup function */
static void cleanup(void) {
    show_cursor();
    disable_raw_mode();
    clear_screen();

    if (g_state != NULL) {
        msync(g_state, sizeof(GameState), MS_SYNC);
        munmap(g_state, sizeof(GameState));
        g_state = NULL;
    }

    if (g_mem_fd != -1) {
        close(g_mem_fd);
        g_mem_fd = -1;
    }
}

/* Setup mmap shared memory */
static int setup_mmap(void) {
    g_mem_fd = open(g_mem_file, O_RDWR | O_CREAT, 0666);
    if (g_mem_fd == -1) {
        perror("open");
        return -1;
    }

    /* Ensure file is correct size (offset + GameState size) */
    struct stat st;
    if (fstat(g_mem_fd, &st) == -1) {
        perror("fstat");
        close(g_mem_fd);
        return -1;
    }

/*    off_t required_size = g_mem_offset + (off_t)sizeof(GameState);
    if (st.st_size < required_size) {
        if (ftruncate(g_mem_fd, required_size) == -1) {
            perror("ftruncate");
            close(g_mem_fd);
            return -1;
        }
    }*/

    g_state = mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_mem_fd, g_mem_offset);
    if (g_state == MAP_FAILED) {
        perror("mmap");
        close(g_mem_fd);
        g_state = NULL;
        return -1;
    }

    if (g_state->magic_number != MAGIC_NUMBER) {
        memset(g_state, 0, sizeof(GameState));
        g_state->magic_number = MAGIC_NUMBER;
    }

    return 0;
}

/* Initialize a new game */
static void init_game(void) {
    g_state->game_state = STATE_RUNNING;
    g_state->score = 0;
    g_state->snake_length = INITIAL_SNAKE_LEN;
    g_state->direction = DIR_RIGHT;

    /* Initialize snake in center */
    int start_x = BOARD_WIDTH / 2;
    int start_y = BOARD_HEIGHT / 2;

    for (uint32_t i = 0; i < g_state->snake_length; i++) {
        g_state->snake[i].x = start_x - i;
        g_state->snake[i].y = start_y;
    }

    spawn_food();
    msync(g_state, sizeof(GameState), MS_SYNC);
}

/* Spawn food at random location */
static void spawn_food(void) {
    bool valid = false;
    int x, y;

    while (!valid) {
        x = rand() % BOARD_WIDTH;
        y = rand() % BOARD_HEIGHT;

        valid = true;
        for (uint32_t i = 0; i < g_state->snake_length; i++) {
            if (g_state->snake[i].x == x && g_state->snake[i].y == y) {
                valid = false;
                break;
            }
        }
    }

    g_state->food_x = x;
    g_state->food_y = y;
}

/* Move the snake */
static void move_snake(void) {
    if (g_state->game_state != STATE_RUNNING) {
        return;
    }

    /* Calculate new head position */
    Point new_head = g_state->snake[0];

    switch (g_state->direction) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
    }

    /* Check wall collision */
    if (new_head.x < 0 || new_head.x >= BOARD_WIDTH ||
        new_head.y < 0 || new_head.y >= BOARD_HEIGHT) {
        g_state->game_state = STATE_GAMEOVER;
        if (g_state->score > g_state->high_score) {
            g_state->high_score = g_state->score;
        }
        msync(g_state, sizeof(GameState), MS_SYNC);
        return;
    }

    /* Check for food collision first */
    bool ate_food = (new_head.x == g_state->food_x && new_head.y == g_state->food_y);

    /* Move body segments */
    if (ate_food) {
        /* Grow snake */
        if (g_state->snake_length < MAX_SNAKE_LEN) {
            /* Shift all segments back */
            for (uint32_t i = g_state->snake_length; i > 0; i--) {
                g_state->snake[i] = g_state->snake[i - 1];
            }
            g_state->snake_length++;
        }
    } else {
        /* Normal move - shift segments */
        for (uint32_t i = g_state->snake_length - 1; i > 0; i--) {
            g_state->snake[i] = g_state->snake[i - 1];
        }
    }

    /* Set new head */
    g_state->snake[0] = new_head;

    /* Check for self collision */
    if (check_collision()) {
        g_state->game_state = STATE_GAMEOVER;
        if (g_state->score > g_state->high_score) {
            g_state->high_score = g_state->score;
        }
    }

    /* Handle food */
    if (ate_food) {
        g_state->score += 10;
        spawn_food();
    }

    msync(g_state, sizeof(GameState), MS_SYNC);
}

/* Check if snake collided with itself */
static bool check_collision(void) {
    Point head = g_state->snake[0];

    for (uint32_t i = 1; i < g_state->snake_length; i++) {
        if (g_state->snake[i].x == head.x && g_state->snake[i].y == head.y) {
            return true;
        }
    }

    return false;
}


/* Get move interval based on score */
static int get_move_interval(void) {
    int interval = BASE_MOVE_INTERVAL_MS - (g_state->score / 50) * 10;
    if (interval < MIN_MOVE_INTERVAL_MS) {
        interval = MIN_MOVE_INTERVAL_MS;
    }
    return interval;
}

/* Handle keyboard input */
static void handle_input(void) {
    while (kbhit()) {
        int c = getch();

        if (c == 'q' || c == 'Q') {
            g_running = 0;
            return;
        }

        if (c == 'p' || c == 'P') {
            if (g_state->game_state == STATE_RUNNING) {
                g_state->game_state = STATE_PAUSED;
            } else if (g_state->game_state == STATE_PAUSED) {
                g_state->game_state = STATE_RUNNING;
            }
            msync(g_state, sizeof(GameState), MS_SYNC);
            continue;
        }

        if (c == 'r' || c == 'R') {
            if (g_state->game_state == STATE_GAMEOVER) {
                init_game();
            }
            continue;
        }

        if (c == 't' || c == 'T') {
            /* Request takeover - hand control to waiting process */
            g_state->takeover_request = 1;
            msync(g_state, sizeof(GameState), MS_SYNC);
            g_is_active = false;  /* Switch to waiting mode */
            g_initiated_takeover = true;  /* Don't respond to our own request */
            return;
        }

        /* Arrow keys (ESC [ A/B/C/D) */
        if (c == 27) {  /* ESC */
            if (kbhit()) {
                c = getch();
                if (c == '[') {
                    if (kbhit()) {
                        c = getch();
                        uint32_t new_dir = g_state->direction;

                        switch (c) {
                            case 'A': /* Up */
                                if (g_state->direction != DIR_DOWN)
                                    new_dir = DIR_UP;
                                break;
                            case 'B': /* Down */
                                if (g_state->direction != DIR_UP)
                                    new_dir = DIR_DOWN;
                                break;
                            case 'C': /* Right */
                                if (g_state->direction != DIR_LEFT)
                                    new_dir = DIR_RIGHT;
                                break;
                            case 'D': /* Left */
                                if (g_state->direction != DIR_RIGHT)
                                    new_dir = DIR_LEFT;
                                break;
                        }

                        if (g_state->game_state == STATE_RUNNING &&
                            new_dir != g_state->direction) {
                            g_state->direction = new_dir;
                            msync(g_state, sizeof(GameState), MS_SYNC);
                        }
                        continue;  /* Don't process arrow key char as WASD */
                    }
                }
            }
            continue;  /* ESC was pressed, skip WASD check */
        }

        /* WASD keys as alternative */
        if (g_state->game_state == STATE_RUNNING) {
            uint32_t new_dir = g_state->direction;

            switch (c) {
                case 'w': case 'W':
                    if (g_state->direction != DIR_DOWN)
                        new_dir = DIR_UP;
                    break;
                case 's': case 'S':
                    if (g_state->direction != DIR_UP)
                        new_dir = DIR_DOWN;
                    break;
                case 'a': case 'A':
                    if (g_state->direction != DIR_RIGHT)
                        new_dir = DIR_LEFT;
                    break;
                case 'd': case 'D':
                    if (g_state->direction != DIR_LEFT)
                        new_dir = DIR_RIGHT;
                    break;
            }

            if (new_dir != g_state->direction) {
                g_state->direction = new_dir;
                msync(g_state, sizeof(GameState), MS_SYNC);
            }
        }
    }
}

/* Render the game board */
static void render(void) {
    move_cursor(1, 1);

    /* Title and score */
    printf("%s========== SNAKE GAME ==========%s\n", COLOR_CYAN, COLOR_RESET);
    printf("Score: %s%u%s  |  High Score: %s%u%s  |  Length: %u\n",
           COLOR_YELLOW, g_state->score, COLOR_RESET,
           COLOR_GREEN, g_state->high_score, COLOR_RESET,
           g_state->snake_length);

    /* Top border */
    printf("%s+", COLOR_WHITE);
    for (int i = 0; i < BOARD_WIDTH; i++) printf("-");
    printf("+%s\n", COLOR_RESET);

    /* Game board */
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        printf("%s|%s", COLOR_WHITE, COLOR_RESET);

        for (int x = 0; x < BOARD_WIDTH; x++) {
            bool is_snake = false;
            bool is_head = false;
            bool is_food = false;

            /* Check if position is snake head */
            if (g_state->snake[0].x == x && g_state->snake[0].y == y) {
                is_head = true;
                is_snake = true;
            }

            /* Check if position is snake body */
            if (!is_head) {
                for (uint32_t i = 1; i < g_state->snake_length; i++) {
                    if (g_state->snake[i].x == x && g_state->snake[i].y == y) {
                        is_snake = true;
                        break;
                    }
                }
            }

            /* Check if position is food */
            if (g_state->food_x == x && g_state->food_y == y) {
                is_food = true;
            }

            if (is_head) {
                printf("%s@%s", COLOR_BRIGHT_GREEN, COLOR_RESET);
            } else if (is_snake) {
                printf("%so%s", COLOR_GREEN, COLOR_RESET);
            } else if (is_food) {
                printf("%s*%s", COLOR_RED, COLOR_RESET);
            } else {
                printf(" ");
            }
        }

        printf("%s|%s\n", COLOR_WHITE, COLOR_RESET);
    }

    /* Bottom border */
    printf("%s+", COLOR_WHITE);
    for (int i = 0; i < BOARD_WIDTH; i++) printf("-");
    printf("+%s\n", COLOR_RESET);

    /* Status and controls - single line to fit 80x24 */
    if (g_state->game_state == STATE_PAUSED) {
        printf("%s*** PAUSED - Press P to resume ***%s", COLOR_YELLOW, COLOR_RESET);
    } else if (g_state->game_state == STATE_GAMEOVER) {
        printf("%s*** GAME OVER - Press R to restart, Q to quit ***%s", COLOR_RED, COLOR_RESET);
    } else {
        printf("Arrows/WASD: Move | P: Pause | T: Transfer | Q: Quit");
    }

    fflush(stdout);
}

/* Render waiting screen with dialog box */
static void render_waiting(void) {
    /* Dialog box dimensions */
    #define DIALOG_WIDTH 50
    #define DIALOG_HEIGHT 9
    int dialog_start_col = (80 - DIALOG_WIDTH) / 2;
    int dialog_start_row = (24 - DIALOG_HEIGHT) / 2;

    move_cursor(1, 1);

    for (int row = 1; row <= 24; row++) {
        move_cursor(row, 1);

        /* Check if this row is part of the dialog */
        int dialog_row = row - dialog_start_row;

        if (dialog_row >= 0 && dialog_row < DIALOG_HEIGHT) {
            /* Print spaces before dialog */
            for (int i = 1; i < dialog_start_col; i++) printf(" ");

            /* Print dialog content */
            if (dialog_row == 0) {
                /* Top border */
                printf("%s+", COLOR_CYAN);
                for (int i = 0; i < DIALOG_WIDTH - 2; i++) printf("-");
                printf("+%s", COLOR_RESET);
            } else if (dialog_row == DIALOG_HEIGHT - 1) {
                /* Bottom border */
                printf("%s+", COLOR_CYAN);
                for (int i = 0; i < DIALOG_WIDTH - 2; i++) printf("-");
                printf("+%s", COLOR_RESET);
            } else {
                /* Content rows */
                printf("%s|%s", COLOR_CYAN, COLOR_RESET);

                char line[DIALOG_WIDTH - 1];
                memset(line, ' ', DIALOG_WIDTH - 2);
                line[DIALOG_WIDTH - 2] = '\0';

                if (dialog_row == 2) {
                    const char *title = "WAITING FOR CONTROL";
                    int title_start = (DIALOG_WIDTH - 2 - (int)strlen(title)) / 2;
                    memcpy(line + title_start, title, strlen(title));
                    printf("%s%s%s", COLOR_YELLOW, line, COLOR_RESET);
                } else if (dialog_row == 4) {
                    const char *msg = "Another process is running the game.";
                    int msg_start = (DIALOG_WIDTH - 2 - (int)strlen(msg)) / 2;
                    memcpy(line + msg_start, msg, strlen(msg));
                    printf("%s", line);
                } else if (dialog_row == 5) {
                    char score_line[64];
                    snprintf(score_line, sizeof(score_line),
                             "Score: %u  |  High Score: %u",
                             g_state->score, g_state->high_score);
                    int score_start = (DIALOG_WIDTH - 2 - (int)strlen(score_line)) / 2;
                    memcpy(line + score_start, score_line, strlen(score_line));
                    printf("%s", line);
                } else if (dialog_row == 7) {
                    const char *quit_msg = "Press Q to quit";
                    int quit_start = (DIALOG_WIDTH - 2 - (int)strlen(quit_msg)) / 2;
                    memcpy(line + quit_start, quit_msg, strlen(quit_msg));
                    printf("%s%s%s", COLOR_WHITE, line, COLOR_RESET);
                } else {
                    printf("%s", line);
                }

                printf("%s|%s", COLOR_CYAN, COLOR_RESET);
            }

            /* Print spaces after dialog */
            for (int i = dialog_start_col + DIALOG_WIDTH; i <= 80; i++) printf(" ");
        } else {
            /* Empty row outside dialog */
            for (int i = 0; i < 80; i++) printf(" ");
        }
    }

    fflush(stdout);
}

/* Print usage */
static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [file] [offset]\n", prog);
    fprintf(stderr, "  file   - mmap file path (default: %s)\n", MEM_FILE);
    fprintf(stderr, "  offset - hex offset in file, e.g. 1000 or 0x1000 (default: 0)\n");
}

/* Main function */
int main(int argc, char *argv[]) {
    /* Parse arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        g_mem_file = argv[1];
    }
    if (argc > 2) {
        char *endptr;
        long long offset = strtoll(argv[2], &endptr, 16);
        if (*endptr != '\0' || offset < 0) {
            fprintf(stderr, "Invalid hex offset: %s\n", argv[2]);
            print_usage(argv[0]);
            return 1;
        }
        g_mem_offset = (off_t)offset;
    }

    /* Initialize random seed */
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    /* Setup cleanup */
    atexit(cleanup);
    setup_signals();

    /* Setup mmap */
    if (setup_mmap() != 0) {
        fprintf(stderr, "Failed to setup shared memory\n");
        return 1;
    }

    /* Enable terminal raw mode */
    enable_raw_mode();
    hide_cursor();
    clear_screen();

    /* Check if another process is active */
    uint64_t initial_heartbeat = g_state->heartbeat;

    printf("Checking for active process...\n");
    fflush(stdout);

    /* Wait 1.5 seconds to check heartbeat */
    usleep(1500000);

    uint64_t current_heartbeat = g_state->heartbeat;

    if (current_heartbeat != initial_heartbeat) {
        /* Another process is active, enter waiting state */
        g_is_active = false;
    } else {
        /* No active process, we become active */
        g_is_active = true;

        /* Initialize new game if needed */
        if (g_state->snake_length == 0) {
            init_game();
        }
    }

    /* Main state loop - can switch between active and waiting */
    while (g_running) {
        if (g_is_active) {
            /* Active game loop */
            clear_screen();

            /* Clear any pending takeover request since we're now active */
            g_state->takeover_request = 0;
            msync(g_state, sizeof(GameState), MS_SYNC);

            uint64_t last_move_time = get_time_ms();
            uint64_t last_heartbeat_time = get_time_ms();

            while (g_running && g_is_active) {
                uint64_t now = get_time_ms();

                /* Handle input (may set g_is_active = false on 't' press) */
                handle_input();

                if (!g_running || !g_is_active) break;

                /* Update heartbeat every 500ms */
                if (now - last_heartbeat_time >= 500) {
                    g_state->heartbeat++;
                    msync(g_state, sizeof(GameState), MS_SYNC);
                    last_heartbeat_time = now;
                }

                /* Move snake at interval */
                int move_interval = get_move_interval();
                if (g_state->game_state == STATE_RUNNING &&
                    now - last_move_time >= (uint64_t)move_interval) {
                    move_snake();
                    last_move_time = now;
                }

                /* Render */
                render();

                /* Small delay to prevent CPU hogging */
                usleep(16000);  /* ~60 FPS */
            }
        } else {
            /* Waiting loop */
            clear_screen();

            uint64_t last_heartbeat = g_state->heartbeat;
            uint64_t last_check_time = get_time_ms();

            while (g_running && !g_is_active) {
                /* Handle quit input */
                while (kbhit()) {
                    int c = getch();
                    if (c == 'q' || c == 'Q') {
                        g_running = 0;
                        break;
                    }
                    /* Handle ESC sequences */
                    if (c == 27) {
                        while (kbhit()) getch();
                    }
                }

                if (!g_running) break;

                /* Check for takeover request (active pressed 't') */
                if (g_state->takeover_request) {
                    if (!g_initiated_takeover) {
                        /* Other process wants to hand over control to us */
                        g_is_active = true;
                        g_state->takeover_request = 0;
                        msync(g_state, sizeof(GameState), MS_SYNC);
                        clear_screen();
                        break;
                    }
                    /* else: We initiated this - wait for other process to respond */
                } else if (g_initiated_takeover) {
                    /* Takeover request was cleared - other process took over */
                    g_initiated_takeover = false;
                }

                /* Check heartbeat every second */
                uint64_t now = get_time_ms();
                if (now - last_check_time >= 1000) {
                    uint64_t current_hb = g_state->heartbeat;

                    if (current_hb == last_heartbeat) {
                        /* Other process died, take over */
                        g_is_active = true;
                        g_initiated_takeover = false;
                        clear_screen();
                        printf("Taking over control...\n");
                        fflush(stdout);
                        usleep(500000);

                        /* Resume from saved state */
                        if (g_state->snake_length == 0) {
                            init_game();
                        }
                        break;
                    }

                    last_heartbeat = current_hb;
                    last_check_time = now;
                }

                render_waiting();
                usleep(100000);  /* 100ms */
            }
        }
    }

    return 0;
}
