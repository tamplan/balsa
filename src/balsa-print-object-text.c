/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 * Copyright (C) 1997-2016 Stuart Parmenter and others
 * Written by (C) Albrecht Dreß <albrecht.dress@arcor.de> 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H
#   include "config.h"
#endif                          /* HAVE_CONFIG_H */
#include "balsa-print-object-text.h"

#include <gtk/gtk.h>
#include <string.h>
#include <glib/gi18n.h>
#include "libbalsa.h"
#include "balsa-print-object.h"
#include "balsa-print-object-decor.h"
#include "balsa-print-object-default.h"


typedef enum {
    PHRASE_BF = 0,
    PHRASE_EM,
    PHRASE_UL,
    PHRASE_TYPE_COUNT
} PhraseType;

typedef struct {
    PhraseType phrase_type;
    guint start_index;
    guint end_index;
} PhraseRegion;


struct _BalsaPrintObjectText {
    BalsaPrintObject parent;

    gint p_label_width;
    gchar *text;
    guint cite_level;
    GList *attributes;
};


/* object related functions */
static void balsa_print_object_text_finalize(GObject *self);

static void balsa_print_object_text_draw(BalsaPrintObject *self,
                                         GtkPrintContext  *context,
                                         cairo_t          *cairo_ctx);

static GList *collect_attrs(GList *all_attr,
                            guint  offset,
                            guint  len);
static PangoAttrList *phrase_list_to_pango(GList *phrase_list);
static GList         *phrase_highlight(const gchar *buffer,
                                       gunichar     tag_char,
                                       PhraseType   tag_type,
                                       GList       *phrase_list);


G_DEFINE_TYPE(BalsaPrintObjectText,
              balsa_print_object_text,
              BALSA_TYPE_PRINT_OBJECT)


static void
balsa_print_object_text_class_init(BalsaPrintObjectTextClass *klass)
{
    BALSA_PRINT_OBJECT_CLASS(klass)->draw = balsa_print_object_text_draw;
    G_OBJECT_CLASS(klass)->finalize       = balsa_print_object_text_finalize;
}


static void
balsa_print_object_text_init(BalsaPrintObjectText *pot)
{
    pot->text       = NULL;
    pot->attributes = NULL;
}


static void
balsa_print_object_text_finalize(GObject *self)
{
    BalsaPrintObjectText *pot = BALSA_PRINT_OBJECT_TEXT(self);

    g_list_free_full(pot->attributes, g_free);
    g_free(pot->text);

    G_OBJECT_CLASS(balsa_print_object_text_parent_class)->finalize(self);
}


/* prepare a text/plain part, which gets
 * - citation bars and colourisation of cited text (prefs dependant)
 * - syntax highlighting (prefs dependant)
 * - RFC 3676 "flowed" processing */
GList *
balsa_print_object_text_plain(GList               *list,
                              GtkPrintContext     *context,
                              LibBalsaMessageBody *body,
                              BalsaPrintSetup     *psetup)
{
    GRegex *rex;
    gchar *textbuf;
    PangoFontDescription *font;
    gdouble c_at_x;
    gdouble c_use_width;
    guint first_page;
    gchar *par_start;
    gchar *eol;
    gint par_len;

    /* set up the regular expression for qouted text */
    if (!(rex = balsa_quote_regex_new()))
        return balsa_print_object_default(list, context, body, psetup);

    /* start on new page if less than 2 lines can be printed */
    if (psetup->c_y_pos + 2 * P_TO_C(psetup->p_body_font_height) >
        psetup->c_height) {
        psetup->c_y_pos = 0;
        psetup->page_count++;
    }
    c_at_x      = psetup->c_x0 + psetup->curr_depth * C_LABEL_SEP;
    c_use_width = psetup->c_width - 2 * psetup->curr_depth * C_LABEL_SEP;

    /* copy the text body to a buffer */
    if (body->buffer)
        textbuf = g_strdup(body->buffer);
    else
        libbalsa_message_body_get_content(body, &textbuf, NULL);

    /* fake an empty buffer if textbuf is NULL */
    if (!textbuf)
        textbuf = g_strdup("");

    /* be sure the we have correct utf-8 stuff here... */
    libbalsa_utf8_sanitize(&textbuf, balsa_app.convert_unknown_8bit, NULL);

    /* apply flowed if requested */
    if (libbalsa_message_body_is_flowed(body)) {
        GString *flowed;

        flowed =
            libbalsa_process_text_rfc2646(textbuf, G_MAXINT, FALSE, FALSE,
                                          FALSE,
                                          libbalsa_message_body_is_delsp
                                              (body));
        g_free(textbuf);
        textbuf = flowed->str;
        g_string_free(flowed, FALSE);
    }

    /* get the font */
    font = pango_font_description_from_string(balsa_app.print_body_font);

    /* loop over paragraphs */
    par_start = textbuf;
    eol       = strchr(par_start, '\n');
    par_len   = eol ? eol - par_start : (gint) strlen(par_start);
    while (*par_start) {
        GString *level_buf;
        guint curr_level;
        guint cite_level;
        GList *par_parts;
        GList *this_par_part;
        GList *attr_list;
        PangoLayout *layout;
        PangoAttrList *pango_attr_list;
        GArray *attr_offs;
        gdouble c_at_y;

        level_buf  = NULL;
        curr_level = 0;         /* just to make the compiler happy */
        do {
            gchar *thispar;
            guint cite_idx;

            thispar = g_strndup(par_start, par_len);

            /* get the cite level and strip off the prefix */
            if (libbalsa_match_regex(thispar, rex, &cite_level, &cite_idx)) {
                gchar *new;

                new = thispar + cite_idx;
                if (g_unichar_isspace(g_utf8_get_char(new)))
                    new = g_utf8_next_char(new);
                new = g_strdup(new);
                g_free(thispar);
                thispar = new;
            }

            /* glue paragraphs with the same cite level together */
            if (!level_buf || (curr_level == cite_level)) {
                if (!level_buf) {
                    level_buf  = g_string_new(thispar);
                    curr_level = cite_level;
                } else {
                    level_buf = g_string_append_c(level_buf, '\n');
                    level_buf = g_string_append(level_buf, thispar);
                }

                par_start = eol ? eol + 1 : par_start + par_len;
                eol       = strchr(par_start, '\n');
                par_len   = eol ? eol - par_start : (gint) strlen(par_start);
            }

            g_free(thispar);
        } while (*par_start && (curr_level == cite_level));

        /* configure the layout so we can use Pango to split the text into pages */
        layout = gtk_print_context_create_pango_layout(context);
        pango_layout_set_font_description(layout, font);
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

        /* leave place for the citation bars */
        pango_layout_set_width(layout,
                               C_TO_P(c_use_width - curr_level * C_LABEL_SEP));

        /* highlight structured phrases if requested */
        if (balsa_app.print_highlight_phrases) {
            attr_list =
                phrase_highlight(level_buf->str, '*', PHRASE_BF, NULL);
            attr_list =
                phrase_highlight(level_buf->str, '/', PHRASE_EM, attr_list);
            attr_list =
                phrase_highlight(level_buf->str, '_', PHRASE_UL, attr_list);
        } else {
            attr_list = NULL;
        }

        /* start on new page if less than one line can be printed */
        if (psetup->c_y_pos + P_TO_C(psetup->p_body_font_height) >
            psetup->c_height) {
            psetup->c_y_pos = 0;
            psetup->page_count++;
        }

        /* split paragraph if necessary */
        pango_attr_list = phrase_list_to_pango(attr_list);
        first_page      = psetup->page_count - 1;
        c_at_y          = psetup->c_y_pos;
        par_parts       =
            split_for_layout(layout, level_buf->str, pango_attr_list,
                             psetup, FALSE, &attr_offs);
        if (pango_attr_list)
            pango_attr_list_unref(pango_attr_list);
        g_object_unref(G_OBJECT(layout));
        g_string_free(level_buf, TRUE);

        /* each part is a new text object */
        this_par_part = par_parts;
        while (this_par_part) {
            BalsaPrintObjectText *pot;
            BalsaPrintObject *po;

            pot = g_object_new(BALSA_TYPE_PRINT_OBJECT_TEXT, NULL);
            g_assert(pot != NULL);
            po = BALSA_PRINT_OBJECT(pot);
            balsa_print_object_set_on_page(po, first_page++);
            balsa_print_object_set_c_at_x(po, c_at_x);
            balsa_print_object_set_c_at_y(po, psetup->c_y0 + c_at_y);
            balsa_print_object_set_depth(po, psetup->curr_depth);
            c_at_y = 0.0;
            balsa_print_object_set_c_width(po, c_use_width);
            /* note: height is calculated when the object is drawn */
            pot->text       = (gchar *) this_par_part->data;
            pot->cite_level = curr_level;
            pot->attributes =
                collect_attrs(attr_list,
                              g_array_index(attr_offs, guint, 0),
                              strlen(pot->text));

            list = g_list_append(list, pot);
            g_array_remove_index(attr_offs, 0);
            this_par_part = this_par_part->next;
        }
        g_list_free_full(attr_list, g_free);
        g_list_free(par_parts);
        g_array_unref(attr_offs);
    }

    /* clean up */
    pango_font_description_free(font);
    g_free(textbuf);
    g_regex_unref(rex);
    return list;
}


/* prepare a text part which is simply printed "as is" without all the bells
 * and whistles of text/plain (see above) */
GList *
balsa_print_object_text(GList               *list,
                        GtkPrintContext     *context,
                        LibBalsaMessageBody *body,
                        BalsaPrintSetup     *psetup)
{
    gchar *textbuf;
    PangoFontDescription *font;
    gdouble c_at_x;
    gdouble c_use_width;
    guint first_page;
    GList *par_parts;
    GList *this_par_part;
    PangoLayout *layout;
    gdouble c_at_y;

    /* start on new page if less than 2 lines can be printed */
    if (psetup->c_y_pos + 2 * P_TO_C(psetup->p_body_font_height) >
        psetup->c_height) {
        psetup->c_y_pos = 0;
        psetup->page_count++;
    }
    c_at_x      = psetup->c_x0 + psetup->curr_depth * C_LABEL_SEP;
    c_use_width = psetup->c_width - 2 * psetup->curr_depth * C_LABEL_SEP;

    /* copy the text body to a buffer */
    if (body->buffer)
        textbuf = g_strdup(body->buffer);
    else
        libbalsa_message_body_get_content(body, &textbuf, NULL);

    /* fake an empty buffer if textbuf is NULL */
    if (!textbuf)
        textbuf = g_strdup("");

    /* be sure the we have correct utf-8 stuff here... */
    libbalsa_utf8_sanitize(&textbuf, balsa_app.convert_unknown_8bit, NULL);

    /* get the font */
    font = pango_font_description_from_string(balsa_app.print_body_font);

    /* configure the layout so we can use Pango to split the text into pages */
    layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_width(layout, C_TO_P(c_use_width));

    /* split paragraph if necessary */
    first_page = psetup->page_count - 1;
    c_at_y     = psetup->c_y_pos;
    par_parts  = split_for_layout(layout, textbuf, NULL, psetup, FALSE, NULL);
    g_object_unref(G_OBJECT(layout));
    pango_font_description_free(font);
    g_free(textbuf);

    /* each part is a new text object */
    this_par_part = par_parts;
    while (this_par_part) {
        BalsaPrintObjectText *pot;
        BalsaPrintObject *po;

        pot = g_object_new(BALSA_TYPE_PRINT_OBJECT_TEXT, NULL);
        g_assert(pot != NULL);
        po = BALSA_PRINT_OBJECT(pot);
        balsa_print_object_set_on_page(po, first_page++);
        balsa_print_object_set_c_at_x(po, c_at_x);
        balsa_print_object_set_c_at_y(po, psetup->c_y0 + c_at_y);
        balsa_print_object_set_depth(po, psetup->curr_depth);
        c_at_y = 0.0;
        balsa_print_object_set_c_width(po, c_use_width);
        /* note: height is calculated when the object is drawn */
        pot->text       = (gchar *) this_par_part->data;
        pot->cite_level = 0;
        pot->attributes = NULL;

        list          = g_list_append(list, pot);
        this_par_part = this_par_part->next;
    }
    g_list_free(par_parts);

    return list;
}


static void
balsa_print_object_text_draw(BalsaPrintObject *self,
                             GtkPrintContext  *context,
                             cairo_t          *cairo_ctx)
{
    BalsaPrintObjectText *pot;
    PangoFontDescription *font;
    gint p_height;
    PangoLayout *layout;
    PangoAttrList *attr_list;

    pot = BALSA_PRINT_OBJECT_TEXT(self);
    g_assert(pot != NULL);

    /* prepare */
    font   = pango_font_description_from_string(balsa_app.print_body_font);
    layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(layout,
                           C_TO_P(balsa_print_object_get_c_width(self) - pot->cite_level *
                                  C_LABEL_SEP));
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, pot->text, -1);
    if ((attr_list = phrase_list_to_pango(pot->attributes))) {
        pango_layout_set_attributes(layout, attr_list);
        pango_attr_list_unref(attr_list);
    }
    pango_layout_get_size(layout, NULL, &p_height);
    if (pot->cite_level > 0) {
        cairo_save(cairo_ctx);
        if (balsa_app.print_highlight_cited) {
            gint k = (pot->cite_level - 1) % MAX_QUOTED_COLOR;

            cairo_set_source_rgb(cairo_ctx,
                                 balsa_app.quoted_color[k].red,
                                 balsa_app.quoted_color[k].green,
                                 balsa_app.quoted_color[k].blue);
        }
    }
    cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(
                      self) + pot->cite_level * C_LABEL_SEP,
                  balsa_print_object_get_c_at_y(self));
    pango_cairo_show_layout(cairo_ctx, layout);
    g_object_unref(G_OBJECT(layout));
    if (pot->cite_level > 0) {
        guint n;

        cairo_new_path(cairo_ctx);
        cairo_set_line_width(cairo_ctx, 1.0);
        for (n = 0; n < pot->cite_level; n++) {
            gdouble c_xpos = balsa_print_object_get_c_at_x(self) + 0.5 + n * C_LABEL_SEP;

            cairo_move_to(cairo_ctx, c_xpos, balsa_print_object_get_c_at_y(self));
            cairo_line_to(cairo_ctx, c_xpos,
                          balsa_print_object_get_c_at_y(self) + P_TO_C(p_height));
        }
        cairo_stroke(cairo_ctx);
        cairo_restore(cairo_ctx);
    }

    balsa_print_object_set_c_height(self, P_TO_C(p_height));    /* needed to properly print
                                                                   borders */
}


#define UNICHAR_PREV(p)  g_utf8_get_char(g_utf8_prev_char(p))

static GList *
phrase_highlight(const gchar *buffer,
                 gunichar     tag_char,
                 PhraseType   tag_type,
                 GList       *phrase_list)
{
    gchar *utf_start;

    /* find the tag char in the text and scan the buffer for
       <buffer start or whitespace><tag char><alnum><any text><alnum><tagchar>
       <whitespace, punctuation or buffer end> */
    utf_start = g_utf8_strchr(buffer, -1, tag_char);
    while (utf_start) {
        gchar *s_next = g_utf8_next_char(utf_start);

        if (((utf_start == buffer)
             || g_unichar_isspace(UNICHAR_PREV(utf_start)))
            && (*s_next != '\0')
            && g_unichar_isalnum(g_utf8_get_char(s_next))) {
            gchar *utf_end;
            gchar *line_end;
            gchar *e_next;

            /* found a proper start sequence - find the end or eject */
            if (!(utf_end = g_utf8_strchr(s_next, -1, tag_char)))
                return phrase_list;

            line_end = g_utf8_strchr(s_next, -1, '\n');
            e_next   = g_utf8_next_char(utf_end);
            while (!g_unichar_isalnum(UNICHAR_PREV(utf_end)) ||
                   !(*e_next == '\0' ||
                     g_unichar_isspace(g_utf8_get_char(e_next)) ||
                     g_unichar_ispunct(g_utf8_get_char(e_next)))) {
                if (!(utf_end = g_utf8_strchr(e_next, -1, tag_char)))
                    return phrase_list;

                e_next = g_utf8_next_char(utf_end);
            }

            /* append the attribute if there is no line break */
            if (!line_end || (line_end >= e_next)) {
                PhraseRegion *new_region = g_new0(PhraseRegion, 1);

                new_region->phrase_type = tag_type;
                new_region->start_index = utf_start - buffer;
                new_region->end_index   = e_next - buffer;
                phrase_list             = g_list_prepend(phrase_list, new_region);

                /* set the next start properly */
                utf_start =
                    *e_next ? g_utf8_strchr(e_next, -1, tag_char) : NULL;
            } else {
                utf_start =
                    *s_next ? g_utf8_strchr(s_next, -1, tag_char) : NULL;
            }
        } else {
            /* no start sequence, find the next start tag char */
            utf_start =
                *s_next ? g_utf8_strchr(s_next, -1, tag_char) : NULL;
        }
    }

    return phrase_list;
}


static PangoAttrList *
phrase_list_to_pango(GList *phrase_list)
{
    PangoAttrList *attr_list;
    PangoAttribute *ph_attr[PHRASE_TYPE_COUNT];
    gint n;

    if (!phrase_list)
        return NULL;

    attr_list          = pango_attr_list_new();
    ph_attr[PHRASE_BF] = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    ph_attr[PHRASE_EM] = pango_attr_style_new(PANGO_STYLE_ITALIC);
    ph_attr[PHRASE_UL] = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);

    while (phrase_list) {
        PhraseRegion *region = (PhraseRegion *) phrase_list->data;
        PangoAttribute *new_attr;

        new_attr              = pango_attribute_copy(ph_attr[region->phrase_type]);
        new_attr->start_index = region->start_index;
        new_attr->end_index   = region->end_index;
        pango_attr_list_insert(attr_list, new_attr);

        phrase_list = phrase_list->next;
    }

    for (n = 0; n < PHRASE_TYPE_COUNT; n++) {
        pango_attribute_destroy(ph_attr[n]);
    }

    return attr_list;
}


static GList *
collect_attrs(GList *all_attr,
              guint  offset,
              guint  len)
{
    GList *attr = NULL;

    while (all_attr) {
        PhraseRegion *region = (PhraseRegion *) all_attr->data;

        if (((region->start_index >= offset)
             && (region->start_index <= offset + len))
            || ((region->end_index >= offset)
                && (region->end_index <= offset + len))) {
            /* scan-build does not see this as initializing this_reg:
               PhraseRegion *this_reg =
                g_memdup(region, sizeof(PhraseRegion));
             */
            PhraseRegion *this_reg;

            this_reg  = g_new(PhraseRegion, 1);
            *this_reg = *region;

            this_reg->start_index = MAX(0, this_reg->start_index - offset);
            this_reg->end_index   = MIN(len, this_reg->end_index - offset);

            attr = g_list_prepend(attr, this_reg);
        }
        all_attr = all_attr->next;
    }

    return attr;
}
