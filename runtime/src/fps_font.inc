/* The FPS overlay's 5x7 bitmap font, written for this runtime (no game
 * data). Glyph order: FPS_FONT_CHARS in fps_overlay.h ("0123456789.FPSms CUG%-"). Two words per glyph:
 * rows 0-3 in bytes 0-3 of the first, rows 4-6 in bytes 0-2 of the second (row 0 at the top); bit 4
 * of a row byte is the leftmost column. Valid C and GLSL: included by src/fps_overlay.c and by
 * src/renderer/shaders/fps_overlay.frag, so both read the same table. No trailing comma (GLSL). */
    0x1513110Eu, 0x000E1119u, /* '0' */
    0x04040C04u, 0x000E0404u, /* '1' */
    0x0201110Eu, 0x001F0804u, /* '2' */
    0x0204021Fu, 0x000E1101u, /* '3' */
    0x120A0602u, 0x0002021Fu, /* '4' */
    0x011E101Fu, 0x000E1101u, /* '5' */
    0x1E100806u, 0x000E1111u, /* '6' */
    0x0402011Fu, 0x00080808u, /* '7' */
    0x0E11110Eu, 0x000E1111u, /* '8' */
    0x0F11110Eu, 0x000C0201u, /* '9' */
    0x00000000u, 0x000C0C00u, /* '.' */
    0x1E10101Fu, 0x00101010u, /* 'F' */
    0x1E11111Eu, 0x00101010u, /* 'P' */
    0x0E10100Fu, 0x001E0101u, /* 'S' */
    0x151A0000u, 0x00111115u, /* 'm' */
    0x100E0000u, 0x001E010Eu, /* 's' */
    0x00000000u, 0x00000000u, /* ' ' */
    0x1010110Eu, 0x000E1110u, /* 'C' */
    0x11111111u, 0x000E1111u, /* 'U' */
    0x1710110Eu, 0x000F1111u, /* 'G' */
    0x04021918u, 0x00031308u, /* '%' */
    0x1F000000u, 0x00000000u /* '-' */
