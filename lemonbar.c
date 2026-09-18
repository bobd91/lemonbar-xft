// vim:sw=4:ts=4:et:
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/randr.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include "utils.h"

// Here be dragons

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

typedef struct font_t {
    int descent, height, width;
    uint32_t char_max;
    uint32_t char_min;
    XftFont* xft_font;
} font_t;

typedef struct bar_t {
    int x, y, width, height;
    xcb_window_t window;
    xcb_pixmap_t pixmap;
} bar_t;

typedef struct monitor_t {
    char *name;
    int x, y, width, height;
    bar_t *bar;
    struct monitor_t *prev, *next;
} monitor_t;

typedef struct area_t {
    unsigned begin;
    unsigned end;
    bool complete;
    unsigned align;
    unsigned button;
    xcb_window_t window;
    char *cmd;
} area_t;

typedef union rgba_t {
    struct {
        uint8_t b;
        uint8_t g;
        uint8_t r;
        uint8_t a;
    };
    uint32_t v;
} rgba_t;

typedef struct area_stack_t {
    area_t *ptr;
    unsigned int index, alloc;
} area_stack_t;

enum {
    ATTR_OVERL = (1<<0),
    ATTR_UNDERL = (1<<1),
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

enum {
    GC_DRAW = 0,
    GC_CLEAR,
    GC_ATTR,
    GC_MAX
};

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_gcontext_t gc[GC_MAX];
static xcb_visualid_t visual;
static xcb_colormap_t colormap;
static monitor_t *monhead, *montail;
static font_t **font_list = NULL;
static int font_count = 0;
static int font_index = -1;
static uint32_t attrs = 0;
static bool dock = false;
static bool topbar = true;
//static int bw = -1, bh = -1, bx = 0, by = 0;
static int bar_height;
static int bu = 1; // Underline height
static rgba_t fgc, bgc, ugc;
static rgba_t dfgc, dbgc, dugc;
static area_stack_t area_stack;

static const rgba_t BLACK = (rgba_t){ .r = 0, .g = 0, .b = 0, .a = 255 };
static const rgba_t WHITE = (rgba_t){ .r = 255, .g = 255, .b = 255, .a = 255 };

static int num_outputs = 0;
static char **output_names = NULL;

#define MAX_WIDTHS (1 << 16)
static wchar_t xft_char[MAX_WIDTHS];
static char    xft_width[MAX_WIDTHS];
static Display *dpy;
static XftDraw *xft_draw;
static Visual *visual_ptr;
static XftColor sel_fg;
static int screen_number = 0;

void
update_gc (void)
{
    xcb_change_gc(c, gc[GC_DRAW], XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });
    xcb_change_gc(c, gc[GC_CLEAR], XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });
    xcb_change_gc(c, gc[GC_ATTR], XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });
    XftColorFree(dpy, visual_ptr, colormap , &sel_fg);
    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);
    if (!XftColorAllocName (dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }
}

void
fill_rect (xcb_drawable_t d, xcb_gcontext_t _gc, int x, int y, int width, int height)
{
    xcb_poly_fill_rectangle(c, d, _gc, 1, (const xcb_rectangle_t []){ { x, y, width, height } });
}

int
shift (bar_t *bar, int x, int align, int ch_width)
{
    switch (align) {
        case ALIGN_C:
            xcb_copy_area(c, bar->pixmap, bar->pixmap, gc[GC_DRAW],
                    bar->width / 2 - x / 2, 0,
                    bar->width / 2 - (x + ch_width) / 2, 0,
                    x, bar->height);
            x = bar->width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            xcb_copy_area(c, bar->pixmap, bar->pixmap, gc[GC_DRAW],
                    bar->width - x, 0,
                    bar->width - x - ch_width, 0,
                    x, bar->height);
            x = bar->width - ch_width;
            break;
    }

    // Draw the background first
    fill_rect(bar->pixmap, gc[GC_CLEAR], x, 0, ch_width, bar->height);
    return x;
}

void
draw_lines (bar_t *bar, int x, int w)
{
    /* We can render both at the same time */
    if (attrs & ATTR_OVERL)
        fill_rect(bar->pixmap, gc[GC_ATTR], x, 0, w, bu);
    if (attrs & ATTR_UNDERL)
        fill_rect(bar->pixmap, gc[GC_ATTR], x, bar->height - bu, w, bu);
}

void
draw_shift (bar_t *bar, int x, int align, int w)
{
    x = shift(bar, x, align, w);
    draw_lines(bar, x, w);
}

int xft_char_width_slot (uint32_t ch) {
    int slot = ch % MAX_WIDTHS;
    while (xft_char[slot] != 0 && xft_char[slot] != ch) {
        slot = (slot + 1) % MAX_WIDTHS;
    }
    return slot;
}

int xft_char_width (uint32_t ch, font_t *cur_font) {
    int slot = xft_char_width_slot(ch);
    if (!xft_char[slot]) {
        XGlyphInfo gi;
        FT_UInt glyph = XftCharIndex (dpy, cur_font->xft_font, (FcChar32) ch);
        XftFontLoadGlyphs (dpy, cur_font->xft_font, FcFalse, &glyph, 1);
        XftGlyphExtents (dpy, cur_font->xft_font, &glyph, 1, &gi);
        XftFontUnloadGlyphs (dpy, cur_font->xft_font, &glyph, 1);
        xft_char[slot] = ch;
        if (gi.xOff >= gi.width) {
            xft_width[slot] = gi.xOff;
        } else {
            xft_width[slot] = gi.width;
        }
        return xft_width[slot];
    } else if (xft_char[slot] == ch) {
        return xft_width[slot];
    } else {
        return 0;
    }
}

int draw_char(bar_t *bar, font_t *cur_font, int x, int align, uint32_t ch) {
    int ch_width = xft_char_width(ch, cur_font);
    x = shift(bar, x, align, ch_width);
    int y = bar->height / 2 + cur_font->height / 2- cur_font->descent;
    XftDrawString32(xft_draw, &sel_fg, cur_font->xft_font, x, y, &ch, 1);
    draw_lines(bar, x, ch_width);
    return ch_width;
}

rgba_t
parse_color (const char *str, char **end, const rgba_t def)
{
    int string_len;
    char *ep;

    if (!str)
        return def;

    // Reset
    if (str[0] == '-') {
        if (end)
            *end = (char *)str + 1;

        return def;
    }

    // Hex representation
    if (str[0] != '#') {
        if (end)
            *end = (char *)str;

        fprintf(stderr, "Invalid color specified\n");
        return def;
    }

    errno = 0;
    rgba_t tmp = (rgba_t)(uint32_t)strtoul(str + 1, &ep, 16);

    if (end)
        *end = ep;

    // Some error checking is definitely good
    if (errno) {
        fprintf(stderr, "Invalid color specified\n");
        return def;
    }

    string_len = ep - (str + 1);

    switch (string_len) {
        case 3:
            // Expand the #rgb format into #rrggbb (aa is set to 0xff)
            tmp.v = (tmp.v & 0xf00) * 0x1100
                  | (tmp.v & 0x0f0) * 0x0110
                  | (tmp.v & 0x00f) * 0x0011;
        case 6:
            // If the code is in #rrggbb form then assume it's opaque
            tmp.a = 255;
            break;
        case 7:
        case 8:
            // Colors in #aarrggbb format, those need no adjustments
            break;
        default:
            fprintf(stderr, "Invalid color specified\n");
            return def;
    }

    // Premultiply the alpha in
    if (tmp.a) {
        // The components are clamped automagically as the rgba_t is made of uint8_t
        return (rgba_t){
            .r = (tmp.r * tmp.a) / 255,
            .g = (tmp.g * tmp.a) / 255,
            .b = (tmp.b * tmp.a) / 255,
            .a = tmp.a,
        };
    }

    return (rgba_t)0U;
}

void
set_attribute (const char modifier, const char attribute)
{
    uint32_t mask;

    switch (attribute) {
        case 'o': mask = ATTR_OVERL;  break;
        case 'u': mask = ATTR_UNDERL; break;
        default:
            fprintf(stderr, "Invalid attribute \"%c\" found\n", attribute);
            return;
    }

    switch (modifier) {
        case '+': attrs |=  mask; break;
        case '-': attrs &= ~mask; break;
        case '!': attrs ^=  mask; break;
    }
}


area_t *
area_get (xcb_window_t win, const int btn, const int x)
{
    // Looping backwards ensures that we get the innermost area first
    for (int i = area_stack.index - 1; i >= 0; i--) {
        area_t *a = &area_stack.ptr[i];
        if (a->window == win && a->button == btn && x >= a->begin && x < a->end)
            return a;
    }
    return NULL;
}

void
area_shift (xcb_window_t win, const int align, int delta)
{
    if (align == ALIGN_L)
        return;
    if (align == ALIGN_C)
        delta /= 2;

    for (int i = 0; i < area_stack.index; i++) {
        area_t *a = &area_stack.ptr[i];
        if (a->window == win && a->align == align && !a->complete) {
            a->begin -= delta;
            a->end -= delta;
        }
    }
}

bool
area_add (char *str, const char *optend, char **end, bar_t *bar, const int x, const int align, const int button)
{
    int i;
    char *trail;
    area_t *a;

    // A wild close area tag appeared!
    if (*str != ':') {
        *end = str;

        // Find most recent unclosed area.
        for (i = area_stack.index - 1; i >= 0 && !area_stack.ptr[i].complete; i--)
            ;
        a = &area_stack.ptr[i];

        // Basic safety checks
        if (!a->cmd || a->align != align || a->window != bar->window) {
            fprintf(stderr, "Invalid geometry for the clickable area\n");
            return false;
        }

        const int size = x - a->begin;

        switch (align) {
            case ALIGN_L:
                a->end = x;
                break;
            case ALIGN_C:
                a->begin = bar->width / 2 - size / 2 + a->begin / 2;
                a->end = a->begin + size;
                break;
            case ALIGN_R:
                // The newest is the rightmost one
                a->begin = bar->width - size;
                a->end = bar->width;
                break;
        }

        a->complete = false;
        return true;
    }

    if (area_stack.index + 1 > area_stack.alloc) {
        area_stack.ptr = xreallocarray(area_stack.ptr, area_stack.index + 1,
                sizeof(area_t));
        area_stack.alloc += 1;
    }
    a = &area_stack.ptr[area_stack.index++];

    // Found the closing : and check if it's just an escaped one
    for (trail = strchr(++str, ':'); trail && trail[-1] == '\\'; trail = strchr(trail + 1, ':'))
        ;

    // Find the trailing : and make sure it's within the formatting block, also reject empty commands
    if (!trail || str == trail || trail > optend) {
        *end = str;
        return false;
    }

    *trail = '\0';

    // Sanitize the user command by unescaping all the :
    for (char *needle = str; *needle; needle++) {
        int delta = trail - &needle[1];
        if (needle[0] == '\\' && needle[1] == ':') {
            memmove(&needle[0], &needle[1], delta);
            needle[delta] = 0;
        }
    }

    // This is a pointer to the string buffer allocated in the main
    a->cmd = str;
    a->complete = true;
    a->align = align;
    a->begin = x;
    a->window = bar->window;
    a->button = button;

    *end = trail + 1;

    return true;
}

bool
font_has_glyph (font_t *font, const uint32_t c)
{
    if (XftCharExists(dpy, font->xft_font, c)) {
        return true;
    } else {
        return false;
    }
}

// returns NULL if character cannot be printed
font_t *
select_drawable_font (const uint32_t c)
{
    // If the user has specified a font to use, try that first.
    if (font_index != -1 && font_has_glyph(font_list[font_index - 1], c))
        return font_list[font_index - 1];

    // If the end is reached without finding an appropriate font, return NULL.
    // If the font can draw the character, return it.
    for (int i = 0; i < font_count; i++) {
        if (font_has_glyph(font_list[i], c))
            return font_list[i];
    }
    return NULL;
}

int
pos_to_absolute(bar_t *bar, int pos, int align)
{
    switch (align) {
        case ALIGN_L: return pos;
        case ALIGN_R: return bar->width - pos;
        case ALIGN_C: return bar->width / 2 + pos / 2;
    }

    return 0;
}

void
parse (char *text)
{
    font_t *cur_font;
    monitor_t *cur_mon;
    int pos_x, align, button;
    char *p = text, *block_end, *ep;
    size_t textlen = strlen(text);

    pos_x = 0;
    align = ALIGN_L;
    cur_mon = monhead;

    // Reset the default color set
    bgc = dbgc;
    fgc = dfgc;
    ugc = dugc;
    update_gc();
    // Reset the default attributes
    attrs = 0;

    // Reset the stack position
    area_stack.index = 0;

    // Open XDraw
    xft_draw = XftDrawCreate(dpy, cur_mon->bar->pixmap, visual_ptr, colormap);
    if (!xft_draw) {
        fprintf(stderr, "Couldn't create xft drawable\n");
    }

    for (monitor_t *m = monhead; m != NULL; m = m->next)
        fill_rect(m->bar->pixmap, gc[GC_CLEAR], 0, 0, m->bar->width, m->bar->height);

    for (;;) {
        if (*p == '\0' || *p == '\n')
            return;

        if (p[0] == '%' && p[1] == '{' && (block_end = strchr(p++, '}'))) {
            p++;
            while (p < block_end) {
                while (isspace(*p))
                    p++;

                switch (*p++) {
                    // Enable/disable attributes.
                    case '+': set_attribute('+', *p++); break;
                    case '-': set_attribute('-', *p++); break;
                    case '!': set_attribute('!', *p++); break;

                    // Reverse foreground/background color.
                    case 'R': {
                        rgba_t tmp = fgc;
                        fgc = bgc;
                        bgc = tmp;
                        update_gc();
                    } break;

                    // Alignment specifiers.
                    // Keep track of where we are and where we're moving to so
                    // that underlines/overlines are correctly drawn over the
                    // empty space.
                    case 'l': {
                        int left_ep = 0;
                        int right_ep = pos_to_absolute(cur_mon->bar, pos_x, align);
                        draw_lines(cur_mon->bar, left_ep, right_ep - left_ep);
                        pos_x = 0; align = ALIGN_L;
                    } break;
                    case 'c': {
                        int left_ep = pos_to_absolute(cur_mon->bar, pos_x, align);
                        int right_ep = cur_mon->width / 2;
                        if (right_ep < left_ep) {
                            int tmp = left_ep;
                            left_ep = right_ep;
                            right_ep = tmp;
                        }
                        draw_lines(cur_mon->bar, left_ep, right_ep - left_ep);
                        pos_x = 0; align = ALIGN_C;
                    } break;
                    case 'r': {
                        int left_ep = pos_to_absolute(cur_mon->bar, pos_x, align);
                        int right_ep = cur_mon->width;
                        if (right_ep < left_ep) {
                            int tmp = left_ep;
                            left_ep = right_ep;
                            right_ep = tmp;
                        }
                        draw_lines(cur_mon->bar, left_ep, right_ep - left_ep);
                        pos_x = 0; align = ALIGN_R;
                    } break;

                    // Define input area.
                    case 'A': {
                        button = XCB_BUTTON_INDEX_1;
                        // The range is 1-5
                        if (isdigit(*p) && (*p > '0' && *p < '6'))
                            button = *p++ - '0';
                        if (!area_add(p, block_end, &p, cur_mon->bar, pos_x, align, button))
                            return;
                    } break;

                    // Set background/foreground/underline color.
                    case 'B': bgc = parse_color(p, &p, dbgc); update_gc(); break;
                    case 'F': fgc = parse_color(p, &p, dfgc); update_gc(); break;
                    case 'U': ugc = parse_color(p, &p, dugc); update_gc(); break;

                    // Set current monitor used for drawing.
                    case 'S': {
                        monitor_t *orig_mon = cur_mon;

                        switch (*p) {
                            case '+': // Next monitor.
                                if (cur_mon->next) cur_mon = cur_mon->next;
                                p += 1;
                                break;
                            case '-': // Previous monitor.
                                if (cur_mon->prev) cur_mon = cur_mon->prev;
                                p += 1;
                                break;
                            case 'f': // First monitor.
                                cur_mon = monhead;
                                p += 1;
                                break;
                            case 'l': // Last monitor.
                                cur_mon = montail ? montail : monhead;
                                p += 1;
                                break;
                            case 'n': { // Named monitor.
                                const size_t name_len = block_end - (p + 1);
                                cur_mon = monhead;
                                while (cur_mon) {
                                    if (cur_mon->name &&
                                            !strncmp(cur_mon->name, p + 1, name_len) &&
                                            cur_mon->name[name_len] == '\0')
                                        break;
                                    cur_mon = cur_mon->next;
                                }
                                if (!cur_mon) cur_mon = orig_mon;
                                p += 1 + name_len;
                            } break;
                            case '0' ... '9': // Numbered monitor.
                                cur_mon = monhead;
                                for (int i = 0; i != *p-'0' && cur_mon->next; i++)
                                    cur_mon = cur_mon->next;
                                p += 1;
                                break;
                            default:
                                fprintf(stderr, "Unknown S specifier '%c'\n", *p++);
                                break;
                        }

                        if (orig_mon != cur_mon) {
                            pos_x = 0;
                            align = ALIGN_L;
                        }

                        XftDrawDestroy(xft_draw);
                        xft_draw = XftDrawCreate(dpy, cur_mon->bar->pixmap, visual_ptr, colormap);
                        if (!xft_draw) {
                            fprintf(stderr, "Couldn't create xft drawable\n");
                        }
                    } break;

                    // Draw a N-pixel wide empty character.
                    case 'O': {
                        errno = 0;
                        int w = (int) strtoul(p, &p, 10);
                        if (errno)
                            continue;

                        draw_shift(cur_mon->bar, pos_x, align, w);

                        pos_x += w;
                        area_shift(cur_mon->bar->window, align, w);
                    } break;

                    case 'T': {
                          if (*p == '-') {
                              // Switch to automatic font selection.
                              font_index = -1;
                              p++;
                          } else if (isdigit(*p)) {
                              font_index = (int)strtoul(p, &ep, 10);
                              // User-specified 'font_index' ∊ (0,font_count]
                              // Otherwise just fallback to the automatic font selection
                              if (!font_index || font_index > font_count) {
                                  fprintf(stderr, "Invalid font index %d\n", font_index);
                                  font_index = -1;
                              }
                              p = ep;
                          } else {
                              // Swallow the invalid character and keep parsing.
                              fprintf(stderr, "Invalid font slot \"%c\"\n", *p++);
                          }
                    } break;

                    // In case of error keep parsing after the closing }
                    default:
                        p = block_end;
                }
            }
            // Eat the trailing }
            p++;
        } else { // utf-8 -> ucs-2
            // Escaped % symbol, eat the first one
            if (p[0] == '%' && p[1] == '%')
                p++;

            uint8_t *utf = (uint8_t *)p;
            uint32_t ucs;

            int len = FcUtf8ToUcs4(utf, &ucs, textlen - (p - text) );
            p += len;

            cur_font = select_drawable_font(ucs);
            if (!cur_font)
                continue;

            int w = draw_char(cur_mon->bar, cur_font, pos_x, align, ucs);

            pos_x += w;
            area_shift(cur_mon->bar->window, align, w);
        }
    }
    XftDrawDestroy(xft_draw);
}

int
font_load(const char *pattern) {
    XftFont *xft_font = XftFontOpenName(dpy, screen_number, pattern);
    if (!xft_font) {
        return 0;
    }

    font_t *ret = xcalloc(1, sizeof(font_t));
    ret->xft_font = xft_font;
    ret->descent = xft_font->descent;
    ret->height = xft_font->ascent + xft_font->descent;

    font_list = xreallocarray(font_list, font_count + 1, sizeof(font_t));
    if (!font_list) {
        fprintf(stderr, "Failed to allocate %d font descriptors", font_count + 1);
        exit(EXIT_FAILURE);
    }
    font_list[font_count++] = ret;

    return 1;
}

enum {
    NET_WM_WINDOW_TYPE,
    NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_DESKTOP,
    NET_WM_STRUT_PARTIAL,
    NET_WM_STRUT,
    NET_WM_STATE,
    NET_WM_STATE_STICKY,
    NET_WM_STATE_ABOVE,
};

void
set_ewmh_atoms (void)
{
    const char *atom_names[] = {
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_DESKTOP",
        "_NET_WM_STRUT_PARTIAL",
        "_NET_WM_STRUT",
        "_NET_WM_STATE",
        // Leave those at the end since are batch-set
        "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_ABOVE",
    };
    const int atoms = sizeof(atom_names)/sizeof(char *);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
    xcb_atom_t atom_list[atoms];
    xcb_intern_atom_reply_t *atom_reply;

    // As suggested fetch all the cookies first (yum!) and then retrieve the
    // atoms to exploit the async'ness
    for (int i = 0; i < atoms; i++)
        atom_cookie[i] = xcb_intern_atom(c, 0, strlen(atom_names[i]), atom_names[i]);

    for (int i = 0; i < atoms; i++) {
        atom_reply = xcb_intern_atom_reply(c, atom_cookie[i], NULL);
        if (!atom_reply)
            return;
        atom_list[i] = atom_reply->atom;
        free(atom_reply);
    }

    // Prepare the strut array
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        bar_t *bar = mon->bar;
        int strut[12] = {0};
        if (topbar) {
            strut[2] = bar->height;
            strut[8] = bar->x;
            strut[9] = bar->x + bar->width - 1;
        } else {
            strut[3]  = bar->height;
            strut[10] = bar->x;
            strut[11] = bar->x + bar->width - 1;
        }

        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
        xcb_change_property(c, XCB_PROP_MODE_APPEND,  mon->bar->window, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ -1 } );
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 3, "bar");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 12, "lemonbar\0Bar");
    }
}

monitor_t *
monitor_new (int x, int y, int width, int height, char *name)
{
    monitor_t *ret;
    bar_t *bar;

    ret = xcalloc(1, sizeof(monitor_t));
    ret->name = name;
    ret->x = x;
    ret->y = y;
    ret->width = width;
    ret->height = height;
    ret->next = ret->prev = NULL;

    bar = xcalloc(1, sizeof(bar_t));
    ret->bar = bar;

    bar->x = x;
    bar->y = (topbar ? 0 : height - bar_height) + y;
    bar->width = width;
    bar->height = bar_height;

    bar->window = xcb_generate_id(c);

    int depth = (visual == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
    xcb_create_window(c, depth, bar->window, scr->root,
            bar->x, bar->y, width, bar->height, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
            XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP,
            (const uint32_t []){ bgc.v, bgc.v, dock, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS, colormap });

    bar->pixmap = xcb_generate_id(c);
    xcb_create_pixmap(c, depth, bar->pixmap, bar->window, width, bar->height);

    return ret;
}

void
monitor_add (monitor_t *mon)
{
    if (!monhead) {
        monhead = mon;
    } else if (!montail) {
        montail = mon;
        monhead->next = mon;
        mon->prev = monhead;
    } else {
        mon->prev = montail;
        montail->next = mon;
        montail = montail->next;
    }
}

int
mon_sort_cb (const void *p1, const void *p2)
{
    const monitor_t *m1 = (monitor_t *)p1;
    const monitor_t *m2 = (monitor_t *)p2;

    if (m1->x < m2->x || m1->y + m1->height <= m2->y)
        return -1;
    if (m1->x > m2->x || m1->y + m1->height > m2->y)
        return  1;

    return 0;
}

void
monitor_create_chain (monitor_t *mons, const int num)
{
    int i;
    int width = 0, height = 0;
    int left = 0;

    // Sort before use, but only if specific outputs were not specified on command line
    if (!num_outputs)
        qsort(mons, num, sizeof(monitor_t), mon_sort_cb);

    for (i = 0; i < num; i++) {
        int h = mons[i].y + mons[i].height;
        // Accumulated width of all monitors
        width += mons[i].width;
        // Get height of screen from y_offset + height of lowest monitor
        if (h >= height)
        height = h;
    }

//    if (bw < 0)
//        bw = width - bx;
//
    // Use the first font height as all the font heights have been set to the biggest of the set
    if (bar_height > height)
        bar_height = font_list[0]->height + bu + 2;

    // Check the geometry
    if (bar_height > height) {
        fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
        exit(EXIT_FAILURE);
    }

    // Left is a positive number or zero therefore monitors with zero width are excluded
    for (i = 0; i < num; i++) {
        if (mons[i].width > left) {
            monitor_t *mon = monitor_new(
                    mons[i].x + left,
                    mons[i].y,
                    min(width, mons[i].width - left),
                    mons[i].height,
                    mons[i].name? xstrdup(mons[i].name) : NULL);

            if (!mon)
                break;

            monitor_add(mon);

            width -= mons[i].width - left;

            // No need to check for other monitors
            if (width <= 0)
                break;
        }

        left -= mons[i].width;

        if (left < 0)
            left = 0;
    }
}

void
get_randr_monitors (void)
{
    xcb_randr_get_screen_resources_current_reply_t *rres_reply;
    xcb_randr_output_t *outputs;
    int i, j, num, valid = 0;

    rres_reply = xcb_randr_get_screen_resources_current_reply(c,
            xcb_randr_get_screen_resources_current(c, scr->root), NULL);

    if (!rres_reply) {
        fprintf(stderr, "Failed to get current randr screen resources\n");
        return;
    }

    num = xcb_randr_get_screen_resources_current_outputs_length(rres_reply);
    outputs = xcb_randr_get_screen_resources_current_outputs(rres_reply);

    // There should be at least one output
    if (num < 1) {
        free(rres_reply);
        return;
    }

    // Every entry starts with a size of 0, making it invalid until we fill in
    // the data retrieved from the Xserver.
    monitor_t *mons = xcalloc(max(num, num_outputs), sizeof(monitor_t));

    // Get all outputs
    for (i = 0; i < num; i++) {
        xcb_randr_get_output_info_reply_t *oi_reply;
        xcb_randr_get_crtc_info_reply_t *ci_reply;

        oi_reply = xcb_randr_get_output_info_reply(c, xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME), NULL);

        // Output disconnected or not attached to any CRTC ?
        if (!oi_reply || oi_reply->crtc == XCB_NONE || oi_reply->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(oi_reply);
            continue;
        }

        ci_reply = xcb_randr_get_crtc_info_reply(c,
                xcb_randr_get_crtc_info(c, oi_reply->crtc, XCB_CURRENT_TIME), NULL);

        if (!ci_reply) {
            fprintf(stderr, "Failed to get RandR crtc info\n");
            free(rres_reply);
            goto cleanup_mons;
        }

        int name_len = xcb_randr_get_output_info_name_length(oi_reply);
        uint8_t *name_ptr = xcb_randr_get_output_info_name(oi_reply);

        bool is_valid = true;

        if (num_outputs) {
            // Skip outputs missing from the list.
            is_valid = false;
            // Allocate monitors following the specified order.
            for (j = 0; j < num_outputs; j++) {
                // Already allocated, the list contains a duplicate.
                if (mons[j].name)
                    break;

                if (!memcmp(output_names[j], name_ptr, name_len) &&
                        strlen(output_names[j]) == name_len) {
                    is_valid = true;
                    break;
                }
            }
        }

        if (is_valid) {
            char *alloc_name = xcalloc(name_len + 1, 1);
            memcpy(alloc_name, name_ptr, name_len);

            // There's no need to handle rotated screens here (see #69)
            mons[i] = (monitor_t){ alloc_name, ci_reply->x, ci_reply->y,
                ci_reply->width, ci_reply->height, NULL, NULL, NULL };
            valid += 1;
        }

        free(oi_reply);
        free(ci_reply);
    }

    free(rres_reply);

    // Check for clones and inactive outputs
    for (i = 0; i < num; i++) {
        if (mons[i].width == 0)
            continue;

        for (j = 0; j < num; j++) {
            // Does I contain J ?

            if (i != j && mons[j].width) {
                if (mons[j].x >= mons[i].x && mons[j].x + mons[j].width <= mons[i].x + mons[i].width &&
                    mons[j].y >= mons[i].y && mons[j].y + mons[j].height <= mons[i].y + mons[i].height) {
                    mons[j].width = 0;
                    valid--;
                }
            }
        }
    }

    if (valid > 0) {
        monitor_t valid_mons[valid];
        for (i = j = 0; i < num && j < valid; i++) {
            if (mons[i].width != 0) {
                valid_mons[j++] = mons[i];
            }
        }

        monitor_create_chain(valid_mons, valid);
    } else {
        fprintf(stderr, "No usable RandR output found\n");
    }

cleanup_mons:
    for (i = 0; i < num; i++) {
        free(mons[i].name);
    }
    free(mons);
}


xcb_visualid_t get_visual() {
    XVisualInfo xv;
    xv.depth = 32;
    int result = 0;
    XVisualInfo* result_ptr = NULL;
    result_ptr = XGetVisualInfo(dpy, VisualDepthMask, &xv, &result);

    if (result > 0) {
        visual_ptr = result_ptr->visual;
        return result_ptr->visualid;
    }

    // Fallback to the default one
    visual_ptr = DefaultVisual(dpy, screen_number);
    return scr->root_visual;
}

// Parse an X-styled geometry string, we don't support signed offsets though.
bool
parse_geometry_string (char *str, int *tmp)
{
    char *p = str;
    int i = 0, j;

    if (!str || !str[0])
        return false;

    // The leading = is optional
    if (*p == '=')
        p++;

    while (*p) {
        // A geometry string has only 4 fields
        if (i >= 4) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        // Move on if we encounter a 'x' or '+'
        if (*p == 'x') {
            if (i > 0) // The 'x' must precede '+'
                break;
            i++; p++; continue;
        }
        if (*p == '+') {
            if (i < 1) // Stray '+', skip the first two fields
                i = 2;
            else
                i++;
            p++; continue;
        }
        // A digit must follow
        if (!isdigit(*p)) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        // Try to parse the number
        errno = 0;
        j = strtoul(p, &p, 10);
        if (errno) {
            fprintf(stderr, "Invalid geometry specified\n");
            return false;
        }
        tmp[i] = j;
    }

    return true;
}

void
parse_output_string(char *str)
{
    if (!str || !*str)
        return;
    output_names = xreallocarray(output_names, num_outputs + 1, sizeof(char*));
    output_names[num_outputs++] = xstrdup(str);
}

void
xconn (void)
{
    // Open XDisplay
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Couldn't execute XOpenDisplay\n");
        xcb_disconnect(c);
        exit(EXIT_FAILURE);
    }

    c = XGetXCBConnection(dpy);
    if (!c) {
        fprintf (stderr, "Couldnt connect to X\n");
        exit (EXIT_FAILURE);
    }

    XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "Couldn't connect to X\n");
        exit(EXIT_FAILURE);
    }

    // Grab infos from the first screen
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    // Try to get a RGBA visual and build the colormap for that
    visual = get_visual();

    colormap = xcb_generate_id(c);
    xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, scr->root, visual);
}

void
init (char *wm_name)
{
    // Try to load a default font
    if (font_count == 0)
        font_load("fixed");

    // We tried and failed hard, there's something wrong
    if (!font_count)
        exit(EXIT_FAILURE);

    // To make the alignment uniform, find maximum height
    int maxh = font_list[0]->height;
    for (int i = 1; i < font_count; i++)
        maxh = max(maxh, font_list[i]->height);

    // Set maximum height to all fonts
    for (int i = 0; i < font_count; i++)
        font_list[i]->height = maxh;

    bar_height = maxh + bu + 2;

    // Generate a list of screens
    const xcb_query_extension_reply_t *qe_reply;

    // Initialize monitor list head and tail
    monhead = montail = NULL;

    // Check if RandR is present
    qe_reply = xcb_get_extension_data(c, &xcb_randr_id);

    if (qe_reply && qe_reply->present) {
        get_randr_monitors();
    }

    if (!monhead && num_outputs != 0) {
        fprintf(stderr, "Failed to find any specified outputs\n");
        exit(EXIT_FAILURE);
    }

    if (!monhead) {
        // If I fits I sits
//        if (bw < 0)
//            bw = scr->width_in_pixels - bx;

        // Adjust the height
//        if (bh < 0 || bh > scr->height_in_pixels)
//            bh = maxh + bu + 2;

        // Check the geometry
        if (bar_height > scr->height_in_pixels) {
            fprintf(stderr, "The geometry specified doesn't fit the screen!\n");
            exit(EXIT_FAILURE);
        }

        // If no RandR outputs or Xinerama screens, fall back to using whole screen
        monhead = monitor_new(0, 0, scr->width_in_pixels, scr->height_in_pixels, NULL);
    }

    if (!monhead)
        exit(EXIT_FAILURE);

    // For WM that support EWMH atoms
    set_ewmh_atoms();

    // Create the gc for drawing
    gc[GC_DRAW] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_DRAW], monhead->bar->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });

    gc[GC_CLEAR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_CLEAR], monhead->bar->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });

    gc[GC_ATTR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_ATTR], monhead->bar->pixmap, XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });

    // Make the bar visible and clear the pixmap
    for (monitor_t *mon = monhead; mon; mon = mon->next) {
        bar_t *bar = mon->bar;
        fill_rect(bar->pixmap, gc[GC_CLEAR], 0, 0, bar->width, bar->height);
        xcb_map_window(c, bar->window);

        // Make sure that the window really gets in the place it's supposed to be
        // Some WM such as Openbox need this
        xcb_configure_window(c, bar->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE, (const uint32_t []){ bar->x, bar->y, XCB_STACK_MODE_BELOW });

        // Set the WM_NAME atom to the user specified value
        if (wm_name)
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, mon->bar->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8 ,strlen(wm_name), wm_name);
    }

    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);

    if (!XftColorAllocName (dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }

    xcb_flush(c);
}

void
cleanup (void)
{
    for (int i = 0; i < num_outputs; i++) {
        free(output_names[i]);
    }
    free(output_names);

    free(area_stack.ptr);

    for (int i = 0; i < font_count; i++) {
        if (font_list[i]->xft_font) {
            XftFontClose(dpy, font_list[i]->xft_font);
            continue;
        }
        free(font_list[i]);
    }
    free(font_list);

    while (monhead) {
        monitor_t *next = monhead->next;
        xcb_destroy_window(c, monhead->bar->window);
        xcb_free_pixmap(c, monhead->bar->pixmap);
        free(monhead->bar);
        free(monhead->name);
        free(monhead);
        monhead = next;
    }

    XftColorFree(dpy, visual_ptr, colormap, &sel_fg);
    xcb_free_colormap(c, colormap);

    if (gc[GC_DRAW])
        xcb_free_gc(c, gc[GC_DRAW]);
    if (gc[GC_CLEAR])
        xcb_free_gc(c, gc[GC_CLEAR]);
    if (gc[GC_ATTR])
        xcb_free_gc(c, gc[GC_ATTR]);
    if (c)
        xcb_disconnect(c);
    if (dpy)
        XCloseDisplay(dpy);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
        exit(EXIT_SUCCESS);
}

int
main (int argc, char **argv)
{
    struct pollfd pollin[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = -1          , .events = POLLIN },
    };
    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;
    xcb_button_press_event_t *press_ev;
    char input[4096] = {0, };
    size_t input_offset = 0;
    bool permanent = false;
    int geom_v[4] = { -1, -1, 0, 0 };
    int ch;
    char *wm_name;

    // Install the parachute!
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    // B/W combo
    dbgc = bgc = BLACK;
    dfgc = fgc = WHITE;
    dugc = ugc = fgc;

    // A safe default
    wm_name = NULL;

    // Connect to the Xserver and initialize scr
    xconn();

    while ((ch = getopt(argc, argv, "hg:o:bdf:a:pu:B:F:U:n:")) != -1) {
        switch (ch) {
            case 'h':
                printf ("lemonbar version %s\n", VERSION);
                printf ("usage: %s [-h | -g | -o | -b | -d | -f | -p | -n | -u | -B | -F]\n"
                        "\t-h Show this help\n"
                        "\t-g Set the bar geometry {width}x{height}+{xoffset}+{yoffset}\n"
                        "\t-o Add randr output by name\n"
                        "\t-b Put the bar at the bottom of the screen\n"
                        "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-f Set the font name to use\n"
                        "\t-p Don't close after the data ends\n"
                        "\t-n Set the WM_NAME atom to the specified value for this bar\n"
                        "\t-u Set the underline/overline height in pixels\n"
                        "\t-B Set background color in #AARRGGBB\n"
                        "\t-F Set foreground color in #AARRGGBB\n", argv[0]);
                exit (EXIT_SUCCESS);
            case 'g': (void)parse_geometry_string(optarg, geom_v); break;
            case 'o': (void)parse_output_string(optarg); break;
            case 'p': permanent = true; break;
            case 'n': wm_name = xstrdup(optarg); break;
            case 'b': topbar = false; break;
            case 'd': dock = true; break;
            case 'f': font_load(optarg); break;
            case 'u': bu = strtoul(optarg, NULL, 10); break;
            case 'B': dbgc = bgc = parse_color(optarg, NULL, BLACK); break;
            case 'F': dfgc = fgc = parse_color(optarg, NULL, WHITE); break;
            case 'U': dugc = ugc = parse_color(optarg, NULL, fgc); break;
        }
    }

    // Initialize the stack holding the clickable areas
    area_stack.index = 0;
    area_stack.alloc = 10;
    area_stack.ptr = xcalloc(10, sizeof(area_t));

    // Copy the geometry values in place
//    bw = geom_v[0];
//    bh = geom_v[1];
//    bx = geom_v[2];
//    by = geom_v[3];

    // Do the heavy lifting
    init(wm_name);
    // The string is strdup'd when the command line arguments are parsed
    free(wm_name);
    // Get the fd to Xserver
    pollin[1].fd = xcb_get_file_descriptor(c);

#ifdef __OpenBSD__
    if (pledge("stdio rpath", NULL) < 0) {
        err(EXIT_FAILURE, "pledge failed");
    }
#endif

    for (;;) {
        bool redraw = false;

        // If connection is in error state, then it has been shut down.
        if (xcb_connection_has_error(c))
            break;

        if (poll(pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {      // No more data...
                if (permanent) pollin[0].fd = -1;   // ...null the fd and continue polling :D
                else break;                         // ...bail out
            }
            if (pollin[0].revents & POLLIN) { // New input, process it
                while (true) {
                    ssize_t r = read(STDIN_FILENO, input + input_offset,
                            sizeof(input) - input_offset);
                    if (r == 0) break;
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        exit(EXIT_FAILURE);
                    }

                    input_offset += r;

                    // Try to find the last complete input line in the buffer.
                    char *input_end = input + input_offset;
                    char *last_nl = memrchr(input, '\n', input_end - input);

                    if (last_nl) {
                        char *prev_nl = (last_nl != input) ?
                                memrchr(input, '\n', last_nl - 1 - input) : NULL;
                        char *begin = prev_nl? prev_nl + 1: input;

                        *last_nl = '\0';

                        parse(begin);
                        redraw = true;

                        // Move the unparsed part back to the beginning.
                        const size_t remaining = input_end - (last_nl + 1);
                        if (remaining != 0) memmove(input, last_nl + 1, remaining);
                        input_offset = remaining;

                        break;
                    }

                    // The input buffer is full and we haven't seen a newline
                    // yet, discard everything and start from zero.
                    if (sizeof(input) == input_offset) {
                        input_offset = 0;
                    }
                }
            }
            if (pollin[1].revents & POLLIN) { // The event comes from the Xorg server
                while ((ev = xcb_poll_for_event(c))) {
                    expose_ev = (xcb_expose_event_t *)ev;

                    switch (ev->response_type & 0x7F) {
                        case XCB_EXPOSE:
                            if (expose_ev->count == 0)
                                redraw = true;
                            break;
                        case XCB_BUTTON_PRESS:
                            press_ev = (xcb_button_press_event_t *)ev;
                            {
                                area_t *area = area_get(press_ev->event, press_ev->detail, press_ev->event_x);
                                // Respond to the click
                                if (area) {
                                    (void)write(STDOUT_FILENO, area->cmd, strlen(area->cmd));
                                    (void)write(STDOUT_FILENO, "\n", 1);
                                }
                            }
                            break;
                    }

                    free(ev);
                }
            }
        }

        if (redraw) { // Copy our temporary pixmap onto the window
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
                bar_t *bar = mon->bar;
                xcb_copy_area(c, bar->pixmap, bar->window, gc[GC_DRAW], 0, 0, 0, 0, bar->width, bar->height);
            }
        }

        xcb_flush(c);
    }

    return EXIT_SUCCESS;
}
