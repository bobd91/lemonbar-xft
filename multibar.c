// vim:sw=4:ts=4:et:

/*
* Intial code taken from https://github.com/lemonboy/bar + xft fonts + wide fonts
* Modified:
* - use randr get_monitors
* - monitor order is as returned by randr
* - default output is to primary monitor
* - reconfigure/redraw bar when monitor layout changed
* Removed:
* - geometry setting
* - specified outputs
* - X font support
*/

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

static FILE *log_fd;
#define LOG(...) (fprintf(log_fd, __VA_ARGS__))

// Here be dragons

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

#define is_cookie(c) (c.sequence != 0)
#define free_cookie(c) (c.sequence = 0)

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
    bool primary;
    xcb_atom_t name_atom;
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
static int bar_height;
static int bar_line = 1; // Underline height
static rgba_t fgc, bgc, ugc;
static rgba_t dfgc, dbgc, dugc;
static area_stack_t area_stack;

static const char *atom_names[] = {
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
static xcb_atom_t atom_list[sizeof(atom_names)/sizeof(char *)];

static const rgba_t BLACK = (rgba_t){ .r = 0, .g = 0, .b = 0, .a = 255 };
static const rgba_t WHITE = (rgba_t){ .r = 255, .g = 255, .b = 255, .a = 255 };

#define MAX_WIDTHS (1 << 16)
static wchar_t xft_char[MAX_WIDTHS];
static char    xft_width[MAX_WIDTHS];
static Display *dpy;
static XftDraw *xft_draw;
static Visual *visual_ptr;
static XftColor sel_fg;
static int screen_number = 0;

static char *wm_class = "multibar\0MultiBar";
static char *wm_name = "MultiBar";
static char *opt_wm_name;

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
        fill_rect(bar->pixmap, gc[GC_ATTR], x, 0, w, bar_line);
    if (attrs & ATTR_UNDERL)
        fill_rect(bar->pixmap, gc[GC_ATTR], x, bar->height - bar_line, w, bar_line);
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
bar_clear(bar_t *bar) {
    fill_rect(bar->pixmap, gc[GC_CLEAR], 0, 0, bar->width, bar->height);
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
        bar_clear(m->bar);

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
                        int right_ep = cur_mon->bar->width / 2;
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
                        int right_ep = cur_mon->bar->width;
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

            //xcb_change_gc(c, gc[GC_DRAW] , XCB_GC_FONT, (const uint32_t []){ 0 });

            int w = draw_char(cur_mon->bar, cur_font, pos_x, align, ucs);

LOG("Drew %d at %d, width %d\n", ucs, pos_x, w);

            pos_x += w;
            area_shift(cur_mon->bar->window, align, w);
        }
    }
    XftDrawDestroy(xft_draw);
}

void 
font_load(const char *pattern) {
    XftFont *xft_font = XftFontOpenName(dpy, screen_number, pattern);
    if (!xft_font) {
        fprintf(stderr, "Could not load font \"%s\"\n", pattern);
        return;
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
intern_ewmh_atoms (void)
{
    const int atoms = sizeof(atom_names)/sizeof(char *);
    xcb_intern_atom_cookie_t atom_cookie[atoms];
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
}

void
update_ewmh_atoms(bar_t *bar) {

    // Prepare the strut array
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

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, bar->window, atom_list[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, strut);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, bar->window, atom_list[NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, strut);
}

void
set_ewmh_atoms(bar_t *bar) {
    char *name = opt_wm_name ? opt_wm_name : wm_name;

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, bar->window, atom_list[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &atom_list[NET_WM_WINDOW_TYPE_DOCK]);
    xcb_change_property(c, XCB_PROP_MODE_APPEND,  bar->window, atom_list[NET_WM_STATE], XCB_ATOM_ATOM, 32, 2, &atom_list[NET_WM_STATE_STICKY]);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, bar->window, atom_list[NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, (const uint32_t []){ -1 } );
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, bar->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(name), name);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, bar->window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 1 + strlen(wm_class) + strlen(wm_class + strlen(wm_class) + 1), wm_class);

    update_ewmh_atoms(bar);
}

bar_t *
bar_new(int x, int y, int width, int height) {
    bar_t *bar = xcalloc(1, sizeof(bar_t));

    bar->x = x;
    bar->y = y;
    bar->width = width;
    bar->height = height;
    bar->window = xcb_generate_id(c);
    bar->pixmap = xcb_generate_id(c);

    int depth = (visual == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
    xcb_create_window(c, depth, bar->window, scr->root,
            x, y, width, height, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
            XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP,
            (const uint32_t []){ bgc.v, bgc.v, dock, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS, colormap });

    // Make sure that the window really gets in the place it's supposed to be
    // Some WM such as Openbox need this
    xcb_configure_window(c, bar->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE, (const uint32_t []){ bar->x, bar->y, XCB_STACK_MODE_BELOW });

    set_ewmh_atoms(bar);

    xcb_create_pixmap(c, depth, bar->pixmap, bar->window, width, height);
    bar_clear(bar);

    xcb_map_window(c, bar->window);

    xcb_flush(c);

    return bar;
}

monitor_t *
monitor_new (bool primary, int x, int y, int width, int height, xcb_atom_t name_atom, char *name)
{
    monitor_t *mon;

    mon = xcalloc(1, sizeof(monitor_t));
    mon->primary = primary;
    mon->name_atom = name_atom;
    mon->name = name;
    mon->x = x;
    mon->y = y;
    mon->width = width;
    mon->height = height;
    mon->next = mon->prev = NULL;

    int bar_y = y + (topbar ? 0 : height - bar_height);
    mon->bar = bar_new(x, bar_y, width, bar_height);

    return mon;
}

// Caller frees
char *
get_atom_name(xcb_atom_t atom) {
    char *name = NULL;
    xcb_get_atom_name_reply_t *name_r;
    name_r = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, atom), NULL);
    if (name_r) {
        int len = xcb_get_atom_name_name_length(name_r);
        name = xcalloc(len + 1, 1);
        memcpy(name, xcb_get_atom_name_name(name_r), len);
        free(name_r);
    }
    return name;
}

// Look for existing monitor with same name atom
// If found remove from linked list and return it
// Otherwise return NULL
monitor_t *
extract_monitor(xcb_atom_t name_atom) {
    if(monhead == NULL) return NULL;

    for(monitor_t *m = monhead ; m ; m = m->next) {
        if(m->name_atom == name_atom) {
            if(m->next) {
                m->next->prev = m->prev;
            } else {
                montail = m->prev;
            }
            if(m->prev) {
                m->prev->next = m->next;
            } else {
                monhead = m->next;
            }
            m->next = m->prev = NULL;
            return m;
        }
    }
    return NULL;
}

void 
remove_bar(bar_t *bar) {
    xcb_destroy_window(c, bar->window);
    xcb_free_pixmap(c, bar->pixmap);
    free(bar);
}

void 
remove_monitor(monitor_t *mon) {
    free(mon->name);
    remove_bar(mon->bar);
    free(mon);
}

// Free memory and xcb resources for all monitors in current list
void 
remove_monitors() {
    monitor_t *mon = monhead;
    monitor_t *tmp;
    while(mon) {
        tmp = mon->next;
        remove_monitor(mon);
        mon = tmp;
    }
    monhead = montail = NULL;
}

void 
reconfigure_bar(bar_t *bar, int x, int y, int width, int height) {
    bool resized = bar->width != width || bar->height != height;

    bar->x = x;
    bar->y = y;
    bar->width = width;
    bar->height = height;

    xcb_configure_window(c, bar->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, (const uint32_t []){ x, y, width, height });
    
    update_ewmh_atoms(bar);
    
    if(resized) {
        xcb_pixmap_t prev_pixmap = bar->pixmap;
        bar->pixmap = xcb_generate_id(c);

        int depth = (visual == scr->root_visual) ? XCB_COPY_FROM_PARENT : 32;
        xcb_create_pixmap(c, depth, bar->pixmap, bar->window, width, height);
        bar_clear(bar);

        xcb_free_pixmap(c, prev_pixmap);
    }

    xcb_flush(c);
}

void
reconfigure_monitor(monitor_t *mon, int x, int y, int width, int height) {
    mon->x = x;
    mon->y = y;
    mon->width = width;
    mon->height = height;

    int bar_y = y + (topbar ? 0 : height - bar_height);
    reconfigure_bar(mon->bar, x, bar_y, width, bar_height);
}

void
display_bar(bar_t *bar) {

    LOG("Display Bar pixmap=%d window=%d width=%d height=%d\n", bar->pixmap, bar->window, bar->width, bar->height);
    
    xcb_copy_area(c, bar->pixmap, bar->window, gc[GC_DRAW], 0, 0, 0, 0, bar->width, bar->height);
}

// Process information on one monitor returned from RRGetMonitors
// If monitor name found in current list then remove from list and, if needed, reconfigure
// Otherwise make a new monitor
monitor_t * 
update_monitor_info(xcb_randr_monitor_info_t *rrmon) {
    xcb_atom_t name_atom = rrmon->name;
    monitor_t *mon = extract_monitor(name_atom);
    if (mon) {
        mon->primary = rrmon->primary;
        if ( mon->x != rrmon->x ||
                mon->y != rrmon->y ||
                mon->width != rrmon->width ||
                mon->height != rrmon->height ) {
            reconfigure_monitor(mon, rrmon->x, rrmon->y, rrmon->width, rrmon->height);
        }
    } else {
        mon = monitor_new(rrmon->primary, rrmon->x, rrmon->y, rrmon->width, rrmon->height, name_atom, get_atom_name(name_atom));
    }
    return mon;
}

void 
update_monitors(xcb_randr_get_monitors_reply_t *rrmon_r) {
    monitor_t monh;
    monitor_t *monp = &monh;
    monitor_t *mon = NULL;

    // Build a new monitor list
    xcb_randr_monitor_info_iterator_t rrmon_i;
    rrmon_i = xcb_randr_get_monitors_monitors_iterator(rrmon_r);
    while (rrmon_i.rem) {
        xcb_randr_monitor_info_t *rrmon = rrmon_i.data;
        mon = update_monitor_info(rrmon);

        monp->next = mon;
        mon->prev = monp;
        monp = mon;

        xcb_randr_monitor_info_next(&rrmon_i);
    }

    // Any monitors left in old list are no longer valid
    remove_monitors();

    // Swap in the new list
    monhead = monh.next;
    montail = mon;
}

void
get_randr_monitors (void)
{
    xcb_randr_get_monitors_reply_t *rrmon_r;

    rrmon_r = xcb_randr_get_monitors_reply(c, 
            xcb_randr_get_monitors(c, scr->root, 1), NULL);

    if (!rrmon_r) {
        fprintf(stderr, "Failed to get current randr monitors\n");
        return;
    }

    update_monitors(rrmon_r);
    free(rrmon_r);
}

xcb_visualid_t
get_visual() {
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

bool
xrandr_version(uint32_t major, uint32_t minor) {
    bool res = false;

    const xcb_query_extension_reply_t *qe_reply = xcb_get_extension_data(c, &xcb_randr_id);

    if (qe_reply && qe_reply->present) {
        xcb_randr_query_version_reply_t *ver_r;
        ver_r = xcb_randr_query_version_reply(c, 
                xcb_randr_query_version(c, major, minor), NULL);
        if (ver_r) {
            res = ver_r->major_version >= major && ver_r->minor_version >= minor;
            free(ver_r);
        }
    }
    return res;
}

void
init ()
{
    // Try to load a default font
    if (font_count == 0)
        font_load("spacing=monospace");

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

    // Set height of all bars
    bar_height = maxh + bar_line + 2;

    // For WM that support EWMH atoms
    intern_ewmh_atoms();

    // Create the gc for drawing
    gc[GC_DRAW] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_DRAW], scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ fgc.v });

    gc[GC_CLEAR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_CLEAR], scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ bgc.v });

    gc[GC_ATTR] = xcb_generate_id(c);
    xcb_create_gc(c, gc[GC_ATTR], scr->root, XCB_GC_FOREGROUND, (const uint32_t []){ ugc.v });


    char color[] = "#ffffff";
    uint32_t nfgc = fgc.v & 0x00ffffff;
    snprintf(color, sizeof(color), "#%06X", nfgc);

    if (!XftColorAllocName (dpy, visual_ptr, colormap, color, &sel_fg)) {
        fprintf(stderr, "Couldn't allocate xft font color '%s'\n", color);
    }

    // Set up monitors - need randr version 1.5 for GetMonitors
    if (xrandr_version(1, 5)) {
        get_randr_monitors();
    }

    if (!monhead) {
        fprintf(stderr, "No RANDR GetMonitors support, using entire screen\n");
        monhead = monitor_new(true, 0, 0, scr->width_in_pixels, scr->height_in_pixels, 0, "Default");
    }

    xcb_flush(c);
}

void
cleanup (void)
{

    free(area_stack.ptr);

    for (int i = 0; i < font_count; i++) {
        XftFontClose(dpy, font_list[i]->xft_font);
    }
    free(font_list);

    remove_monitors();  

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
    
    // The string is strdup'd when the command line arguments are parsed
    free(opt_wm_name);
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

log_fd = fopen("multibar.log", "a");

    struct pollfd pollin[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = -1          , .events = POLLIN },
    };
    xcb_generic_event_t *ev;
    xcb_expose_event_t *expose_ev;
    xcb_button_press_event_t *press_ev;
    xcb_configure_notify_event_t *configure_ev;
    xcb_randr_get_monitors_cookie_t rrmon_cookie = {.sequence = 0};
    xcb_randr_get_monitors_reply_t *rrmon_reply;

    char input[4096] = {0, };
    char copy[4096] = {0, };
    size_t input_offset = 0;
    bool permanent = false;
    int ch;
    bool monitors_changed = false;

    // Install the parachute!
    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    // B/W combo
    dbgc = bgc = BLACK;
    dfgc = fgc = WHITE;
    dugc = ugc = fgc;

    // Connect to the Xserver and initialize scr
    xconn();

    while ((ch = getopt(argc, argv, "hbdf:pn:u:B:F:U:")) != -1) {
        switch (ch) {
            case 'h':
                printf ("multibar version %s\n", VERSION);
                printf ("usage: %s [-h | -b | -d | -f | -p | -n | -u | -B | -F | -U]\n"
                        "\t-h Show this help\n"
                        "\t-b Put the bar at the bottom of the screen\n"
                        "\t-d Force docking (use this if your WM isn't EWMH compliant)\n"
                        "\t-f Set the font name to use\n"
                        "\t-p Don't close after the data ends\n"
                        "\t-n Set the WM_NAME atom to the specified value for this bar\n"
                        "\t-u Set the underline/overline height in pixels\n"
                        "\t-B Set background color in #AARRGGBB\n"
                        "\t-F Set foreground color in #AARRGGBB\n"
                        "\t-U Set underline/overline color in #AARRGGBB\n"
                        , argv[0]);
                exit (EXIT_SUCCESS);
            case 'b': topbar = false; break;
            case 'd': dock = true; break;
            case 'f': font_load(optarg); break;
            case 'p': permanent = true; break;
            case 'n': opt_wm_name = xstrdup(optarg); break;
            case 'u': bar_line = strtoul(optarg, NULL, 10); break;
            case 'B': dbgc = bgc = parse_color(optarg, NULL, BLACK); break;
            case 'F': dfgc = fgc = parse_color(optarg, NULL, WHITE); break;
            case 'U': dugc = ugc = parse_color(optarg, NULL, fgc); break;
        }
    }

    // Initialize the stack holding the clickable areas
    area_stack.index = 0;
    area_stack.alloc = 10;
    area_stack.ptr = xcalloc(10, sizeof(area_t));

    // Do the heavy lifting
    init();
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

        // may get multiple monitor change notifications for a single reconfig
        // so use async xcb request
        // then we can discard result if more change notifications arrive
        if (monitors_changed && !is_cookie(rrmon_cookie)) {
            monitors_changed = false;
            rrmon_cookie = xcb_randr_get_monitors(c, scr->root, 1);
        }

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

                        memcpy(copy, begin, 1 + last_nl - begin);

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
                        case XCB_CONFIGURE_NOTIFY:
                            configure_ev = (xcb_configure_notify_event_t *)ev;
                            if (configure_ev->event == scr->root && configure_ev->window == scr->root) {
                                monitors_changed = true;
                            }
                    }

                    free(ev);
                }
                if (is_cookie(rrmon_cookie) && xcb_poll_for_reply(c, rrmon_cookie.sequence, (void **)&rrmon_reply, NULL)) {
                    free_cookie(rrmon_cookie);
                    if (rrmon_reply) {
                        // check if monitors have changed since we requested them 
                        if (!monitors_changed) {
                            update_monitors(rrmon_reply);
                            parse(copy);
                            redraw = true;
                        }
                        free(rrmon_reply);
                    }
                }
            }
        }

        if (redraw) { // Copy our temporary pixmap onto the window
            for (monitor_t *mon = monhead; mon; mon = mon->next) {
                display_bar(mon->bar);

                LOG("Monitor %d %s %d %d %d %d\n", mon->name_atom, mon->name, mon->x, mon->y, mon->width, mon->height);
                bar_t *bar = mon->bar;
                LOG("Bar %d %d %d %d %d %d\n", bar->pixmap, bar->window, bar->x, bar->y, bar->width, bar->height);

            }
        }

        xcb_flush(c);
        

    }

    return EXIT_SUCCESS;
}
