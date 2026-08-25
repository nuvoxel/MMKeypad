/* Embeds firmware-idf/main/logo.png and exposes the _binary_logo_png_{start,end}
 * symbols the ESP build gets from EMBED_FILES, so the sim renders the real
 * NuVoxel wordmark on the Wi-Fi setup / claim / settings screens. ui.c computes
 * the size as (end - start), so `end` MUST sit immediately after the data --
 * .incbin guarantees that (two separate C arrays would not be adjacent). The
 * path is relative to the build cwd (sim/, where make runs). */
__asm__(
    ".section __DATA,__const\n"
    ".globl _binary_logo_png_start\n"
    ".p2align 2\n"
    "_binary_logo_png_start:\n"
    ".incbin \"../../firmware-idf/main/logo.png\"\n"
    ".globl _binary_logo_png_end\n"
    "_binary_logo_png_end:\n"
);
