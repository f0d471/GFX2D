#include "board.h"

static int s_product_keys;

void board_product_keys(int enabled)
{
    s_product_keys = enabled != 0;
    board_input_window_policy(!s_product_keys);
}

int board_product_mode(void) { return s_product_keys; }

uint32_t board_keys(void)
{
    uint32_t physical=0;
    uint32_t remote=board_input_raw(&physical);
    return board_map_physical_keys(physical)|remote;
}

uint32_t board_map_physical_keys(uint32_t keys)
{
    if (!s_product_keys)
        return keys;
    if ((keys & (BOARD_KEY_LEFT | BOARD_KEY_RIGHT)) ==
                (BOARD_KEY_LEFT | BOARD_KEY_RIGHT)) {
        keys &= ~(BOARD_KEY_LEFT | BOARD_KEY_RIGHT);
        keys |= BOARD_KEY_CONFIRM;
    }
    if ((keys & (BOARD_KEY_UP | BOARD_KEY_DOWN)) ==
                (BOARD_KEY_UP | BOARD_KEY_DOWN)) {
        keys &= ~(BOARD_KEY_UP | BOARD_KEY_DOWN);
        keys |= BOARD_KEY_BACK;
    }
    return keys;
}
