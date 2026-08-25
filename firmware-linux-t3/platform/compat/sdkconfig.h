#pragma once
/* tjpgd (esp_jpeg) compile-time config -- values match esp_jpeg's Kconfig
 * defaults, RGB565 output (all art.c asks for), scaling on (art.c downscales). */
#define CONFIG_JD_SZBUF           512
#define CONFIG_JD_FORMAT          1   /* 1 = RGB565 */
#define CONFIG_JD_USE_SCALE       1
#define CONFIG_JD_TBLCLIP         1
#define CONFIG_JD_FASTDECODE      2
#define CONFIG_JD_DEFAULT_HUFFMAN 1
