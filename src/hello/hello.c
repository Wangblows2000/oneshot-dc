/* SPDX-License-Identifier: GPL-3.0-or-later
 * Milestone 1 toolchain proof: smallest useful KOS program.
 * PVR init, a coloured quad that moves with the controller, debug output. */
#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

static void quad(float x, float y, float w, float h, uint32_t argb) {
    pvr_poly_cxt_t cxt; pvr_poly_hdr_t hdr; pvr_vertex_t v;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    v.z = 1.0f; v.argb = argb; v.oargb = 0; v.u = v.v = 0;
    v.flags = PVR_CMD_VERTEX;     v.x = x;     v.y = y;     pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX;     v.x = x + w; v.y = y;     pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX;     v.x = x;     v.y = y + h; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL; v.x = x + w; v.y = y + h; pvr_prim(&v, sizeof(v));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    pvr_init_defaults();
    pvr_set_bg_color(0.10f, 0.05f, 0.25f);
    printf("OneShot DC toolchain proof: hello from KallistiOS on SH-4\n");
    float x = 288, y = 208;
    for (int frame = 0;; frame++) {
        maple_device_t *c = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (c) {
            cont_state_t *s = (cont_state_t *)maple_dev_status(c);
            if (s) {
                if (s->buttons & CONT_DPAD_LEFT) x -= 4;
                if (s->buttons & CONT_DPAD_RIGHT) x += 4;
                if (s->buttons & CONT_DPAD_UP) y -= 4;
                if (s->buttons & CONT_DPAD_DOWN) y += 4;
            }
        }
        if (frame % 300 == 0) printf("frame %d, quad at %.0f,%.0f\n", frame, x, y);
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        quad(0, 400, 640, 80, 0xFF2A1A4A);
        quad(x, y, 64, 64, 0xFFF0C040);   /* lightbulb yellow */
        pvr_list_finish();
        pvr_scene_finish();
    }
    return 0;
}
