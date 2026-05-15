/*
 * Quilon OS -- Host-Side Unit Tests: PS/2 Mouse Driver (section 13)
 *
 * Tests the pure-C helpers in mouse.h:
 *   mouse_packet_dx   -- sign-extend X delta from a 3-byte PS/2 packet
 *   mouse_packet_dy   -- sign-extend Y delta (inverted for screen coords)
 *   mouse_clamp       -- integer clamping
 *
 * Hardware-dependent code (mouse_initialize, mouse_irq_handler) is guarded
 * by #ifdef __is_kernel and is NOT tested here.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"
#include <kernel/mouse.h>

/* -------------------------------------------------------------------------
 * mouse_packet_dx tests
 * ---------------------------------------------------------------------- */

static void test_dx_positive(void)
{
    /* byte 0 bit 4 = 0 -> positive delta; dx = pkt[1] */
    uint8_t pkt[3] = { 0x08, 5, 0 };
    ASSERT_EQ(mouse_packet_dx(pkt), 5, "dx positive: pkt[1]=5 -> +5");
}

static void test_dx_zero(void)
{
    uint8_t pkt[3] = { 0x08, 0, 0 };
    ASSERT_EQ(mouse_packet_dx(pkt), 0, "dx zero: pkt[1]=0 -> 0");
}

static void test_dx_max_positive(void)
{
    /* maximum positive without sign bit: 255 */
    uint8_t pkt[3] = { 0x08, 255, 0 };
    ASSERT_EQ(mouse_packet_dx(pkt), 255, "dx max positive: pkt[1]=255 -> 255");
}

static void test_dx_negative_minus_one(void)
{
    /* pkt[1] = 255, X sign set -> 255 - 256 = -1 */
    uint8_t pkt[3] = { (uint8_t)(0x08u | MOUSE_X_SIGN), 255, 0 };
    ASSERT_EQ(mouse_packet_dx(pkt), -1, "dx: pkt[1]=255, sign -> -1");
}

static void test_dx_negative_small(void)
{
    /* pkt[1] = 1, X sign set -> 1 - 256 = -255 */
    uint8_t pkt[3] = { (uint8_t)(0x08u | MOUSE_X_SIGN), 1, 0 };
    ASSERT_EQ(mouse_packet_dx(pkt), -255, "dx: pkt[1]=1, sign -> -255");
}

static void test_dx_negative_128(void)
{
    /* pkt[1] = 128, X sign set -> 128 - 256 = -128 */
    uint8_t pkt[3] = { (uint8_t)(0x08u | MOUSE_X_SIGN), 128, 0 };
    ASSERT_EQ(mouse_packet_dx(pkt), -128, "dx: pkt[1]=128, sign -> -128");
}

/* -------------------------------------------------------------------------
 * mouse_packet_dy tests (inverted: PS/2 positive = up, screen positive = down)
 * ---------------------------------------------------------------------- */

static void test_dy_positive_raw_inverted(void)
{
    /* No Y sign bit: pkt[2]=5 -> raw=5, inverted -> -5 */
    uint8_t pkt[3] = { 0x08, 0, 5 };
    ASSERT_EQ(mouse_packet_dy(pkt), -5, "dy: pkt[2]=5, no sign -> -5 (inverted)");
}

static void test_dy_zero(void)
{
    uint8_t pkt[3] = { 0x08, 0, 0 };
    ASSERT_EQ(mouse_packet_dy(pkt), 0, "dy: pkt[2]=0 -> 0");
}

static void test_dy_sign_set_minus_one_raw(void)
{
    /* pkt[2] = 255, Y sign set -> raw = 255 - 256 = -1, inverted = +1 */
    uint8_t pkt[3] = { (uint8_t)(0x08u | MOUSE_Y_SIGN), 0, 255 };
    ASSERT_EQ(mouse_packet_dy(pkt), 1, "dy: pkt[2]=255, sign -> +1 (inverted -1)");
}

static void test_dy_sign_set_minus_128_raw(void)
{
    /* pkt[2] = 128, Y sign set -> raw = -128, inverted = +128 */
    uint8_t pkt[3] = { (uint8_t)(0x08u | MOUSE_Y_SIGN), 0, 128 };
    ASSERT_EQ(mouse_packet_dy(pkt), 128, "dy: pkt[2]=128, sign -> +128 (inverted)");
}

static void test_dy_max_positive_raw(void)
{
    /* pkt[2] = 100, no sign -> raw = 100, inverted = -100 */
    uint8_t pkt[3] = { 0x08, 0, 100 };
    ASSERT_EQ(mouse_packet_dy(pkt), -100, "dy: pkt[2]=100, no sign -> -100");
}

/* -------------------------------------------------------------------------
 * mouse_clamp tests
 * ---------------------------------------------------------------------- */

static void test_clamp_in_range(void)
{
    ASSERT_EQ(mouse_clamp(5, 0, 10), 5, "clamp: 5 in [0,10] -> 5");
}

static void test_clamp_at_lo(void)
{
    ASSERT_EQ(mouse_clamp(0, 0, 10), 0, "clamp: 0 at lo -> 0");
}

static void test_clamp_at_hi(void)
{
    ASSERT_EQ(mouse_clamp(10, 0, 10), 10, "clamp: 10 at hi -> 10");
}

static void test_clamp_below_lo(void)
{
    ASSERT_EQ(mouse_clamp(-1, 0, 799), 0, "clamp: -1 below 0 -> 0");
}

static void test_clamp_above_hi(void)
{
    ASSERT_EQ(mouse_clamp(800, 0, 799), 799, "clamp: 800 above 799 -> 799");
}

static void test_clamp_negative_range(void)
{
    ASSERT_EQ(mouse_clamp(-50, -100, -10), -50, "clamp: -50 in [-100,-10] -> -50");
}

static void test_clamp_below_negative_lo(void)
{
    ASSERT_EQ(mouse_clamp(-200, -100, -10), -100, "clamp: -200 < -100 -> -100");
}

static void test_clamp_above_negative_hi(void)
{
    ASSERT_EQ(mouse_clamp(0, -100, -10), -10, "clamp: 0 > -10 -> -10");
}

static void test_clamp_equal_bounds(void)
{
    ASSERT_EQ(mouse_clamp(0, 5, 5), 5, "clamp: 0, equal bounds [5,5] -> 5");
    ASSERT_EQ(mouse_clamp(10, 5, 5), 5, "clamp: 10, equal bounds [5,5] -> 5");
}

/* -------------------------------------------------------------------------
 * Packet flag constant tests
 * ---------------------------------------------------------------------- */

static void test_btn_flags_distinct(void)
{
    ASSERT((MOUSE_BTN_LEFT & MOUSE_BTN_RIGHT)   == 0, "BTN_LEFT & BTN_RIGHT == 0");
    ASSERT((MOUSE_BTN_LEFT & MOUSE_BTN_MIDDLE)  == 0, "BTN_LEFT & BTN_MIDDLE == 0");
    ASSERT((MOUSE_BTN_RIGHT & MOUSE_BTN_MIDDLE) == 0, "BTN_RIGHT & BTN_MIDDLE == 0");
}

static void test_flag_values(void)
{
    ASSERT_EQ((int)MOUSE_START_BIT,  0x08, "MOUSE_START_BIT == 0x08");
    ASSERT_EQ((int)MOUSE_X_SIGN,     0x10, "MOUSE_X_SIGN    == 0x10");
    ASSERT_EQ((int)MOUSE_Y_SIGN,     0x20, "MOUSE_Y_SIGN    == 0x20");
    ASSERT_EQ((int)MOUSE_X_OVERFLOW, 0x40, "MOUSE_X_OVERFLOW == 0x40");
    ASSERT_EQ((int)MOUSE_Y_OVERFLOW, 0x80, "MOUSE_Y_OVERFLOW == 0x80");
}

static void test_btn_flag_values(void)
{
    ASSERT_EQ((int)MOUSE_BTN_LEFT,   0x01, "MOUSE_BTN_LEFT   == 0x01");
    ASSERT_EQ((int)MOUSE_BTN_RIGHT,  0x02, "MOUSE_BTN_RIGHT  == 0x02");
    ASSERT_EQ((int)MOUSE_BTN_MIDDLE, 0x04, "MOUSE_BTN_MIDDLE == 0x04");
}

/* -------------------------------------------------------------------------
 * Combined motion tests
 * ---------------------------------------------------------------------- */

static void test_combined_right_up(void)
{
    /* Moving right (+X) and up (+Y PS/2 = down screen Y becomes negative) */
    uint8_t pkt[3] = { 0x08, 10, 5 };
    ASSERT_EQ(mouse_packet_dx(pkt),  10, "combined: dx=+10");
    ASSERT_EQ(mouse_packet_dy(pkt),  -5, "combined: dy=-5 (screen down is negative)");
}

static void test_combined_left_down(void)
{
    /* Moving left: X sign set, pkt[1]=2 -> dx=-254
     * Moving down: Y sign set, pkt[2]=254 -> raw=-2, inverted=+2 */
    uint8_t pkt[3] = {
        (uint8_t)(0x08u | MOUSE_X_SIGN | MOUSE_Y_SIGN), 2, 254
    };
    ASSERT_EQ(mouse_packet_dx(pkt), -254, "combined: dx=-254");
    ASSERT_EQ(mouse_packet_dy(pkt),    2, "combined: dy=+2 (screen down)");
}

static void test_buttons_all_pressed(void)
{
    uint8_t pkt[3] = { (uint8_t)(0x08u | 0x07u), 0, 0 };
    int btns = (int)(pkt[0] & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));
    ASSERT_EQ(btns, 0x07, "all three buttons: mask == 0x07");
}

static void test_buttons_only_left(void)
{
    uint8_t pkt[3] = { (uint8_t)(0x08u | MOUSE_BTN_LEFT), 0, 0 };
    int btns = (int)(pkt[0] & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));
    ASSERT_EQ(btns, (int)MOUSE_BTN_LEFT, "only left button: mask == BTN_LEFT");
}

/* -------------------------------------------------------------------------
 * Cursor constants
 * ---------------------------------------------------------------------- */

static void test_cursor_dimensions(void)
{
    ASSERT_EQ((int)MOUSE_CURSOR_W, 8, "MOUSE_CURSOR_W == 8");
    ASSERT_EQ((int)MOUSE_CURSOR_H, 8, "MOUSE_CURSOR_H == 8");
}

/* -------------------------------------------------------------------------
 * Overflow detection
 * ---------------------------------------------------------------------- */

static void test_overflow_bits_detectable(void)
{
    uint8_t xovf[3] = { (uint8_t)(0x08u | MOUSE_X_OVERFLOW), 0, 0 };
    uint8_t yovf[3] = { (uint8_t)(0x08u | MOUSE_Y_OVERFLOW), 0, 0 };
    uint8_t norm[3] = { 0x08, 10, 5 };

    ASSERT(xovf[0] & MOUSE_X_OVERFLOW, "X overflow packet: flag is set");
    ASSERT(yovf[0] & MOUSE_Y_OVERFLOW, "Y overflow packet: flag is set");
    ASSERT(!(norm[0] & MOUSE_X_OVERFLOW), "normal packet: X overflow not set");
    ASSERT(!(norm[0] & MOUSE_Y_OVERFLOW), "normal packet: Y overflow not set");
}

/* -------------------------------------------------------------------------
 * Runner
 * ---------------------------------------------------------------------- */

int main(void)
{
    RUN_SUITE(test_dx_positive);
    RUN_SUITE(test_dx_zero);
    RUN_SUITE(test_dx_max_positive);
    RUN_SUITE(test_dx_negative_minus_one);
    RUN_SUITE(test_dx_negative_small);
    RUN_SUITE(test_dx_negative_128);

    RUN_SUITE(test_dy_positive_raw_inverted);
    RUN_SUITE(test_dy_zero);
    RUN_SUITE(test_dy_sign_set_minus_one_raw);
    RUN_SUITE(test_dy_sign_set_minus_128_raw);
    RUN_SUITE(test_dy_max_positive_raw);

    RUN_SUITE(test_clamp_in_range);
    RUN_SUITE(test_clamp_at_lo);
    RUN_SUITE(test_clamp_at_hi);
    RUN_SUITE(test_clamp_below_lo);
    RUN_SUITE(test_clamp_above_hi);
    RUN_SUITE(test_clamp_negative_range);
    RUN_SUITE(test_clamp_below_negative_lo);
    RUN_SUITE(test_clamp_above_negative_hi);
    RUN_SUITE(test_clamp_equal_bounds);

    RUN_SUITE(test_btn_flags_distinct);
    RUN_SUITE(test_flag_values);
    RUN_SUITE(test_btn_flag_values);

    RUN_SUITE(test_combined_right_up);
    RUN_SUITE(test_combined_left_down);
    RUN_SUITE(test_buttons_all_pressed);
    RUN_SUITE(test_buttons_only_left);

    RUN_SUITE(test_cursor_dimensions);
    RUN_SUITE(test_overflow_bits_detectable);

    TEST_SUMMARY();
}
