#include <stdint.h>
#include "address_map_niosv.h"

// Screen dimensions for 160x120 mode
#define MAX_X 160
#define MAX_Y 120
#define YSHIFT 8          // y * 256 + x

typedef uint16_t pixel_t;

/* VGA & IO pointers */
volatile pixel_t  * const pVGA        = (pixel_t  *) FPGA_PIXEL_BUF_BASE;
volatile uint32_t * const pSW        = (uint32_t *) SW_BASE;
volatile uint32_t * const pLEDR      = (uint32_t *) LEDR_BASE;
volatile uint32_t * const pKEY       = (uint32_t *) KEY_BASE;
volatile uint32_t * const pHEX3_HEX0 = (uint32_t *) HEX3_HEX0_BASE;
volatile uint32_t * const pHEX5_HEX4 = (uint32_t *) HEX5_HEX4_BASE;

#define KEY_INTMASK (*(volatile uint32_t *)(KEY_BASE + 0x8))
#define KEY_EDGE    (*(volatile uint32_t *)(KEY_BASE + 0xC))

/* Timer registers (64-bit) */
#define MTIME_LO    (*(volatile uint32_t *)(MTIMER_BASE + 0x0))
#define MTIME_HI    (*(volatile uint32_t *)(MTIMER_BASE + 0x4))
#define MTIMECMP_LO (*(volatile uint32_t *)(MTIMER_BASE + 0x8))
#define MTIMECMP_HI (*(volatile uint32_t *)(MTIMER_BASE + 0xC))

/* ===== CPU timer helpers ===== */

/* Reads MTIME 64-bit value atomically */
static uint64_t mtime_read(void) {
    uint32_t hi1, lo, hi2;
    do {
        hi1 = MTIME_HI;
        lo  = MTIME_LO;
        hi2 = MTIME_HI;
    } while (hi1 != hi2);
    return ((uint64_t)hi2 << 32) | lo;
}

/* Writes MTIMECMP 64-bit value (HIGH then LOW) */
static void mtimecmp_write(uint64_t t) {
    MTIMECMP_HI = (uint32_t)(t >> 32);
    MTIMECMP_LO = (uint32_t)(t & 0xFFFFFFFFu);
}

/* ===== CPU interrupt setup (clean) ===== */

void handler(void) __attribute__((interrupt("machine")));  // forward declaration

static void set_cpu_irqs(uint32_t new_mie_value) {
    const uint32_t MSTATUS_MIE = (1u << 3);
    uintptr_t mtvec_value = (uintptr_t)&handler;

    /* 1. Disable global machine interrupts */
    __asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE));

    /* 2. Install trap/interrupt handler address */
    __asm__ volatile("csrw mtvec, %0" :: "r"(mtvec_value));

    /* 3. Program which IRQ lines are enabled (mie) */
    __asm__ volatile("csrw mie, %0" :: "r"(new_mie_value));

    /* 4. Re-enable global machine interrupts */
    __asm__ volatile("csrs mstatus, %0" :: "r"(MSTATUS_MIE));
}

/* Enable timer IRQ (7) and external KEY IRQ (18) */
static void init_interrupts(void) {
    uint32_t irq_mask = (1u << 7) | (1u << 18);   // timer + KEY
    set_cpu_irqs(irq_mask);
}

/* Game types and global variables */

const pixel_t blk = 0x0000;
const pixel_t wht = 0xffff;
const pixel_t red = 0xf800;
const pixel_t grn = 0x07e0;
const pixel_t blu = 0x001f;

typedef struct {
    int x, y;
    int dx, dy;
    pixel_t colour;
} Player;

typedef enum { TURN_NONE, TURN_LEFT, TURN_RIGHT } TurnPending;
typedef enum { WINNER_NONE, WINNER_HUMAN, WINNER_ROBOT, WINNER_BOTH } Winner;

const uint8_t HEX_DIGITS[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66,
    0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
const uint8_t HEX_BLANK = 0x00;

volatile Player p1, p2;
volatile TurnPending pending_turn = TURN_NONE;
volatile Winner      winner_flag  = WINNER_NONE;
int score_p1 = 0;
int score_p2 = 0;

void delay(int N) {
    for (int i = 0; i < N; i++)
        *pVGA;
}

void drawPixel(int y, int x, pixel_t colour) {
    if (x >= 0 && x < MAX_X && y >= 0 && y < MAX_Y) {
        *(pVGA + (y << YSHIFT) + x) = colour;
    }
}

pixel_t readPixel(int y, int x) {
    if (x < 1 || x >= MAX_X - 1 || y < 1 || y >= MAX_Y - 1) {
        return wht; // Treat border and out-of-bounds as wall
    }
    return *(pVGA + (y << YSHIFT) + x);
}

void rect(int y1, int y2, int x1, int x2, pixel_t c) {
    for (int y = y1; y < y2; y++)
        for (int x = x1; x < x2; x++)
            drawPixel(y, x, c);
}

void clearScreen(void) {
    rect(0, MAX_Y, 0, MAX_X, blk);
}

void drawBorder(pixel_t c) {
    rect(0, 1, 0, MAX_X, c);              // top
    rect(MAX_Y - 1, MAX_Y, 0, MAX_X, c);  // bottom
    rect(0, MAX_Y, 0, 1, c);              // left
    rect(0, MAX_Y, MAX_X - 1, MAX_X, c);  // right
}

void drawObstacles(void) {
    rect(MAX_Y - 10, MAX_Y - 9, MAX_X / 4, MAX_X / 4 + 8, wht);

    rect(MAX_Y / 2, MAX_Y / 2 + 6, MAX_X - 8, MAX_X - 7, wht);

    rect(5, 7, 5, 7, wht);

    rect(MAX_Y - 15, MAX_Y - 14, 10, 15, wht);
    rect(MAX_Y - 14, MAX_Y - 13, 11, 16, wht);
}

int is_safe(int x, int y) {
    pixel_t p = readPixel(y, x);
    return (p == blk);
}

/* ====== Robot AI ====== */

void robot_update_direction(volatile Player *p) {
    int x  = p->x;
    int y  = p->y;
    int dx = p->dx;
    int dy = p->dy;

    int ahead1_safe = is_safe(x + dx,       y + dy);
    int ahead2_safe = is_safe(x + 2 * dx,   y + 2 * dy);

    if (ahead1_safe && ahead2_safe) {
        return;
    }

    int ldx = -dy;
    int ldy = dx;
    int rdx = dy;
    int rdy = -dx;

    int left_safe  = is_safe(x + ldx, y + ldy);
    int right_safe = is_safe(x + rdx, y + rdy);

    if (left_safe) {
        p->dx = ldx;
        p->dy = ldy;
    } else if (right_safe) {
        p->dx = rdx;
        p->dy = rdy;
    }
}

void display_scores(int score_p1, int score_p2) {
    uint8_t d1 = HEX_DIGITS[score_p1 % 10];
    uint8_t d2 = HEX_DIGITS[score_p2 % 10];

    uint32_t value = 0;
    value |= ((uint32_t)d1);              // HEX0
    value |= ((uint32_t)d2) << 8;         // HEX1
    value |= ((uint32_t)HEX_BLANK) << 16; // HEX2
    value |= ((uint32_t)HEX_BLANK) << 24; // HEX3
    *pHEX3_HEX0 = value;

    *pHEX5_HEX4 = (uint32_t)HEX_BLANK | ((uint32_t)HEX_BLANK << 8);
}

static uint64_t game_speed(uint32_t sw) {
    if (sw & 0x8)      return 5000000ULL;
    else if (sw & 0x4) return 8000000ULL;
    else if (sw & 0x2) return 12000000ULL;
    else if (sw & 0x1) return 16000000ULL;
    else               return 50000000ULL;
}

static void update_leds(void) {
    uint32_t v = *pLEDR & ~0x3;       // Clear LEDR[1:0]
    if (pending_turn == TURN_LEFT)  v |= (1u << 1);
    if (pending_turn == TURN_RIGHT) v |= (1u << 0);
    v |= (*pLEDR & (1u << 2));        // keep LED2 state
    *pLEDR = v;
}

void reset(volatile Player *pp1, volatile Player *pp2) {
    clearScreen();
    drawBorder(wht);
    drawObstacles();

    // Player 1 (Human, RED)
    pp1->x = MAX_X / 2;
    pp1->y = MAX_Y / 2 - 10;
    pp1->dx = 1;
    pp1->dy = 0;
    pp1->colour = red;

    // Player 2 (Robot, BLUE)
    pp2->x = MAX_X / 2 - 20;
    pp2->y = MAX_Y / 2 + 10;
    pp2->dx = 1;
    pp2->dy = 0;
    pp2->colour = blu;
}

/* ISRs & interrupt handler */

void isr_timer(void);
void isr_key(void);

/* Machine-level trap handler */
void handler(void) {
    uint32_t cause;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));

    uint32_t is_interrupt = cause >> 31;        // MSB
    uint32_t code         = cause & 0x7FFFFFFF; // bits [30:0]

    if (!is_interrupt) {
        // ignore exceptions (currently)
        return;
    }

    // timer is usually IRQ 7, KEY as some external IRQ.
    if (code == 7) {          // Machine timer IRQ
        isr_timer();
        return;
    }
    if (code == 18) {         // External IRQ for KEY (per system)
        isr_key();
        return;
    }
}

/* KEY ISR */
void isr_key(void) {
    uint32_t edges = KEY_EDGE;
    KEY_EDGE = edges; // clear edge flags

    int key0 = edges & 0x1;
    int key1 = edges & 0x2;

    if (key1) {
        pending_turn = (pending_turn == TURN_LEFT) ? TURN_NONE : TURN_LEFT;
    } else if (key0) {
        pending_turn = (pending_turn == TURN_RIGHT) ? TURN_NONE : TURN_RIGHT;
    }
    update_leds();
}

/* TIMER ISR */
void isr_timer(void) {
    /* Blink on LED[2] */
    static uint32_t blink_div = 0;
    if (++blink_div >= 50) {
        *pLEDR ^= (1u << 2);
        blink_div = 0;
    }

    /* Re-arm timer */
    uint64_t now = mtime_read();
    uint32_t sw  = *pSW;
    mtimecmp_write(now + game_speed(sw));

    /* Apply pending human turn */
    if (pending_turn == TURN_LEFT) {
        int old_dx = p1.dx, old_dy = p1.dy;
        p1.dx = -old_dy;
        p1.dy =  old_dx;
        pending_turn = TURN_NONE;
        update_leds();
    } else if (pending_turn == TURN_RIGHT) {
        int old_dx = p1.dx, old_dy = p1.dy;
        p1.dx =  old_dy;
        p1.dy = -old_dx;
        pending_turn = TURN_NONE;
        update_leds();
    }

    int nextX1 = p1.x + p1.dx;
    int nextY1 = p1.y + p1.dy;

    /* Robot AI */
    robot_update_direction(&p2);
    int nextX2 = p2.x + p2.dx;
    int nextY2 = p2.y + p2.dy;

    /* === HEAD-ON COLLISION CHECKS (by coordinates) === */

    /* Both move into the same square */
    if (nextX1 == nextX2 && nextY1 == nextY2) {
        winner_flag = WINNER_BOTH;   // both die in a head-on
        return;
    }

    /* Swap positions in one tick (P1 -> old P2, P2 -> old P1) */
    if (nextX1 == p2.x && nextY1 == p2.y &&
        nextX2 == p1.x && nextY2 == p1.y) {
        winner_flag = WINNER_BOTH;
        return;
    }

    /* Trail collision logic */
    pixel_t next_pixel1 = readPixel(nextY1, nextX1);
    pixel_t next_pixel2 = readPixel(nextY2, nextX2);

    if (next_pixel1 != blk && next_pixel2 != blk) {
        winner_flag = WINNER_BOTH;
    } else if (next_pixel1 != blk) {
        winner_flag = WINNER_ROBOT;
    } else if (next_pixel2 != blk) {
        winner_flag = WINNER_HUMAN;
    } else {
        // both safe: move and draw
        p1.x = nextX1;
        p1.y = nextY1;
        p2.x = nextX2;
        p2.y = nextY2;

        drawPixel(p1.y, p1.x, p1.colour);
        drawPixel(p2.y, p2.x, p2.colour);
    }
}

int main(void) {
    // Started pattern
    *pLEDR      = 0x0000AAAA;
    *pHEX3_HEX0 = 0x00000000;
    *pHEX5_HEX4 = 0x00000000;

    delay(200000);

    *pLEDR      = 0x00000000;
    *pHEX3_HEX0 = 0x00000000;
    *pHEX5_HEX4 = 0x00000000;

    // Initial game state
    reset(&p1, &p2);
    drawPixel(p1.y, p1.x, p1.colour);
    drawPixel(p2.y, p2.x, p2.colour);
    display_scores(score_p1, score_p2);

    // KEY interrupts (device-side)
    KEY_INTMASK = 0x3;
    KEY_EDGE    = 0x3;

    // First timer event
    uint64_t now = mtime_read();
    mtimecmp_write(now + game_speed(*pSW));

    // CPU-side interrupt setup (clean helpers)
    init_interrupts();

    // LED2 on so timer ISR will blink it
    *pLEDR |= (1u << 2);

    // Small delay before game loop, optional
    delay(1000);

    // Main game loop
    while (1) {
        if (score_p1 >= 9) {
            while (1) { rect(0, MAX_Y, 0, MAX_X, red); }
        } else if (score_p2 >= 9) {
            while (1) { rect(0, MAX_Y, 0, MAX_X, blu); }
        }

        if (winner_flag != WINNER_NONE) {
            if (winner_flag == WINNER_HUMAN) {
                score_p1++;
            } else if (winner_flag == WINNER_ROBOT) {
                score_p2++;
            }

            rect(MAX_Y/2 - 5, MAX_Y/2 + 5, MAX_X/2 - 5, MAX_X/2 + 5, wht);
            delay(500000);

            display_scores(score_p1, score_p2);
            reset(&p1, &p2);
            drawPixel(p1.y, p1.x, p1.colour);
            drawPixel(p2.y, p2.x, p2.colour);

            winner_flag = WINNER_NONE;
        }
    }
}
