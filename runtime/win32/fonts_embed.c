/* Liberation Sans Regular/Bold (SIL Open Font License), embedded for GDI text. */
#ifndef FONT_DIR
#error "FONT_DIR must point at third_party/fonts"
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global g_font_sans\n"
        "g_font_sans:\n"
        ".incbin \"" STR(FONT_DIR) "/LiberationSans-Regular.ttf\"\n"
        "g_font_sans_end:\n"
        ".balign 16\n"
        ".global g_font_sans_bold\n"
        "g_font_sans_bold:\n"
        ".incbin \"" STR(FONT_DIR) "/LiberationSans-Bold.ttf\"\n"
        "g_font_sans_bold_end:\n"
        ".balign 4\n"
        ".global g_font_sans_size\n"
        "g_font_sans_size: .int g_font_sans_end - g_font_sans\n"
        ".global g_font_sans_bold_size\n"
        "g_font_sans_bold_size: .int g_font_sans_bold_end - g_font_sans_bold\n"
        ".text\n");
